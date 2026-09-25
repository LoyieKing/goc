//===- GocColorEscape.cpp - P17 pointer color + escape analysis -----------===//
// Frontend vertical slice: refine auto_ptr, encode stack pointers stored in
// cptr T* slots as uptr, and annotate IR for later backend hooks.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdlib>
#include <string>
#include <system_error>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input ll/bc>"),
                                          cl::Required);
static cl::opt<std::string> OutputFilename("o", cl::desc("Output annotated IR"),
                                          cl::value_desc("filename"),
                                          cl::init("-"));
static cl::opt<bool> FatalErrors("fatal-errors",
                                 cl::desc("Exit non-zero if color/escape errors"),
                                 cl::init(true));
static cl::opt<bool> Verbose("verbose", cl::desc("Verbose diagnostics"),
                             cl::init(false));
static cl::opt<std::string> DefaultPtrColor(
    "default-ptr-color",
    cl::desc("Color for unannotated pointer slots/formals: cptr|sptr|uptr|gptr "
             "(default: auto = per-value inference)"),
    cl::init("auto"));

enum class Color : uint8_t {
  None = 0,
  Auto,
  CPtr,
  SPtr,
  UPtr,
  GPtr,
};

static const char *colorName(Color C) {
  switch (C) {
  case Color::None:
    return "none";
  case Color::Auto:
    return "auto";
  case Color::CPtr:
    return "cptr";
  case Color::SPtr:
    return "sptr";
  case Color::UPtr:
    return "uptr";
  case Color::GPtr:
    return "gptr";
  }
  return "?";
}

static Color parseColorToken(StringRef S) {
  if (S == "goc.color.cptr" || S == "cptr")
    return Color::CPtr;
  if (S == "goc.color.sptr" || S == "sptr")
    return Color::SPtr;
  if (S == "goc.color.uptr" || S == "uptr")
    return Color::UPtr;
  if (S == "goc.color.auto" || S == "auto" || S == "auto_ptr")
    return Color::Auto;
  if (S == "goc.color.gptr" || S == "gptr")
    return Color::GPtr;
  return Color::None;
}

static StringRef annotationString(Value *V) {
  if (auto *C = dyn_cast<Constant>(V)) {
    if (auto *G = dyn_cast<GlobalVariable>(C->stripPointerCasts())) {
      if (G->hasInitializer())
        if (auto *CA = dyn_cast<ConstantDataArray>(G->getInitializer()))
          if (CA->isString())
            return CA->getAsCString();
    }
    // strip to GV through ConstantExpr bitcast
    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
      if (CE->isCast())
        return annotationString(CE->getOperand(0));
    }
  }
  if (auto *G = dyn_cast<GlobalVariable>(V)) {
    if (G->hasInitializer())
      if (auto *CA = dyn_cast<ConstantDataArray>(G->getInitializer()))
        if (CA->isString())
          return CA->getAsCString();
  }
  return StringRef();
}

struct PtrInfo {
  Color Declared = Color::None; // from annotate, if any
  Color Refined = Color::Auto;  // after analysis
  bool StackProv = false;       // derived from alloca / stack
  bool HeapProv = false;        // malloc / global / goc_alloc
  bool GoHeapProv = false;      // gptr track
  bool EncodedUPtr = false;     // through goc_uptr_from_*
  bool DynamicUPtr = false;     // decoded cptr|sptr value; may be stack-backed
  bool Escapes = false;         // stored to non-stack or returned
};

struct Diagnostic {
  std::string Msg;
  Instruction *At = nullptr;
};

struct ColorEscapeState {
  DenseMap<Value *, PtrInfo> Info;
  DenseMap<Value *, Color> SlotColor; // alloca/global/ptr.annotation slot → color
  DenseSet<Value *> DefaultColoredSlots;
  DenseSet<const Argument *> StackOnlyFormals;
  DenseSet<Value *> AutoUptrSlots;
  DenseMap<Value *, SmallVector<std::string, 2>> AutoUptrRootFields;
  SmallVector<std::string, 8> AutoUptrFields;
  SmallVector<Diagnostic, 8> Diags;
  unsigned ErrorCount = 0;

  void error(Instruction *I, const Twine &T) {
    Diags.push_back({T.str(), I});
    ++ErrorCount;
    errs() << "goc-color-escape: error: " << T << "\n";
    if (I) {
      errs() << "  at: ";
      I->print(errs());
      errs() << "\n";
      if (I->getFunction())
        errs() << "  in function '" << I->getFunction()->getName() << "'\n";
    }
  }

  void note(const Twine &T) {
    if (Verbose)
      errs() << "goc-color-escape: note: " << T << "\n";
  }

  PtrInfo &info(Value *V) { return Info[V]; }

  Color effective(Value *V) {
    auto It = Info.find(V);
    if (It == Info.end())
      return Color::Auto;
    if (It->second.Declared != Color::None && It->second.Declared != Color::Auto)
      return It->second.Declared;
    return It->second.Refined;
  }
};

static bool isAnnotationIntrinsic(Function *F) {
  if (!F)
    return false;
  auto ID = F->getIntrinsicID();
  return ID == Intrinsic::var_annotation || ID == Intrinsic::ptr_annotation ||
         ID == Intrinsic::annotation;
}

static bool isUptrFromSptr(StringRef N) {
  return N == "goc_uptr_from_sptr" || N == "goc_uptr_from_sptr_hi";
}
static bool isUptrFromPtr(StringRef N) { return N == "goc_uptr_from_ptr"; }
static bool isUptrDecode(StringRef N) { return N == "goc_uptr_decode"; }
static bool isUptrFromCptr(StringRef N) {
  return N == "goc_uptr_from_cptr" || N == "goc_uptr_from_cptr_hi";
}
static bool isUptrAsSptr(StringRef N) {
  return N == "goc_uptr_as_sptr" || N == "goc_uptr_as_sptr_hi";
}
static bool isUptrAsCptr(StringRef N) {
  return N == "goc_uptr_as_cptr" || N == "goc_uptr_as_cptr_hi";
}
static bool isGocAlloc(StringRef N) {
  return N == "goc_alloc" || N == "goc_dynalloc" || N == "malloc" || N == "calloc" ||
         N == "realloc" || N == "goc_malloc" || N == "goc_calloc" ||
         N == "goc_realloc";
}
static bool isGptrFrom(StringRef N) { return N == "goc_gptr_from_handle"; }

// These are the implementations of the compiler's own uptr primitives. They
// return a checked/decode result by contract; ordinary user functions must
// still reject raw sptr returns.
static bool isUptrRuntimeReturn(StringRef N) {
  return isUptrAsSptr(N) || isUptrDecode(N) ||
         N == "goc_uptr_require_cptr";
}

static Value *stripAC(Value *V) {
  return V->stripPointerCasts();
}

/// True only if every possible Dest address is based on a local alloca.
static bool isStackLocation(Value *Dest) {
  SmallVector<Value *, 8> Work;
  DenseSet<Value *> Seen;
  Work.push_back(Dest);
  while (!Work.empty()) {
    Value *V = stripAC(Work.pop_back_val());
    if (!Seen.insert(V).second)
      continue;
    if (isa<AllocaInst>(V))
      continue;
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      Work.push_back(GEP->getPointerOperand());
      continue;
    }
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      Work.push_back(BC->getOperand(0));
      continue;
    }
    if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
      Work.push_back(ASC->getOperand(0));
      continue;
    }
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
        Work.push_back(II->getArgOperand(0));
        continue;
      }
    }
    if (auto *SI = dyn_cast<Instruction>(V)) {
      // select/phi of locations — require all stack
      if (auto *Sel = dyn_cast<SelectInst>(SI)) {
        Work.push_back(Sel->getTrueValue());
        Work.push_back(Sel->getFalseValue());
        continue;
      }
      if (auto *Phi = dyn_cast<PHINode>(SI)) {
        for (Value *In : Phi->incoming_values())
          Work.push_back(In);
        continue;
      }
    }
    return false;
  }
  return true;
}

static bool isGlobalLocation(Value *Dest) {
  Value *V = stripAC(Dest);
  if (isa<GlobalVariable>(V))
    return true;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return isGlobalLocation(GEP->getPointerOperand());
  if (auto *II = dyn_cast<IntrinsicInst>(Dest))
    if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
      return isGlobalLocation(II->getArgOperand(0));
  return false;
}

static void seedAnnotations(Module &M, ColorEscapeState &S) {
  // Globals via llvm.global.annotations
  if (GlobalVariable *GA = M.getGlobalVariable("llvm.global.annotations")) {
    if (auto *Init = dyn_cast<ConstantArray>(GA->getInitializer())) {
      for (unsigned i = 0, e = Init->getNumOperands(); i != e; ++i) {
        auto *Sstruct = dyn_cast<ConstantStruct>(Init->getOperand(i));
        if (!Sstruct || Sstruct->getNumOperands() < 2)
          continue;
        Value *Annotated =
            Sstruct->getOperand(0)->stripPointerCasts();
        Color C = parseColorToken(annotationString(Sstruct->getOperand(1)));
        if (C != Color::None) {
          S.SlotColor[Annotated] = C;
          auto &PI = S.info(Annotated);
          PI.Declared = C;
          PI.Refined = C;
          if (C == Color::CPtr)
            PI.HeapProv = true;
          if (C == Color::GPtr)
            PI.GoHeapProv = true;
          if (C == Color::UPtr)
            PI.EncodedUPtr = true;
        }
      }
    }
  }

  for (Function &F : M) {
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *Callee = CB->getCalledFunction();
      if (!isAnnotationIntrinsic(Callee))
        continue;
      if (CB->arg_size() < 2)
        continue;
      Value *Target = CB->getArgOperand(0)->stripPointerCasts();
      Color C = parseColorToken(annotationString(CB->getArgOperand(1)));
      if (C == Color::None)
        continue;
      S.SlotColor[Target] = C;
      // For var.annotation, Target is the alloca of the pointer variable.
      auto &PI = S.info(Target);
      PI.Declared = C;
      if (C != Color::Auto)
        PI.Refined = C;
      if (C == Color::SPtr)
        PI.StackProv = true;
      if (C == Color::CPtr)
        PI.HeapProv = true;
      if (C == Color::GPtr)
        PI.GoHeapProv = true;
      if (C == Color::UPtr)
        PI.EncodedUPtr = true;
      S.note(Twine("annotation ") + colorName(C) + " on " + Target->getName());
    }
  }
}

static void seedProducers(Module &M, ColorEscapeState &S) {
  for (Function &F : M) {
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        // Address of alloca = stack object pointer (auto→sptr candidate).
        auto &PI = S.info(AI);
        PI.StackProv = true;
        if (PI.Declared == Color::None)
          PI.Refined = Color::Auto;
        continue;
      }
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        continue;
      StringRef N = Callee->getName();
      auto &PI = S.info(CB);
      if (isUptrFromPtr(N)) {
        PI.Refined = Color::UPtr;
        PI.EncodedUPtr = true;
        continue;
      }
      if (isUptrDecode(N)) {
        PI.Refined = Color::Auto;
        PI.StackProv = true;
        PI.DynamicUPtr = true;
        continue;
      }
      if (isUptrFromSptr(N)) {
        PI.Refined = Color::UPtr;
        PI.EncodedUPtr = true;
        PI.StackProv = true; // logically stack, but encoded
        continue;
      }
      if (isUptrFromCptr(N)) {
        PI.Refined = Color::UPtr;
        PI.EncodedUPtr = true;
        PI.HeapProv = true;
        continue;
      }
      if (isUptrAsSptr(N)) {
        PI.Refined = Color::SPtr;
        PI.StackProv = true;
        continue;
      }
      if (isUptrAsCptr(N)) {
        PI.Refined = Color::CPtr;
        PI.HeapProv = true;
        continue;
      }
      if (isGocAlloc(N)) {
        PI.Refined = Color::CPtr;
        PI.HeapProv = true;
        continue;
      }
      if (isGptrFrom(N)) {
        PI.Refined = Color::GPtr;
        PI.GoHeapProv = true;
        continue;
      }
    }
    // Formals: default auto unless annotated via slot (rare for formals).
    for (Argument &A : F.args()) {
      if (!A.getType()->isPointerTy())
        continue;
      auto &PI = S.info(&A);
      if (PI.Declared == Color::None && PI.Refined == Color::None)
        PI.Refined = Color::Auto;
    }
  }
}

static Color joinColor(Color A, Color B) {
  if (A == Color::None)
    return B;
  if (B == Color::None)
    return A;
  if (A == B)
    return A;
  if (A == Color::Auto)
    return B;
  if (B == Color::Auto)
    return A;
  // Distinct concrete colors — keep first; callers check mixes.
  return A;
}

// A declared color (annotation, or the TU-level default color) is
// authoritative for the values loaded from that slot: align provenance flags
// with it so stale stack-provenance from earlier stores cannot fake an sptr
// (and an explicit cptr/uptr/gptr promise is honored).
static void applyColorProv(PtrInfo &PI, Color C) {
  switch (C) {
  case Color::CPtr:
    PI.StackProv = false;
    PI.HeapProv = true;
    PI.GoHeapProv = false;
    PI.EncodedUPtr = false;
    break;
  case Color::SPtr:
    PI.StackProv = true;
    PI.HeapProv = false;
    PI.GoHeapProv = false;
    PI.EncodedUPtr = false;
    break;
  case Color::GPtr:
    PI.GoHeapProv = true;
    PI.StackProv = false;
    PI.HeapProv = false;
    PI.EncodedUPtr = false;
    break;
  case Color::UPtr:
    PI.EncodedUPtr = true;
    PI.StackProv = false;
    break;
  default:
    break;
  }
}

static void propagate(Function &F, ColorEscapeState &S) {
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 64) {
    Changed = false;
    for (Instruction &I : instructions(F)) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
        auto &Dst = S.info(LI);
        PtrInfo Old = Dst;
        if (LI->getMetadata("goc.uptr_encoded")) {
          Dst.Declared = Color::UPtr;
          Dst.Refined = Color::UPtr;
          Dst.StackProv = false;
          Dst.HeapProv = false;
          Dst.GoHeapProv = false;
          Dst.EncodedUPtr = true;
          Dst.DynamicUPtr = false;
          if (Dst.Refined != Old.Refined || Dst.EncodedUPtr != Old.EncodedUPtr)
            Changed = true;
          continue;
        }
        // If loading from an annotated pointer slot alloca, value gets that color.
        Color SlotC = Color::None;
        auto It = S.SlotColor.find(Ptr);
        if (It != S.SlotColor.end())
          SlotC = It->second;
        auto &SrcSlot = S.info(Ptr);
        if (SlotC != Color::None && SlotC != Color::Auto) {
          Dst.Declared = SlotC;
          Dst.Refined = SlotC;
          applyColorProv(Dst, SlotC);
        } else if (SrcSlot.Declared != Color::None &&
                   SrcSlot.Declared != Color::Auto) {
          Dst.Refined = SrcSlot.Declared;
          applyColorProv(Dst, SrcSlot.Declared);
        } else {
          // Provenance: if slot holds stack addr, load is stack.
          Dst.StackProv |= SrcSlot.StackProv;
          Dst.HeapProv |= SrcSlot.HeapProv;
          Dst.GoHeapProv |= SrcSlot.GoHeapProv;
          Dst.EncodedUPtr |= SrcSlot.EncodedUPtr;
        }
        Dst.DynamicUPtr |= SrcSlot.DynamicUPtr;
        if (Dst.DynamicUPtr) {
          Dst.StackProv = true;
          Dst.Refined = Color::Auto;
        }
        if (Dst.Refined == Color::Auto && Dst.StackProv && !Dst.HeapProv &&
            !Dst.EncodedUPtr)
          Dst.Refined = Color::SPtr;
        if (Dst.Refined == Color::Auto && Dst.HeapProv && !Dst.StackProv)
          Dst.Refined = Color::CPtr;
        if (Dst.Refined == Color::Auto && Dst.GoHeapProv)
          Dst.Refined = Color::GPtr;
        if (Dst.Refined != Old.Refined || Dst.StackProv != Old.StackProv ||
            Dst.DynamicUPtr != Old.DynamicUPtr)
          Changed = true;
        continue;
      }

      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *Val = SI->getValueOperand();
        Value *Dest = SI->getPointerOperand();
        if (!Val->getType()->isPointerTy())
          continue;
        auto &VI = S.info(Val);
        Value *DestBase = Dest->stripPointerCasts();
        // If dest is ptr.annotation, use annotated field color.
        Color DestField = Color::None;
        if (auto *II = dyn_cast<IntrinsicInst>(Dest)) {
          if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
            DestField =
                parseColorToken(annotationString(II->getArgOperand(1)));
            DestBase = II->getArgOperand(0)->stripPointerCasts();
          }
        }
        auto SlotIt = S.SlotColor.find(DestBase);
        if (SlotIt != S.SlotColor.end())
          DestField = SlotIt->second;

        // Propagate into slot alloca info (for subsequent loads).
        if (auto *AI = dyn_cast<AllocaInst>(DestBase)) {
          auto &Slot = S.info(AI);
          PtrInfo Old = Slot;
          Slot.StackProv |= VI.StackProv;
          Slot.HeapProv |= VI.HeapProv;
          Slot.GoHeapProv |= VI.GoHeapProv;
          Slot.EncodedUPtr |= VI.EncodedUPtr;
          Slot.DynamicUPtr |= VI.DynamicUPtr;
          if (VI.Declared != Color::None)
            Slot.Declared = joinColor(Slot.Declared, VI.Declared);
          Color Eff = S.effective(Val);
          if (Eff != Color::Auto && Eff != Color::None)
            Slot.Refined = Eff;
          else if (VI.StackProv && !VI.EncodedUPtr)
            Slot.Refined = Color::SPtr;
          if (Slot.Refined != Old.Refined || Slot.StackProv != Old.StackProv ||
              Slot.DynamicUPtr != Old.DynamicUPtr)
            Changed = true;
        }
        (void)DestField;
        continue;
      }

      if (auto *BC = dyn_cast<BitCastInst>(&I)) {
        if (!BC->getType()->isPointerTy())
          continue;
        auto &D = S.info(BC);
        auto &O = S.info(BC->getOperand(0));
        PtrInfo Old = D;
        D.StackProv |= O.StackProv;
        D.HeapProv |= O.HeapProv;
        D.GoHeapProv |= O.GoHeapProv;
        D.EncodedUPtr |= O.EncodedUPtr;
        D.DynamicUPtr |= O.DynamicUPtr;
        D.Refined = joinColor(D.Refined, O.Refined);
        if (O.Declared != Color::None)
          D.Declared = O.Declared;
        if (D.Refined != Old.Refined || D.DynamicUPtr != Old.DynamicUPtr)
          Changed = true;
        continue;
      }

      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        auto &D = S.info(GEP);
        auto &O = S.info(GEP->getPointerOperand());
        PtrInfo Old = D;
        D.StackProv |= O.StackProv;
        D.HeapProv |= O.HeapProv;
        D.GoHeapProv |= O.GoHeapProv;
        D.EncodedUPtr |= O.EncodedUPtr;
        D.DynamicUPtr |= O.DynamicUPtr;
        // GEP of stack object still stack pointer (same color family).
        if (O.Refined == Color::SPtr || O.StackProv)
          D.Refined = (O.EncodedUPtr ? Color::UPtr : Color::SPtr);
        else
          D.Refined = joinColor(D.Refined, O.Refined);
        if (D.Refined != Old.Refined || D.DynamicUPtr != Old.DynamicUPtr)
          Changed = true;
        continue;
      }

      if (auto *Phi = dyn_cast<PHINode>(&I)) {
        if (!Phi->getType()->isPointerTy())
          continue;
        auto &D = S.info(Phi);
        PtrInfo Old = D;
        for (Value *In : Phi->incoming_values()) {
          auto &O = S.info(In);
          D.StackProv |= O.StackProv;
          D.HeapProv |= O.HeapProv;
          D.GoHeapProv |= O.GoHeapProv;
          D.EncodedUPtr |= O.EncodedUPtr;
          D.DynamicUPtr |= O.DynamicUPtr;
          D.Refined = joinColor(D.Refined, S.effective(In));
        }
        if (D.Refined != Old.Refined || D.DynamicUPtr != Old.DynamicUPtr)
          Changed = true;
        continue;
      }

      if (auto *Sel = dyn_cast<SelectInst>(&I)) {
        if (!Sel->getType()->isPointerTy())
          continue;
        auto &D = S.info(Sel);
        PtrInfo Old = D;
        for (Value *In : {Sel->getTrueValue(), Sel->getFalseValue()}) {
          auto &O = S.info(In);
          D.StackProv |= O.StackProv;
          D.HeapProv |= O.HeapProv;
          D.GoHeapProv |= O.GoHeapProv;
          D.EncodedUPtr |= O.EncodedUPtr;
          D.DynamicUPtr |= O.DynamicUPtr;
          D.Refined = joinColor(D.Refined, S.effective(In));
        }
        if (D.Refined != Old.Refined || D.DynamicUPtr != Old.DynamicUPtr)
          Changed = true;
        continue;
      }
    }
  }
}

static bool provenStackActual(Value *Actual,
                              const DenseSet<const Argument *> &Candidates);

static bool isStackDest(Value *Dest, ColorEscapeState &S) {
  if (isStackLocation(Dest))
    return true;
  return provenStackActual(Dest, S.StackOnlyFormals);
}

static bool isEscapingDest(Value *Dest, ColorEscapeState &S) {
  if (isGlobalLocation(Dest))
    return true;
  if (isStackDest(Dest, S))
    return false;
  // Non-stack, non-global: treat as heap-like (malloc object, param memory).
  Value *Base = Dest->stripPointerCasts();
  if (auto *II = dyn_cast<IntrinsicInst>(Dest))
    if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
      Base = II->getArgOperand(0)->stripPointerCasts();
  if (auto *GEP = dyn_cast<GEPOperator>(Base))
    Base = GEP->getPointerOperand()->stripPointerCasts();
  // Pointer loaded from somewhere / argument → assume may be heap.
  if (isa<Argument>(Base) || isa<LoadInst>(Base) || isa<CallBase>(Base))
    return true;
  return true; // conservative
}

static Color pointerSlotColor(Value *Address, ColorEscapeState &S) {
  if (!Address)
    return Color::None;
  if (auto *II = dyn_cast<IntrinsicInst>(Address)) {
    if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
      Color C = parseColorToken(annotationString(II->getArgOperand(1)));
      if (C != Color::None)
        return C;
      Address = II->getArgOperand(0);
    }
  }
  Address = Address->stripPointerCasts();
  auto It = S.SlotColor.find(Address);
  if (It != S.SlotColor.end())
    return It->second;
  auto PI = S.Info.find(Address);
  return PI == S.Info.end() ? Color::None : PI->second.Declared;
}

static SmallVector<Value *, 4> pointerStorageRoots(Value *Address) {
  SmallVector<Value *, 4> Roots, Work{Address};
  DenseSet<Value *> Seen;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val()->stripPointerCasts();
    if (!Seen.insert(V).second)
      continue;
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
        Work.push_back(II->getArgOperand(0));
        continue;
      }
    }
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      Work.push_back(GEP->getPointerOperand());
      continue;
    }
    if (auto *Sel = dyn_cast<SelectInst>(V)) {
      Work.push_back(Sel->getTrueValue());
      Work.push_back(Sel->getFalseValue());
      continue;
    }
    if (auto *Phi = dyn_cast<PHINode>(V)) {
      for (Value *In : Phi->incoming_values())
        Work.push_back(In);
      continue;
    }
    Roots.push_back(V);
  }
  return Roots;
}

static const GEPOperator *pointerFieldGEP(Value *Address) {
  if (auto *II = dyn_cast<IntrinsicInst>(Address))
    if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
      Address = II->getArgOperand(0);
  // stripPointerCasts also removes zero-offset GEPs. Field zero is still a
  // distinct storage field (JSStackFrame.prev_frame, JSValueLink.next): losing
  // it made an encoded write look like a raw pointer load on the next read.
  if (auto *GEP = dyn_cast<GEPOperator>(Address))
    return GEP;
  Address = Address->stripPointerCasts();
  return dyn_cast<GEPOperator>(Address);
}

static std::string pointerFieldKey(Value *Address) {
  const auto *GEP = pointerFieldGEP(Address);
  if (!GEP || !GEP->getSourceElementType()->isStructTy())
    return {};

  std::string Key;
  raw_string_ostream OS(Key);
  GEP->getSourceElementType()->print(OS);
  OS << ':';
  for (unsigned I = 1; I < GEP->getNumOperands(); ++I) {
    auto *CI = dyn_cast<ConstantInt>(GEP->getOperand(I));
    if (CI)
      OS << CI->getSExtValue();
    else
      OS << '*';
    OS << '.';
  }
  OS.flush();
  return Key;
}

static void markAutoUptrSlot(Value *Address, ColorEscapeState &S) {
  // A conditional lvalue is a phi/select of *addresses*. Keep each possible
  // field's identity: a write to Holder.pointer must not mark Holder.unrelated
  // as encoded, while the local branch of a global/local alias must decode.
  SmallVector<Value *, 8> Work{Address};
  DenseSet<Value *> Seen;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (auto *II = dyn_cast<IntrinsicInst>(V))
      if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
        Work.push_back(II->getArgOperand(0));
        continue;
      }
    if (auto *Sel = dyn_cast<SelectInst>(V)) {
      Work.push_back(Sel->getTrueValue());
      Work.push_back(Sel->getFalseValue());
      continue;
    }
    if (auto *Phi = dyn_cast<PHINode>(V)) {
      for (Value *In : Phi->incoming_values())
        Work.push_back(In);
      continue;
    }
    std::string Key = pointerFieldKey(V);
    if (Key.empty()) {
      for (Value *Root : pointerStorageRoots(V))
        S.AutoUptrSlots.insert(Root);
      continue;
    }
    if (std::find(S.AutoUptrFields.begin(), S.AutoUptrFields.end(), Key) ==
        S.AutoUptrFields.end())
      S.AutoUptrFields.push_back(Key);
    for (Value *Root : pointerStorageRoots(V)) {
      auto &Fields = S.AutoUptrRootFields[Root];
      if (std::find(Fields.begin(), Fields.end(), Key) == Fields.end())
        Fields.push_back(Key);
    }
  }
}

static bool addressMayEscape(Value *Address);

static bool isAutoUptrSlot(Value *Address, ColorEscapeState &S) {
  std::string Key = pointerFieldKey(Address);
  auto Roots = pointerStorageRoots(Address);
  if (!Key.empty()) {
    for (Value *Root : Roots) {
      auto It = S.AutoUptrRootFields.find(Root);
      if (It != S.AutoUptrRootFields.end())
        for (const std::string &Field : It->second)
          if (Field == Key)
            return true;
    }
    // A promoted field type may also occur in an independent stack-only
    // instance. Its raw sptr slot has never been encoded; keep that instance
    // in the stack-map track instead of relabeling its loads as uptr.
    if (isStackDest(Address, S)) {
      bool Escapes = false;
      for (Value *Root : Roots)
        Escapes |= addressMayEscape(Root);
      if (!Escapes)
        return false;
    }
    for (const std::string &Existing : S.AutoUptrFields)
      if (Existing == Key)
        return true;
    return false;
  }
  for (Value *Root : Roots)
    if (S.AutoUptrSlots.count(Root))
      return true;
  return false;
}

static bool addressMayEscape(Value *Address) {
  SmallVector<Value *, 8> Work{Address};
  DenseSet<Value *> Seen;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (User *U : V->users()) {
      if (auto *CB = dyn_cast<CallBase>(U)) {
        Function *Callee = CB->getCalledFunction();
        if (isAnnotationIntrinsic(Callee)) {
          if (CB->getType()->isPointerTy())
            Work.push_back(CB);
          continue;
        }
        if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
          auto ID = II->getIntrinsicID();
          if (ID == Intrinsic::lifetime_start || ID == Intrinsic::lifetime_end ||
              ID == Intrinsic::dbg_value || ID == Intrinsic::dbg_declare)
            continue;
        }
        return true;
      }
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() == V)
          return true;
        continue;
      }
      if (isa<LoadInst>(U))
        continue;
      if (isa<ReturnInst>(U))
        return true;
      if (isa<GEPOperator>(U) || isa<BitCastOperator>(U) ||
          isa<AddrSpaceCastOperator>(U) || isa<PHINode>(U) ||
          isa<SelectInst>(U)) {
        Work.push_back(cast<Value>(U));
        continue;
      }
      // Unknown uses are treated as escapes; decoding raw cptr words is a no-op.
      return true;
    }
  }
  return false;
}

static FunctionCallee getUnaryPtrHelper(Module &M, StringRef Name) {
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *FT = FunctionType::get(PtrTy, {PtrTy}, false);
  return M.getOrInsertFunction(Name, FT);
}

static bool borrowedFromStackFormal(Value *V, ColorEscapeState &S,
                                    DenseSet<Value *> &Seen);

static bool mayDirectlyNameStack(Value *V, DenseSet<Value *> &Seen) {
  V = V->stripPointerCasts();
  if (isa<AllocaInst>(V))
    return true;
  if (!Seen.insert(V).second)
    return false;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return mayDirectlyNameStack(GEP->getPointerOperand(), Seen);
  if (auto *Sel = dyn_cast<SelectInst>(V))
    return mayDirectlyNameStack(Sel->getTrueValue(), Seen) ||
           mayDirectlyNameStack(Sel->getFalseValue(), Seen);
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    for (Value *In : Phi->incoming_values())
      if (mayDirectlyNameStack(In, Seen))
        return true;
  }
  return false; // do not mistake a heap pointer loaded from a local for &local
}

static unsigned encodeEscapingSptrStores(Module &M, ColorEscapeState &S) {
  SmallVector<StoreInst *, 64> Stores;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F))
      if (auto *SI = dyn_cast<StoreInst>(&I))
        Stores.push_back(SI);
  }

  FunctionCallee Encode = getUnaryPtrHelper(M, "goc_uptr_from_ptr");
  unsigned N = 0;
  for (StoreInst *SI : Stores) {
    Value *Val = SI->getValueOperand();
    Value *Dest = SI->getPointerOperand();
    if (!Val->getType()->isPointerTy())
      continue;

    Color DestColor = pointerSlotColor(Dest, S);
    bool StackDest = isStackDest(Dest, S);
    PtrInfo &VI = S.info(Val);
    Color Eff = S.effective(Val);
    bool MayBeStack = Eff == Color::SPtr || VI.Declared == Color::SPtr ||
                      (VI.StackProv && !VI.EncodedUPtr && !VI.GoHeapProv &&
                       Eff != Color::GPtr && Eff != Color::UPtr) ||
                      VI.DynamicUPtr;
    if (!MayBeStack || VI.EncodedUPtr || VI.GoHeapProv ||
        Eff == Color::GPtr || Eff == Color::UPtr)
      continue;

    DenseSet<Value *> BorrowedSeen;
    DenseSet<Value *> DirectSeen;
    bool ProvenStackValue = mayDirectlyNameStack(Val, DirectSeen) ||
                            borrowedFromStackFormal(Val, S, BorrowedSeen);
    if (StackDest && DestColor == Color::CPtr && ProvenStackValue) {
      // TU-level defaults describe the common case, not a promise that a
      // local pointer variable never holds a stack address. Retain a raw
      // sptr in this *stack* slot so the frame map can adjust it; only
      // non-stack cptr storage needs uptr encoding.
      if (auto *Slot = dyn_cast<AllocaInst>(Dest->stripPointerCasts()))
        if (S.DefaultColoredSlots.count(Slot)) {
          S.SlotColor.erase(Slot);
          auto &SI = S.info(Slot);
          SI.Declared = Color::Auto;
          SI.Refined = Color::SPtr;
          SI.StackProv = true;
          SI.HeapProv = VI.HeapProv;
          for (User *U : Slot->users())
            if (auto *LI = dyn_cast<LoadInst>(U)) {
              auto &L = S.info(LI);
              L.Declared = Color::Auto;
              L.Refined = Color::SPtr;
              L.StackProv = true;
              L.HeapProv = VI.HeapProv;
              L.EncodedUPtr = false;
              L.DynamicUPtr = false;
            }
        }
    }
    bool AutoStorage = isAutoUptrSlot(Dest, S) ||
                       (!StackDest &&
                        (DestColor == Color::CPtr ||
                         ((DestColor == Color::None || DestColor == Color::Auto) &&
                          isEscapingDest(Dest, S))));
    if (!AutoStorage || DestColor == Color::SPtr || DestColor == Color::UPtr ||
        DestColor == Color::GPtr)
      continue;

    IRBuilder<> B(SI);
    auto *Encoded = B.CreateCall(Encode, {Val}, "goc.uptr.store");
    PtrInfo &EI = S.info(Encoded);
    EI.Declared = Color::UPtr;
    EI.Refined = Color::UPtr;
    EI.EncodedUPtr = true;
    EI.StackProv = false;
    EI.HeapProv = false;
    EI.GoHeapProv = false;
    EI.DynamicUPtr = false;
    SI->setOperand(0, Encoded);
    markAutoUptrSlot(Dest, S);
    ++N;
  }
  return N;
}

static unsigned decodeAutoUptrLoads(Module &M, ColorEscapeState &S) {
  SmallVector<LoadInst *, 128> Loads;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F))
      if (auto *LI = dyn_cast<LoadInst>(&I))
        if (LI->getType()->isPointerTy())
          Loads.push_back(LI);
  }

  FunctionCallee Decode = getUnaryPtrHelper(M, "goc_uptr_decode");
  FunctionCallee AsCptr = getUnaryPtrHelper(M, "goc_uptr_as_cptr");
  unsigned N = 0;
  LLVMContext &Ctx = M.getContext();
  MDNode *EncodedMD = MDNode::get(Ctx, {MDString::get(Ctx, "1")});

  for (LoadInst *LI : Loads) {
    Value *Address = LI->getPointerOperand();
    if (!isAutoUptrSlot(Address, S))
      continue;
    Color SlotColor = pointerSlotColor(Address, S);
    if (SlotColor == Color::UPtr || SlotColor == Color::GPtr ||
        SlotColor == Color::SPtr || LI->getMetadata("goc.uptr_encoded"))
      continue;

    LI->setMetadata("goc.uptr_encoded", EncodedMD);
    PtrInfo &Raw = S.info(LI);
    Raw.Declared = Color::UPtr;
    Raw.Refined = Color::UPtr;
    Raw.StackProv = false;
    Raw.HeapProv = false;
    Raw.GoHeapProv = false;
    Raw.EncodedUPtr = true;
    Raw.DynamicUPtr = false;

    IRBuilder<> B(LI->getNextNode());
    auto *Decoded = B.CreateCall(Decode, {LI}, "goc.uptr.decode");
    PtrInfo &DI = S.info(Decoded);
    DI.Declared = Color::Auto;
    DI.Refined = Color::Auto;
    DI.StackProv = true;
    DI.HeapProv = false;
    DI.GoHeapProv = false;
    DI.EncodedUPtr = false;
    DI.DynamicUPtr = true;

    SmallVector<User *, 8> Uses;
    for (User *U : LI->users())
      if (U != Decoded)
        Uses.push_back(U);
    for (User *U : Uses) {
      if (auto *RI = dyn_cast<ReturnInst>(U)) {
        if (RI->getReturnValue() == LI) {
          IRBuilder<> RB(RI);
          auto *Checked = RB.CreateCall(AsCptr, {LI}, "goc.uptr.return.cptr");
          PtrInfo &CI = S.info(Checked);
          CI.Declared = Color::CPtr;
          CI.Refined = Color::CPtr;
          CI.HeapProv = true;
          RI->setOperand(0, Checked);
          continue;
        }
      }
      U->replaceUsesOfWith(LI, Decoded);
    }
    ++N;
  }
  return N;
}

static unsigned guardDynamicUptrReturns(Module &M, ColorEscapeState &S) {
  FunctionCallee RequireCptr = getUnaryPtrHelper(M, "goc_uptr_require_cptr");
  unsigned N = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    SmallVector<ReturnInst *, 8> Returns;
    for (Instruction &I : instructions(F))
      if (auto *RI = dyn_cast<ReturnInst>(&I))
        Returns.push_back(RI);
    for (ReturnInst *RI : Returns) {
      Value *V = RI->getReturnValue();
      if (!V || !V->getType()->isPointerTy())
        continue;
      auto It = S.Info.find(V);
      if (It == S.Info.end() || !It->second.DynamicUPtr)
        continue;
      IRBuilder<> B(RI);
      auto *Checked = B.CreateCall(RequireCptr, {V}, "goc.uptr.return.check");
      PtrInfo &CI = S.info(Checked);
      CI.Declared = Color::CPtr;
      CI.Refined = Color::CPtr;
      CI.HeapProv = true;
      RI->setOperand(0, Checked);
      ++N;
    }
  }
  return N;
}

// A static helper may return a pointer borrowed from a caller-owned stack
// object (dtoa_malloc's temporary arithmetic is one example). That is not a
// local-object escape: every callsite of its source formal was proven to pass
// an alloca, and the caller frame outlives this helper's return. An explicitly
// annotated sptr formal or any pointer derived from this function's own alloca
// is still rejected below.
static bool borrowedFromStackFormal(Value *V, ColorEscapeState &S,
                                    DenseSet<Value *> &Seen) {
  V = V->stripPointerCasts();
  if (auto *A = dyn_cast<Argument>(V))
    return S.StackOnlyFormals.count(A);
  if (!Seen.insert(V).second)
    return false;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return borrowedFromStackFormal(GEP->getPointerOperand(), S, Seen);
  auto *LI = dyn_cast<LoadInst>(V);
  if (!LI)
    return false;
  auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
  if (!Slot)
    return borrowedFromStackFormal(LI->getPointerOperand(), S, Seen);
  if (!Slot->getAllocatedType()->isPointerTy())
    return false;
  bool HasStore = false;
  for (User *U : Slot->users()) {
    auto *Store = dyn_cast<StoreInst>(U);
    if (!Store || Store->getPointerOperand()->stripPointerCasts() != Slot)
      continue;
    HasStore = true;
    if (!borrowedFromStackFormal(Store->getValueOperand(), S, Seen))
      return false;
  }
  return HasStore;
}

static void checkEscapesAndMix(Function &F, ColorEscapeState &S) {
  for (Instruction &I : instructions(F)) {
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      Value *Val = SI->getValueOperand();
      Value *Dest = SI->getPointerOperand();
      if (!Val->getType()->isPointerTy())
        continue;

      Color Eff = S.effective(Val);
      auto &VI = S.info(Val);

      // Resolve dest field annotation.
      Color DestField = Color::None;
      Value *AnnotDest = Dest;
      if (auto *II = dyn_cast<IntrinsicInst>(Dest)) {
        if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
          DestField =
              parseColorToken(annotationString(II->getArgOperand(1)));
          AnnotDest = II;
        }
      }
      Value *DestBase = Dest->stripPointerCasts();
      if (auto *II = dyn_cast<IntrinsicInst>(Dest))
        if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
          DestBase = II->getArgOperand(0)->stripPointerCasts();
      auto SlotIt = S.SlotColor.find(DestBase);
      if (SlotIt != S.SlotColor.end())
        DestField = SlotIt->second;

      bool DestStack = isStackDest(Dest, S);
      bool EscDest = isEscapingDest(Dest, S);

      // Refine Auto at store edge.
      if (Eff == Color::Auto || VI.Declared == Color::Auto ||
          VI.Declared == Color::None) {
        if (VI.StackProv && !VI.EncodedUPtr && !VI.GoHeapProv) {
          if (EscDest && !DestStack) {
            // Must have been uptr from the start — raw stack store to heap.
            Eff = Color::SPtr; // treat as nailed sptr for error path
            VI.Refined = Color::SPtr;
          } else if (DestStack) {
            VI.Refined = Color::SPtr;
            Eff = Color::SPtr;
          }
        } else if (VI.GoHeapProv) {
          VI.Refined = Color::GPtr;
          Eff = Color::GPtr;
        } else if (VI.HeapProv && !VI.StackProv) {
          VI.Refined = Color::CPtr;
          Eff = Color::CPtr;
        } else if (VI.EncodedUPtr) {
          VI.Refined = Color::UPtr;
          Eff = Color::UPtr;
        }
      }

      // gptr must not mix into cptr/sptr/uptr/auto slots.
      if (Eff == Color::GPtr || VI.GoHeapProv) {
        if (DestField == Color::CPtr || DestField == Color::SPtr ||
            DestField == Color::UPtr || DestField == Color::Auto) {
          S.error(&I,
                  "gptr cannot be stored into a " + Twine(colorName(DestField)) +
                      " field/slot (no implicit convert between Go-heap and "
                      "C/stack pointer colors)");
        }
        // Also: storing gptr into unannotated non-gptr global/cptr-ish
        if (DestField == Color::None && EscDest && !DestStack)
          S.error(&I, "gptr cannot be stored into an uncolored heap/global "
                      "slot; declare a gptr destination (no implicit "
                      "convert to cptr/auto)");
      }
      if ((Eff == Color::CPtr || Eff == Color::SPtr || Eff == Color::UPtr ||
           Eff == Color::Auto) &&
          DestField == Color::GPtr) {
        S.error(&I, Twine(colorName(Eff)) +
                        " cannot be stored into a gptr field/slot (no "
                        "implicit convert)");
      }

      // Dest expects uptr → value must be uptr / encoded.
      if (DestField == Color::UPtr && Eff != Color::UPtr && !VI.EncodedUPtr) {
        S.error(&I, "uptr field requires an encoded uptr value (use "
                    "goc_uptr_from_sptr / goc_uptr_from_cptr); refusing silent "
                    "sptr→uptr promote");
      }

      // Core escape rule: sptr / raw stack absolute must not enter heap.
      bool IsSPtrLike =
          (Eff == Color::SPtr) ||
          (VI.Declared == Color::SPtr) ||
          (VI.StackProv && !VI.EncodedUPtr && Eff != Color::UPtr &&
           Eff != Color::CPtr && Eff != Color::GPtr);

      if (IsSPtrLike && EscDest && !DestStack) {
        S.error(&I,
                "sptr escape: stack pointer color must not be stored into a "
                "heap/global field; encode with goc_uptr_from_sptr or declare "
                "the value as auto_ptr/uptr before escape (no automatic "
                "promote to uptr)");
        VI.Escapes = true;
      }

      // Explicit sptr annotation nailed then escape — always error (even if
      // somehow encoded missing).
      if (VI.Declared == Color::SPtr && EscDest && !DestStack &&
          !VI.EncodedUPtr) {
        // already errored above; ensure refined stays sptr
        VI.Refined = Color::SPtr;
      }

      (void)AnnotDest;
      continue;
    }

    if (auto *RI = dyn_cast<ReturnInst>(&I)) {
      Value *V = RI->getReturnValue();
      if (!V || !V->getType()->isPointerTy())
        continue;
      Color Eff = S.effective(V);
      auto &VI = S.info(V);
      bool IsSPtrLike =
          (Eff == Color::SPtr) || (VI.Declared == Color::SPtr) ||
          (VI.StackProv && !VI.EncodedUPtr && Eff != Color::UPtr);
      DenseSet<Value *> Seen;
      if (IsSPtrLike && !isUptrRuntimeReturn(F.getName()) &&
          !borrowedFromStackFormal(V, S, Seen)) {
        S.error(RI,
                "sptr escape: returning a stack pointer is forbidden; use "
                "uptr encoding or keep the pointer on-stack (no automatic "
                "promote to uptr)");
      }
      continue;
    }

    // Call args: if callee is known to sink to heap globals — conservative
    // check for gptr/cptr mix on annotated params is limited in P17.
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee || isAnnotationIntrinsic(Callee))
        continue;
      StringRef N = Callee->getName();
      // Passing gptr to goc_uptr_from_sptr etc. is wrong.
      if (isUptrFromSptr(N) || isUptrFromCptr(N) || isUptrFromPtr(N)) {
        Value *Arg = CB->getArgOperand(0);
        auto &AI = S.info(Arg);
        if (AI.GoHeapProv || S.effective(Arg) == Color::GPtr) {
          S.error(&I, "gptr cannot be passed to " + Twine(N) +
                          " (gptr is a separate track)");
        }
      }
      if (isGptrFrom(N) == false && N.starts_with("goc_") == false) {
        // Detect direct call that stores-like: skip.
      }
    }
  }
}

static void attachMetadata(Module &M, ColorEscapeState &S) {
  LLVMContext &Ctx = M.getContext();
  auto getMD = [&](Color C) -> MDNode * {
    return MDNode::get(Ctx, {MDString::get(Ctx, colorName(C))});
  };

  // Module-level schema note.
  M.addModuleFlag(Module::Warning, "goc.color.schema",
                  MDString::get(Ctx, "annotate+!goc.color;v0.2.2-P17"));

  for (auto &KV : S.Info) {
    Value *V = KV.first;
    Color C = S.effective(V);
    if (C == Color::None || C == Color::Auto) {
      if (KV.second.StackProv && !KV.second.EncodedUPtr)
        C = Color::SPtr;
      else if (KV.second.EncodedUPtr)
        C = Color::UPtr;
      else if (KV.second.GoHeapProv)
        C = Color::GPtr;
      else if (KV.second.HeapProv)
        C = Color::CPtr;
      else if (C == Color::Auto) {
        // leave auto
      } else
        continue;
    }
    if (auto *I = dyn_cast<Instruction>(V)) {
      // Only pointer-typed results (or void calls that produce encoded ptrs).
      if (!I->getType()->isPointerTy() && !isa<CallBase>(I))
        continue;
      if (!I->getType()->isPointerTy() && isa<CallBase>(I)) {
        // Keep metadata on annotation intrinsics / builders only if ptr-typed.
        continue;
      }
      I->setMetadata("goc.color", getMD(C));
      if (KV.second.StackProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "stack")}));
      else if (KV.second.GoHeapProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "goheap")}));
      else if (KV.second.HeapProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "cheap")}));
      if (KV.second.EncodedUPtr)
        I->setMetadata("goc.uptr_encoded",
                       MDNode::get(Ctx, {MDString::get(Ctx, "1")}));
    }
  }

  // Also tag annotated allocas/slots.
  for (auto &KV : S.SlotColor) {
    if (auto *I = dyn_cast<Instruction>(KV.first))
      I->setMetadata("goc.color", getMD(KV.second));
    else if (auto *G = dyn_cast<GlobalVariable>(KV.first)) {
      // Globals cannot take inst metadata; use named md attachment via
      // !goc.global.colors later if needed. Skip.
      (void)G;
    }
  }
}

static bool provenStackActual(Value *Actual,
                              const DenseSet<const Argument *> &Candidates) {
  SmallVector<Value *, 8> Work{Actual};
  DenseSet<Value *> Seen;
  bool HasOrigin = false;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val()->stripPointerCasts();
    if (!Seen.insert(V).second)
      continue;
    if (isa<ConstantPointerNull>(V) || isa<AllocaInst>(V)) {
      HasOrigin = true;
      continue;
    }
    if (auto *A = dyn_cast<Argument>(V)) {
      if (!Candidates.count(A))
        return false;
      HasOrigin = true;
      continue;
    }
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      Work.push_back(GEP->getPointerOperand());
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
      if (!Slot)
        return false;
      bool HasStore = false;
      for (User *U : Slot->users())
        if (auto *S = dyn_cast<StoreInst>(U))
          if (S->getPointerOperand() == Slot) {
            HasStore = true;
            Work.push_back(S->getValueOperand());
          }
      if (!HasStore)
        return false;
      continue;
    }
    if (auto *Sel = dyn_cast<SelectInst>(V)) {
      Work.push_back(Sel->getTrueValue());
      Work.push_back(Sel->getFalseValue());
      continue;
    }
    if (auto *Phi = dyn_cast<PHINode>(V)) {
      for (Value *In : Phi->incoming_values())
        Work.push_back(In);
      continue;
    }
    return false;
  }
  return HasOrigin;
}

// A file-local function with no address-taken uses and only stack-alloca
// actual arguments has a stack-only formal. Unlike a generic T* parameter,
// dereferencing it is a stack destination (e.g. dtoa_malloc(&mptr),
// js_inner_module_linking(&stack_top)). Recursive forwarding preserves this
// proof; unknown or external callsites keep the regular cptr default.
static void proveStackOnlyFormals(Module &M, ColorEscapeState &S) {
  DenseMap<Function *, SmallVector<CallBase *, 8>> Callers;
  SmallVector<Argument *, 32> Candidates;
  for (Function &F : M) {
    if (F.isDeclaration() || !F.hasLocalLinkage())
      continue;
    SmallVector<CallBase *, 8> Calls;
    bool AddressTaken = false;
    for (User *U : F.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &F) {
        AddressTaken = true;
        break;
      }
      Calls.push_back(CB);
    }
    if (AddressTaken || Calls.empty())
      continue;
    Callers[&F] = std::move(Calls);
    for (Argument &A : F.args()) {
      if (!A.getType()->isPointerTy() ||
          (S.info(&A).Declared != Color::None &&
           S.info(&A).Declared != Color::Auto))
        continue;
      Candidates.push_back(&A);
      S.StackOnlyFormals.insert(&A);
    }
  }
  bool Changed;
  do {
    Changed = false;
    for (Argument *A : Candidates) {
      if (!S.StackOnlyFormals.count(A))
        continue;
      for (CallBase *CB : Callers[A->getParent()]) {
        if (A->getArgNo() >= CB->arg_size() ||
            !provenStackActual(CB->getArgOperand(A->getArgNo()),
                               S.StackOnlyFormals)) {
          S.StackOnlyFormals.erase(A);
          Changed = true;
          break;
        }
      }
    }
  } while (Changed);
  for (Argument *A : Candidates)
    if (S.StackOnlyFormals.count(A)) {
      auto &PI = S.info(A);
      PI.Refined = Color::SPtr;
      PI.StackProv = true;
      PI.HeapProv = false;
    }
}

// Bulk coloring for large unannotated codebases (e.g. quickjs-ng): treat
// ordinary unannotated pointer slots/formals as the given color. Proven
// stack-only formals and their spill slots retain their source provenance.
// Explicit annotations (goc_sptr/uptr/gptr/...) still win.
static void applyDefaultPtrColor(Module &M, ColorEscapeState &S) {
  Color C = parseColorToken(DefaultPtrColor);
  if (C == Color::None || C == Color::Auto)
    return;
  auto apply = [&](Value *V, PtrInfo &PI) {
    if (PI.Declared != Color::None && PI.Declared != Color::Auto)
      return;
    PI.Declared = C;
    if (PI.Refined == Color::None || PI.Refined == Color::Auto)
      PI.Refined = C;
    switch (C) {
    case Color::CPtr:
      PI.HeapProv = true;
      break;
    case Color::SPtr:
      PI.StackProv = true;
      break;
    case Color::GPtr:
      PI.GoHeapProv = true;
      break;
    case Color::UPtr:
      PI.EncodedUPtr = true;
      break;
    default:
      break;
    }
    S.SlotColor[V] = C;
    S.DefaultColoredSlots.insert(V);
  };
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    DenseSet<AllocaInst *> stackFormalSlots;
    for (Instruction &I : F.getEntryBlock())
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (auto *A = dyn_cast<Argument>(SI->getValueOperand()))
          if (S.StackOnlyFormals.count(A))
            if (auto *AI = dyn_cast<AllocaInst>(SI->getPointerOperand()->stripPointerCasts()))
              stackFormalSlots.insert(AI);
    for (Instruction &I : instructions(F))
      if (auto *AI = dyn_cast<AllocaInst>(&I))
        if (AI->getAllocatedType()->isPointerTy() && !stackFormalSlots.count(AI))
          apply(AI, S.info(AI));
    for (Argument &A : F.args())
      if (A.getType()->isPointerTy() && !S.StackOnlyFormals.count(&A))
        apply(&A, S.info(&A));
  }
  errs() << "goc-color-escape: default-ptr-color=" << DefaultPtrColor << "\n";
}

// Go's pcsp table cannot describe an arbitrary change to SP in the middle of
// a C frame. Lower C alloca(size) to a per-invocation scoped allocation before
// coloring, so its result is a cptr and nested calls can grow the g stack.
// As with C alloca, repeated executions stay allocated until function return.
static unsigned lowerDynamicAllocas(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *SizeTy = Type::getInt64Ty(Ctx);
  unsigned Count = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    SmallVector<AllocaInst *, 8> Dynamic;
    SmallVector<ReturnInst *, 8> Returns;
    bool StackRestore = false;
    bool NonlocalExit = false;
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!AI->isStaticAlloca())
          Dynamic.push_back(AI);
      } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        Returns.push_back(RI);
      } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        StackRestore |= II->getIntrinsicID() == Intrinsic::stacksave ||
                        II->getIntrinsicID() == Intrinsic::stackrestore;
      } else if (isa<ResumeInst>(I) || isa<CleanupReturnInst>(I) ||
                 isa<CatchReturnInst>(I) || isa<InvokeInst>(I)) {
        NonlocalExit = true;
      }
    }
    if (Dynamic.empty())
      continue;
    if (StackRestore || NonlocalExit)
      report_fatal_error(Twine("goc: unsupported dynamic alloca lifetime in ") + F.getName());
    for (AllocaInst *AI : Dynamic)
      if (!AI->getAllocatedType()->isIntegerTy(8) ||
          AI->getAlign().value() > 16 ||
          !AI->getArraySize()->getType()->isIntegerTy() ||
          AI->getArraySize()->getType()->getIntegerBitWidth() > 64)
        report_fatal_error(Twine("goc: unsupported dynamic alloca element/alignment in ") + F.getName());
    FunctionCallee Allocate = M.getOrInsertFunction(
        "goc_dynalloc", FunctionType::get(Ptr, {SizeTy, Ptr}, false));
    FunctionCallee Release = M.getOrInsertFunction(
        "goc_dynrelease", FunctionType::get(Type::getVoidTy(Ctx), {Ptr}, false));
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    auto *Head = Entry.CreateAlloca(Ptr, nullptr, "goc.dynalloc.head");
    Entry.CreateStore(ConstantPointerNull::get(cast<PointerType>(Ptr)), Head);
    for (AllocaInst *AI : Dynamic) {
      IRBuilder<> B(AI);
      Value *Bytes = B.CreateZExtOrTrunc(AI->getArraySize(), SizeTy);
      auto *Storage = B.CreateCall(Allocate, {Bytes, Head}, "goc.dynalloc");
      AI->replaceAllUsesWith(Storage);
      AI->eraseFromParent();
      ++Count;
    }
    for (ReturnInst *RI : Returns) {
      IRBuilder<> B(RI);
      B.CreateCall(Release, {Head});
    }
  }
  return Count;
}

static int runOnModule(Module &M) {
  unsigned DynamicAllocas = lowerDynamicAllocas(M);
  ColorEscapeState S;
  seedAnnotations(M, S);
  seedProducers(M, S);
  proveStackOnlyFormals(M, S);
  applyDefaultPtrColor(M, S);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
  }

  // A decoded uptr can be assigned to a different cptr field, promoting its
  // storage in turn. Continue until every newly encoded slot's loads have also
  // been decoded; a fixed two-pass sequence silently returned encoded words.
  unsigned EncodedStores = 0, DecodedLoads = 0;
  for (;;) {
    unsigned NewEncoded = encodeEscapingSptrStores(M, S);
    unsigned NewDecoded = decodeAutoUptrLoads(M, S);
    EncodedStores += NewEncoded;
    DecodedLoads += NewDecoded;
    if (NewEncoded == 0 && NewDecoded == 0)
      break;
    for (Function &F : M) {
      if (!F.isDeclaration())
        propagate(F, S);
    }
  }
  unsigned GuardedReturns = guardDynamicUptrReturns(M, S);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    checkEscapesAndMix(F, S);
  }
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
  }
  attachMetadata(M, S);

  errs() << "goc-color-escape: summary: " << S.ErrorCount
         << " error(s), scoped dynamic allocas=" << DynamicAllocas
         << ", implicit uptr stores=" << EncodedStores
         << ", decoded pointer loads=" << DecodedLoads
         << ", guarded returns=" << GuardedReturns << " in module '"
         << M.getModuleIdentifier() << "'\n";
  return (int)S.ErrorCount;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc P17 color/escape analysis\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 2;
  }

  int Errors = runOnModule(*M);

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
