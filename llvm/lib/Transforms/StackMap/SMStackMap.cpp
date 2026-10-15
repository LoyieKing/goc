//===- SMStackMap.cpp - the goc-stackmap pass ------------------------===//

#include "StackMapInternal.h"

using namespace llvm;

namespace goc::sm {

struct GocStackMap : PassInfoMixin<GocStackMap> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
    bool Hoisted = hoistTlsG(F);

    const DataLayout &DL = F.getParent()->getDataLayout();
    // Computed before this pass rewrites uses. Includes allocas, GEPs of
    // them, and loads of slots that actually hold those addresses.
    DenseSet<Value *> stackDerived = stackAddresses(F, DL);
    DenseSet<const Argument *> stackFormals;
    for (User *U : F.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &F)
        continue;
      for (unsigned i = 0; i < CB->arg_size() && i < F.arg_size(); ++i) {
        Value *Actual = CB->getArgOperand(i);
        if (!Actual->getType()->isPointerTy())
          continue;
        int64_t Offset = 0;
        if (isa_and_nonnull<AllocaInst>(GetPointerBaseWithConstantOffset(
                Actual, Offset, DL)))
          stackFormals.insert(F.getArg(i));
      }
    }
    DenseSet<AllocaInst *> stackFormalSlots;
    if (!stackFormals.empty())
      for (Instruction &I : F.getEntryBlock()) {
        auto *S = dyn_cast<StoreInst>(&I);
        if (!S || !stackFormals.count(dyn_cast<Argument>(S->getValueOperand())))
          continue;
        auto *Slot = dyn_cast<AllocaInst>(S->getPointerOperand()->stripPointerCasts());
        if (Slot && Slot->getAllocatedType()->isPointerTy())
          stackFormalSlots.insert(Slot);
      }

    // Only *pointer-typed* values: the color pass also marks integer-typed
    // values (a uintptr_t may hold a stack pointer after goc_uptr_as_sptr), but
    // a slot holding such an integer must not be reported as a pointer — the
    // runtime rejects that loudly ("bad pointer in frame").
    SmallVector<Instruction *, 16> sptr;
    for (Instruction &I : instructions(F)) {
      // An alloca is a frame index, not a pointer word retained across a
      // safepoint. LLVM rematerializes its address from the adjusted BP;
      // rewriting its memory uses through a root would obscure the slot's
      // identity from the real-MF map extractor.
      if (!I.getType()->isPointerTy() || isa<AllocaInst>(I))
        continue;
      auto *LI = dyn_cast<LoadInst>(&I);
      bool FromStackFormal = LI && stackFormalSlots.count(
          dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts()));
      if (isSptrColored(I) || FromStackFormal || stackDerived.count(&I))
        sptr.push_back(&I);
    }
    bool Changed = Hoisted;
    if (!sptr.empty()) {
      Changed = true;
      // The frame map marks an sptr-holding alloca for the whole function, so
      // a slot that has not been assigned yet would hold junk — and the
      // runtime validates every marked slot ("bad pointer in frame" for
      // 0 < p < 4096). Zero-initialising pointer allocas is free of false
      // positives: 0 is skipped by adjustpointers, and a later store makes the
      // slot a real pointer. (Reading an uninitialised variable is UB in C.)
      SmallVector<AllocaInst *, 8> inits;
      for (Instruction &I : instructions(F))
        if (auto *Slot = dyn_cast<AllocaInst>(&I))
          if (Slot->getAllocatedType()->isPointerTy() && !Slot->isArrayAllocation() &&
              !stackFormalSlots.count(Slot) && !isSptrColored(*Slot))
            inits.push_back(Slot);
      for (AllocaInst *Slot : inits) {
        IRBuilder<> B(Slot->getNextNode());
        B.CreateStore(ConstantPointerNull::get(cast<PointerType>(Slot->getAllocatedType())), Slot);
      }
    }

    // A pointer can also be stored in one word of a stack aggregate (for
    // example JSValue.u or va_list.overflow_arg_area). The address of that
    // word is often rooted above and reloaded through goc.spill.root, so the
    // textual IR after this pass no longer identifies its frame object. Tag
    // the *writer* (a store, or va_start/va_copy, whose backend lowering fills
    // va_list.overflow_arg_area/reg_save_area with stack addresses) with its
    // original alloca and byte offsets before rewriting uses; the post-PEI map
    // extractor resolves that alloca to an actual BP offset. Initialize the
    // words first: they are marked for the entire frame, including calls
    // preceding this particular writer.
    DenseMap<AllocaInst *, SmallVector<uint64_t, 4>> frameWords;
    auto tagWords = [&](Instruction &Writer, AllocaInst *Slot,
                        ArrayRef<uint64_t> Words) {
      TypeSize Size = DL.getTypeAllocSize(Slot->getAllocatedType());
      for (uint64_t off : Words)
        if (Size.isScalable() || off + 8 > Size.getFixedValue())
          report_fatal_error("goc-stackmap: pointer word outside its frame alloca");
      auto &previous = frameWords[Slot];
      for (uint64_t off : Words)
        if (std::find(previous.begin(), previous.end(), off) == previous.end())
          previous.push_back(off);
      Writer.setMetadata("goc.frame.words", goc::frameWordsTag(Slot, Words));
    };
    for (Instruction &I : instructions(F))
      if (auto *Slot = dyn_cast<AllocaInst>(&I))
        if (Slot->getAllocatedType()->isPointerTy() &&
            !Slot->isArrayAllocation() && isSptrColored(*Slot)) {
          tagWords(*Slot, Slot, {0});
        }
    for (Instruction &I : instructions(F)) {
      if (auto *VA = dyn_cast<IntrinsicInst>(&I)) {
        if (VA->getIntrinsicID() != Intrinsic::vastart &&
            VA->getIntrinsicID() != Intrinsic::vacopy)
          continue;
        // x86-64 va_list element: { i32 gp, i32 fp, ptr overflow, ptr save }.
        int64_t Offset = 0;
        auto *Slot = dyn_cast_or_null<AllocaInst>(GetPointerBaseWithConstantOffset(
            VA->getArgOperand(0), Offset, DL));
        if (!Slot || Slot->isArrayAllocation() || Offset < 0 || Offset % 8)
          report_fatal_error("goc-stackmap: va_list is not a fixed frame object");
        uint64_t Base = uint64_t(Offset);
        tagWords(*VA, Slot, {Base + 8, Base + 16});
        continue;
      }
      auto *S = dyn_cast<StoreInst>(&I);
      if (!S || !S->getValueOperand()->getType()->isPointerTy())
        continue;
      if (auto *A = dyn_cast<Argument>(S->getValueOperand())) {
        if (stackFormals.count(A)) {
          auto *Slot = dyn_cast<AllocaInst>(S->getPointerOperand()->stripPointerCasts());
          if (Slot && stackFormalSlots.count(Slot)) {
            tagWords(*S, Slot, {0});
            continue;
          }
        }
      }
      auto *V = dyn_cast<Instruction>(S->getValueOperand());
      int64_t StoredOff = 0;
      Value *StoredBase = GetPointerBaseWithConstantOffset(
          S->getValueOperand(), StoredOff, DL);
      // A GEP of a stack argument (&s->token) may not itself be in the set if
      // instcombine folded the base. The field address is still a stack pointer
      // and the slot that keeps it across calls must be in the frame map.
      if (!(V && isSptrColored(*V)) && !stackDerived.count(S->getValueOperand()) &&
          !(StoredBase && stackDerived.count(StoredBase)))
        continue;

      int64_t Offset = 0;
      auto *Slot = frameAlloca(S->getPointerOperand(), Offset, DL);
      SmallVector<uint64_t, 4> words;
      if (Slot) {
        if (Offset >= 0 && Offset % 8 == 0)
          words.push_back(uint64_t(Offset));
      } else {
        // A runtime index into a small local pointer array can name any of
        // its words; report all of them rather than guessing the index.
        Slot = dyn_cast<AllocaInst>(getUnderlyingObject(S->getPointerOperand()));
        auto *Array = Slot ? dyn_cast<ArrayType>(Slot->getAllocatedType()) : nullptr;
        if (Array && Array->getElementType()->isPointerTy() &&
            Array->getNumElements() <= 64)
          for (uint64_t j = 0; j < Array->getNumElements(); ++j)
            words.push_back(j * 8);
      }
      if (!Slot || words.empty() || Slot->isArrayAllocation())
        continue; // unresolved stack addresses fail closed in the extractor
      tagWords(*S, Slot, words);
    }
    // Clang lowers `link = (Struct){old_stack_ptr, ...}` as stores into a
    // compound-literal alloca followed by memcpy to `link`. Mapping only the
    // source leaves the live destination pointer stale after copystack. Track
    // exact alloca-to-alloca copies (including transitive copies) before
    // finalizing the frame bitmap; do not mistake a copied pointer field for
    // an encoded heap field or blanket-mark the whole aggregate.
    SmallVector<MemTransferInst *, 16> copies;
    for (Instruction &I : instructions(F))
      if (auto *Copy = dyn_cast<MemTransferInst>(&I))
        copies.push_back(Copy);
    bool Again;
    do {
      Again = false;
      for (MemTransferInst *Copy : copies) {
        int64_t SrcOffset = 0, DstOffset = 0;
        auto *Src = dyn_cast_or_null<AllocaInst>(GetPointerBaseWithConstantOffset(
            Copy->getSource(), SrcOffset, DL));
        if (!Src || !frameWords.count(Src))
          continue;
        auto *Size = dyn_cast<ConstantInt>(Copy->getLength());
        if (!Size || SrcOffset < 0)
          report_fatal_error("goc-stackmap: dynamic copy of a pointer-bearing frame object");
        // O3 turns a field copy into llvm.memcpy of a subrange. The alloca
        // may hold a stack address in a word this copy does not touch (a
        // ValueBuffer's inline storage pointer sits beside the bytes being
        // moved to the heap). Only a copied pointer word has to be tracked.
        uint64_t Start = uint64_t(SrcOffset);
        uint64_t Bytes = Size->getZExtValue();
        SmallVector<uint64_t, 4> Overlap;
        for (uint64_t Off : frameWords[Src]) {
          if (Off < Start || Off - Start >= Bytes)
            continue;
          if (Off - Start + 8 > Bytes)
            report_fatal_error("goc-stackmap: partial copy of a pointer word");
          Overlap.push_back(Off);
        }
        if (Overlap.empty())
          continue;
        auto *Dst = dyn_cast_or_null<AllocaInst>(GetPointerBaseWithConstantOffset(
            Copy->getDest(), DstOffset, DL));
        if (!Dst || Dst->isArrayAllocation() || DstOffset < 0)
          report_fatal_error("goc-stackmap: pointer-bearing frame copy has an untracked destination");
        SmallVector<uint64_t, 4> New;
        SmallVector<uint64_t, 4> Mapped;
        for (uint64_t Off : Overlap) {
          uint64_t Target = uint64_t(DstOffset) + Off - Start;
          Mapped.push_back(Target);
          auto &Existing = frameWords[Dst];
          if (std::find(Existing.begin(), Existing.end(), Target) == Existing.end())
            New.push_back(Target);
        }
        if (!New.empty()) {
          tagWords(*Copy, Dst, Mapped);
          Again = true;
        }
      }
    } while (Again);
    for (auto &entry : frameWords) {
      Changed = true;
      // An optimizer can replace or fold the writer that first identified a
      // pointer word. The alloca is kept alive by the volatile initialization
      // below; attach the complete map to it as well so frame extraction
      // remains valid after IR optimization.
      tagWords(*entry.first, entry.first, entry.second);
      IRBuilder<> B(entry.first->getNextNode());
      for (uint64_t off : entry.second) {
        auto *Init = B.CreateStore(B.getInt64(0), B.CreateGEP(B.getInt8Ty(),
                                                                entry.first, B.getInt64(off)));
        // This word is in the Go frame bitmap for the whole function. Keep
        // its physical slot even if LLVM scalarizes every ordinary use.
        Init->setVolatile(true);
      }
    }
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    Module *M = F.getParent();
    FunctionCallee SM =
        Intrinsic::getDeclaration(M, Intrinsic::experimental_stackmap);

    uint64_t id = 0;
    SmallVector<CallBase *, 16> sites;
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || CB->isInlineAsm() || isNonSafepointIntrinsic(*CB) ||
          CB->isDebugOrPseudoInst())
        continue;
      sites.push_back(CB);
    }

    DenseMap<CallBase *, SmallVector<Instruction *, 16>> liveAt;
    DenseMap<Instruction *, std::unique_ptr<CrossCallLiveness>> liveness;
    for (Instruction *V : sptr) {
      SmallVector<Instruction *, 16> pts;
      for (Use &U : V->uses())
        if (isa<Instruction>(U.getUser()))
          pts.push_back(usePoint(U));
      liveness[V] = std::make_unique<CrossCallLiveness>(V, pts);
    }
    for (CallBase *CB : sites) {
      for (Instruction *V : sptr) {
        if (V == CB || !DT.dominates(V, CB))
          continue;
        if (liveness[V]->liveAcross(CB)) {
          liveAt[CB].push_back(V);
          continue;
        }
        // An argument is live through the call even when its last use is
        // precisely that call. A callee can grow the stack before it reads
        // the argument, so dropping last-use sptr values loses a root.
        bool liveHere = false;
        for (Value *Arg : CB->args())
          if (Arg == V) {
            liveHere = true;
            break;
          }
        // PHI operands are used on their incoming edges, not in the PHI block.
        // A call on an incoming edge can therefore keep an SSA value alive
        // even though it does not dominate the PHI instruction.
        for (User *U : V->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI)
            continue;
          if (auto *Phi = dyn_cast<PHINode>(UI)) {
            for (unsigned n = 0; n < Phi->getNumIncomingValues(); ++n)
              if (Phi->getIncomingValue(n) == V &&
                  DT.dominates(CB, Phi->getIncomingBlock(n)->getTerminator())) {
                liveHere = true;
                break;
              }
          } else if (DT.dominates(CB, UI)) {
            liveHere = true;
          }
          if (liveHere)
            break;
        }
        if (liveHere)
          liveAt[CB].push_back(V);
      }
    }

    DenseMap<Instruction *, AllocaInst *> roots;
    for (Instruction *V : sptr) {
      bool needRoot = false;
      // A static alloca address is rbp+off. Rematerialize it at the use
      // instead of storing it in the prologue; morestack adjusts rbp.
      if (rematerializableFrameAddress(V, DL))
        continue; // rematCrossSafepointUses runs after this loop
      for (CallBase *CB : sites)
        if (auto It = liveAt.find(CB); It != liveAt.end())
          for (Instruction *Live : It->second)
            needRoot |= Live == V;
      if (!needRoot)
        continue;

      IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
      auto *Root = Entry.CreateAlloca(V->getType(), nullptr, "goc.spill.root");
      goc::markRoot(Root, "spill");
      roots[V] = Root;

      // Remember original users before adding the save. The slot is the
      // stackmap root. Between safepoints the reloaded SSA value stays in a
      // register; the next safepoint reloads, because morestack may have
      // adjusted the slot. The store and the reload stay volatile so DSE
      // cannot drop the store (the stackmap names the slot address) and so
      // the post-call load is not forwarded from before the call.
      SmallVector<Use *, 16> uses;
      for (Use &U : V->uses())
        uses.push_back(&U);
      Instruction *IP = isa<PHINode>(V) ? V->getParent()->getFirstNonPHI()
                                        : V->getNextNode();
      if (!IP)
        IP = V->getParent()->getTerminator();
      IRBuilder<> Save(IP);
      Save.CreateStore(V, Root)->setVolatile(true);
      // One volatile reload per safepoint-free stretch in a block. Later
      // uses in that stretch keep the SSA value in a register. The next
      // call reloads: morestack may have adjusted the slot, and a
      // callee-saved copy of the pre-call bits is not adjusted. Volatile
      // stops the post-call load from being forwarded. Frame addresses are
      // not spilled here; GocFrameAddrFix re-derives them after tail
      // duplication. A switch can list one predecessor twice; those edges
      // must share one reload instruction.
      for (BasicBlock &BB : F)
        BB.renumberInstructions();
      SmallVector<Use *, 16> ordered(uses.begin(), uses.end());
      std::sort(ordered.begin(), ordered.end(), [](Use *A, Use *B) {
        Instruction *IA = usePoint(*A);
        Instruction *IB = usePoint(*B);
        if (IA == IB)
          return false;
        if (IA->getParent() != IB->getParent())
          return IA->getParent() < IB->getParent();
        return IA->comesBefore(IB);
      });
      DenseMap<std::pair<PHINode *, BasicBlock *>, LoadInst *> phiReload;
      SmallVector<LoadInst *, 8> placed;
      for (Use *U : ordered) {
        auto *UserInst = dyn_cast<Instruction>(U->getUser());
        if (!UserInst)
          report_fatal_error("goc-stackmap: non-instruction sptr user");
        if (auto *Call = dyn_cast<CallBase>(UserInst))
          if (isNonSafepointIntrinsic(*Call))
            continue; // preserve the original alloca for annotations
        Instruction *At = usePoint(*U);
        PHINode *Phi = dyn_cast<PHINode>(UserInst);
        std::pair<PHINode *, BasicBlock *> Key{nullptr, nullptr};
        if (Phi) {
          Key = std::make_pair(Phi, Phi->getIncomingBlock(U->getOperandNo()));
          if (LoadInst *Existing = phiReload.lookup(Key)) {
            U->set(Existing);
            continue;
          }
        }
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
          if (Phi)
            phiReload[Key] = Share;
          U->set(Share);
          continue;
        }
        IRBuilder<> B(At);
        auto *Reload = B.CreateLoad(V->getType(), Root, "goc.spill.reload");
        Reload->setVolatile(true);
        placed.push_back(Reload);
        if (Phi)
          phiReload[Key] = Reload;
        U->set(Reload);
      }
    }
    for (Instruction *V : sptr)
      if (rematerializableFrameAddress(V, DL))
        Changed |= rematCrossSafepointUses(V, DL, F) != 0;
    for (Argument &A : F.args())
      if (A.hasByValAttr())
        Changed |= rematCrossSafepointUses(&A, DL, F) != 0;

    // Stack-passed pointer arguments are reported as Constant locations: the
    // byte offset from SP at the call (the extractor converts it to a BP
    // distance with the fixed frame size). They are exact per call, so once a
    // function has any, *every* call gets its own record: a call without one
    // would inherit the previous call's map and mark whatever scalar now sits
    // in that outgoing word.
    DenseMap<CallBase *, SmallVector<int64_t, 4>> outgoing;
    bool anyOutgoing = false;
    for (CallBase *CB : sites) {
      if (CB->isInlineAsm())
        continue;
      SmallVector<int64_t, 4> offsets;
      if (!outgoingPointerWords(*CB, DL, offsets))
        report_fatal_error(Twine("goc-stackmap: unmodelled SysV argument in a call from ") +
                           F.getName());
      anyOutgoing |= !offsets.empty();
      outgoing[CB] = std::move(offsets);
    }

    for (CallBase *CB : sites) {
      auto It = liveAt.find(CB);
      bool hasRoots = It != liveAt.end() && !It->second.empty();
      if (!hasRoots && !anyOutgoing)
        continue;
      Changed = true;
      IRBuilder<> B(CB);
      SmallVector<Value *, 18> args;
      args.push_back(ConstantInt::get(B.getInt64Ty(), GocStackMapTag | ++id));
      args.push_back(ConstantInt::get(B.getInt32Ty(), 0)); // no shadow bytes
      if (hasRoots)
        for (Instruction *V : It->second)
          if (AllocaInst *Root = roots.lookup(V))
            args.push_back(Root);
      for (int64_t off : outgoing.lookup(CB))
        args.push_back(ConstantInt::get(B.getInt64Ty(), off));
      B.CreateCall(SM, args);
    }

    // Do not add noinline. Inlining already ran; an inlined body is this
    // function's frame, and elfpack inserts morestack only at TEXT entries
    // that still exist. Stamping noinline here would only block a later
    // pass, and a later inline would invalidate the maps just recorded.
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

void addGocStackMap(FunctionPassManager &FPM) { FPM.addPass(GocStackMap()); }

} // namespace goc::sm
