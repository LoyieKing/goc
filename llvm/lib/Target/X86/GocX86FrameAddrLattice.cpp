//===- GocX86FrameAddrLattice.cpp - frame-address fix: transfer functions -===//
//
// Forward dataflow over machine instructions for GocFrameAddrFix: what each
// register unit / tracked spill slot holds (see the Kind lattice in the header).
//===----------------------------------------------------------------------===//
#include "GocX86FrameAddrFix.h"

namespace goc::fa {

int GocFrameAddrFix::spillSlotOf(const MachineInstr &MI, uint64_t *Size) const {
  for (const MachineMemOperand *MMO : MI.memoperands()) {
    int Sl = -1;
    if (const PseudoSourceValue *PSV = MMO->getPseudoValue()) {
      if (auto *FS = dyn_cast<FixedStackPseudoSourceValue>(PSV)) {
        auto It = SlotIdx.find(FS->getFrameIndex());
        if (It != SlotIdx.end())
          Sl = (int)It->second;
      }
    } else if (const Value *V = MMO->getValue()) {
      if (auto *AI = dyn_cast<AllocaInst>(V->stripPointerCasts())) {
        auto It = AllocaSlot.find(AI);
        if (It != AllocaSlot.end())
          Sl = (int)It->second;
      }
    }
    if (Sl < 0)
      continue;
    if (Size)
      *Size = MMO->getSize().hasValue() ? MMO->getSize().getValue() : 0;
    return Sl;
  }
  return -1;
}

bool GocFrameAddrFix::loadsGocRoot(const MachineInstr &MI) {
  for (const MachineMemOperand *MMO : MI.memoperands())
    if (const Value *V = MMO->getValue())
      if (goc::isCopiedRoot(V))
        return true;
  return false;
}

Val GocFrameAddrFix::readReg(const State &S, unsigned R) const {
  if (!R || R == X86::RIP || R == X86::EFLAGS)
    return mk(NotFA);
  if (isFrameBase(R))
    return fa(R, 0);
  Val V;
  bool First = true;
  for (MCRegUnit U : TRI->regunits(R)) {
    V = First ? S.Units[U] : join(V, S.Units[U]);
    First = false;
  }
  if (First || V.K == Bot)
    return mk(NotFA);
  return V;
}

void GocFrameAddrFix::writeReg(State &S, unsigned R, const Val &V) const {
  if (!R || isFrameBase(R))
    return;
  for (MCRegUnit U : TRI->regunits(R))
    S.Units[U] = V;
}

void GocFrameAddrFix::invalidateBase(State &S, unsigned B, const MachineInstr &MI) const {
  // RSP/RBP itself changed (dynamic alloca, epilogue): b+c can no longer be
  // re-derived from b, but the value still moves with the stack.
  for (Val &V : S.Units)
    if (V.K == FA && V.Base == B)
      V = mk(D1);
  for (Val &V : S.Slots)
    if (V.K == FA && V.Base == B)
      V = mk(D1);
}

void GocFrameAddrFix::addrOps(const MachineInstr &MI, int &BaseOp, int &IndexOp) const {
  BaseOp = IndexOp = -1;
  const MCInstrDesc &D = MI.getDesc();
  int M = X86II::getMemoryOperandNo(D.TSFlags);
  if (M < 0)
    return;
  M += X86II::getOperandBias(D);
  BaseOp = M + X86::AddrBaseReg;
  IndexOp = M + X86::AddrIndexReg;
}

bool GocFrameAddrFix::anyFrameValueOperand(const State &S, const MachineInstr &MI) const {
  int BaseOp, IndexOp;
  addrOps(MI, BaseOp, IndexOp);
  for (unsigned i = 0, e = MI.getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI.getOperand(i);
    if (!MO.isReg() || !MO.isUse() || MO.isUndef() || !MO.getReg())
      continue;
    if ((int)i == BaseOp || (int)i == IndexOp)
      continue;
    if (MO.isImplicit() && isFrameBase(MO.getReg()))
      continue; // push/pop adjust RSP; they do not copy its value
    if (readReg(S, MO.getReg()).frameish())
      return true;
  }
  return false;
}

void GocFrameAddrFix::step(State &S, const MachineInstr &MI) const {
  if (MI.isDebugInstr() || MI.isCFIInstruction() || MI.isLabel())
    return;
  unsigned Op = MI.getOpcode();
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isDef() && MO.getReg().isPhysical()) {
      unsigned R = MO.getReg();
      if (TRI->regsOverlap(R, X86::RSP))
        invalidateBase(S, X86::RSP, MI);
      if (TRI->regsOverlap(R, X86::RBP))
        invalidateBase(S, X86::RBP, MI);
    }

  Val V = mk(NotFA);
  bool Exact = true;
  if (Op == TargetOpcode::IMPLICIT_DEF) {
    V = mk(Bot);
  } else if (Op == X86::LEA64r) {
    unsigned B = MI.getOperand(1 + X86::AddrBaseReg).getReg();
    int64_t Sc = MI.getOperand(1 + X86::AddrScaleAmt).getImm();
    unsigned I = MI.getOperand(1 + X86::AddrIndexReg).getReg();
    unsigned Sg = MI.getOperand(1 + X86::AddrSegmentReg).getReg();
    const MachineOperand &Disp = MI.getOperand(1 + X86::AddrDisp);
    Val VB = B ? readReg(S, B) : mk(NotFA);
    Val VI = I ? readReg(S, I) : mk(NotFA);
    if (!VB.frameish() && !VI.frameish())
      V = mk(NotFA);
    else if (Sg || !Disp.isImm())
      V = taint(&MI);
    else if (VB.K == FA && !I)
      V = fa(VB.Base, VB.Off + Disp.getImm());
    else if (VB.moves() && !VI.frameish())
      V = mk(D1); // &a[i]
    else if (!VB.frameish() && VI.moves() && Sc == 1)
      V = mk(D1);
    else if (VB.K == MS && !VI.frameish())
      V = mk(MS);
    else if (!VB.frameish() && VI.K == MS && Sc == 1)
      V = mk(MS);
    else
      V = taint(&MI);
  } else if (Op == X86::MOV64rr) {
    unsigned Src = MI.getOperand(1).getReg();
    V = readReg(S, Src);
    // Group the source with this copy so a later spill of either one is
    // reloaded into both. The id is the instruction address, stable across
    // dataflow iterations.
    if (V.CopyId == 0 && Src && Src != MI.getOperand(0).getReg()) {
      V.CopyId = reinterpret_cast<uintptr_t>(&MI);
      writeReg(S, Src, V);
    }
  } else if ((Op == X86::ADD64ri32 || Op == X86::ADD64ri8 ||
              Op == X86::SUB64ri32 || Op == X86::SUB64ri8) &&
             MI.getOperand(2).isImm()) {
    Val VS = readReg(S, MI.getOperand(1).getReg());
    int64_t C = MI.getOperand(2).getImm();
    if (Op == X86::SUB64ri32 || Op == X86::SUB64ri8)
      C = -C;
    if (VS.K == FA)
      V = fa(VS.Base, VS.Off + C);
    else
      V = VS.frameish() ? VS : mk(NotFA);
    if (C != 0) {
      V.Alias = -1;
      V.CopyId = 0;
    }
  } else if (Op == X86::INC64r || Op == X86::DEC64r) {
    Val VS = readReg(S, MI.getOperand(1).getReg());
    if (VS.K == FA)
      V = fa(VS.Base, VS.Off + (Op == X86::INC64r ? 1 : -1));
    else
      V = VS.frameish() ? VS : mk(NotFA);
    V.Alias = -1;
    V.CopyId = 0;
  } else if (Op == X86::ADD64rr || Op == X86::ADD64rm) {
    Val A = readReg(S, MI.getOperand(1).getReg());
    Val B = Op == X86::ADD64rr ? readReg(S, MI.getOperand(2).getReg()) : mk(NotFA);
    if (Op == X86::ADD64rm) {
      int Sl = spillSlotOf(MI);
      if (Sl >= 0 && S.Slots[Sl].frameish())
        B = S.Slots[Sl];
    }
    if (!A.frameish() && !B.frameish())
      V = mk(NotFA);
    else if ((A.moves() && !B.frameish()) || (!A.frameish() && B.moves()))
      V = mk(D1); // pointer + integer
    else if ((A.K == MS && !B.frameish()) || (!A.frameish() && B.K == MS))
      V = mk(MS);
    else
      V = taint(&MI);
  } else if (Op == X86::CMOV64rr || Op == X86::CMOV64rm) {
    // dst = cond ? src : dst -- one of the two values.
    Val A = readReg(S, MI.getOperand(1).getReg());
    Val B = mk(NotFA);
    if (Op == X86::CMOV64rr) {
      B = readReg(S, MI.getOperand(2).getReg());
    } else {
      int Sl = spillSlotOf(MI);
      if (Sl >= 0 && S.Slots[Sl].K != Bot)
        B = S.Slots[Sl];
    }
    V = join(A, B);
    if (V.K == Taint && !V.Org)
      V.Org = &MI;
  } else if (Op == X86::SUB64rr || Op == X86::SUB64rm) {
    Val A = readReg(S, MI.getOperand(1).getReg());
    Val B = mk(NotFA);
    bool BStackPtr = false;
    if (Op == X86::SUB64rr) {
      B = readReg(S, MI.getOperand(2).getReg());
    } else {
      int Sl = spillSlotOf(MI);
      if (Sl >= 0)
        B = S.Slots[Sl];
      BStackPtr = loadsGocRoot(MI);
    }
    if (!A.frameish() && !B.frameish())
      V = mk(NotFA);
    else if (A.repairable() && (B.repairable() || BStackPtr))
      V = mk(NotFA); // difference of two (maybe-)stack addresses: invariant
    else
      V = taint(&MI); // p - x: pointer or pointer difference?
  } else {
    Exact = false;
    // Full-width reload of a tracked slot. isLoadFromStackSlotPostFE misses
    // volatile reloads whose memoperand still names the IR alloca.
    uint64_t LdSize = 0;
    int Sl = spillSlotOf(MI, &LdSize);
    if (Op == X86::MOV64rm && Sl >= 0 && LdSize == 8 &&
        MFI->getObjectSize(SlotFI[Sl]) == 8 && MI.getOperand(0).isReg() &&
        MI.getOperand(0).getReg()) {
      V = S.Slots[Sl];
      if (V.K == Bot)
        V = mk(NotFA);
      // This register is now the slot's value, including after a stack
      // copy rewrites the slot.
      V.Alias = Sl;
      if (V.CopyId == 0)
        V.CopyId = reinterpret_cast<uintptr_t>(&MI);
      Exact = true;
    }
  }
  if (!Exact) {
    bool T = anyFrameValueOperand(S, MI);
    if (MI.mayLoad()) {
      int Sl = spillSlotOf(MI);
      if (Sl >= 0 && S.Slots[Sl].frameish())
        T = true;
    }
    V = T ? taint(&MI) : mk(NotFA);
  }
  auto forgetAlias = [&](int Sl) {
    for (Val &U : S.Units)
      if (U.Alias == Sl)
        U.Alias = -1;
    for (Val &U : S.Slots)
      if (U.Alias == Sl)
        U.Alias = -1;
  };
  // Spill slot writes.
  if (MI.mayStore()) {
    uint64_t Size = 0;
    int Sl = spillSlotOf(MI, &Size);
    if (Sl >= 0) {
      int FI = 0;
      Register R = TII->isStoreToStackSlotPostFE(MI, FI);
      if (!R && Op == X86::MOV64mr && Size == 8 &&
          MFI->getObjectSize(SlotFI[Sl]) == 8) {
        const MachineOperand &Src = MI.getOperand(X86::AddrNumOperands);
        if (Src.isReg() && Src.getReg())
          R = Src.getReg();
      }
      if (R && Op == X86::MOV64mr && Size == 8 &&
          MFI->getObjectSize(SlotFI[Sl]) == 8) {
        Val VR = readReg(S, R);
        uintptr_t Id = VR.CopyId;
        forgetAlias(Sl);
        VR = readReg(S, R);
        S.Slots[Sl] = VR;
        S.Slots[Sl].Alias = -1;
        // Every register still holding this exact value aliases the slot.
        if (Id == 0) {
          VR.Alias = Sl;
          writeReg(S, R, VR);
        } else {
          for (Val &U : S.Units)
            if (U.CopyId == Id)
              U.Alias = Sl;
        }
      } else {
        // Partial or non-MOV write: tainted if it already was or if any
        // value operand (not the store's own address) is frame-derived.
        forgetAlias(Sl);
        bool T = S.Slots[Sl].frameish() || anyFrameValueOperand(S, MI);
        S.Slots[Sl] = T ? taint(&MI) : mk(NotFA);
      }
    }
  }
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isDef() && MO.getReg() && MO.getReg().isPhysical())
      writeReg(S, MO.getReg(), V);
}

void GocFrameAddrFix::stepCall(State &S, const MachineInstr &MI) const {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isRegMask()) {
      for (unsigned R = 1, e = TRI->getNumRegs(); R != e; ++R)
        if (MO.clobbersPhysReg(R) && !isFrameBase(R))
          writeReg(S, R, mk(Bot));
    } else if (MO.isReg() && MO.isDef() && MO.getReg() &&
               !isFrameBase(MO.getReg())) {
      writeReg(S, MO.getReg(), mk(NotFA));
    }
  }
}

void GocFrameAddrFix::derivedLive(MachineFunction &MF, StringRef What, const MachineInstr &Call,
                 const Val &V) {
  ++St.Derived;
  const char *Mode = std::getenv("GOC_FRAMEADDR_TAINT");
  bool Fatal = Mode && std::strcmp(Mode, "fatal") == 0;
  bool Quiet = !Mode || std::strcmp(Mode, "quiet") == 0;
  if (Quiet && !Fatal)
    return;
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "goc-frameaddr-fix: " << MF.getName() << ": " << What
     << " holds an unrepairable frame-derived value across call to ";
  for (const MachineOperand &MO : Call.operands())
    if (MO.isGlobal()) OS << MO.getGlobal()->getName();
    else if (MO.isSymbol()) OS << MO.getSymbolName();
  OS << " in bb." << Call.getParent()->getNumber() << "\n";
  if (V.Org) {
    OS << "    tainted by: ";
    V.Org->print(OS, /*IsStandalone=*/true, /*SkipOpers=*/false, /*SkipDebugLoc=*/true);
  } else if (V.Base) {
    OS << "    merge of " << TRI->getName(V.Base) << "+" << V.Off << " with another value\n";
  } else {
    OS << "    (merge)\n";
  }
  if (!Fatal) {
    errs() << OS.str();
    return;
  }
  if (std::getenv("GOC_FRAMEADDR_DEBUG"))
    MF.print(errs());
  report_fatal_error(Twine(OS.str()));
}

} // namespace goc::fa
