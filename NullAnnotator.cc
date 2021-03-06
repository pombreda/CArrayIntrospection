#define DEBUG_TYPE "null-annotator"
#include "Answer.hh"
#include "BacktrackPhiNodes.hh"
#include "FindSentinels.hh"
#include "IIGlueReader.hh"

#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/foreach.hpp>
#include <boost/lambda/core.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/combine.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/iterator_range.hpp>
#include <fstream>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>

#if (1000 * LLVM_VERSION_MAJOR + LLVM_VERSION_MINOR) >= 3005
#include <llvm/IR/InstIterator.h>
#else  // LLVM 3.4 or earlier
#include <llvm/Support/InstIterator.h>
#endif	// LLVM 3.4 or earlier

using namespace boost;
using namespace boost::adaptors;
using namespace boost::algorithm;
using namespace boost::property_tree;
using namespace llvm;
using namespace std;


namespace {
	class NullAnnotator : public ModulePass {
	public:
		// standard LLVM pass interface
		NullAnnotator();
		static char ID;
		void getAnalysisUsage(AnalysisUsage &) const final override;
		bool runOnModule(Module &) final override;
		void print(raw_ostream &, const Module *) const final override;

		// access to analysis results derived by this pass
		bool annotate(const Argument &) const;

	private:
		// map from function name and argument number to whether or not that argument gets annotated
		typedef unordered_map<const Argument *, Answer> AnnotationMap;
		AnnotationMap annotations;
		unordered_map<const Argument *, string> reasons;
		typedef unordered_set<const CallInst *> CallInstSet;
		unordered_map<const Function *, CallInstSet> functionToCallSites;
		Answer getAnswer(const Argument &) const;
		void dumpToFile(const string &filename, const IIGlueReader &, const Module &) const;
		void populateFromFile(const string &filename, const Module &);
	};


	char NullAnnotator::ID;
	static const RegisterPass<NullAnnotator> registration("null-annotator",
		"Determine whether and how to annotate each function with the null-terminated annotation",
		true, true);
	static cl::list<string>
		dependencyFileNames("dependency",
			cl::ZeroOrMore,
			cl::value_desc("filename"),
			cl::desc("Filename containing NullAnnotator results for dependencies; use multiple times to read multiple files"));
	static cl::opt<string>
		outputFileName("output",
			cl::Optional,
			cl::value_desc("filename"),
			cl::desc("Filename to write results to"));
}


static bool existsNonOptionalSentinelCheck(const FindSentinels::FunctionResults *checks, const Argument &arg) {
	if (checks == nullptr)
		return false;
	return any_of(*checks | map_values,
		      [&](const ArgumentToBlockSet &entry) { return !entry.at(&arg).second; });
}


static bool hasLoopWithSentinelCheck(const FindSentinels::FunctionResults *checks, const Argument &arg) {
	if (checks == nullptr)
		return false;
	return any_of(*checks | map_values,
		      [&](const ArgumentToBlockSet &entry) { return !entry.at(&arg).first.empty(); });
}


////////////////////////////////////////////////////////////////////////
//
//  test whether a specific argument may flow into a specific value
//  across zero or more phi nodes
//

namespace {
	class ArgumentReachesValue : public BacktrackPhiNodes {
	public:
		ArgumentReachesValue(const Argument &);
		void visit(const Argument &) final override;

	private:
		const Argument &goal;
	};
}


inline ArgumentReachesValue::ArgumentReachesValue(const Argument &goal)
	: goal(goal) {
}


void ArgumentReachesValue::visit(const Argument &reached) {
	if (&reached == &goal)
		throw this;
}


static bool argumentReachesValue(const Argument &goal, const Value &start) {
	ArgumentReachesValue explorer(goal);
	try {
		explorer.backtrack(start);
	} catch (const ArgumentReachesValue *) {
		return true;
	}
	return false;
}


////////////////////////////////////////////////////////////////////////


inline NullAnnotator::NullAnnotator()
	: ModulePass(ID) {
}


bool NullAnnotator::annotate(const Argument &arg) const {
	const AnnotationMap::const_iterator found = annotations.find(&arg);
	return found != annotations.end() && found->second == NULL_TERMINATED;
}


void NullAnnotator::getAnalysisUsage(AnalysisUsage &usage) const {
	// read-only pass never changes anything
	usage.setPreservesAll();
	usage.addRequired<IIGlueReader>();
	usage.addRequired<FindSentinels>();
}


Answer NullAnnotator::getAnswer(const Argument &arg) const {
	const AnnotationMap::const_iterator found = annotations.find(&arg);
	return found == annotations.end() ? DONT_CARE : found->second;
}


void NullAnnotator::populateFromFile(const string &filename, const Module &module) {
	ptree root;
	read_json(filename, root);
	const ptree &libraryFunctions = root.get_child("library_functions");
	for (const auto &framePair : libraryFunctions) {
		// find corresponding LLVM function object
		const string &name = framePair.first;
		const Function * const function = module.getFunction(name);
		if (!function) {
			errs() << "warning: found function " << name << " in iiglue results but not in bitcode\n";
			continue;
		}

		const Function::ArgumentListType &arguments = function->getArgumentList();
		const ptree &arg_annotations = framePair.second.get_child("argument_annotations");
		if (arguments.size() != arg_annotations.size()) {
			errs() << "Warning: Arity mismatch between function " << name
			       << " in the .json file provided: " << filename
			       << " and the one found in the bitcode. Skipping.\n";
			continue;
		}
		for (const auto &slot : boost::combine(arguments, arg_annotations)) {
			const Argument &argument = slot.get<0>();
			const Answer annotation = static_cast<Answer>(slot.get<1>().second.get_value<int>());
			annotations[&argument] = annotation;
		}
	}
}


template<typename Detail> static
void dumpArgumentDetails(ostream &out, const Function::ArgumentListType &argumentList, const char key[], const Detail &detail) {
	out << "\t\t\t\"" << key << "\": [";
	for (const Argument &argument : argumentList) {
		if (&argument != argumentList.begin())
			out << ", ";
		out << detail(argument);
	}
	out << ']';
}


void NullAnnotator::dumpToFile(const string &filename, const IIGlueReader &iiglue, const Module &module) const {
	ofstream out(filename);
	out << "{\n\t\"library_functions\": {\n";
	for (const Function &function : module) {

		if (&function != module.begin())
			out << ",\n";

		out << "\t\t\"" << function.getName().str() << "\": {\n";
		const Function::ArgumentListType &argumentList = function.getArgumentList();

		dumpArgumentDetails(out, argumentList, "argument_names",
				    [](const Argument &arg) {
					    return '\"' + arg.getName().str() + '\"';
				    }
			);
		out << ",\n";

		dumpArgumentDetails(out, argumentList, "argument_annotations",
				    [&](const Argument &arg) {
					    return getAnswer(arg);
				    }
			);
		out << ",\n";

		dumpArgumentDetails(out, argumentList, "args_array_receivers",
				    [&](const Argument &arg) {
					    return iiglue.isArray(arg);
				    }
			);
		out << ",\n";

		dumpArgumentDetails(out, argumentList, "argument_reasons",
				    [&](const Argument &arg) {
					    const auto reason = reasons.find(&arg);
					    return '\"' + (reason == reasons.end() ? "" : reason->second) + '\"';
				    }
			);

		out << "\n\t\t}";
	}
	out << "\n\t}\n}\n";
}


bool NullAnnotator::runOnModule(Module &module) {
	for (const string &dependency : dependencyFileNames) {
		populateFromFile(dependency, module);
	}
	const IIGlueReader &iiglue = getAnalysis<IIGlueReader>();

	// collect calls in each function for repeated scanning later
	for (const Function &func : iiglue.arrayReceivers()) {
		const auto instructions =
			make_iterator_range(inst_begin(func), inst_end(func))
			| transformed([](const Instruction &inst) { return dyn_cast<CallInst>(&inst); })
			| filtered(boost::lambda::_1);
		functionToCallSites.emplace(&func, CallInstSet(instructions.begin(), instructions.end()));
		DEBUG(dbgs() << "went through all the instructions and grabbed calls\n");
		DEBUG(dbgs() << "We found " << functionToCallSites[&func].size() << " calls in " << func.getName() << '\n');
	}

	const FindSentinels &findSentinels = getAnalysis<FindSentinels>();
	bool firstTime = true;
	bool changed;

	do {
		changed = false;
		for (const Function &func : iiglue.arrayReceivers()) {
			DEBUG(dbgs() << "About to get the map for this function\n");
			const FindSentinels::FunctionResults * const functionChecks = findSentinels.getResultsForFunction(&func);
			for (const Argument &arg : iiglue.arrayArguments(func)) {
				DEBUG(dbgs() << "\tConsidering " << arg.getArgNo() << "\n");
				Answer oldResult = getAnswer(arg);
				DEBUG(dbgs() << "\tOld result: " << oldResult << '\n');
				if (oldResult == NULL_TERMINATED)
					continue;
				if (firstTime) {
					// process loops exactly once
					if (existsNonOptionalSentinelCheck(functionChecks, arg)) {
						DEBUG(dbgs() << "\tFound a non-optional sentinel check in some loop!\n");
						annotations[&arg] = NULL_TERMINATED;
						reasons[&arg] = "Found a non-optional sentinel check in some loop of this function.";
						changed = true;
						continue;
					}
				}
				// if we haven't yet continued, process evidence from callees.
				bool foundDontCare = false;
				bool foundNonNullTerminated = false;
				bool nextArgumentPlease = false;
				for (const CallInst &call : functionToCallSites[&func] | indirected) {
					DEBUG(dbgs() << "About to iterate over the arguments to the call\n");
					DEBUG(dbgs() << "Call: " << call.getName() << "\n");
					DEBUG(dbgs() << "getCalledFunction name: " << call.getCalledFunction() << "\n");
					const auto calledFunction = call.getCalledFunction();
					if (calledFunction == nullptr)
						continue;
					const auto formals = calledFunction->getArgumentList().begin();
					DEBUG(dbgs() << "Got formals\n");
					for (const unsigned argNo : irange(0u, call.getNumArgOperands())) {
						DEBUG(dbgs() << "Starting iteration\n");
						const Value &actual = *call.getArgOperand(argNo);
						if (!argumentReachesValue(arg, actual)) continue;
						DEBUG(dbgs() << "Name of arg: " << arg.getName() << "\n");
						DEBUG(dbgs() << "hey, it matches!\n");

						auto parameter = next(formals, argNo);
						if (parameter == calledFunction->getArgumentList().end() || argNo != parameter->getArgNo()) {
							continue;
						}
						DEBUG(dbgs() << "About to enter the switch\n");
						switch (getAnswer(*parameter)) {
						case NULL_TERMINATED:
							DEBUG(dbgs() << "Marking NULL_TERMINATED\n");
							annotations[&arg] = NULL_TERMINATED;
							reasons[&arg] = "Called " + calledFunction->getName().str() + ", marked as null terminated in this position";
							changed = true;
							nextArgumentPlease = true;
							break;

						case NON_NULL_TERMINATED:
							// maybe set/check a flag for error reporting
							foundNonNullTerminated = true;
							break;

						case DONT_CARE:
							// maybe set/check a flag for error reporting
							if (foundNonNullTerminated) {
								DEBUG(dbgs() << "Found both DONT_CARE and NON_NULL_TERMINATED among callees.\n");
							}
							foundDontCare = true;
							break;

						default:
							// should never happen!
							abort();
						}
					}

					if (nextArgumentPlease) {
						break;
					}
				}
				if (nextArgumentPlease) {
					continue;
				}
				// if we haven't yet marked NULL_TERMINATED, might be NON_NULL_TERMINATED
				if (hasLoopWithSentinelCheck(functionChecks, arg)) {
					if (oldResult != NON_NULL_TERMINATED) {
						DEBUG(dbgs() << "Marking NOT_NULL_TERMINATED\n");
						annotations[&arg] = NON_NULL_TERMINATED;
						reasons[&arg] = "Has a loop with an optional sentinel check";
						changed = true;
						if (foundDontCare) {
							DEBUG(dbgs() << "Marking NOT_NULL_TERMINATED even though other calls say DONT_CARE.\n");
							// do error reporting stuff
						}
						continue;
					}
				}
				// otherwise it stays as DONT_CARE for now.
			}
		}
		firstTime = false;
	} while (changed);
	if (!outputFileName.empty())
		dumpToFile(outputFileName, iiglue, module);
	return false;
}


void NullAnnotator::print(raw_ostream &sink, const Module *module) const {
	const IIGlueReader &iiglue = getAnalysis<IIGlueReader>();
	for (const Function &func : *module) {
		for (const Argument &arg : iiglue.arrayArguments(func))
			if (annotate(arg))
				sink << func.getName() << " with argument " << arg.getArgNo()
				     << " should be annotated NULL_TERMINATED (" << (getAnswer(arg))
				     << ").\n";
	}
}
