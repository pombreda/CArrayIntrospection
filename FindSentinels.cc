#include "IIGlueReader.hh"
#include "PatternMatch-extras.hh"

#include <boost/container/flat_set.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/range/combine.hpp>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/PassManager.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/PatternMatch.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <unordered_map>

using namespace boost::adaptors;
using namespace boost::property_tree;
using namespace llvm;
using namespace llvm::PatternMatch;
using namespace std;

/**
 * This mutually-recursive group of functions check whether a given list of sentinel checks is
 * optional using a modified depth first search.  The basic question they attempt to answer is: "Is
 * there a nontrivial path from loop entry to loop entry without passing through a sentinel check?"
 *
 * Sentinel checks are pre-added to foundSoFar, current starts as loop entry, and the goal is loop
 * entry.
 **/

typedef unordered_set<const BasicBlock *> BlockSet;

static bool reachable(const Loop &, BlockSet &foundSoFar, const BasicBlock &current, const BasicBlock &goal);

static bool reachableNontrivially(const Loop &loop, BlockSet &foundSoFar, const BasicBlock &current, const BasicBlock &goal) {
	// look for reachable path across any one successor
	return any_of(succ_begin(&current), succ_end(&current),
		      [&](const BasicBlock * const succ) { return reachable(loop, foundSoFar, *succ, goal); });
}

static bool reachable(const Loop &loop, BlockSet &foundSoFar, const BasicBlock &current, const BasicBlock &goal) {
	// mark as found so we don't revisit in the future
	const bool novel = foundSoFar.insert(&current).second;

	// already explored here, or is intentionally closed-off sentinel check
	if (!novel) return false;
	
	// trivially reached goal
	if (&current == &goal){
		return true;
	}

	// not allowed to leave this loop
	if (!loop.contains(&current)){
		return false;
	}

	// not trivially done, so look for nontrivial path
	return reachableNontrivially(loop, foundSoFar, current, goal);
}

static bool DFSCheckSentinelOptional(const Loop &loop, BlockSet &foundSoFar) {
	const BasicBlock &loopEntry = *loop.getHeader();
	return reachableNontrivially(loop, foundSoFar, loopEntry, loopEntry);
}

namespace {
	class FindSentinels : public FunctionPass {
	public:
		FindSentinels();
		~FindSentinels();
		static char ID;
		void getAnalysisUsage(AnalysisUsage &) const final;
		bool runOnFunction(Function &) override final;
		void print(raw_ostream &, const Module *) const;
	private:
		unordered_map<Function *, unordered_map<BasicBlock const *, pair<BlockSet, bool>>> allSentinelChecks;
		Function * current; //Awful HACK because of the way llvm reuses Pass objects.
	};

	char FindSentinels::ID;
}

static const RegisterPass<FindSentinels> registration("find-sentinels",
		"Find each branch used to exit a loop when a sentinel value is found in an array",
		true, true);


inline FindSentinels::FindSentinels()
: FunctionPass(ID)
{
  outs() << "Constructor for " << this << "\n";
}

inline FindSentinels::~FindSentinels(){
	outs() << "Destructor for " << this << "\n";
}

void FindSentinels::getAnalysisUsage(AnalysisUsage &usage) const
{
	// read-only pass never changes anything
	usage.setPreservesAll();
	usage.addRequired<LoopInfo>();
	usage.addRequired<IIGlueReader>();
}


bool FindSentinels::runOnFunction(Function &func) {
	current = &func;
	const LoopInfo &LI = getAnalysis<LoopInfo>();
	const IIGlueReader &iiglue = getAnalysis<IIGlueReader>();
	unordered_map<BasicBlock const *, pair<BlockSet, bool>> functionSentinelChecks;
	// bail out early if func has no array arguments
	//up for discussion - seems to lead to some unintuitive results that I want to discuss before readding.
	/*if (!any_of(func.arg_begin(), func.arg_end(), [&](const Argument &arg) {
				return iiglue.isArray(arg);
			}))
		return false;*/
	//We must look through all the loops to determine if any of them contain a sentinel check.
	for (const Loop * const loop : LI) {
		BlockSet sentinelChecks;

		SmallVector<BasicBlock *, 4> exitingBlocks;
		loop->getExitingBlocks(exitingBlocks);
		for (BasicBlock * const exitingBlock : exitingBlocks) {

			TerminatorInst * const terminator = exitingBlock->getTerminator();
			// to be bound to pattern elements if match succeeds
			BasicBlock *trueBlock, *falseBlock;
			CmpInst::Predicate predicate;
			//This will need to be checked to make sure it corresponds to an argument identified as an array.
			Argument *pointer;
			Value *slot;

			// reusable pattern fragments

			auto loadPattern = m_Load(
				m_GetElementPointer(
					m_FormalArgument(pointer),
					m_Value(slot)
					)
				);

			auto compareZeroPattern = m_ICmp(predicate,
							 m_CombineOr(
								 loadPattern,
								 m_SExt(loadPattern)
								 ),
							 m_Zero()
				);

			// Clang 3.4 without optimization, after running mem2reg:
			//
			//     %0 = getelementptr inbounds i8* %pointer, i64 %slot
			//     %1 = load i8* %0, align 1
			//     %element = sext i8 %1 to i32
			//     %2 = icmp ne i32 %element, 0
			//     br i1 %2, label %trueBlock, label %falseBlock
			//
			// Clang 3.4 with any level of optimization:
			//
			//     %0 = getelementptr inbounds i8* %pointer, i64 %slot
			//     %1 = load i8* %0, align 1
			//     %element = icmp eq i8 %1, 0
			//     br i1 %element, label %trueBlock, label %falseBlock
			// When optimized code has an OR:
			//    %arrayidx = getelementptr inbounds i8* %pointer, i64 %slot
			//    %0 = load i8* %arrayidx, align 1, !tbaa !0
			//    %cmp = icmp eq i8 %0, %goal
			//    %cmp6 = icmp eq i8 %0, 0
			//    %or.cond = or i1 %cmp, %cmp6
			//    %indvars.iv.next = add i64 %indvars.iv, 1
			//    br i1 %or.cond, label %for.end, label %for.cond
			if (match(terminator,
				  m_Br(
					  m_CombineOr(
						  compareZeroPattern,
						  m_CombineOr(
							  m_Or(
								  compareZeroPattern,
								  m_Value()
								  ),
							  m_Or(
								  m_Value(),
								  compareZeroPattern
								  )
							  )
						  ),
					  trueBlock,
					  falseBlock)
				    )) {
				if (!iiglue.isArray(*pointer)) {
					continue;
				}
				// check that we actually leave the loop when sentinel is found
				const BasicBlock *sentinelDestination;
				switch (predicate) {
				case CmpInst::ICMP_EQ:
					sentinelDestination = trueBlock;
					break;
				case CmpInst::ICMP_NE:
					sentinelDestination = falseBlock;
					break;
				default:
					continue;
				}
				if (loop->contains(sentinelDestination)) {
					DEBUG(dbgs() << "dest still in loop!\n");
					continue;
				}
				// all tests pass; this is a possible sentinel check!

				DEBUG(dbgs() << "found possible sentinel check of %" << pointer->getName() << "[%" << slot->getName() << "]\n"
				      << "  exits loop by jumping to %" << sentinelDestination->getName() << '\n');
				//mark this block as one of the sentinel checks this loop.
				sentinelChecks.insert(exitingBlock);
				auto induction(loop->getCanonicalInductionVariable());
				if (induction)
					DEBUG(dbgs() << "  loop has canonical induction variable %" << induction->getName() << '\n');
				else
					DEBUG(dbgs() << "  loop has no canonical induction variable\n");
			}
		}
		if (sentinelChecks.empty()) {
			DEBUG(dbgs() << "There were no sentinel checks in this loop.\n");
			functionSentinelChecks[loop->getHeader()] = pair<BlockSet, bool>(sentinelChecks, true);
			continue;
		}
		BlockSet foundSoFar = sentinelChecks;
		bool optional = DFSCheckSentinelOptional(*loop, foundSoFar);
		functionSentinelChecks[loop->getHeader()] = pair<BlockSet, bool>(sentinelChecks, optional);
		if (optional) {
			DEBUG(dbgs() << "The sentinel check was optional!\n");
		}
		else {
			DEBUG(dbgs() << "The sentinel check was non-optional - hooray!\n");
		}
	}
	allSentinelChecks[&func] = functionSentinelChecks;
	// read-only pass never changes anything
	return false;
}
class loopCompare { // simple comparison function
   public:
      bool operator()(const BasicBlock* x,const BasicBlock* y) { return x->getName() < y->getName(); } 
};
void FindSentinels::print(raw_ostream &sink, const Module*) const {
	sink << "Analyzing function: " << current->getName() << "\n";
	if(allSentinelChecks.find(current) == allSentinelChecks.end()){
		sink << "\tDetected no sentinel checks\n";
		return;
	}
	unordered_map<BasicBlock const *, pair<BlockSet, bool>> loopSentinelChecks = allSentinelChecks.at(current);
	sink << "\tWe found: " << loopSentinelChecks.size() << " loops\n";
	set<BasicBlock const*, loopCompare> loops;
	for (auto mapElements : loopSentinelChecks) {
		loops.insert(mapElements.first);
	}

	for (const BasicBlock * const loop : loops) {
		pair<BlockSet, bool> entry = loopSentinelChecks[loop];
		BlockSet sentinelChecks = entry.first;
		bool optional = entry.second;
    	sink << "\tThere are " << sentinelChecks.size() << " sentinel checks in this loop(" << loop->getName() << ")\n";
    	sink << "\tWe can " << (optional?"":"not ") << "bypass all sentinel checks.\n";
    	const auto names =
    		sentinelChecks
    		| indirected
    		| transformed([](const BasicBlock &block) {
    		return (block.getName()).str();
    		});
		// print in sorted order for consistent output
  		boost::container::flat_set<string> ordered(names.begin(), names.end());
  		sink << "\tSentinel checks: \n";
  		for (const auto &sentinelCheck : ordered)
    		sink << "\t\t" << sentinelCheck << '\n';
    		}
}

//To be used if I need passes to run at a specific time in the opt-cycle - for now, this is unnecessary since we don't actually
//run very many opt passes, just the ones from O0 and mem2reg.
#if 0
static RegisterStandardPasses MyPassRegistration(PassManagerBuilder::EP_LoopOptimizerEnd,
						 [](const PassManagerBuilder&, PassManagerBase& PM) {
							 DEBUG(dbgs() << "Registered pass!\n");
							 PM.add(new FindSentinels());
						 });
#endif
