//===- SMAddresses.cpp - stack maps: which values are frame addresses -===//

#include "StackMapInternal.h"

using namespace llvm;

namespace goc::sm {

DenseSet<Value *> stackAddresses(Function &F, const DataLayout &DL) {
  struct SlotInfo {
    bool Seeded = false;
    SmallVector<Value *, 4> Stored;
    SmallVector<LoadInst *, 4> Loads;
  };
  DenseMap<AllocaInst *, DenseMap<int64_t, SlotInfo>> slots;
  auto infoFor = [&](Value *Ptr) -> SlotInfo * {
    int64_t Off = 0;
    auto *AI = dyn_cast<AllocaInst>(GetPointerBaseWithConstantOffset(Ptr, Off, DL));
    if (!AI || !AI->isStaticAlloca())
      return nullptr;
    return &slots[AI][Off];
  };

  for (Instruction &I : instructions(F)) {
    if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (!S->getValueOperand()->getType()->isPointerTy())
        continue;
      if (SlotInfo *Info = infoFor(S->getPointerOperand()))
        Info->Stored.push_back(S->getValueOperand());
    } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->getType()->isPointerTy())
        continue;
      if (SlotInfo *Info = infoFor(LI->getPointerOperand()))
        Info->Loads.push_back(LI);
    }
  }

  DenseSet<Value *> stack;
  for (Instruction &I : instructions(F))
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      if (AI->isStaticAlloca())
        stack.insert(AI);
  const DenseSet<const Argument *> &stackArgs =
      stackPointerArguments(*F.getParent(), DL);
  for (Argument &A : F.args())
    if (stackArgs.count(&A))
      stack.insert(&A);

  auto close = [&]() {
    bool Grew = true;
    while (Grew) {
      Grew = false;
      for (Instruction &I : instructions(F)) {
        if (stack.count(&I))
          continue;
        if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          if (stack.count(GEP->getPointerOperand())) {
            stack.insert(GEP);
            Grew = true;
          }
        } else if (auto *BC = dyn_cast<BitCastInst>(&I)) {
          if (stack.count(BC->getOperand(0))) {
            stack.insert(BC);
            Grew = true;
          }
        } else if (auto *ASC = dyn_cast<AddrSpaceCastInst>(&I)) {
          if (stack.count(ASC->getOperand(0))) {
            stack.insert(ASC);
            Grew = true;
          }
        } else if (auto *Fr = dyn_cast<FreezeInst>(&I)) {
          if (Fr->getType()->isPointerTy() && stack.count(Fr->getOperand(0))) {
            stack.insert(Fr);
            Grew = true;
          }
        } else if (auto *Phi = dyn_cast<PHINode>(&I)) {
          if (!Phi->getType()->isPointerTy())
            continue;
          bool Any = false, All = true;
          for (Value *Inc : Phi->incoming_values()) {
            if (isa<ConstantPointerNull>(Inc) || isa<UndefValue>(Inc))
              continue;
            if (!stack.count(Inc)) {
              All = false;
              break;
            }
            Any = true;
          }
          if (Any && All) {
            stack.insert(Phi);
            Grew = true;
          }
        } else if (auto *Sel = dyn_cast<SelectInst>(&I)) {
          if (!Sel->getType()->isPointerTy())
            continue;
          Value *T = Sel->getTrueValue(), *Fa = Sel->getFalseValue();
          auto known = [&](Value *V) {
            return isa<ConstantPointerNull>(V) || isa<UndefValue>(V) || stack.count(V);
          };
          if (known(T) && known(Fa) && (stack.count(T) || stack.count(Fa))) {
            stack.insert(Sel);
            Grew = true;
          }
        } else if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (CB->getIntrinsicID() == Intrinsic::frameaddress &&
              CB->getType()->isPointerTy()) {
            stack.insert(CB);
            Grew = true;
          }
        }
      }
    }
  };

  close();
  bool Progress = true;
  while (Progress) {
    Progress = false;
    for (auto &AIEntry : slots) {
      for (auto &OffEntry : AIEntry.second) {
        if (OffEntry.second.Seeded)
          continue;
        // Do not seed from the color tag alone: every sptr-colored slot,
        // including the bytecode pc, carries goc.frame.words. Seeding those
        // spills heap pointers that do not move. A real frame address stored
        // into the slot (or copied from one that was) is the seed.
        bool Seed = false;
        for (Value *Stored : OffEntry.second.Stored)
          if (!isa<ConstantPointerNull>(Stored) && !isa<UndefValue>(Stored) &&
              stack.count(Stored))
            Seed = true;
        if (!Seed)
          continue;
        OffEntry.second.Seeded = true;
        for (LoadInst *LI : OffEntry.second.Loads)
          if (stack.insert(LI).second)
            Progress = true;
      }
    }
    if (Progress)
      close();
  }
  return stack;
}

// llc's StackColoring merges allocas whose lifetime.start/end ranges are
// disjoint (-no-stack-slot-sharing only covers spill slots). A frame word
// that realbody reports as a pointer slot must keep its own slot: a merged
// object carries the other alloca's name in the post-PEI MIR (realbody then
// cannot find it) and can hold a scalar while the map calls it a pointer.
// The leaq asm hid most such loads from realbody's check; with plain GEPs
// they are visible, so drop the markers of every alloca realbody may report:
// frame-word-tagged allocas and allocas that receive a pointer or sptr store.
bool unmergeReportedAllocas(Function &F) {
  DenseSet<AllocaInst *> keep;
  auto slots = goc::slotMap(F);
  auto tagged = [&](const Instruction &I) {
    if (AllocaInst *AI = goc::frameWordsSlot(slots, I.getMetadata("goc.frame.words")))
      keep.insert(AI);
  };
  for (Instruction &I : instructions(F)) {
    tagged(I);
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      if (AI->getMetadata("goc.frame.words"))
        keep.insert(AI);
    auto *St = dyn_cast<StoreInst>(&I);
    if (!St)
      continue;
    Value *Val = St->getValueOperand();
    auto *VI = dyn_cast<Instruction>(Val);
    if (!Val->getType()->isPointerTy() && !(VI && isSptrColored(*VI)))
      continue;
    if (auto *AI = dyn_cast<AllocaInst>(getUnderlyingObject(St->getPointerOperand())))
      keep.insert(AI);
  }
  if (keep.empty())
    return false;
  SmallVector<IntrinsicInst *, 16> dead;
  for (Instruction &I : instructions(F))
    if (auto *II = dyn_cast<IntrinsicInst>(&I))
      if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
          II->getIntrinsicID() == Intrinsic::lifetime_end)
        if (auto *AI = dyn_cast<AllocaInst>(getUnderlyingObject(II->getArgOperand(1))))
          if (keep.count(AI))
            dead.push_back(II);
  for (IntrinsicInst *II : dead)
    II->eraseFromParent();
  return !dead.empty();
}

} // namespace goc::sm
