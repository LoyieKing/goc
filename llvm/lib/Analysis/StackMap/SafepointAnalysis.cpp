//===- SafepointAnalysis.cpp - stack maps: safepoints and stack addresses ---===//
//
// Read-only queries the stack-map passes share: which calls are safepoints,
// which values are stack addresses or frame addresses, the uses of a value
// that lie across a safepoint, and pointer words passed on the stack.
//===----------------------------------------------------------------------===//

#include "Goc/Internal/StackMap.h"

using namespace llvm;

namespace goc::sm {


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
  // arm64: stack arguments sit 16 above SP (goc-llc AArch64Frame) and are
  // not described here yet. TODO: a stack pointer passed on the stack and
  // reloaded by the callee after the stack moved is not adjusted.
  if (gocArchArm64())
    return true;
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

bool isSafepoint(const Instruction &I) {
  auto *CB = dyn_cast<CallBase>(&I);
  if (!CB || CB->isDebugOrPseudoInst())
    return false;
  // Inline asm does not call morestack. The frame-address remat is itself
  // inline asm; treating it as a safepoint would spill across the remat.
  if (CB->isInlineAsm())
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

bool rematerializableFrameAddress(Value *V, const DataLayout &DL) {
  if (pureFrameAddress(V, DL))
    return true;
  Value *Cur = V;
  while (Cur) {
    if (auto *A = dyn_cast<Argument>(Cur))
      return A->hasByValAttr();
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
      return false;
    APInt Acc(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
    if (!GEP->accumulateConstantOffset(DL, Acc))
      return false;
    Cur = GEP->getPointerOperand();
  }
  return false;
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
          if (!CB->getType()->isPointerTy() || CB->isInlineAsm())
            continue;
          if (CB->getIntrinsicID() == Intrinsic::frameaddress) {
            stack.insert(CB);
            Grew = true;
            continue;
          }
          // A pointer returned by a call that was handed a frame address may
          // point back into this frame: p = pick(&slot), strchr(buf, c). This
          // pass is not interprocedural, so treat the result as frame-derived;
          // otherwise it rides a callee-saved register or spill across a
          // later stack copy and is stale afterwards (test/e2e/retptr).
          for (Value *Arg : CB->args())
            if (Arg->getType()->isPointerTy() && stack.count(Arg)) {
              stack.insert(CB);
              Grew = true;
              break;
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
} // namespace goc::sm
