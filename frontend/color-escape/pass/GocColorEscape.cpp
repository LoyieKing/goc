//===- GocColorEscape.cpp - P17 pointer color + escape analysis -----------===//
// Frontend vertical slice: refine auto_ptr, reject illegal sptr escapes,
// annotate IR with !goc.color metadata for later backend hooks.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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
  bool Escapes = false;         // stored to non-stack or returned
};

struct Diagnostic {
  std::string Msg;
  Instruction *At = nullptr;
};

struct ColorEscapeState {
  DenseMap<Value *, PtrInfo> Info;
  DenseMap<Value *, Color> SlotColor; // alloca/global/ptr.annotation slot → color
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
  return N == "goc_alloc" || N == "malloc" || N == "calloc" || N == "realloc";
}
static bool isGptrFrom(StringRef N) { return N == "goc_gptr_from_handle"; }

static Value *stripAC(Value *V) {
  return V->stripPointerCasts();
}

/// True if Dest address is based on a function-local alloca (stack slot).
static bool isStackLocation(Value *Dest) {
  SmallVector<Value *, 8> Work;
  DenseSet<Value *> Seen;
  Work.push_back(Dest);
  while (!Work.empty()) {
    Value *V = stripAC(Work.pop_back_val());
    if (!Seen.insert(V).second)
      continue;
    if (isa<AllocaInst>(V))
      return true;
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
  }
  return false;
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
      // Conservatively: formals may be either; leave Auto.
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

static void propagate(Function &F, ColorEscapeState &S) {
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 64) {
    Changed = false;
    for (Instruction &I : instructions(F)) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
        auto &Dst = S.info(LI);
        // If loading from an annotated pointer slot alloca, value gets that color.
        Color SlotC = Color::None;
        auto It = S.SlotColor.find(Ptr);
        if (It != S.SlotColor.end())
          SlotC = It->second;
        auto &SrcSlot = S.info(Ptr);
        PtrInfo Old = Dst;
        if (SlotC != Color::None && SlotC != Color::Auto) {
          Dst.Declared = SlotC;
          Dst.Refined = SlotC;
        } else if (SrcSlot.Declared != Color::None &&
                   SrcSlot.Declared != Color::Auto) {
          Dst.Refined = SrcSlot.Declared;
        }
        // Provenance: if slot holds stack addr, load is stack.
        // Look at stores to this slot later — for now copy slot flags if set.
        Dst.StackProv |= SrcSlot.StackProv;
        Dst.HeapProv |= SrcSlot.HeapProv;
        Dst.GoHeapProv |= SrcSlot.GoHeapProv;
        Dst.EncodedUPtr |= SrcSlot.EncodedUPtr;
        if (Dst.Refined == Color::Auto && Dst.StackProv && !Dst.HeapProv &&
            !Dst.EncodedUPtr)
          Dst.Refined = Color::SPtr;
        if (Dst.Refined == Color::Auto && Dst.HeapProv && !Dst.StackProv)
          Dst.Refined = Color::CPtr;
        if (Dst.Refined == Color::Auto && Dst.GoHeapProv)
          Dst.Refined = Color::GPtr;
        if (Dst.Refined != Old.Refined || Dst.StackProv != Old.StackProv)
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
          if (VI.Declared != Color::None)
            Slot.Declared = joinColor(Slot.Declared, VI.Declared);
          Color Eff = S.effective(Val);
          if (Eff != Color::Auto && Eff != Color::None)
            Slot.Refined = Eff;
          else if (VI.StackProv && !VI.EncodedUPtr)
            Slot.Refined = Color::SPtr;
          if (Slot.Refined != Old.Refined || Slot.StackProv != Old.StackProv)
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
        D.Refined = joinColor(D.Refined, O.Refined);
        if (O.Declared != Color::None)
          D.Declared = O.Declared;
        if (D.Refined != Old.Refined)
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
        // GEP of stack object still stack pointer (same color family).
        if (O.Refined == Color::SPtr || O.StackProv)
          D.Refined = (O.EncodedUPtr ? Color::UPtr : Color::SPtr);
        else
          D.Refined = joinColor(D.Refined, O.Refined);
        if (D.Refined != Old.Refined)
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
          D.Refined = joinColor(D.Refined, S.effective(In));
        }
        if (D.Refined != Old.Refined)
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
          D.Refined = joinColor(D.Refined, S.effective(In));
        }
        if (D.Refined != Old.Refined)
          Changed = true;
        continue;
      }
    }
  }
}

static bool isEscapingDest(Value *Dest, ColorEscapeState &S) {
  if (isGlobalLocation(Dest))
    return true;
  if (isStackLocation(Dest))
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

      bool DestStack = isStackLocation(Dest);
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
        if (DestField == Color::None && EscDest && !DestStack) {
          // If dest slot declared cptr via global annotations already handled.
        }
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
      if (IsSPtrLike) {
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
      if (isUptrFromSptr(N) || isUptrFromCptr(N)) {
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
                  MDString::get(Ctx, "annotate+!goc.color;v0.2.1-P17"));

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

static int runOnModule(Module &M) {
  ColorEscapeState S;
  seedAnnotations(M, S);
  seedProducers(M, S);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
    checkEscapesAndMix(F, S);
  }
  // Second refine+metadata after checks.
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
  }
  attachMetadata(M, S);

  errs() << "goc-color-escape: summary: " << S.ErrorCount
         << " error(s) in module '" << M.getModuleIdentifier() << "'\n";
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
