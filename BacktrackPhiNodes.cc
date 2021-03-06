#include "BacktrackPhiNodes.hh"
#include <boost/range/iterator_range_core.hpp>
#include <llvm/IR/Instructions.h>

using namespace boost;
using namespace llvm;


BacktrackPhiNodes::~BacktrackPhiNodes() {
}


void BacktrackPhiNodes::backtrack(const Value &value) {
	if (!alreadySeen.insert(&value).second)
		return;

	if (const Argument * const argument = dyn_cast<Argument>(&value))
		visit(*argument);

	else if (const PHINode * const phi = dyn_cast<PHINode>(&value)) {
		const auto operands = make_iterator_range(phi->op_begin(), phi->op_end());
		for (const Use &operand : operands)
			backtrack(*operand);
	}
}
