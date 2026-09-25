// GocStackMap: IR pass that records sptr-colored values at every call site via
// llvm.experimental.stackmap.
//
// Why: a goroutine stack growth copies frames, and the runtime adjusts the
// slots its stack map marks. goc's coloring marks *values* (!goc.color), not
// slots, so frame roots come from existing pointer allocas and the dedicated
// pointer-typed root allocas created here for SSA values live across calls.
// Make live sptr values reside in pointer-typed frame slots and reload them
// after possible stack growth. The stackmap operands are the *addresses of
// these root slots*, never the raw SSA pointer values: LLVM Direct locations
// then identify precisely which frame words hold pointers. Ordinary Register
// locations are not enough; Go's stack copier cannot adjust a value kept only
// in a C register. The post-PEI MIR also reports these named slots.
//
// Do not report dead SSA values: a reused spill slot may hold a scalar when
// the runtime stops at a later call, and Go would mis-adjust it as a pointer.
//
// The driver runs O3, including the inliner, before this pass. Maps are
// recorded on the merged frame, so an inlined call-bearing callee does not
// need its old offsets retargeted: its allocas and calls are the caller's.
// An inlined copy has no TEXT entry; elfpack emits one morestack prologue
// for the surviving caller. Nothing after this pass may inline — a later
// caller cannot retarget frame-word offsets or stackmap IDs already emitted.
// goc-reanchor then spills any alloca-derived address this pass did not
// already root, including a pointer argument that points at a caller's frame.
// O3 keeps that argument in a callee-saved register. The SysV morestack stub
// restores those registers verbatim — they also hold small integers, so the
// stub cannot mark them (0 < p < 4096 is "bad pointer in frame"). The reload
// from an adjusted slot is the copy the copier can see.
//
// O3 also drops !goc.color on instructions it rewrites. A pointer value is
// still a stack address when it is derived from a static alloca (or loaded
// from a slot seeded by one). Those values are roots even without a color tag.
// A heap pointer that merely lives in a stack slot is not a root. Go's
// adjustpointer rewrites a marked slot only when the value is inside the old
// stack, so a heap pointer in a marked slot would be left alone; it is still
// not marked, because a slot that also holds a small integer trips
// "bad pointer in frame". uptr is for an sptr stored where the copier cannot
// see it (the heap), not for a frame slot this map already scans.
//
// SROA splits an i64 JSValue into 32-bit halves and leaves them in SSA across
// calls. With stack-slot sharing, llc can spill a half into a slot later
// reused for g; the join then looks like a pointer and JS_FreeValueRT faults
// (seen in js_regexp_Symbol_replace). goc-pin-i64 exists for that case. The
// default pipeline does not run it: llc gets -no-stack-slot-sharing, so an
// integer spill is never reused, and the pin allocas were a per-call memory
// tax (137 slots in JS_CallInternal). Stack-derived integers stay with
// goc-reanchor so growth can still adjust them.
#include <algorithm>
#include <optional>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

// Defined below. O3 may have dropped !goc.color; this walk does not need it.
DenseSet<Value *> stackAddresses(Function &F, const DataLayout &DL);

// The in-tree Sema pass attaches !goc.color !N to values, where !N is a node
// holding the color name ("sptr" / "cptr" / "uptr").
bool isSptrColored(const Instruction &I) {
  const MDNode *M = I.getMetadata("goc.color");
  if (!M || M->getNumOperands() != 1)
    return false;
  const auto *S = dyn_cast<MDString>(M->getOperand(0));
  return S && S->getString() == "sptr";
}

bool isNonSafepointIntrinsic(const CallBase &CB) {
  const Function *F = CB.getCalledFunction();
  if (!F)
    return false;
  switch (F->getIntrinsicID()) {
  case Intrinsic::experimental_stackmap:
  case Intrinsic::var_annotation:
  case Intrinsic::ptr_annotation:
  case Intrinsic::annotation:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
    return true; // these do not lower to machine CALL instructions
  default:
    return false;
  }
}

// A stack aggregate is often referenced through a local T* initialized with
// &aggregate (JSONStringifyContext *jsc = &jsc_s). LLVM's pointer-base helper
// stops at that load; follow one proven local alias to map the *aggregate's*
// word, not the unrelated pointer-variable slot.
AllocaInst *frameAlloca(Value *Address, int64_t &Offset,
                        const DataLayout &DL) {
  Value *Base = GetPointerBaseWithConstantOffset(Address, Offset, DL);
  if (auto *Slot = dyn_cast_or_null<AllocaInst>(Base))
    return Slot;
  auto *LI = dyn_cast_or_null<LoadInst>(Base);
  auto *PointerSlot = LI ? dyn_cast<AllocaInst>(
      LI->getPointerOperand()->stripPointerCasts()) : nullptr;
  if (!PointerSlot || !PointerSlot->getAllocatedType()->isPointerTy())
    return nullptr;
  AllocaInst *Target = nullptr;
  int64_t TargetOffset = 0;
  for (User *U : PointerSlot->users()) {
    auto *S = dyn_cast<StoreInst>(U);
    if (!S || S->getPointerOperand()->stripPointerCasts() != PointerSlot)
      continue;
    Value *V = S->getValueOperand();
    if (isa<ConstantPointerNull>(V))
      continue; // zero before the alias is initialized
    int64_t Off = 0;
    auto *Object = dyn_cast_or_null<AllocaInst>(
        GetPointerBaseWithConstantOffset(V, Off, DL));
    if (!Object || (Target && (Target != Object || TargetOffset != Off)))
      return nullptr;
    Target = Object;
    TargetOffset = Off;
  }
  if (Target)
    Offset += TargetOffset;
  return Target;
}

// SysV x86-64 placement of a call's arguments, restricted to what Clang emits
// for C after its own ABI lowering. Collects the byte offsets (from SP at the
// call) of stack-passed pointer arguments. A C callee reads them from its
// incoming area, which physically belongs to the caller's frame; if the callee
// grows the goroutine stack in its prologue, only the caller's map at this call
// can tell the runtime that those words hold (possibly stack) pointers.
bool outgoingPointerWords(const CallBase &CB, const DataLayout &DL,
                          SmallVectorImpl<int64_t> &Offsets) {
  unsigned GPR = 0, SSE = 0;
  uint64_t Stack = 0;
  for (unsigned I = 0; I < CB.arg_size(); ++I) {
    Type *T = CB.getArgOperand(I)->getType();
    if (CB.paramHasAttr(I, Attribute::InAlloca) ||
        CB.paramHasAttr(I, Attribute::Preallocated))
      return false;
    if (CB.isByValArgument(I)) {
      uint64_t Size = DL.getTypeAllocSize(CB.getParamByValType(I)).getFixedValue();
      uint64_t A = std::max<uint64_t>(8, CB.getParamAlign(I).valueOrOne().value());
      Stack = alignTo(alignTo(Stack, A) + Size, 8);
      continue;
    }
    if (T->isPointerTy() || (T->isIntegerTy() && T->getIntegerBitWidth() <= 64)) {
      if (GPR < 6) {
        ++GPR;
        continue;
      }
      if (T->isPointerTy())
        Offsets.push_back(int64_t(Stack));
      Stack += 8;
      continue;
    }
    if (T->isIntegerTy(128)) {
      if (GPR <= 4) {
        GPR += 2;
        continue;
      }
      Stack = alignTo(Stack, 16) + 16;
      continue;
    }
    if (T->isFloatTy() || T->isDoubleTy() ||
        (T->isVectorTy() && DL.getTypeAllocSize(T).getFixedValue() <= 16)) {
      if (SSE < 8) {
        ++SSE;
        continue;
      }
      uint64_t Size = DL.getTypeAllocSize(T).getFixedValue();
      Stack = alignTo(alignTo(Stack, Size > 8 ? 16 : 8) + Size, 8);
      continue;
    }
    if (T->isX86_FP80Ty()) {
      Stack = alignTo(Stack, 16) + 16;
      continue;
    }
    return false;
  }
  return true;
}

struct GocStackMap : PassInfoMixin<GocStackMap> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();

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
    bool Changed = false;
    if (!sptr.empty()) {
      Changed = true;
      // Clang may emit numbered (nameless) allocas. MIR drops those names, but
      // the map extractor must identify pointer slots in the post-PEI frame.
      for (Instruction &I : instructions(F))
        if (auto *Slot = dyn_cast<AllocaInst>(&I))
          if (Slot->getAllocatedType()->isPointerTy() && Slot->getName().empty())
            Slot->setName("goc.ptr.slot");

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
      if (Slot->getName().empty())
        Slot->setName("goc.frame.field");
      SmallVector<Metadata *, 8> tag{
          MDString::get(F.getContext(), "goc.frame.words"),
          MDString::get(F.getContext(), Slot->getName())};
      auto &previous = frameWords[Slot];
      for (uint64_t off : Words) {
        tag.push_back(ConstantAsMetadata::get(ConstantInt::get(
            Type::getInt64Ty(F.getContext()), off)));
        if (std::find(previous.begin(), previous.end(), off) == previous.end())
          previous.push_back(off);
      }
      Writer.setMetadata("goc.frame.words", MDNode::get(F.getContext(), tag));
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

    constexpr uint64_t GocStackMapTag = 0x474f430000000000ULL;
    uint64_t id = 0;
    SmallVector<CallBase *, 16> sites;
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || isNonSafepointIntrinsic(*CB) || CB->isDebugOrPseudoInst())
        continue;
      sites.push_back(CB);
    }

    DenseMap<CallBase *, SmallVector<Instruction *, 16>> liveAt;
    for (CallBase *CB : sites) {
      for (Instruction *V : sptr) {
        if (V == CB || !DT.dominates(V, CB))
          continue;
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
      for (CallBase *CB : sites)
        if (auto It = liveAt.find(CB); It != liveAt.end())
          for (Instruction *Live : It->second)
            needRoot |= Live == V;
      if (!needRoot)
        continue;

      IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
      auto *Root = Entry.CreateAlloca(V->getType(), nullptr, "goc.spill.root");
      auto *Init = Entry.CreateStore(Constant::getNullValue(V->getType()), Root);
      Init->setVolatile(true);
      roots[V] = Root;

      // Remember original users before adding the save. Every reloaded use
      // sees the stack-copied root, including uses on a PHI incoming edge.
      SmallVector<Use *, 16> uses;
      for (Use &U : V->uses())
        uses.push_back(&U);
      Instruction *AfterDef = V->getNextNode();
      if (isa<PHINode>(V))
        AfterDef = V->getParent()->getFirstNonPHI();
      if (!AfterDef)
        report_fatal_error("goc-stackmap: unsupported terminator sptr value");
      IRBuilder<> SaveBuilder(AfterDef);
      auto *Save = SaveBuilder.CreateStore(V, Root);
      Save->setVolatile(true);

      // A switch or duplicated CFG edge can list one predecessor twice. Those
      // incoming values must be the same instruction; two loads are not.
      DenseMap<std::pair<PHINode *, BasicBlock *>, LoadInst *> phiReload;
      for (Use *U : uses) {
        auto *UserInst = dyn_cast<Instruction>(U->getUser());
        if (!UserInst)
          report_fatal_error("goc-stackmap: non-instruction sptr user");
        if (auto *Call = dyn_cast<CallBase>(UserInst))
          if (isNonSafepointIntrinsic(*Call))
            continue; // preserve the original alloca for annotations
        if (auto *Phi = dyn_cast<PHINode>(UserInst)) {
          BasicBlock *Inc = Phi->getIncomingBlock(U->getOperandNo());
          auto Key = std::make_pair(Phi, Inc);
          LoadInst *Reload = phiReload.lookup(Key);
          if (!Reload) {
            IRBuilder<> B(Inc->getTerminator());
            Reload = B.CreateLoad(V->getType(), Root, "goc.spill.reload");
            Reload->setVolatile(true);
            phiReload[Key] = Reload;
          }
          U->set(Reload);
          continue;
        }
        IRBuilder<> B(UserInst);
        auto *Reload = B.CreateLoad(V->getType(), Root, "goc.spill.reload");
        Reload->setVolatile(true);
        U->set(Reload);
      }
    }

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
          args.push_back(roots.lookup(V));
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

bool isSafepoint(const Instruction &I) {
  auto *CB = dyn_cast<CallBase>(&I);
  if (!CB || CB->isDebugOrPseudoInst())
    return false;
  if (const Function *Callee = CB->getCalledFunction()) {
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::experimental_stackmap:
    case Intrinsic::var_annotation:
    case Intrinsic::ptr_annotation:
    case Intrinsic::annotation:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::donothing:
    case Intrinsic::assume:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_label:
    case Intrinsic::vastart:
    case Intrinsic::vaend:
    case Intrinsic::vacopy:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
      return false;
    default:
      break; // memcpy/memset/memmove may become calls after this pass
    }
  }
  return true;
}

bool ignoreUse(const Instruction *User) {
  auto *CB = dyn_cast<CallBase>(User);
  return CB && isNonSafepointIntrinsic(*CB);
}

// True when a stack growth can occur after Def and before UsePoint.
// Cross-block is conservative: a later reload from an adjusted slot is
// equivalent when no growth happened, and required when it did.
bool separatedBySafepoint(const Instruction *Def, const Instruction *UsePoint) {
  if (Def->getParent() != UsePoint->getParent())
    return true;
  if (!Def->comesBefore(UsePoint))
    return true;
  for (const Instruction *I = Def->getNextNode(); I && I != UsePoint;
       I = I->getNextNode())
    if (isSafepoint(*I))
      return true;
  return false;
}

Instruction *usePoint(Use &U) {
  auto *User = cast<Instruction>(U.getUser());
  if (auto *Phi = dyn_cast<PHINode>(User))
    return Phi->getIncomingBlock(U.getOperandNo())->getTerminator();
  return User;
}

std::optional<std::pair<AllocaInst *, int64_t>>
pureFrameAddress(Value *V, const DataLayout &DL) {
  int64_t Off = 0;
  while (V) {
    if (auto *AI = dyn_cast<AllocaInst>(V)) {
      if (!AI->isStaticAlloca())
        return std::nullopt;
      return std::make_pair(AI, Off);
    }
    if (auto *BC = dyn_cast<BitCastInst>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V)) {
      V = ASC->getOperand(0);
      continue;
    }
    auto *GEP = dyn_cast<GetElementPtrInst>(V);
    if (!GEP)
      return std::nullopt;
    APInt Acc(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
    if (!GEP->accumulateConstantOffset(DL, Acc))
      return std::nullopt;
    Off += Acc.getSExtValue();
    V = GEP->getPointerOperand();
  }
  return std::nullopt;
}

// Stack addresses O3 may cache in SSA: static allocas, GEPs of them, and
// loads of frame slots that hold those addresses (JSValue *sp, JSStackFrame
// *sf). A slot is seeded by a store of a known frame address, or by copying
// such a load into another slot. Heap pointers that merely increment
// themselves (bytecode pc) are not seeded and are left alone.
//
// A pointer argument is a stack address when a caller passes an alloca (or
// another such argument). next_token(s) keeps s in RBX across free_token;
// free_token's split stub saves RBX and restores the pre-copy address.
// Computed once per module: a per-function caller walk re-traverses QuickJS
// for every function and does not finish.
bool operandIsKnownStack(const Value *V, const DataLayout &DL,
                         const DenseSet<const Argument *> &Known, unsigned Depth) {
  if (!V || Depth > 4 || !V->getType()->isPointerTy())
    return false;
  if (isa<ConstantPointerNull>(V) || isa<UndefValue>(V))
    return false;
  if (const auto *Phi = dyn_cast<PHINode>(V)) {
    for (const Value *Inc : Phi->incoming_values()) {
      if (isa<ConstantPointerNull>(Inc) || isa<UndefValue>(Inc))
        continue;
      if (operandIsKnownStack(Inc, DL, Known, Depth + 1))
        return true;
    }
    return false;
  }
  if (const auto *Sel = dyn_cast<SelectInst>(V))
    return operandIsKnownStack(Sel->getTrueValue(), DL, Known, Depth + 1) ||
           operandIsKnownStack(Sel->getFalseValue(), DL, Known, Depth + 1);
  int64_t Off = 0;
  const Value *Base = GetPointerBaseWithConstantOffset(V, Off, DL);
  if (!Base)
    return false;
  if (isa<AllocaInst>(Base))
    return true;
  if (const auto *A = dyn_cast<Argument>(Base))
    return Known.count(A);
  return false;
}

const DenseSet<const Argument *> &stackPointerArguments(const Module &M,
                                                        const DataLayout &DL) {
  static const Module *CachedMod = nullptr;
  static DenseSet<const Argument *> Cached;
  if (CachedMod == &M)
    return Cached;
  Cached.clear();
  CachedMod = &M;
  SmallVector<const Argument *, 64> work;
  auto consider = [&](const CallBase &CB) {
    const Function *Callee = CB.getCalledFunction();
    if (!Callee)
      return;
    unsigned N = std::min(CB.arg_size(), unsigned(Callee->arg_size()));
    for (unsigned i = 0; i < N; ++i) {
      const Argument *A = Callee->getArg(i);
      if (!A->getType()->isPointerTy() || Cached.count(A))
        continue;
      if (!operandIsKnownStack(CB.getArgOperand(i), DL, Cached, 0))
        continue;
      Cached.insert(A);
      work.push_back(A);
    }
  };
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const Instruction &I : instructions(F))
      if (const auto *CB = dyn_cast<CallBase>(&I))
        consider(*CB);
  }
  while (!work.empty()) {
    const Argument *A = work.pop_back_val();
    const Function *F = A->getParent();
    if (F->isDeclaration())
      continue;
    for (const Instruction &I : instructions(*F))
      if (const auto *CB = dyn_cast<CallBase>(&I))
        consider(*CB);
  }
  return Cached;
}

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

struct GocReanchor : PassInfoMixin<GocReanchor> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
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
      if (Slot->getName().empty())
        Slot->setName("goc.ptr.slot");
      SmallVector<Metadata *, 4> tag{
          MDString::get(F.getContext(), "goc.frame.words"),
          MDString::get(F.getContext(), Slot->getName()),
          ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt64Ty(F.getContext()), uint64_t(SOff)))};
      S->setMetadata("goc.frame.words", MDNode::get(F.getContext(), tag));
    }

    struct Fix {
      Instruction *Def;
      SmallVector<Use *, 8> Uses; // uses that cross a safepoint
    };
    SmallVector<Fix, 16> fixes;
    for (Value *V : stack) {
      auto *Def = dyn_cast<Instruction>(V);
      if (!Def)
        continue;
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
      if (!stack.count(&A))
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

    unsigned anchors = 0, reloads = 0, remats = 0;
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    for (Fix &fix : fixes) {
      if (isa<InvokeInst>(fix.Def))
        report_fatal_error("goc-reanchor: invoke result holds a stack address");
      // Always spill across the safepoint. A fresh GEP at the use is not
      // enough: MachineCSE folds it back into the pre-call LEA, and that
      // register is not a stackmap root. A volatile reload cannot be folded,
      // and the copier adjusts the anchor slot.
      AllocaInst *Anchor = Entry.CreateAlloca(fix.Def->getType(), nullptr,
                                              "goc.anchor");
      Anchor->setAlignment(Align(8));
      IRBuilder<> Zero(Anchor->getNextNode());
      auto *Z = Zero.CreateStore(Constant::getNullValue(fix.Def->getType()), Anchor);
      Z->setVolatile(true);
      SmallVector<Metadata *, 4> tag{
          MDString::get(F.getContext(), "goc.frame.words"),
          MDString::get(F.getContext(), Anchor->getName()),
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(F.getContext()), 0))};
      MDNode *Node = MDNode::get(F.getContext(), tag);
      Anchor->setMetadata("goc.frame.words", Node);
      Z->setMetadata("goc.frame.words", Node);

      Instruction *After = isa<PHINode>(fix.Def) ? fix.Def->getParent()->getFirstNonPHI()
                                                : fix.Def->getNextNode();
      if (!After)
        report_fatal_error("goc-reanchor: stack address defined by a terminator");
      IRBuilder<> Save(After);
      auto *St = Save.CreateStore(fix.Def, Anchor);
      St->setVolatile(true);
      ++anchors;

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
    }
    Instruction *IP = &*F.getEntryBlock().getFirstInsertionPt();
    for (ArgFix &fix : argFixes) {
      IRBuilder<> B(IP);
      auto *Anchor = B.CreateAlloca(fix.Arg->getType(), nullptr, "goc.arganchor");
      Anchor->setAlignment(Align(8));
      auto *Z = B.CreateStore(
          ConstantPointerNull::get(cast<PointerType>(fix.Arg->getType())), Anchor);
      Z->setVolatile(true);
      auto *St = B.CreateStore(fix.Arg, Anchor);
      St->setVolatile(true);
      SmallVector<Metadata *, 4> tag{
          MDString::get(F.getContext(), "goc.frame.words"),
          MDString::get(F.getContext(), Anchor->getName()),
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(F.getContext()), 0))};
      MDNode *Node = MDNode::get(F.getContext(), tag);
      Anchor->setMetadata("goc.frame.words", Node);
      Z->setMetadata("goc.frame.words", Node);
      St->setMetadata("goc.frame.words", Node);
      ++anchors;

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
        IRBuilder<> ReloadB(At);
        auto *Reload = ReloadB.CreateLoad(fix.Arg->getType(), Anchor, "goc.reanchor");
        Reload->setVolatile(true);
        placed.push_back(Reload);
        U->set(Reload);
        ++reloads;
      }
    }
    if (anchors || remats)
      errs() << "goc-reanchor: " << F.getName() << " anchors=" << anchors
             << " reloads=" << reloads << " remats=" << remats << "\n";
    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

bool isShift32(const Instruction &I, unsigned Opcode) {
  const auto *BO = dyn_cast<BinaryOperator>(&I);
  if (!BO || BO->getOpcode() != Opcode)
    return false;
  const auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
  return C && C->getValue() == 32;
}

bool isHalfMask(const Value *V) {
  const auto *C = dyn_cast<ConstantInt>(V);
  if (!C || C->getBitWidth() != 64)
    return false;
  const uint64_t M = C->getZExtValue();
  return M == 0xffffffffULL || M == 0xffffffff00000000ULL;
}

// Integers that are (or carry) a stack address. Pinning them verbatim would
// freeze a frame pointer across growth. Mixed phis are excluded at the use
// site so one stack incoming cannot poison a heap-pointer edge into a pin.
DenseSet<Value *> stackInts(Function &F, const DenseSet<Value *> &stackPtrs) {
  DenseSet<Value *> ints;
  bool Grew = true;
  while (Grew) {
    Grew = false;
    for (Instruction &I : instructions(F)) {
      if (!I.getType()->isIntegerTy() || ints.count(&I))
        continue;
      bool Mark = false;
      if (const auto *P = dyn_cast<PtrToIntInst>(&I)) {
        Mark = stackPtrs.count(P->getOperand(0));
      } else if (const auto *Phi = dyn_cast<PHINode>(&I)) {
        bool Any = false, All = true;
        for (Value *Inc : Phi->incoming_values()) {
          if (isa<Constant>(Inc) || isa<UndefValue>(Inc))
            continue;
          if (ints.count(Inc))
            Any = true;
          else
            All = false;
        }
        Mark = Any && All;
      } else if (const auto *Sel = dyn_cast<SelectInst>(&I)) {
        const Value *T = Sel->getTrueValue(), *Fa = Sel->getFalseValue();
        auto known = [&](const Value *V) {
          return isa<Constant>(V) || isa<UndefValue>(V) || ints.count(V);
        };
        Mark = known(T) && known(Fa) && (ints.count(T) || ints.count(Fa));
      } else if (const auto *BO = dyn_cast<BinaryOperator>(&I)) {
        Value *A = BO->getOperand(0), *B = BO->getOperand(1);
        Mark = (ints.count(A) && (ints.count(B) || isa<Constant>(B))) ||
               (ints.count(B) && isa<Constant>(A));
      } else if (const auto *Cast = dyn_cast<CastInst>(&I)) {
        Mark = ints.count(Cast->getOperand(0));
      }
      if (Mark && ints.insert(&I).second)
        Grew = true;
    }
  }
  return ints;
}

bool integerResult(const Instruction &I) {
  return I.getType()->isIntegerTy() && I.getType()->getIntegerBitWidth() <= 64;
}

bool isPinSeed(const Instruction &I) {
  if (!integerResult(I))
    return false;
  if (const auto *CB = dyn_cast<CallBase>(&I))
    return CB->getIntrinsicID() == Intrinsic::not_intrinsic;
  if (isa<ExtractValueInst>(I))
    return true;
  if (isShift32(I, Instruction::LShr) || isShift32(I, Instruction::AShr) ||
      isShift32(I, Instruction::Shl))
    return true;
  if (const auto *BO = dyn_cast<BinaryOperator>(&I))
    if (BO->getOpcode() == Instruction::And &&
        (isHalfMask(BO->getOperand(0)) || isHalfMask(BO->getOperand(1))))
      return true;
  if (const auto *T = dyn_cast<TruncInst>(&I))
    return T->getSrcTy()->isIntegerTy(64);
  return false;
}

bool growsFromPin(const Instruction &I, const DenseSet<Value *> &known) {
  if (!integerResult(I) || known.count(&I))
    return false;
  if (isa<ZExtInst>(I) || isa<SExtInst>(I) || isa<TruncInst>(I) ||
      isa<FreezeInst>(I))
    return known.count(I.getOperand(0));
  if (const auto *Sel = dyn_cast<SelectInst>(&I))
    return known.count(Sel->getTrueValue()) || known.count(Sel->getFalseValue());
  if (const auto *Phi = dyn_cast<PHINode>(&I)) {
    for (Value *Inc : Phi->incoming_values())
      if (known.count(Inc))
        return true;
    return false;
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(&I)) {
    switch (BO->getOpcode()) {
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      return known.count(BO->getOperand(0)) || known.count(BO->getOperand(1));
    default:
      return false;
    }
  }
  return false;
}

bool freezesStackAddress(const Instruction &I, const DenseSet<Value *> &stackInts,
                         const DenseSet<Value *> &stackPtrs) {
  if (stackInts.count(&I))
    return true;
  for (const Use &Op : I.operands())
    if (stackInts.count(Op.get()) || stackPtrs.count(Op.get()))
      return true;
  return false;
}

// A reload inserted before the instruction that produces the value would read
// the slot before the store. Invoke results used as phi incomings have the
// invoke itself as their use point; that edge does not cross a later safepoint.
bool useAfter(const Instruction *Def, const Instruction *UsePt) {
  if (Def == UsePt)
    return false;
  if (Def->getParent() == UsePt->getParent())
    return Def->comesBefore(UsePt);
  return true;
}

Instruction *pinStorePoint(Instruction *Def) {
  if (auto *II = dyn_cast<InvokeInst>(Def))
    return &*II->getNormalDest()->getFirstInsertionPt();
  if (isa<PHINode>(Def))
    return Def->getParent()->getFirstNonPHI();
  return Def->getNextNode();
}

struct GocPinI64 : PassInfoMixin<GocPinI64> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
    bool AnySafe = false;
    for (Instruction &I : instructions(F))
      if (isSafepoint(I)) {
        AnySafe = true;
        break;
      }
    if (!AnySafe)
      return PreservedAnalyses::all();

    const DataLayout &DL = F.getParent()->getDataLayout();
    DenseSet<Value *> stackPtrs = stackAddresses(F, DL);
    DenseSet<Value *> ints = stackInts(F, stackPtrs);

    DenseSet<Value *> pin;
    bool Grew = true;
    while (Grew) {
      Grew = false;
      for (Instruction &I : instructions(F)) {
        if (pin.count(&I) || freezesStackAddress(I, ints, stackPtrs))
          continue;
        if (isPinSeed(I) || growsFromPin(I, pin))
          if (pin.insert(&I).second)
            Grew = true;
      }
    }
    if (pin.empty())
      return PreservedAnalyses::all();

    struct Fix {
      Instruction *Def;
      SmallVector<Use *, 8> Uses;
    };
    SmallVector<Fix, 16> fixes;
    for (Value *V : pin) {
      auto *Def = cast<Instruction>(V);
      if (auto *LI = dyn_cast<LoadInst>(Def)) {
        auto *AI = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
        if (AI && AI->getName().starts_with("goc.pin"))
          continue;
      }
      Fix fix{Def, {}};
      for (Use &U : Def->uses()) {
        auto *User = dyn_cast<Instruction>(U.getUser());
        if (!User || ignoreUse(User))
          continue;
        Instruction *At = usePoint(U);
        if (useAfter(Def, At) && separatedBySafepoint(Def, At))
          fix.Uses.push_back(&U);
      }
      if (!fix.Uses.empty())
        fixes.push_back(std::move(fix));
    }
    if (fixes.empty())
      return PreservedAnalyses::all();

    unsigned slots = 0, reloads = 0;
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    DenseMap<std::pair<Instruction *, Instruction *>, Value *> reused;
    for (Fix &fix : fixes) {
      AllocaInst *Slot = Entry.CreateAlloca(fix.Def->getType(), nullptr, "goc.pin");
      Slot->setAlignment(Align(8));
      IRBuilder<> Zero(Slot->getNextNode());
      auto *Z = Zero.CreateStore(Constant::getNullValue(fix.Def->getType()), Slot);
      // Non-volatile: these are integer fragments, not stack pointers. A
      // volatile reload would force a memory round-trip at every call.
      // -no-stack-slot-sharing keeps the slot from being reused for g if
      // the value is spilled; the register itself does not move.

      Instruction *After = pinStorePoint(fix.Def);
      if (!After || isa<PHINode>(After))
        report_fatal_error("goc-pin-i64: no insertion point after definition");
      IRBuilder<> Save(After);
      auto *St = Save.CreateStore(fix.Def, Slot);
      (void)St;
      ++slots;

      for (Use *U : fix.Uses) {
        Instruction *At = usePoint(*U);
        auto Key = std::make_pair(fix.Def, At);
        if (Value *Existing = reused.lookup(Key)) {
          U->set(Existing);
          continue;
        }
        IRBuilder<> B(At);
        auto *Reload = B.CreateLoad(fix.Def->getType(), Slot, "goc.pin.reload");
        reused[Key] = Reload;
        U->set(Reload);
        ++reloads;
      }
    }
    errs() << "goc-pin-i64: " << F.getName() << " slots=" << slots
           << " reloads=" << reloads << "\n";
    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

// Runs before O3. Call-bearing functions may inline. Maps are recorded on the
// merged frame afterwards. A stack-pointer argument kept in a callee-saved
// register, and a local that stores &s->token, are reanchored / tagged so
// copystack adjusts them. Source noinline is left untouched.
struct GocInlineGate : PassInfoMixin<GocInlineGate> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    (void)F;
    return PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "GocStackMap", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "goc-stackmap") {
                    FPM.addPass(GocStackMap());
                    return true;
                  }
                  if (Name == "goc-reanchor") {
                    FPM.addPass(GocReanchor());
                    return true;
                  }
                  if (Name == "goc-pin-i64") {
                    FPM.addPass(GocPinI64());
                    return true;
                  }
                  if (Name == "goc-inline-gate") {
                    FPM.addPass(GocInlineGate());
                    return true;
                  }
                  return false;
                });
          }};
}
