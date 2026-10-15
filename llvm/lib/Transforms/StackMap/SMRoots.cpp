//===- SMRoots.cpp - stack maps: root records and frame-address remat -------===//
//
// IR rewrites shared by goc-stackmap and goc-reanchor: STACKMAP root records,
// TLS g hoisting, frame-address rematerialization after safepoints and
// splitting allocas merged across reported roots.
//===----------------------------------------------------------------------===//

#include "Goc/Internal/StackMap.h"

using namespace llvm;

namespace goc::sm {


CallBase *stackmapBefore(Instruction *Safepoint) {
  for (Instruction *Prev = Safepoint->getPrevNode(); Prev;
       Prev = Prev->getPrevNode()) {
    if (Prev->isDebugOrPseudoInst())
      continue;
    auto *CB = dyn_cast<CallBase>(Prev);
    if (CB && CB->getIntrinsicID() == Intrinsic::experimental_stackmap)
      return CB;
    return nullptr;
  }
  return nullptr;
}

void ensureStackmapRoot(CallBase *Safepoint, Value *Root, uint64_t &NextId) {
  if (CallBase *SM = stackmapBefore(Safepoint)) {
    for (Value *Op : SM->args())
      if (Op == Root)
        return;
    SmallVector<Value *, 16> Args(SM->arg_begin(), SM->arg_end());
    Args.push_back(Root);
    IRBuilder<> B(SM);
    CallInst *New = B.CreateCall(SM->getCalledFunction(), Args);
    New->copyMetadata(*SM);
    SM->eraseFromParent();
    return;
  }
  Function *Decl = Intrinsic::getDeclaration(
      Safepoint->getModule(), Intrinsic::experimental_stackmap);
  IRBuilder<> B(Safepoint);
  B.CreateCall(Decl,
               {ConstantInt::get(B.getInt64Ty(), GocStackMapTag | NextId++),
                ConstantInt::get(B.getInt32Ty(), 0), Root});
}

// g is one value for the whole goroutine. O3 often leaves a separate
// FS:-8 / x28 read at each inlined stack check. Fold them to a single read
// in the entry block. stack.lo / stack.hi loads stay volatile.
bool hoistTlsG(Function &F) {
  SmallVector<CallInst *, 8> Reads;
  for (Instruction &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI || !CI->isInlineAsm())
      continue;
    const auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand());
    if (!IA)
      continue;
    StringRef Asm = IA->getAsmString();
    if (Asm == "movq %fs:-8, $0" || Asm == "mov $0, x28")
      Reads.push_back(CI);
  }
  if (Reads.size() <= 1)
    return false;
  IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
  CallInst *Keep = cast<CallInst>(Reads[0]->clone());
  B.Insert(Keep);
  for (CallInst *CI : Reads) {
    CI->replaceAllUsesWith(Keep);
    CI->eraseFromParent();
  }
  return true;
}

// Frame-address rematerialization: a plain GEP of the alloca / byval
// argument at the use, which llc folds into addressing modes. Plain IR is not
// enough on its own: DAG/Machine CSE, LICM and the register allocator may
// still compute the LEA before the call and keep it in a callee-saved
// register or spill slot. goc-llc's GocFrameAddrFix pass repairs exactly
// those: after every call it re-derives each live callee-saved register /
// spill slot that holds RBP/RSP+const. realbody refuses a stock llc.
Value *rematFrameAddress(IRBuilder<> &B, Value *V, const DataLayout &DL) {
  Value *Addr = nullptr;
  int64_t Off = 0;
  if (auto Frame = pureFrameAddress(V, DL)) {
    Addr = Frame->first;
    Off = Frame->second;
  } else {
    // byval (and a constant GEP of it) is an address in this frame. llc
    // lowers a plain use to one prologue LEA; the *m asm recomputes it
    // from rbp at the use, after morestack.
    Value *Cur = V;
    while (Cur && !Addr) {
      if (auto *A = dyn_cast<Argument>(Cur)) {
        if (A->hasByValAttr())
          Addr = A;
        break;
      }
      if (auto *BC = dyn_cast<BitCastInst>(Cur)) {
        Cur = BC->getOperand(0);
        continue;
      }
      if (auto *ASC = dyn_cast<AddrSpaceCastInst>(Cur)) {
        Cur = ASC->getOperand(0);
        continue;
      }
      auto *GEP = dyn_cast<GetElementPtrInst>(Cur);
      if (!GEP)
        break;
      APInt Acc(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
      if (!GEP->accumulateConstantOffset(DL, Acc))
        break;
      Off += Acc.getSExtValue();
      Cur = GEP->getPointerOperand();
    }
  }
  if (!Addr)
    return nullptr;
  Value *R = Addr;
  if (Off != 0)
    R = B.CreateInBoundsGEP(B.getInt8Ty(), Addr, B.getInt64(Off), "goc.fa");
  if (R->getType() != V->getType())
    R = B.CreatePointerBitCastOrAddrSpaceCast(R, V->getType());
  return R;
}

unsigned rematCrossSafepointUses(Value *V, const DataLayout &DL, Function &F) {
  if (!rematerializableFrameAddress(V, DL))
    return 0;
  auto *Def = dyn_cast<Instruction>(V);
  SmallVector<Use *, 16> uses;
  for (Use &U : V->uses())
    uses.push_back(&U);
  DenseMap<std::pair<PHINode *, BasicBlock *>, Value *> shared;
  unsigned n = 0;
  for (Use *U : uses) {
    auto *User = dyn_cast<Instruction>(U->getUser());
    if (!User || ignoreUse(User))
      continue;
    if (auto *CB = dyn_cast<CallBase>(User))
      if (CB->isInlineAsm())
        continue;
    Instruction *At = usePoint(*U);
    bool cross = false;
    if (Def) {
      cross = separatedBySafepoint(Def, At);
    } else if (At->getParent() != &F.getEntryBlock()) {
      cross = true;
    } else {
      for (const Instruction &I : F.getEntryBlock()) {
        if (&I == At)
          break;
        if (isSafepoint(I)) {
          cross = true;
          break;
        }
      }
    }
    if (!cross)
      continue;
    Value *R = nullptr;
    if (auto *Phi = dyn_cast<PHINode>(User)) {
      BasicBlock *Inc = Phi->getIncomingBlock(U->getOperandNo());
      auto Key = std::make_pair(Phi, Inc);
      R = shared.lookup(Key);
      if (!R) {
        IRBuilder<> B(Inc->getTerminator());
        R = rematFrameAddress(B, V, DL);
        shared[Key] = R;
      }
    } else {
      IRBuilder<> B(At);
      R = rematFrameAddress(B, V, DL);
    }
    if (!R)
      report_fatal_error("goc-stackmap: frame address did not rematerialize");
    U->set(R);
    ++n;
  }
  return n;
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
