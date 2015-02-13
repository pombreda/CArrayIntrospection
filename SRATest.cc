#include <llvm/Pass.h>
#include <llvm/SymbolicRange/SymbolicRangeAnalysis.h>
#include <llvm/SymbolicRange/Expr.h>
#include <llvm/SymbolicRange/Range.h>

using namespace llvm;

namespace llvm {

  class SRATest: public ModulePass {

    public:
      static char ID;

      SRATest(): ModulePass(ID){};

      bool runOnModule(Module &M){
        SymbolicRangeAnalysis& sra = getAnalysis<SymbolicRangeAnalysis>();
        for(auto& F : M){
          for (auto& B : F){
            for (auto& I : B) {
              Range R = sra.getRange(&I);
              errs() << I << "  [" << R.getLower() << ", " << R.getUpper()
                << "]\n";
            }
          }
        }
        return false;
      };

      void getAnalysisUsage(AnalysisUsage &AU) const {
        AU.addRequired<SymbolicRangeAnalysis> ();
        AU.setPreservesAll();
      }
  };
}

char SRATest::ID = 0;
static RegisterPass<SRATest> X("sra-printer",
    "Symbolic Range Analysis Printer");