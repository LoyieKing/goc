//===- libGocPasses.so: goc's IR passes as one new-PM plugin ---------------===//
//
// opt -load-pass-plugin=libGocPasses.so -passes=<name>
//   module:   goc-color-escape
//   function: goc-inline-gate, goc-stackmap, goc-reanchor, goc-pin-i64
//
//===----------------------------------------------------------------------===//
#include "Goc/ColorEscape.h"
#include "Goc/StackMap.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
struct GocColorEscapePass : PassInfoMixin<GocColorEscapePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    if (int Errors = goc::runColorEscape(M))
      report_fatal_error("goc-color-escape: " + Twine(Errors) + " color error(s)",
                         /*gen_crash_diag=*/false);
    return PreservedAnalyses::none();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "GocPasses", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(goc::parseStackMapPass);
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "goc-color-escape") {
                    MPM.addPass(GocColorEscapePass());
                    return true;
                  }
                  return false;
                });
          }};
}
