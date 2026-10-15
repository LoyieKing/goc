//===- goc-color-escape: run the color/escape transform on one IR file ----===//
//
// goc-color-escape IN.ll -o OUT.ll [--default-ptr-color=...]
// The transform lives in lib/Transforms/ColorEscape; the same code is
// available as the "goc-color-escape" pass in libGocPasses.so.
//
//===----------------------------------------------------------------------===//
#include "Goc/ColorEscape.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input ll/bc>"),
                                          cl::Required);
static cl::opt<std::string> OutputFilename("o", cl::desc("Output annotated IR"),
                                          cl::value_desc("filename"),
                                          cl::init("-"));
static cl::opt<bool> FatalErrors("fatal-errors",
                                 cl::desc("Exit non-zero if color/escape errors"),
                                 cl::init(true));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc color/escape analysis\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 2;
  }

  int Errors = goc::runColorEscape(*M);

  std::error_code EC;
  raw_fd_ostream OS(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "unable to open output '" << OutputFilename << "': " << EC.message()
           << "\n";
    return 2;
  }
  M->print(OS, nullptr);

  if (Errors && FatalErrors)
    return 1;
  return 0;
}
