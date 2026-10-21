//===- CERewrite.cpp - colors: implicit uptr encode/decode rewrites ---===//

#include "Goc/Internal/ColorEscape.h"

using namespace llvm;

namespace goc::ce {

bool isStackDest(Value *Dest, ColorEscapeState &S) {
  if (isStackLocation(Dest))
    return true;
  return provenStackActual(Dest, S.StackOnlyFormals);
}

bool isEscapingDest(Value *Dest, ColorEscapeState &S) {
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

Color pointerSlotColor(Value *Address, ColorEscapeState &S) {
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

SmallVector<Value *, 4> pointerStorageRoots(Value *Address) {
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

const GEPOperator *pointerFieldGEP(Value *Address) {
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

std::string pointerFieldKey(Value *Address) {
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

void markAutoUptrSlot(Value *Address, ColorEscapeState &S) {
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

// True if this pointer value's referent may escape. A load of a local
// `T *p = &local;` only names the slot; the escape is of what was stored
// into it (QuickJS: `JSStackFrame sf_s, *sf = &sf_s;`).
static bool rootOrPointeeMayEscape(Value *Root) {
  if (addressMayEscape(Root))
    return true;
  auto *LI = dyn_cast<LoadInst>(Root);
  if (!LI)
    return false;
  auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
  if (!Slot || !Slot->getAllocatedType()->isPointerTy())
    return false;
  for (User *U : Slot->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getPointerOperand() != Slot)
      continue;
    if (addressMayEscape(SI->getValueOperand()))
      return true;
  }
  return false;
}

bool isAutoUptrSlot(Value *Address, ColorEscapeState &S) {
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
        Escapes |= rootOrPointeeMayEscape(Root);
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

bool addressMayEscape(Value *Address) {
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

FunctionCallee getUnaryPtrHelper(Module &M, StringRef Name) {
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *FT = FunctionType::get(PtrTy, {PtrTy}, false);
  return M.getOrInsertFunction(Name, FT);
}

bool mayDirectlyNameStack(Value *V, DenseSet<Value *> &Seen) {
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

unsigned encodeEscapingSptrStores(Module &M, ColorEscapeState &S) {
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

unsigned decodeAutoUptrLoads(Module &M, ColorEscapeState &S) {
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

// goc_uptr_from_ptr(goc_uptr_decode(X)) is X. A stack word is an offset from
// stack.hi, so decoding and re-encoding it under the same g gives the same
// offset back; a cptr word (MSB clear, outside the stack) passes through
// both unchanged. Copying an encoded field into another uptr slot
// (sf->prev_frame = rt->current_stack_frame) then costs no range checks.
unsigned foldReencodedDecodes(Module &M, ColorEscapeState &S) {
  SmallVector<CallInst *, 32> Folded;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      auto *Enc = dyn_cast<CallInst>(&I);
      if (!Enc || !Enc->getCalledFunction() ||
          !isUptrFromPtr(Enc->getCalledFunction()->getName()))
        continue;
      auto *Dec = dyn_cast<CallInst>(Enc->getArgOperand(0));
      if (!Dec || !Dec->getCalledFunction() ||
          !isUptrDecode(Dec->getCalledFunction()->getName()))
        continue;
      Enc->replaceAllUsesWith(Dec->getArgOperand(0));
      Folded.push_back(Enc);
    }
  }
  // The -O0 form keeps the decoded word in a local first
  // (prev_sf = rt->current_stack_frame; sf->prev_frame = prev_sf). When every
  // assignment of that local is a decode, a shadow local carries the encoded
  // words alongside, and an encode of a load of the local reads the shadow.
  SmallVector<std::pair<CallInst *, LoadInst *>, 16> SlotFolds;
  DenseMap<AllocaInst *, AllocaInst *> Shadow;
  auto decodedOnlySlot = [&](AllocaInst *A) {
    if (!A->getAllocatedType()->isPointerTy())
      return false;
    bool AnyStore = false;
    for (User *U : A->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        auto *D = dyn_cast<CallInst>(SI->getValueOperand());
        if (SI->getPointerOperand() != A || !D || !D->getCalledFunction() ||
            !isUptrDecode(D->getCalledFunction()->getName()))
          return false;
        AnyStore = true;
        continue;
      }
      if (isa<LoadInst>(U))
        continue;
      if (auto *II = dyn_cast<IntrinsicInst>(U))
        if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
            II->getIntrinsicID() == Intrinsic::lifetime_end)
          continue;
      return false; // escapes or is written some other way
    }
    return AnyStore;
  };
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      auto *Enc = dyn_cast<CallInst>(&I);
      if (!Enc || !Enc->getCalledFunction() ||
          !isUptrFromPtr(Enc->getCalledFunction()->getName()))
        continue;
      auto *L = dyn_cast<LoadInst>(Enc->getArgOperand(0));
      if (!L)
        continue;
      auto *A = dyn_cast<AllocaInst>(L->getPointerOperand());
      if (!A || !decodedOnlySlot(A))
        continue;
      SlotFolds.push_back({Enc, L});
    }
  }
  for (auto [Enc, L] : SlotFolds) {
    auto *A = cast<AllocaInst>(L->getPointerOperand());
    AllocaInst *&Sh = Shadow[A];
    if (!Sh) {
      Sh = new AllocaInst(A->getAllocatedType(), A->getAddressSpace(),
                          A->getName() + ".uptr", A->getNextNode());
      Sh->setAlignment(A->getAlign());
      S.SlotColor[Sh] = Color::UPtr;
      PtrInfo &SI = S.info(Sh);
      SI.Declared = Color::UPtr;
      SI.Refined = Color::UPtr;
      SI.StackProv = true; // the slot itself lives on the stack
      SmallVector<StoreInst *, 4> Stores;
      for (User *U : A->users())
        if (auto *St = dyn_cast<StoreInst>(U))
          Stores.push_back(St);
      for (StoreInst *St : Stores) {
        auto *D = cast<CallInst>(St->getValueOperand());
        new StoreInst(D->getArgOperand(0), Sh, St->getNextNode());
      }
    }
    auto *ShL = new LoadInst(L->getType(), Sh, L->getName() + ".uptr", Enc);
    PtrInfo &LI = S.info(ShL);
    LI.Declared = Color::UPtr;
    LI.Refined = Color::UPtr;
    LI.EncodedUPtr = true;
    Enc->replaceAllUsesWith(ShL);
    S.Info.erase(Enc);
    Enc->eraseFromParent();
  }
  for (CallInst *Enc : Folded) {
    auto *Dec = cast<CallInst>(Enc->getArgOperand(0));
    S.Info.erase(Enc);
    Enc->eraseFromParent();
    if (Dec->use_empty()) {
      S.Info.erase(Dec);
      Dec->eraseFromParent();
    }
  }
  return Folded.size() + SlotFolds.size();
}

unsigned guardDynamicUptrReturns(Module &M, ColorEscapeState &S) {
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
bool borrowedFromStackFormal(Value *V, ColorEscapeState &S,
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

} // namespace goc::ce
