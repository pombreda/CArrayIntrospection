#include "IIGlueReader.hh"

#include <boost/container/flat_set.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/range/combine.hpp>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace boost::adaptors;
using namespace boost::property_tree;
using namespace llvm;
using namespace std;


char IIGlueReader::ID;

namespace {
	const RegisterPass<IIGlueReader>
	registration("iiglue-reader",
		     "Read iiglue analysis results and add them as metadata on corresponding LLVM entities",
		     true, true);

	static cl::opt<string>
	iiglueFileName("iiglue-read-file",
		       cl::Required,
		       cl::value_desc("filename"),
		       cl::desc("Filename containing iiglue analysis results"));
}


void IIGlueReader::getAnalysisUsage(AnalysisUsage &usage) const {
	// read-only pass never changes anything
	usage.setPreservesAll();
}


bool IIGlueReader::runOnModule(Module &module) {
	// make sure we know what to do, or complain if we don't
	if (iiglueFileName.empty()) {
		errs() << "warning: no input file given for iiglue reader\n";
		return false;
	}

	// read entire JSON-formatted iiglue output as property tree
	ptree root;
	json_parser::read_json(iiglueFileName, root);

	// iterate over iiglue-recognized library functions
	const ptree &libraryFunctions = root.get_child("libraryFunctions");
	for (const auto &functionInfo : libraryFunctions | map_values) {
		// find corresponding LLVM function object
		const string name = functionInfo.get<string>("foreignFunctionName");
		const Function * const function = module.getFunction(name);
		if (!function) {
			errs() << "warning: found function " << name << " in iiglue results but not in bitcode\n";
			continue;
		}

		// focus on function parameters, and check for arity mismatch
		const ptree &foreignFunctionParameters = functionInfo.get_child("foreignFunctionParameters");
		const Function::ArgumentListType &args = function->getArgumentList();
		if (foreignFunctionParameters.size() != args.size()) {
			errs() << "warning: function " << name << " has " << foreignFunctionParameters.size() << " arguments in iiglue results but " << args.size() << " arguments in bitcode\n";
			continue;
		}

		// iterate across each parameter and corresponding annotations
		const auto parameterAnnotations =
			foreignFunctionParameters
			| map_values
			| transformed([](const ptree &pt) {
					return pt.get_child("parameterAnnotations");
				})
			;
		for (const auto &slot : boost::combine(parameterAnnotations, args)) {
			// extract just the names (tags) of each annotation
			const auto annotationTags =
				slot.get<0>()
				| map_values
				| transformed([](const ptree &pt) {
						assert(pt.size() == 1);
						return pt.front();
					})
				| map_keys
				;

			// PAArray annotation means iiglue thinks this is an array;
			// ignore inferred array dimensionality: not needed yet
			if (find(annotationTags, "PAArray") != end(annotationTags)) {
				arrays.insert(&slot.get<1>());
				atLeastOneArrayArg.insert(function);
			}
		}
	}

	// we never change anything; we just stash information in private
	// fields of this pass instance for later use
	return false;
}


void IIGlueReader::print(raw_ostream &sink, const Module *) const {
	sink << "\tarray arguments:\n";

	// function-qualified names of array arguments
	const auto names =
		arrays
		| indirected
		| transformed([](const Argument &arg) {
				return (arg.getParent()->getName() + "::" + arg.getName()).str();
			});

	// print in sorted order for consistent output
	boost::container::flat_set<string> ordered(names.begin(), names.end());
	for (const auto &argument : ordered)
		sink << "\t\t" << argument << '\n';
}
