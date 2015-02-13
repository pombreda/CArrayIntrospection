#include "FindLengthChecks.hh"
#include "IIGlueReader.hh"
#include "UpperBoundIndexing.hh"

#include <llvm/Analysis/LoopPass.h>
#include <llvm/InstVisitor.h>
#include <llvm/IR/Module.h>

#include "/p/polyglot/public/tools/range-analysis/range-analysis-read-only/src/RangeAnalysis/RangeAnalysis.h"

using namespace boost;
using namespace llvm;
using namespace std;

struct CheckGetElementPtrVisitor : public InstVisitor<CheckGetElementPtrVisitor> {
	const IIGlueReader *iiglue;
	ArgumentToMaxIndexMap &maxIndexes;
	set<const Argument *> notBounded;
	IntraProceduralRA<Cousot> &rangeAnalysis;
	CheckGetElementPtrVisitor(const IIGlueReader *r, ArgumentToMaxIndexMap &map, IntraProceduralRA<Cousot> &ra) 
	: maxIndexes(map), rangeAnalysis(ra) {
		iiglue = r;
	}
	~CheckGetElementPtrVisitor() {
		for (const Argument * arg : notBounded) {
	  		maxIndexes.erase(maxIndexes.find(arg));
	  	}
	}
	void visitGetElementPtrInst(GetElementPtrInst& gepi) {
		errs() << "Top of visitor\n";
		Value *pointer = gepi.getPointerOperand();
		Argument *arg = dyn_cast<Argument>(pointer);
		errs() << "GEPI: " << gepi << "\n";
		if (gepi.getNumIndices() != 1 || (arg? !(iiglue->isArray(*arg)) : true)) {
			errs() << "Ignoring this one!\n";
			return; //in this case, we don't care.
			//need to do some thinking about higher number of indices, and make sure to have a 
			//consistent way of thinking about it.
			//should probably look at how it's usually documented.
		}
		errs() << "About to get the range\n";
		Range r = rangeAnalysis.getRange(gepi.idx_begin()->get());
		errs() << "Got it!\n";
		if (!r.isUnknown()) { //check range-analysis
			errs() << "Range not unknown!\n";
			int index = (int)r.getUpper().roundToDouble(); //TODO: talk to Ben about this - precision issue that may not be an issue.
			errs() << "index = " << index << "\n";
			if (index > maxIndexes[arg]) {
				errs() << "Yay range analysis! Adding to the map!\n";
				maxIndexes[arg] = index;
			}
		}
		else {
			errs() << "Not constant index\n";
			gepi.dump();
			errs() << "Index in question = " << *gepi.idx_begin()->get() << "\n";
			ConstantInt *constant;
			if ((constant = dyn_cast<ConstantInt>(gepi.idx_begin()->get()))) {
				errs() << "lol range-analysis and constants don't play nice?\n";
				int index = constant->getSExtValue();
				if(index > maxIndexes[arg]) {
					maxIndexes[arg] = index;
					errs() << "Adding to map\n";
				}
			}
			else {
			
				notBounded.insert(arg);
			}
		}
		errs() << "Bottom of visitor\n";
		/*
		else {
			ConstantInt *constant;
			if ((constant = dyn_cast<ConstantInt>(gepi.idx_begin()->get()))) {
				int index = constant->getSExtValue();
				if(index > maxIndexes[arg]) {
					maxIndexes[arg] = index;
					errs() << "Adding to map\n";
				}
			}
		 
			else {
				errs() << "Not constant index\n";
				notBounded.insert(arg);
			}
		}*/
	}
};

static const RegisterPass<FindLengthChecks> registration("find-length",
		"Find loops that have a fixed number of iterations that also index into arrays.",
		true, true);

char FindLengthChecks::ID;


inline FindLengthChecks::FindLengthChecks()
	: ModulePass(ID) {
}

void FindLengthChecks::getAnalysisUsage(AnalysisUsage &usage) const {
	// read-only pass never changes anything
	//usage.setPreservesAll();
	//usage.addRequired<LoopInfo>();
	usage.addRequired<IIGlueReader>();
	usage.addRequired<UpperBoundIndexing>();
	usage.addRequired<IntraProceduralRA<Cousot> >();

	//usage.addRequired<ScalarEvolution>();
}


bool FindLengthChecks::runOnModule(Module &module) {
	const IIGlueReader &iiglue = getAnalysis<IIGlueReader>();
	errs() << "Top of runOnModule()\n";
	for (Function &func : module) {
		errs() << "Analyzing " << func.getName() << "\n";
		CheckGetElementPtrVisitor visitor(&iiglue, maxIndexes[&func], getAnalysis<IntraProceduralRA<Cousot> >(func));
		for(BasicBlock &visitee :  func) {
			errs() << "Visiting a new basic block...\n";
			visitor.visit(visitee);
		}
	}
	// read-only pass never changes anything
	return false;
}

void FindLengthChecks::print(raw_ostream &sink, const Module *module) const {
	const IIGlueReader &iiglue = getAnalysis<IIGlueReader>();
	for (const Function &func : *module) {
		const ArgumentToMaxIndexMap map = maxIndexes.at(&func);
		sink << "Analyzing " << func.getName() << "\n";
		for (const Argument &arg : make_iterator_range(func.arg_begin(), func.arg_end())) {
			if (map.count(&arg))
				sink << "Argument " << arg.getName() << " has max index " << map.at(&arg) << '\n';
			else if (iiglue.isArray(arg))
				sink << "Argument " << arg.getName() << " has unknown max index.\n";
		}
	}
	
}