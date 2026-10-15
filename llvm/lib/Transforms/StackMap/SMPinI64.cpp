//===- SMPinI64.cpp - the goc-pin-i64 pass ---------------------------===//

#include "StackMapInternal.h"

using namespace llvm;

namespace goc::sm {

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
    for (Instruction &DefI : instructions(F)) { // program order (see above)
      if (!pin.count(&DefI))
        continue;
      Instruction *Def = &DefI;
      if (auto *LI = dyn_cast<LoadInst>(Def)) {
        auto *AI = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
        if (goc::rootKind(AI) == "pin")
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
      goc::markRoot(Slot, "pin");
      Slot->setAlignment(Align(8));
      IRBuilder<> Zero(Slot->getNextNode());
      auto *Z = Zero.CreateStore(Constant::getNullValue(fix.Def->getType()), Slot);
      // Non-volatile: these are integer fragments, not stack pointers. A
      // volatile reload would force a memory round-trip at every call.
      // This pass is not in the default pipeline.

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

void addGocPinI64(FunctionPassManager &FPM) { FPM.addPass(GocPinI64()); }

} // namespace goc::sm
