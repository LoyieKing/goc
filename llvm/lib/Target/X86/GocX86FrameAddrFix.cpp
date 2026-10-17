//===- GocX86FrameAddrFix.cpp - rebase frame addresses across calls -------===//
//
// GocFrameAddrFix (x86-64, post-RA, last before the AsmPrinter) — part of goc-llc,
// which is llc-19 plus two machine passes: GocStackmapPlacement (drops
// STACKMAP records not followed by a CALL in their block, e.g. before an
// intrinsic that was expanded inline) and the post-RA GocFrameAddrFix.
//
// Why a driver: stock llc has no hook to insert a plugin MachineFunction pass
// into its codegen pipeline. This file rebuilds exactly what llc's
// LLVMTargetMachine::addPassesToEmitFile does (same TargetPassConfig, same
// cl::opt flags, same TargetOptions) and appends the pass as the very last
// machine pass before the AsmPrinter (-enable-tail-merge=false is the
// default unless given explicitly).
//
// The problem it solves (see Transforms/StackMap/SMRoots.cpp, rematFrameAddress): a goroutine
// stack is copied during a call (the callee's morestack prologue, or the Go
// runtime). A value "RBP+c" (the address of a local) that the register
// allocator kept in a callee-saved register, or in a spill slot, across that
// call still points into the old stack afterwards. The morestack stub and the
// callee epilogues restore callee-saved registers verbatim; Go's copier only
// rewrites stack words that a stack map marks. Previously goc-reanchor
// re-derived every such address with an opaque `asm sideeffect "leaq"` at each
// use after a safepoint; that blocked addressing-mode folding everywhere.
// goc-reanchor emits a plain GEP instead and this pass restores the invariant
// on the final machine code:
//
//   After every call, no callee-saved register and no spill slot that is live
//   holds a frame address computed before the call.
//
// Safety argument.
//   1. The stack only moves inside a call: morestack runs in a callee's
//      prologue, and the runtime copies/shrinks a goroutine stack only while
//      it is stopped at a call. Between two calls RBP/RSP-relative addresses
//      are stable. (Same assumption the leaq asm made: it re-derived from RBP
//      at the use, not at the call.)
//   2. After a call returns, RBP and RSP are correct for the (possibly moved)
//      frame: Go adjusts saved frame pointers when it copies, and RSP comes
//      back from the new stack. So "LEA c(%rbp)" executed after the call is
//      the rebased value of the same local's address.
//   3. Caller-saved registers do not survive a call (regmask), so only
//      callee-saved GPRs (RBX, R12-R15) and spill slots can carry a stale
//      address across it. XMM registers are all caller-saved in SysV.
//   4. A forward analysis over the lattice below (Val/Kind) tracks, per
//      register unit and per spill slot, whether it holds exactly
//      FA(base, c) = base+c with base in {RBP, RSP} on every path (LEA
//      base+disp, MOV copies, ADD/SUB imm, full-width spill/reload), a value
//      that moves with the stack by an unknown offset (D1: &a[i], merges of
//      different frame addresses), or "maybe a stack address" (MS). After each
//      call, every live callee-saved register and live spill slot in state FA
//      is re-derived with LEA (a slot via the dead caller-saved R11 and a
//      store). D1/MS locations are rebased by delta = RBP_after - RBP_before
//      (RBP_before is saved in goc.oldfp, which is in no stack map, so a copy
//      moves it verbatim); MS only if the rebased value lies in this frame.
//      The rebase runs in a cold block entered only when delta != 0, so the
//      hot path costs one store and one compare per such call. Re-deriving or
//      rebasing a value a location already holds correctly is harmless even
//      where liveness is imprecise.
//   5. A value derived from a frame address with an unknown coefficient
//      (p - x, p * k) is "tainted" and cannot be repaired here. It is counted
//      and reported in a note (-goc-frameaddr-taint=warn lists them,
//      =fatal aborts the compile). For quickjs these are libregexp's
//      `sp - stack_buf` style differences that are re-added to a reloaded
//      base, i.e. offsets, not addresses; the same values are unrepaired in
//      the legacy asm mode too. goc-reanchor keeps frame addresses that must
//      survive a call in stack-map-adjusted anchor slots.
//   6. Any non-call write to RSP (dynamic alloca) or RBP invalidates FA
//      states based on it (they become tainted).
//   7. Addresses loaded from memory (a pointer stored in a local, a pointer
//      argument into the caller's frame) are not FA values. Go's copystack
//      rewrites the stack slot when its stack map marks it, but a
//      callee-saved register that still holds the pre-call copy is restored
//      verbatim by the morestack stub. After the call this pass reloads that
//      register from the slot. The slots are spill slots and the 8-byte root
//      allocas goc-reanchor emits (goc.arganchor, goc.anchor, goc.spill.root):
//      a volatile reload in the IR does not stop the allocator from keeping
//      the pre-call bits in a CSR, which is how Bellard's inlined round_to_d
//      kept a stale bignum pointer in r14 across mpb_shr_round. Root allocas
//      are not also rebased here; Go already rewrote them. The reload is
//      unconditional: if the stack did not move, the slot and the register
//      already agree.
#include "GocX86FrameAddrFix.h"
#include "Goc/MachinePasses.h"
#include "llvm/Support/CommandLine.h"

static llvm::cl::opt<std::string> GocFrameAddrTrace(
    "goc-frameaddr-trace", llvm::cl::value_desc("function"),
    llvm::cl::desc("goc-frameaddr-fix: trace the repair of this function"));
static llvm::cl::opt<bool> GocFrameAddrStats(
    "goc-frameaddr-stats",
    llvm::cl::desc("goc-frameaddr-fix: print per-function repair counts"));

namespace goc::fa {

unsigned TotalUnrepairable = 0, FnsUnrepairable = 0, TotalRepairCalls = 0;

bool GocFrameAddrFix::runOnMachineFunction(MachineFunction &MF) {
  TRI = MF.getSubtarget().getRegisterInfo();
  TII = MF.getSubtarget().getInstrInfo();
  MFI = &MF.getFrameInfo();
  NU = TRI->getNumRegUnits();
  SlotIdx.clear();
  AllocaSlot.clear();
  SlotFI.clear();
  SlotIsRoot.clear();
  St = Stats();
  int OldFP = INT_MIN;
  for (int FI = MFI->getObjectIndexBegin(), FE = MFI->getObjectIndexEnd(); FI != FE; ++FI) {
    if (MFI->isDeadObjectIndex(FI))
      continue;
    if (MFI->isSpillSlotObjectIndex(FI)) {
      SlotIdx[FI] = SlotFI.size();
      SlotFI.push_back(FI);
      SlotIsRoot.push_back(0);
    } else if (const AllocaInst *AI = MFI->getObjectAllocation(FI)) {
      if (AI->getName() == "goc.oldfp")
        OldFP = FI;
      else if (MFI->getObjectSize(FI) == 8 && isCopiedRoot(AI)) {
        AllocaSlot[AI] = SlotFI.size();
        SlotIdx[FI] = SlotFI.size();
        SlotFI.push_back(FI);
        SlotIsRoot.push_back(1);
      }
    }
  }
  unsigned NS = SlotFI.size();

  // ---- Backward liveness (register units + spill slots). ----
  unsigned NB = MF.getNumBlockIDs();
  std::vector<BitVector> LiveInU(NB, BitVector(NU)), LiveInS(NB, BitVector(NS));
  auto stepBack = [&](const MachineInstr &MI, BitVector &LU, BitVector &LS) {
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isRegMask()) {
        for (unsigned R = 1, e = TRI->getNumRegs(); R != e; ++R)
          if (MO.clobbersPhysReg(R))
            for (MCRegUnit U : TRI->regunits(R))
              LU.reset(U);
      } else if (MO.isReg() && MO.isDef() && MO.getReg()) {
        for (MCRegUnit U : TRI->regunits(MO.getReg()))
          LU.reset(U);
      }
    }
    if (MI.mayStore() && !MI.mayLoad()) {
      uint64_t Size = 0;
      int Sl = spillSlotOf(MI, &Size);
      if (Sl >= 0 && Size >= (uint64_t)MFI->getObjectSize(SlotFI[Sl]))
        LS.reset(Sl);
    }
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.isUse() && !MO.isUndef() && MO.getReg())
        for (MCRegUnit U : TRI->regunits(MO.getReg()))
          LU.set(U);
    if (MI.mayLoad()) {
      int Sl = spillSlotOf(MI);
      if (Sl >= 0)
        LS.set(Sl);
    }
  };
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (MachineBasicBlock *MBB : post_order(&MF)) {
      BitVector LU(NU), LS(NS);
      for (MachineBasicBlock *Succ : MBB->successors()) {
        LU |= LiveInU[Succ->getNumber()];
        LS |= LiveInS[Succ->getNumber()];
      }
      for (auto It = MBB->rbegin(); It != MBB->rend(); ++It)
        stepBack(*It, LU, LS);
      if (LU != LiveInU[MBB->getNumber()] || LS != LiveInS[MBB->getNumber()]) {
        LiveInU[MBB->getNumber()] = LU;
        LiveInS[MBB->getNumber()] = LS;
        Changed = true;
      }
    }
  }
  // Live sets right after each safepoint.
  DenseMap<const MachineInstr *, std::pair<BitVector, BitVector>> LiveAfter;
  for (MachineBasicBlock &MBB : MF) {
    BitVector LU(NU), LS(NS);
    for (MachineBasicBlock *Succ : MBB.successors()) {
      LU |= LiveInU[Succ->getNumber()];
      LS |= LiveInS[Succ->getNumber()];
    }
    for (auto It = MBB.rbegin(); It != MBB.rend(); ++It) {
      if (isSafepoint(*It))
        LiveAfter[&*It] = {LU, LS};
      stepBack(*It, LU, LS);
    }
  }
  if (LiveAfter.empty())
    return false;

  // ---- Forward must-analysis. ----
  State Init;
  Init.Units.assign(NU, mk(NotFA));
  // Spill slots are uninitialized at entry: identity for the join (a path
  // that reaches a reload without a spill reads garbage either way).
  Init.Slots.assign(NS, mk(Bot));
  State BotS;
  BotS.Units.assign(NU, mk(Bot));
  BotS.Slots.assign(NS, mk(Bot));
  std::vector<State> In(NB, BotS), Out(NB, BotS);
  std::vector<bool> Seen(NB, false);
  ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
  Changed = true;
  while (Changed) {
    Changed = false;
    for (MachineBasicBlock *MBB : RPOT) {
      unsigned N = MBB->getNumber();
      State S = MBB == &MF.front() ? Init : BotS;
      for (MachineBasicBlock *P : MBB->predecessors()) {
        if (!Seen[P->getNumber()])
          continue;
        const State &PO = Out[P->getNumber()];
        for (unsigned i = 0; i < NU; ++i) S.Units[i] = join(S.Units[i], PO.Units[i]);
        for (unsigned i = 0; i < NS; ++i) S.Slots[i] = join(S.Slots[i], PO.Slots[i]);
      }
      In[N] = S;
      for (MachineInstr &MI : *MBB) {
        if (isSafepoint(MI))
          stepCall(S, MI);
        else
          step(S, MI);
      }
      if (!Seen[N] || !(S == Out[N])) {
        Out[N] = std::move(S);
        Seen[N] = true;
        Changed = true;
      }
    }
  }

  // ---- Repair. ----
  static const unsigned CSRs[] = {X86::RBX, X86::R12, X86::R13, X86::R14, X86::R15};
  static const unsigned Scratch[] = {X86::R11, X86::R10, X86::R9, X86::R8,
                                     X86::RCX, X86::RSI, X86::RDI};
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  auto frameRef = [&](int FI, Register &FrameReg) {
    return TFL->getFrameIndexReference(MF, FI, FrameReg).getFixed();
  };
  auto imm32 = [&](int64_t V) {
    if (V < INT32_MIN || V > INT32_MAX)
      report_fatal_error("goc-frameaddr-fix: offset out of range");
    return V;
  };
  // Top of this frame's addressable area relative to RBP: the return
  // address and the incoming stack arguments (byval, varargs area).
  // Fixed-object offsets are relative to the entry SP (= RBP + 8).
  int64_t TopOff = 16;
  for (int FI = MFI->getObjectIndexBegin(); FI < 0; ++FI)
    if (!MFI->isDeadObjectIndex(FI))
      TopOff = std::max<int64_t>(TopOff, 8 + MFI->getObjectOffset(FI) + MFI->getObjectSize(FI));
  bool Trace = !GocFrameAddrTrace.empty() && MF.getName() == GocFrameAddrTrace;
  bool Any = false;
  std::vector<MachineBasicBlock *> Order(RPOT.begin(), RPOT.end());
  for (MachineBasicBlock *Orig : Order) {
    State S = In[Orig->getNumber()];
    MachineBasicBlock *MBB = Orig;
    for (auto It = MBB->begin(); It != MBB->end();) {
      MachineInstr &MI = *It;
      if (!isSafepoint(MI)) {
        step(S, MI);
        if (Trace) {
          errs() << "  [bb." << MBB->getNumber() << "] ";
          for (const MachineOperand &MO : MI.operands())
            if (MO.isReg() && MO.isDef() && MO.getReg() && MO.getReg().isPhysical() &&
                !MO.isImplicit()) {
              Val V = readReg(S, MO.getReg());
              errs() << TRI->getName(MO.getReg()) << "=" << "BNFDMT"[V.K] << " ";
            }
          MI.print(errs(), true, false, true);
        }
        ++It;
        continue;
      }
      stepCall(S, MI);
      auto &LA = LiveAfter[&MI];
      DebugLoc DL = MI.getDebugLoc();
      SmallVector<std::pair<unsigned, Val>, 4> Regs;
      SmallVector<std::pair<unsigned, Val>, 4> Slots;
      bool NeedDelta = false, NeedRange = false;
      for (unsigned R : CSRs) {
        if (MRI.isReserved(R))
          continue;
        bool Live = false, AnyFrame = false;
        for (MCRegUnit U : TRI->regunits(R)) {
          Live |= LA.first.test(U);
          AnyFrame |= S.Units[U].frameish();
        }
        if (!Live || !AnyFrame)
          continue;
        Val V = readReg(S, R);
        // Go already rewrote the root slot. Reloading below is the adjusted
        // pointer; LEA/rebase on top of that would apply the delta twice.
        if (aliasesCopiedRoot(V))
          continue;
        if (!V.repairable()) {
          derivedLive(MF, TRI->getName(R), MI, V);
          continue;
        }
        Regs.push_back({R, V});
        NeedDelta |= V.K != FA;
        NeedRange |= V.K == MS;
      }
      for (unsigned Sl = 0; Sl < NS; ++Sl) {
        if (Sl < SlotIsRoot.size() && SlotIsRoot[Sl])
          continue;
        const Val &V = S.Slots[Sl];
        if (!LA.second.test(Sl) || !V.frameish())
          continue;
        if (!V.repairable()) {
          derivedLive(MF, "spill slot", MI, V);
          continue;
        }
        Slots.push_back({Sl, V});
        NeedDelta |= V.K != FA;
        NeedRange |= V.K == MS;
      }
      // CSR that is an exact copy of a spill. copystack rewrites the slot
      // and the morestack stub restores the register, so reload it.
      SmallVector<std::pair<unsigned, int>, 4> Reloads;
      for (unsigned R : CSRs) {
        if (MRI.isReserved(R))
          continue;
        bool Live = false;
        for (MCRegUnit U : TRI->regunits(R))
          Live |= LA.first.test(U);
        if (!Live)
          continue;
        Val V = readReg(S, R);
        // Root-slot copies are reloaded even when the value is also a frame
        // address. Other slots are reloaded only when this pass cannot
        // rebase them itself.
        if (aliasesCopiedRoot(V) || (V.Alias >= 0 && !V.repairable()))
          Reloads.push_back({R, V.Alias});
      }
      if (Regs.empty() && Slots.empty() && Reloads.empty()) {
        ++It;
        continue;
      }
      // Reload before any cold split, on every path. Tail is the first
      // instruction that was already after the call.
      MachineBasicBlock::iterator Tail = std::next(It);
      for (auto &P : Reloads) {
        int FI = SlotFI[P.second];
        Register FR;
        int64_t SOff = frameRef(FI, FR);
        auto mmo = MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI),
                                           MachineMemOperand::MOLoad, 8, MFI->getObjectAlign(FI));
        BuildMI(*MBB, Tail, DL, TII->get(X86::MOV64rm), P.first)
            .addReg(FR).addImm(1).addReg(0).addImm(SOff).addReg(0)
            .addMemOperand(mmo);
        ++St.Reload;
      }
      if (Regs.empty() && Slots.empty()) {
        Any = true;
        ++St.Calls;
        It = Tail;
        continue;
      }
      // Scratch registers: clobbered by the call, not a result, dead after.
      SmallVector<unsigned, 5> Free;
      for (unsigned R : Scratch) {
        bool Ok = false;
        for (const MachineOperand &MO : MI.operands())
          if (MO.isRegMask() && MO.clobbersPhysReg(R))
            Ok = true;
        for (const MachineOperand &MO : MI.operands())
          if (MO.isReg() && MO.isDef() && MO.getReg() && TRI->regsOverlap(MO.getReg(), R))
            Ok = false;
        for (MCRegUnit U : TRI->regunits(R))
          if (LA.first.test(U))
            Ok = false;
        if (Ok)
          Free.push_back(R);
      }
      if (Free.size() < 5)
        report_fatal_error(Twine("goc-frameaddr-fix: ") + MF.getName() +
                           ": not enough dead scratch registers after a call");
      const unsigned D = Free[0], SZ = Free[1], T1 = Free[2], T2 = Free[3], T3 = Free[4];
      // FA-only: re-derive inline right after the call (1-2 instructions
      // per location). Otherwise the hot path is: store RBP to goc.oldfp
      // before the call, compare after it, and branch to an out-of-line
      // block that rebases everything only if the stack actually moved.
      MachineBasicBlock *FixMBB = MBB;
      MachineBasicBlock::iterator InsertAt = Tail;
      MachineBasicBlock *Cont = nullptr;
      Register OFR;
      int64_t OOff = 0;
      if (NeedDelta) {
        if (OldFP == INT_MIN)
          report_fatal_error(Twine("goc-frameaddr-fix: ") + MF.getName() +
                             ": needs a rebase but has no goc.oldfp slot");
        // goc.oldfp is in no stack map, so a stack copy moves it verbatim:
        // after the call it still holds the pre-call RBP.
        OOff = frameRef(OldFP, OFR);
        auto oMMO = [&](MachineMemOperand::Flags F) {
          return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, OldFP), F, 8, Align(8));
        };
        BuildMI(*MBB, It, DL, TII->get(X86::MOV64mr))
            .addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0).addReg(X86::RBP)
            .addMemOperand(oMMO(MachineMemOperand::MOStore));
        Cont = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
        MF.insert(std::next(MBB->getIterator()), Cont);
        Cont->splice(Cont->end(), MBB, Tail, MBB->end());
        Cont->transferSuccessors(MBB);
        FixMBB = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
        MF.push_back(FixMBB);
        MBB->addSuccessor(Cont);
        MBB->addSuccessor(FixMBB);
        FixMBB->addSuccessor(Cont);
        // Flags are dead right after a call.
        BuildMI(*MBB, MBB->end(), DL, TII->get(X86::CMP64mr))
            .addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0).addReg(X86::RBP)
            .addMemOperand(oMMO(MachineMemOperand::MOLoad));
        BuildMI(*MBB, MBB->end(), DL, TII->get(X86::JCC_1)).addMBB(FixMBB).addImm(X86::COND_NE);
        InsertAt = FixMBB->end();
      }
      auto B = [&](unsigned Opc) { return BuildMI(*FixMBB, InsertAt, DL, TII->get(Opc)); };
      auto Bd = [&](unsigned Opc, unsigned Dst) { return BuildMI(*FixMBB, InsertAt, DL, TII->get(Opc), Dst); };
      auto leaTo = [&](unsigned Dst, unsigned Base, unsigned Index, int64_t Off) {
        Bd(X86::LEA64r, Dst).addReg(Base).addImm(1).addReg(Index).addImm(imm32(Off)).addReg(0);
      };
      if (NeedDelta) {
        // delta = RBP - goc.oldfp
        Bd(X86::MOV64rr, D).addReg(X86::RBP);
        Bd(X86::SUB64rm, D).addReg(D).addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0)
            .addMemOperand(MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, OldFP),
                                                   MachineMemOperand::MOLoad, 8, Align(8)));
      }
      if (NeedRange) {
        // SZ = frame top - RSP: a rebased value w is in this frame iff
        // (w - RSP) <u SZ. Only this frame's addresses can be MS here.
        leaTo(SZ, X86::RBP, 0, TopOff);
        Bd(X86::SUB64rr, SZ).addReg(SZ).addReg(X86::RSP);
      }
      // v := v rebased, for v in register V.
      auto fixReg = [&](unsigned V, const Val &Vl) {
        if (Vl.K == FA) {
          leaTo(V, Vl.Base, 0, Vl.Off);
          ++St.Lea;
        } else if (Vl.K == D1) {
          Bd(X86::ADD64rr, V).addReg(V).addReg(D);
          ++St.Rebase;
        } else { // MS
          leaTo(T1, V, D, 0);                 // T1 = v + delta
          Bd(X86::MOV64rr, T2).addReg(T1);
          Bd(X86::SUB64rr, T2).addReg(T2).addReg(X86::RSP);
          B(X86::CMP64rr).addReg(T2).addReg(SZ);
          Bd(X86::CMOV64rr, V).addReg(V).addReg(T1).addImm(X86::COND_B);
          ++St.Range;
        }
      };
      for (auto &P : Regs)
        fixReg(P.first, P.second);
      for (auto &P : Slots) {
        int FI = SlotFI[P.first];
        Register FR;
        int64_t SOff = frameRef(FI, FR);
        auto mmo = [&](MachineMemOperand::Flags F) {
          return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI), F, 8,
                                         MFI->getObjectAlign(FI));
        };
        if (P.second.K != FA)
          Bd(X86::MOV64rm, T3).addReg(FR).addImm(1).addReg(0).addImm(SOff).addReg(0)
              .addMemOperand(mmo(MachineMemOperand::MOLoad));
        fixReg(T3, P.second);
        B(X86::MOV64mr).addReg(FR).addImm(1).addReg(0).addImm(SOff).addReg(0).addReg(T3)
            .addMemOperand(mmo(MachineMemOperand::MOStore));
        ++St.Slot;
      }
      ++St.Calls;
      Any = true;
      if (Cont) {
        B(X86::JMP_1).addMBB(Cont);
        ++St.Split;
        MBB = Cont; // continue the walk in the split-off tail
        It = MBB->begin();
        continue;
      }
      It = InsertAt; // skip the inserted instructions
    }
  }
  TotalUnrepairable += St.Derived;
  FnsUnrepairable += St.Derived ? 1 : 0;
  TotalRepairCalls += St.Calls;
  if (GocFrameAddrStats && (St.Calls || St.Derived))
    errs() << "goc-frameaddr-fix: " << MF.getName() << " calls=" << St.Calls
           << " lea=" << St.Lea << " rebase=" << St.Rebase << " range=" << St.Range
           << " slot=" << St.Slot << " reload=" << St.Reload << " split=" << St.Split
           << " unrepairable=" << St.Derived << "\n";
  return Any;
}

char GocFrameAddrFix::ID = 0;

} // namespace goc::fa

llvm::MachineFunctionPass *goc::createGocX86FrameAddrFixPass() { return new fa::GocFrameAddrFix(); }

void goc::printFrameAddrFixNote(llvm::raw_ostream &OS) {
  if (fa::TotalUnrepairable)
    OS << "goc-llc: note: " << fa::TotalUnrepairable
       << " frame-derived values of unknown provenance stay live across calls in "
       << fa::FnsUnrepairable << " functions (not rebased; -goc-frameaddr-taint=warn lists them)\n";
}
