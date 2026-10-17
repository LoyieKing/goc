//===- libGocPasses.so: goc's IR passes as one new-PM plugin ---------------===//
//
// opt -load-pass-plugin=libGocPasses.so -passes=<name>
//   module:   goc-color-escape, print<goc-color>
//   function: goc-inline-gate, goc-stackmap, goc-reanchor
//
//===----------------------------------------------------------------------===//
#include "Goc/ColorEscape.h"
#include "Goc/StackMap.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

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

// print<goc-color>: the color each source-level pointer resolves to, as the
// transform would decide it. Runs the transform on a clone, so the module is
// unchanged and color errors are listed, not fatal. Build with -g for names
// and lines.
struct GocColorPrinterPass : PassInfoMixin<GocColorPrinterPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    std::unique_ptr<Module> Clone = CloneModule(M);
    goc::runColorEscape(*Clone, &errs());
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
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
                  if (Name == "print<goc-color>") {
                    MPM.addPass(GocColorPrinterPass());
                    return true;
                  }
                  return false;
                });
          }};
}
