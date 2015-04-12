#include <llvm/IR/Module.h>

#include "BacktrackPhiNodes.hh"

////////////////////////////////////////////////////////////////////////
//
//  test whether a specific argument may flow into a specific value
//  across zero or more phi nodes
//

namespace {
	class ValueReachesValue : public BacktrackPhiNodes {
	public:
		ValueReachesValue(const llvm::Value &);
		void visit(const llvm::Value &) final override;
		bool shouldVisit(const llvm::Value &) final override;
	private:
		const llvm::Value &goal;
	};
}


inline ValueReachesValue::ValueReachesValue(const llvm::Value &goal)
	: goal(goal) {
}

void ValueReachesValue::visit(const llvm::Value &reached) {
	if (&reached == &goal)
		throw this;
}

bool ValueReachesValue::shouldVisit(const llvm::Value &) {
	return true;
}

static bool valueReachesValue(const llvm::Value &goal, const llvm::Value &start) {
	ValueReachesValue explorer(goal);
	try {
		explorer.backtrack(start);
	} catch (const ValueReachesValue *) {
		return true;
	}
	return false;
}


////////////////////////////////////////////////////////////////////////