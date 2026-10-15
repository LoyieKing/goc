//===- CEAnalysis.cpp - colors: seeding and per-function propagation -===//

#include "ColorEscapeInternal.h"

using namespace llvm;

namespace goc::ce {

cl::opt<bool> Verbose("verbose", cl::desc("Verbose diagnostics"),
                             cl::init(false));

cl::opt<std::string> DefaultPtrColor(
    "default-ptr-color",
    cl::desc("Color for unannotated pointer slots/formals: cptr|sptr|uptr|gptr "
             "(default: auto = per-value inference)"),
    cl::init("auto"));

const char *colorName(Color C) {
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

Color parseColorToken(StringRef S) {
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

StringRef annotationString(Value *V) {
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

bool isAnnotationIntrinsic(Function *F) {
  if (!F)
    return false;
  auto ID = F->getIntrinsicID();
  return ID == Intrinsic::var_annotation || ID == Intrinsic::ptr_annotation ||
         ID == Intrinsic::annotation;
}

bool isUptrFromSptr(StringRef N) {
  return N == "goc_uptr_from_sptr" || N == "goc_uptr_from_sptr_hi";
}

bool isUptrFromPtr(StringRef N) { return N == "goc_uptr_from_ptr"; }

bool isUptrDecode(StringRef N) { return N == "goc_uptr_decode"; }

bool isUptrFromCptr(StringRef N) {
  return N == "goc_uptr_from_cptr" || N == "goc_uptr_from_cptr_hi";
}

bool isUptrAsSptr(StringRef N) {
  return N == "goc_uptr_as_sptr" || N == "goc_uptr_as_sptr_hi";
}

bool isUptrAsCptr(StringRef N) {
  return N == "goc_uptr_as_cptr" || N == "goc_uptr_as_cptr_hi";
}

bool isGocAlloc(StringRef N) {
  return N == "goc_alloc" || N == "goc_dynalloc" || N == "malloc" || N == "calloc" ||
         N == "realloc" || N == "goc_malloc" || N == "goc_calloc" ||
         N == "goc_realloc";
}

bool isGptrFrom(StringRef N) { return N == "goc_gptr_from_handle"; }

// These are the implementations of the compiler's own uptr primitives. They
// return a checked/decode result by contract; ordinary user functions must
// still reject raw sptr returns.
bool isUptrRuntimeReturn(StringRef N) {
  return isUptrAsSptr(N) || isUptrDecode(N) ||
         N == "goc_uptr_require_cptr";
}

Value *stripAC(Value *V) {
  return V->stripPointerCasts();
}

/// True only if every possible Dest address is based on a local alloca.
bool isStackLocation(Value *Dest) {
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

bool isGlobalLocation(Value *Dest) {
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

void seedAnnotations(Module &M, ColorEscapeState &S) {
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

void seedProducers(Module &M, ColorEscapeState &S) {
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

Color joinColor(Color A, Color B) {
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
void applyColorProv(PtrInfo &PI, Color C) {
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

void propagate(Function &F, ColorEscapeState &S) {
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

} // namespace goc::ce
