//===- SMLiveness.cpp - stack maps: safepoints, liveness, remat -------===//

#include "StackMapInternal.h"

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

} // namespace goc::sm
