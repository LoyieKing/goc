//===- SMReanchor.cpp - the goc-reanchor pass ------------------------===//

#include "Goc/Internal/StackMap.h"

using namespace llvm;

namespace goc::sm {

struct GocReanchor : PassInfoMixin<GocReanchor> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
    bool Changed = false;
    Changed |= unmergeReportedAllocas(F);
    Changed |= addOldFramePointerSlot(F);
    PreservedAnalyses PA = run2(F, FAM);
    return Changed ? PreservedAnalyses::none() : PA;
  }

  // goc-llc's GocFrameAddrFix stores RBP here right before a call after
  // which it must rebase a derived frame address (&a[i] kept in a
  // callee-saved register, or a merge of &local with a heap pointer). The
  // slot is not in any stack map, so a stack copy moves it verbatim and it
  // still holds the pre-call RBP afterwards: delta = RBP - goc.oldfp.
  static bool addOldFramePointerSlot(Function &F) {
    bool AnySafe = false;
    for (Instruction &I : instructions(F))
      if (isSafepoint(I)) {
        AnySafe = true;
        break;
      }
    if (!AnySafe)
      return false;
    IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
    B.CreateAlloca(B.getInt64Ty(), nullptr, "goc.oldfp");
    return true;
  }

  PreservedAnalyses run2(Function &F, FunctionAnalysisManager &FAM) {
    bool AnySafe = false;
    for (Instruction &I : instructions(F))
      if (isSafepoint(I)) {
        AnySafe = true;
        break;
      }
    if (!AnySafe)
      return PreservedAnalyses::all();

    const DataLayout &DL = F.getParent()->getDataLayout();
    DenseSet<Value *> stack = stackAddresses(F, DL);
    // A local that keeps &s->token is not adjusted just because the address
    // of that local is. Tag the word so copystack rewrites the stored pointer.
    for (Instruction &I : instructions(F)) {
      auto *S = dyn_cast<StoreInst>(&I);
      if (!S || !S->getValueOperand()->getType()->isPointerTy())
        continue;
      int64_t VOff = 0;
      Value *VBase = GetPointerBaseWithConstantOffset(S->getValueOperand(), VOff, DL);
      if (!stack.count(S->getValueOperand()) && !(VBase && stack.count(VBase)))
        continue;
      int64_t SOff = 0;
      auto *Slot = dyn_cast<AllocaInst>(GetPointerBaseWithConstantOffset(
          S->getPointerOperand(), SOff, DL));
      if (!Slot || !Slot->isStaticAlloca() || Slot->isArrayAllocation() ||
          SOff < 0 || SOff % 8)
        continue;
      S->setMetadata("goc.frame.words", goc::frameWordsTag(Slot, {uint64_t(SOff)}));
    }

    struct Fix {
      Instruction *Def;
      SmallVector<Use *, 8> Uses; // uses that cross a safepoint
    };
    SmallVector<Fix, 16> fixes;
    // Program order, not the set's pointer order: the anchors are created in
    // this order, and their order is the frame layout.
    for (Instruction &DefI : instructions(F)) {
      if (!stack.count(&DefI))
        continue;
      Instruction *Def = &DefI;
      // An alloca is a frame index, but a use after a safepoint must not reuse
      // a LEA that was computed before the call. llc -O3 keeps that LEA in a
      // callee-saved register; morestack adjusts BP and the slot, not the
      // register, so the comparison sees the pre-copy address.
      Fix fix{Def, {}};
      for (Use &U : Def->uses()) {
        auto *User = dyn_cast<Instruction>(U.getUser());
        if (!User || ignoreUse(User))
          continue;
        // An inline asm memory operand of a pure frame address is addressed
        // from RBP. Replacing it with a volatile reload needs a GPR, and
        // several shims clobber every GPR. RBP is adjusted by the copier, so
        // the frame index is already the post-copy address.
        if (auto *CB = dyn_cast<CallBase>(User))
          if (CB->isInlineAsm() && pureFrameAddress(Def, DL))
            continue;
        if (separatedBySafepoint(Def, usePoint(U)))
          fix.Uses.push_back(&U);
      }
      if (!fix.Uses.empty())
        fixes.push_back(std::move(fix));
    }
    // Pointer arguments are not Instructions, so the loop above skipped them.
    // A use after any safepoint must reload: the SysV split stub restored the
    // callee-saved copy from the old stack.
    struct ArgFix {
      Argument *Arg;
      SmallVector<Use *, 8> Uses;
    };
    SmallVector<ArgFix, 4> argFixes;
    auto argUseAfterSafepoint = [&](const Instruction *UsePoint) {
      const BasicBlock &Entry = F.getEntryBlock();
      if (UsePoint->getParent() != &Entry)
        return true;
      for (const Instruction &I : Entry) {
        if (&I == UsePoint)
          return false;
        if (isSafepoint(I))
          return true;
      }
      return false;
    };
    for (Argument &A : F.args()) {
      // A proven frame address must be reloaded after growth. An ordinary
      // pointer argument must too: the caller may pass an address of its own
      // frame (a Go slice on the goroutine stack) even when this TU cannot
      // prove that. The copier adjusts the slot only when the value lies in
      // the old stack, so a heap pointer stored here is left alone.
      if (!stack.count(&A) && !A.getType()->isPointerTy())
        continue;
      ArgFix fix{&A, {}};
      for (Use &U : A.uses()) {
        auto *User = dyn_cast<Instruction>(U.getUser());
        if (!User || ignoreUse(User))
          continue;
        if (argUseAfterSafepoint(usePoint(U)))
          fix.Uses.push_back(&U);
      }
      if (!fix.Uses.empty())
        argFixes.push_back(std::move(fix));
    }
    if (fixes.empty() && argFixes.empty())
      return PreservedAnalyses::all();

    DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    SmallVector<CallBase *, 16> safepoints;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (isSafepoint(*CB))
          safepoints.push_back(CB);
    uint64_t nextMapId = 0x80000001ULL;
    auto publish = [&](Instruction *Def, Value *Saved, Value *Anchor,
                       ArrayRef<Instruction *> UsePoints) {
      SmallVector<CallBase *, 8> invalidators;
      CrossCallLiveness L(Def, UsePoints);
      for (CallBase *CB : safepoints) {
        if (Def && !DT.dominates(Def, CB))
          continue;
        bool live = L.liveAcross(CB);
        if (!live)
          for (Instruction *U : UsePoints)
            if (U != CB && DT.dominates(CB, U)) {
              live = true;
              break;
            }
        if (!live)
          continue;
        invalidators.push_back(CB);
        ensureStackmapRoot(CB, Anchor, nextMapId);
      }
    };

    unsigned anchors = 0, reloads = 0, remats = 0;
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    for (Fix &fix : fixes) {
      if (rematerializableFrameAddress(fix.Def, DL)) {
        DenseMap<std::pair<PHINode *, BasicBlock *>, Value *> shared;
        for (Use *U : fix.Uses) {
          auto *User = cast<Instruction>(U->getUser());
          if (auto *Phi = dyn_cast<PHINode>(User)) {
            BasicBlock *Inc = Phi->getIncomingBlock(U->getOperandNo());
            auto Key = std::make_pair(Phi, Inc);
            Value *R = shared.lookup(Key);
            if (!R) {
              IRBuilder<> B(Inc->getTerminator());
              R = rematFrameAddress(B, fix.Def, DL);
              if (!R)
                report_fatal_error("goc-reanchor: frame address did not rematerialize");
              shared[Key] = R;
            }
            U->set(R);
            ++remats;
            continue;
          }
          IRBuilder<> B(usePoint(*U));
          Value *R = rematFrameAddress(B, fix.Def, DL);
          if (!R)
            report_fatal_error("goc-reanchor: frame address did not rematerialize");
          U->set(R);
          ++remats;
        }
        continue;
      }
      // publish() may erase the entry stackmap this builder was inserting
      // before. Recompute so the next alloca is not attached to a dead block.
      Entry.SetInsertPoint(&F.getEntryBlock(),
                           F.getEntryBlock().getFirstInsertionPt());
      if (isa<InvokeInst>(fix.Def))
        report_fatal_error("goc-reanchor: invoke result holds a stack address");
      // Always spill across the safepoint. A fresh GEP at the use is not
      // enough: MachineCSE folds it back into the pre-call LEA, and that
      // register is not a stackmap root. A volatile reload cannot be folded,
      // and the copier adjusts the anchor slot.
      AllocaInst *Anchor = Entry.CreateAlloca(fix.Def->getType(), nullptr,
                                              "goc.anchor");
      Anchor->setAlignment(Align(8));
      goc::markRoot(Anchor, "anchor");
      MDNode *Node = goc::frameWordsTag(Anchor, {0});
      Anchor->setMetadata("goc.frame.words", Node);

      ++anchors;
      Instruction *SaveAt = isa<PHINode>(fix.Def)
                                ? fix.Def->getParent()->getFirstNonPHI()
                                : fix.Def->getNextNode();
      if (!SaveAt)
        SaveAt = fix.Def->getParent()->getTerminator();
      IRBuilder<> Save(SaveAt);
      Save.CreateStore(fix.Def, Anchor)->setVolatile(true);

      // One volatile reload per safepoint-free stretch, not per use. Extra
      // uses in the same stretch stay in a register. A PHI that lists one
      // predecessor twice must still see one incoming value.
      SmallVector<Use *, 8> ordered(fix.Uses.begin(), fix.Uses.end());
      std::sort(ordered.begin(), ordered.end(), [](Use *A, Use *B) {
        Instruction *IA = usePoint(*A);
        Instruction *IB = usePoint(*B);
        if (IA == IB)
          return false;
        if (IA->getParent() != IB->getParent())
          return IA->getParent() < IB->getParent();
        return IA->comesBefore(IB);
      });
      SmallVector<Instruction *, 8> usePoints;
      for (Use *U : ordered)
        usePoints.push_back(usePoint(*U));
      SmallVector<LoadInst *, 8> placed;
      for (Use *U : ordered) {
        Instruction *At = usePoint(*U);
        LoadInst *Share = nullptr;
        for (LoadInst *Prev : placed) {
          if (Prev->getParent() != At->getParent())
            continue;
          if (separatedBySafepoint(Prev, At))
            continue;
          Share = Prev;
          break;
        }
        if (Share) {
          U->set(Share);
          continue;
        }
        IRBuilder<> B(At);
        auto *Reload = B.CreateLoad(fix.Def->getType(), Anchor, "goc.reanchor");
        Reload->setVolatile(true);
        placed.push_back(Reload);
        U->set(Reload);
        ++reloads;
      }
      publish(fix.Def, fix.Def, Anchor, usePoints);
    }
    for (ArgFix &fix : argFixes) {
      if (fix.Arg->hasByValAttr()) {
        DenseMap<std::pair<PHINode *, BasicBlock *>, Value *> shared;
        for (Use *U : fix.Uses) {
          auto *User = cast<Instruction>(U->getUser());
          if (auto *Phi = dyn_cast<PHINode>(User)) {
            BasicBlock *Inc = Phi->getIncomingBlock(U->getOperandNo());
            auto Key = std::make_pair(Phi, Inc);
            Value *R = shared.lookup(Key);
            if (!R) {
              IRBuilder<> B(Inc->getTerminator());
              R = rematFrameAddress(B, fix.Arg, DL);
              if (!R)
                report_fatal_error("goc-reanchor: byval address did not rematerialize");
              shared[Key] = R;
            }
            U->set(R);
            ++remats;
            continue;
          }
          IRBuilder<> B(usePoint(*U));
          Value *R = rematFrameAddress(B, fix.Arg, DL);
          if (!R)
            report_fatal_error("goc-reanchor: byval address did not rematerialize");
          U->set(R);
          ++remats;
        }
        continue;
      }
      Type *ArgTy = fix.Arg->getType();
      if (!ArgTy->isSized()) {
        errs() << "goc-reanchor: skip unsized arg in " << F.getName() << "\n";
        continue;
      }
      IRBuilder<> B(&F.getEntryBlock(), F.getEntryBlock().getFirstInsertionPt());
      auto *Anchor = B.CreateAlloca(ArgTy, nullptr, "goc.arganchor");
      Anchor->setAlignment(Align(8));
      goc::markRoot(Anchor, "arganchor");
      MDNode *Node = goc::frameWordsTag(Anchor, {0});
      Anchor->setMetadata("goc.frame.words", Node);
      ++anchors;
      Instruction *At = Anchor->getNextNode()
                            ? Anchor->getNextNode()
                            : Anchor->getParent()->getTerminator();
      IRBuilder<> Save(At);
      Save.CreateStore(fix.Arg, Anchor)->setVolatile(true);

      // Use-list order is enough to share a reload inside one block. Sorting
      // with Instruction::comesBefore is not a strict weak order once the
      // block's numbering is stale, and std::sort then corrupts the heap.
      SmallVector<Instruction *, 8> usePoints;
      for (Use *U : fix.Uses)
        usePoints.push_back(usePoint(*U));
      SmallVector<LoadInst *, 8> placed;
      for (Use *U : fix.Uses) {
        Instruction *At = usePoint(*U);
        LoadInst *Share = nullptr;
        for (LoadInst *Prev : placed) {
          if (Prev->getParent() != At->getParent())
            continue;
          if (separatedBySafepoint(Prev, At))
            continue;
          Share = Prev;
          break;
        }
        if (Share) {
          U->set(Share);
          continue;
        }
        IRBuilder<> ReloadB(At);
        auto *Reload = ReloadB.CreateLoad(fix.Arg->getType(), Anchor, "goc.reanchor");
        Reload->setVolatile(true);
        placed.push_back(Reload);
        U->set(Reload);
        ++reloads;
      }
      publish(nullptr, fix.Arg, Anchor, usePoints);
    }
    if (anchors || remats)
      errs() << "goc-reanchor: " << F.getName() << " anchors=" << anchors
             << " reloads=" << reloads << " remats=" << remats << "\n";
    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

void addGocReanchor(FunctionPassManager &FPM) { FPM.addPass(GocReanchor()); }

} // namespace goc::sm
