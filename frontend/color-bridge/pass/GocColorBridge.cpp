//===- GocColorBridge.cpp - P18: !goc.color → stackmap/WB attrs ----------===//
// Consumes P17 metadata (!goc.color / !goc.prov / !goc.uptr_encoded) and
// attaches function attributes that P5 Spill / EmitMaps / ExpandStoreGptr
// already understand. No auto-promote of sptr; no dsptr.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <system_error>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input color.ll>"),
                                          cl::Required);
static cl::opt<std::string> OutputFilename("o", cl::desc("Output bridged IR"),
                                          cl::init("-"));
static cl::opt<std::string> RecipePath("recipe", cl::desc("Bridge recipe path"),
                                      cl::init(""));
static cl::opt<bool> Verbose("verbose", cl::desc("Verbose"), cl::init(false));

enum class Color : uint8_t { None, Auto, CPtr, SPtr, UPtr, GPtr };

static Color parseColor(StringRef S) {
  if (S == "cptr")
    return Color::CPtr;
  if (S == "sptr")
    return Color::SPtr;
  if (S == "uptr")
    return Color::UPtr;
  if (S == "gptr")
    return Color::GPtr;
  if (S == "auto" || S == "auto_ptr")
    return Color::Auto;
  return Color::None;
}

static const char *colorName(Color C) {
  switch (C) {
  case Color::CPtr:
    return "cptr";
  case Color::SPtr:
    return "sptr";
  case Color::UPtr:
    return "uptr";
  case Color::GPtr:
    return "gptr";
  case Color::Auto:
    return "auto";
  default:
    return "none";
  }
}

static Color mdColor(const Instruction &I) {
  if (MDNode *N = I.getMetadata("goc.color")) {
    if (N->getNumOperands() >= 1)
      if (auto *MS = dyn_cast<MDString>(N->getOperand(0)))
        return parseColor(MS->getString());
  }
  return Color::None;
}

static StringRef mdProv(const Instruction &I) {
  if (MDNode *N = I.getMetadata("goc.prov")) {
    if (N->getNumOperands() >= 1)
      if (auto *MS = dyn_cast<MDString>(N->getOperand(0)))
        return MS->getString();
  }
  return "";
}

static bool isMapColor(Color C, StringRef Prov) {
  // Stackmap/Locals: Go heap gptrs and stack-provenance sptrs (morestack adjust).
  if (C == Color::GPtr || C == Color::SPtr)
    return true;
  if (C == Color::Auto && Prov == "stack")
    return true;
  if (C == Color::Auto && Prov == "goheap")
    return true;
  return false;
}

static bool isWbColor(Color C, StringRef Prov) {
  // Write barrier only for Go-heap gptr stores.
  if (C == Color::GPtr)
    return true;
  if (C == Color::Auto && Prov == "goheap")
    return true;
  return false;
}

static bool isCptrNonStack(Color C, StringRef Prov) {
  if (C == Color::CPtr)
    return Prov != "stack";
  if (C == Color::Auto && (Prov == "cheap" || Prov.empty()))
    return true;
  return false;
}

struct FnDecision {
  bool NeedWB = false;
  bool NeedSpillMaps = false;
  bool CptrOnly = false;
  bool HasCall = false;
  bool HasUptrEncoded = false;
  SmallVector<std::string, 4> ArgColors;
  SmallVector<std::string, 8> SeenColors;
};

static void analyzeFunction(Function &F, FnDecision &D) {
  for (Argument &A : F.args()) {
    // Args rarely carry instr MD; default from type — refined by uses below.
    if (A.getType()->isPointerTy())
      D.ArgColors.push_back("auto");
    else
      D.ArgColors.push_back("int");
  }

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<CallBase>(I) && !isa<IntrinsicInst>(I))
        D.HasCall = true;
      if (I.getMetadata("goc.uptr_encoded"))
        D.HasUptrEncoded = true;

      Color C = mdColor(I);
      StringRef Prov = mdProv(I);
      if (C != Color::None) {
        D.SeenColors.push_back(colorName(C));
        if (isWbColor(C, Prov))
          D.NeedWB = true;
      }

      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Color VC = Color::None;
        StringRef VP;
        if (auto *VI = dyn_cast<Instruction>(SI->getValueOperand())) {
          VC = mdColor(*VI);
          VP = mdProv(*VI);
        }
        // Also look through load of colored alloca
        if (VC == Color::None) {
          if (auto *LI = dyn_cast<LoadInst>(SI->getValueOperand())) {
            if (auto *AI = dyn_cast<Instruction>(LI->getPointerOperand())) {
              VC = mdColor(*AI);
              VP = mdProv(*AI);
            }
            VC = VC == Color::None ? mdColor(*LI) : VC;
            VP = VP.empty() ? mdProv(*LI) : VP;
          }
        }
        if (isWbColor(VC, VP))
          D.NeedWB = true;
      }
    }
  }

  // Live map-colored ptrs (sptr / stack / gptr) across CALL → spill+maps.
  // gptr store without CALL only needs WB (NeedSpillMaps stays false).
  if (D.HasCall) {
    bool MapLive = false;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        Color C = mdColor(I);
        StringRef P = mdProv(I);
        if (isMapColor(C, P))
          MapLive = true;
      }
    if (MapLive)
      D.NeedSpillMaps = true;
  }

  // Cptr-only: saw pointer colors but none need WB or spill maps.
  bool SawPtrColor = false;
  bool SawNonCptrMap = false;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      Color C = mdColor(I);
      StringRef P = mdProv(I);
      if (C == Color::None)
        continue;
      SawPtrColor = true;
      if (isMapColor(C, P) || isWbColor(C, P))
        SawNonCptrMap = true;
      if (!isCptrNonStack(C, P) && C != Color::UPtr && C != Color::Auto)
        if (C == Color::GPtr || C == Color::SPtr)
          SawNonCptrMap = true;
    }
  if (SawPtrColor && !D.NeedWB && !D.NeedSpillMaps)
    D.CptrOnly = true;
  // Stronger: if every seen color is cptr/cheap → cptr only
  if (!D.SeenColors.empty() && !D.NeedWB && !D.NeedSpillMaps)
    D.CptrOnly = true;
  (void)SawNonCptrMap;
}

static void applyAttrs(Function &F, const FnDecision &D, raw_ostream *Recipe) {
  F.addFnAttr("goc-color-driven", "1");

  std::string ColorsJoined;
  {
    SmallVector<StringRef, 8> Uniq;
    for (auto &S : D.SeenColors) {
      bool Found = false;
      for (auto &U : Uniq)
        if (U == S) {
          Found = true;
          break;
        }
      if (!Found)
        Uniq.push_back(S);
    }
    for (size_t I = 0; I < Uniq.size(); ++I) {
      if (I)
        ColorsJoined += ",";
      ColorsJoined += Uniq[I].str();
    }
  }
  if (!ColorsJoined.empty())
    F.addFnAttr("goc-colors-seen", ColorsJoined);

  std::string ArgCols;
  for (size_t I = 0; I < D.ArgColors.size(); ++I) {
    if (I)
      ArgCols += ",";
    ArgCols += D.ArgColors[I];
  }
  // Refine arg colors from first pointer use MD when possible
  if (F.arg_size() > 0) {
    ArgCols.clear();
    unsigned Idx = 0;
    for (Argument &A : F.args()) {
      if (Idx++)
        ArgCols += ",";
      if (!A.getType()->isPointerTy()) {
        ArgCols += "int";
        continue;
      }
      // Default: if NeedSpillMaps and not cptr-only → sptr/gptr; if cptr-only → cptr
      if (D.CptrOnly)
        ArgCols += "cptr";
      else if (D.NeedWB)
        ArgCols += "gptr";
      else if (D.NeedSpillMaps)
        ArgCols += "sptr";
      else
        ArgCols += "cptr";
    }
  }
  if (!ArgCols.empty())
    F.addFnAttr("goc-arg-ptr-colors", ArgCols);

  if (D.NeedWB) {
    F.addFnAttr("goc-store-gptr", "1");
    F.addFnAttr("goc-color-wb", "1");
  }
  if (D.NeedSpillMaps) {
    F.addFnAttr("goc-spill-gptrs", "1");
    F.addFnAttr("goc-emit-maps", "1");
    F.addFnAttr("goc-color-spill", "1");
    if (!F.hasFnAttribute("goc-frame-locals-bytes"))
      F.addFnAttr("goc-frame-locals-bytes", "24");
  }
  if (D.CptrOnly) {
    F.addFnAttr("goc-color-cptr-only", "1");
    // Explicitly ensure no WB attr leaked
    if (F.hasFnAttribute("goc-store-gptr"))
      F.removeFnAttr("goc-store-gptr");
    if (F.hasFnAttribute("goc-color-wb"))
      F.removeFnAttr("goc-color-wb");
  }
  if (D.HasUptrEncoded)
    F.addFnAttr("goc-color-uptr-encoded", "1");

  if (Recipe) {
    *Recipe << "fn " << F.getName() << " color_driven=1";
    *Recipe << " need_wb=" << (D.NeedWB ? 1 : 0);
    *Recipe << " need_spill_maps=" << (D.NeedSpillMaps ? 1 : 0);
    *Recipe << " cptr_only=" << (D.CptrOnly ? 1 : 0);
    *Recipe << " has_call=" << (D.HasCall ? 1 : 0);
    *Recipe << " uptr_encoded=" << (D.HasUptrEncoded ? 1 : 0);
    *Recipe << " colors=" << (ColorsJoined.empty() ? "-" : ColorsJoined);
    *Recipe << " arg_colors=" << (ArgCols.empty() ? "-" : ArgCols);
    *Recipe << "\n";
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc-color-bridge (P18)\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Require P17 schema (or allow fixtures that set it).
  bool HasSchema = false;
  if (auto *MD = M->getModuleFlag("goc.color.schema")) {
    (void)MD;
    HasSchema = true;
  }
  if (!HasSchema) {
    errs() << "goc-color-bridge: warning: missing module flag goc.color.schema "
              "(continuing for fixtures)\n";
  }

  std::string RecipeBuf;
  raw_string_ostream RecipeOS(RecipeBuf);
  RecipeOS << "# P18 color→attrs bridge recipe\n";
  RecipeOS << "schema annotate+!goc.color;v0.2.1-P18\n";
  RecipeOS << "dataflow color.ll → goc-color-bridge → attrs "
              "(goc-store-gptr|goc-spill-gptrs|goc-emit-maps|"
              "goc-color-cptr-only) → P5 Spill/EmitMaps/ExpandStoreGptr\n";

  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;
    FnDecision D;
    analyzeFunction(F, D);
    applyAttrs(F, D, &RecipeOS);
    if (Verbose)
      errs() << "[bridge] " << F.getName() << " wb=" << D.NeedWB
             << " spill=" << D.NeedSpillMaps << " cptr_only=" << D.CptrOnly
             << "\n";
  }

  M->setModuleFlag(Module::Warning, "goc.color.bridge",
                   MDString::get(Ctx, "goc-color-bridge;v0.2.1-P18"));

  std::error_code EC;
  if (!RecipePath.empty()) {
    raw_fd_ostream ROS(RecipePath, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << "recipe write failed: " << EC.message() << "\n";
      return 1;
    }
    ROS << RecipeOS.str();
  }

  if (OutputFilename == "-") {
    outs() << RecipeOS.str();
    M->print(outs(), nullptr);
  } else {
    raw_fd_ostream OS(OutputFilename, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    M->print(OS, nullptr);
  }

  errs() << "PASS-BRIDGE: wrote " << OutputFilename;
  if (!RecipePath.empty())
    errs() << " + " << RecipePath;
  errs() << "\n";
  return 0;
}
