//===- GocAArch64Frame.cpp - LR at 0(RSP) for Go's arm64 unwinder --------===//
//
// See Goc/AArch64Frame.h.
//
//===----------------------------------------------------------------------===//
#include "Goc/AArch64Frame.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include <climits>
#include <set>
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

constexpr unsigned LRArea = 16; // LR word + pad, keeps SP 16-aligned

unsigned findOpcode(const TargetInstrInfo &TII, StringRef N) {
  static StringMap<unsigned> Cache;
  auto It = Cache.find(N);
  if (It != Cache.end())
    return It->second;
  for (unsigned I = 0, E = TII.getNumOpcodes(); I < E; ++I)
    if (TII.getName(I) == N)
      return Cache[N] = I;
  report_fatal_error("goc: AArch64 opcode " + Twine(N) + " not found");
}

Register findReg(const TargetRegisterInfo &TRI, StringRef N) {
  for (unsigned I = 1, E = TRI.getNumRegs(); I < E; ++I)
    if (StringRef(TRI.getName(I)) == N)
      return I;
  report_fatal_error("goc: AArch64 register " + Twine(N) + " not found");
}

struct GocA64CallFrame : MachineFunctionPass {
  static char ID;
  GocA64CallFrame() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "goc arm64 LR area in call frames"; }

  // Byte scale of an unsigned-offset load/store's immediate, 0 if MI is not
  // one we rewrite. Unscaled (STUR/LDUR) forms return 1 with Unscaled set.
  static unsigned immScale(StringRef N, bool &Unscaled, bool &Pair) {
    Unscaled = N.starts_with("STUR") || N.starts_with("LDUR");
    Pair = N.starts_with("STP") || N.starts_with("LDP");
    if (Unscaled)
      return 1;
    if (!N.ends_with("ui") && !(Pair && N.ends_with("i")))
      return 0;
    N = N.drop_front(3); // STR / LDR / STP / LDP: the rest names the width
    if (N.contains("Q"))
      return 16;
    if (N.contains("X") || N.contains("D"))
      return 8;
    if (N.contains("W") || N.contains("S"))
      return 4;
    if (N.contains("HH") || N.contains("H"))
      return 2;
    if (N.contains("BB") || N.contains("B"))
      return 1;
    return 0;
  }

  // Shift an SP-relative address by LRArea. Pre-PEI the call lowering
  // writes stack arguments through "COPY $sp" vregs (or $sp itself).
  void shift(MachineInstr &MI, unsigned BaseIdx, const TargetInstrInfo &TII, StringRef Fn) {
    StringRef N = TII.getName(MI.getOpcode());
    if (N == "ADDXri" && BaseIdx == 1) {
      if (MI.getOperand(3).getImm() != 0)
        report_fatal_error("goc: " + Fn + ": shifted add from SP in a call sequence");
      MI.getOperand(2).setImm(MI.getOperand(2).getImm() + LRArea);
      return;
    }
    bool Unscaled, Pair;
    unsigned Scale = immScale(N, Unscaled, Pair);
    unsigned ImmIdx = BaseIdx + 1;
    if (!Scale || ImmIdx >= MI.getNumOperands() || !MI.getOperand(ImmIdx).isImm())
      report_fatal_error("goc: " + Fn + ": unexpected use of SP in a call sequence: " + N);
    int64_t Imm = MI.getOperand(ImmIdx).getImm() + LRArea / Scale;
    if (Unscaled && (Imm < -256 || Imm > 255))
      report_fatal_error("goc: " + Fn + ": stack argument offset out of range");
    MI.getOperand(ImmIdx).setImm(Imm);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
    MachineRegisterInfo &MRI = MF.getRegInfo();
    MachineFrameInfo &MFI = MF.getFrameInfo();
    Register SP = findReg(TRI, "SP");
    unsigned Setup = TII.getCallFrameSetupOpcode();
    unsigned Destroy = TII.getCallFrameDestroyOpcode();
    bool Changed = false;
    // Incoming stack arguments (fixed objects at or above the entry SP)
    // sit LRArea higher: every caller keeps its LR copy below them.
    for (int FI = MFI.getObjectIndexBegin(); FI < 0; ++FI)
      if (MFI.getObjectOffset(FI) >= 0) {
        MFI.setObjectOffset(FI, MFI.getObjectOffset(FI) + LRArea);
        Changed = true;
      }
    for (MachineBasicBlock &MBB : MF) {
      bool InSeq = false;
      SmallVector<MachineInstr *, 8> Uses;
      for (MachineInstr &MI : MBB) {
        unsigned Opc = MI.getOpcode();
        if (Opc == Setup || Opc == Destroy) {
          MI.getOperand(0).setImm(MI.getOperand(0).getImm() + LRArea);
          InSeq = Opc == Setup;
          Changed = true;
          continue;
        }
        if (!InSeq || MI.isCall())
          continue;
        // Stores of outgoing arguments: base $sp, or a vreg copied from it.
        for (unsigned I = 0; I < MI.getNumOperands(); ++I) {
          const MachineOperand &O = MI.getOperand(I);
          if (!O.isReg() || !O.isUse() || !O.getReg() || O.isImplicit())
            continue;
          bool FromSP = O.getReg() == SP;
          if (!FromSP && O.getReg().isVirtual())
            if (MachineInstr *D = MRI.getUniqueVRegDef(O.getReg()))
              FromSP = D->isCopy() && D->getOperand(1).getReg() == SP;
          if (FromSP && !MI.isCopy()) {
            shift(MI, I, TII, MF.getName());
            break;
          }
        }
      }
    }
    if (Changed && MFI.hasVarSizedObjects())
      report_fatal_error("goc: " + MF.getName() +
                         ": arm64 functions with variable-sized allocas are not supported");
    return Changed;
  }
};
char GocA64CallFrame::ID = 0;

struct GocA64SaveLR : MachineFunctionPass {
  static char ID;
  GocA64SaveLR() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "goc arm64 LR copy at 0(RSP)"; }
  bool runOnMachineFunction(MachineFunction &MF) override {
    const MachineFrameInfo &MFI = MF.getFrameInfo();
    if (!MFI.hasCalls() || MFI.getStackSize() < LRArea)
      return false;
    if (MFI.getMaxCallFrameSize() < LRArea)
      report_fatal_error("goc: " + MF.getName() + ": arm64 call frame has no LR area");
    const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
    MachineBasicBlock &Entry = MF.front();
    // After the last frame-setup instruction (SP is final, LR untouched).
    MachineBasicBlock::iterator At = Entry.begin();
    for (auto I = Entry.begin(), E = Entry.end(); I != E; ++I)
      if (I->getFlag(MachineInstr::FrameSetup))
        At = std::next(I);
    BuildMI(Entry, At, DebugLoc(), TII.get(findOpcode(TII, "STRXui")))
        .addReg(findReg(TRI, "LR"))
        .addReg(findReg(TRI, "SP"))
        .addImm(0)
        .setMIFlag(MachineInstr::FrameSetup);
    return true;
  }
};
char GocA64SaveLR::ID = 0;

// Frame addresses that survive a call. The call can copy the goroutine
// stack: SP and X29 come back adjusted (sched.sp, the marked X29 slots),
// but a callee-saved register or a register-allocator spill slot still
// holds the old address. Before such a call the old SP goes to 8(RSP) (the
// pad word of the LR area); after it, every live CSR and spill slot that
// may hold a frame-derived value gets the same correction copystack's
// adjustpointer applies: old.lo <= v < old.hi ? v + delta : v, with
// delta = SP - oldSP and old.hi = g.stack.hi - delta. When the stack did
// not move, delta is 0 and nothing changes.
struct GocA64FrameAddrFix : MachineFunctionPass {
  static char ID;
  GocA64FrameAddrFix() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "goc arm64 frame-address repair after calls"; }

  struct State {
    uint32_t Regs = 0;      // bit i: Xi may hold a frame-derived value
    std::set<int> Slots;    // spill slots that may hold one
    bool operator==(const State &O) const { return Regs == O.Regs && Slots == O.Slots; }
    bool operator!=(const State &O) const { return !(*this == O); }
    void join(const State &O) {
      Regs |= O.Regs;
      Slots.insert(O.Slots.begin(), O.Slots.end());
    }
  };

  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  const MachineFrameInfo *MFI = nullptr;
  Register X[31];
  Register SP, FP, XZR, G;

  int xIndex(Register R) const {
    for (unsigned I = 0; I < 31; ++I)
      if (X[I] == R || (TRI->isSubRegister(X[I], R)))
        return I;
    return -1;
  }
  unsigned op(StringRef N) const { return findOpcode(*TII, N); }

  bool tainted(const State &S, Register R) const {
    if (R == SP || R == FP)
      return true;
    int I = xIndex(R);
    return I >= 0 && I != 29 && I != 30 && (S.Regs >> I & 1);
  }

  static int spillSlot(const MachineInstr &MI, const MachineFrameInfo &MFI) {
    for (const MachineMemOperand *MMO : MI.memoperands())
      if (auto *PSV = dyn_cast_or_null<FixedStackPseudoSourceValue>(MMO->getPseudoValue()))
        if (MFI.isSpillSlotObjectIndex(PSV->getFrameIndex()) && MMO->getSize().hasValue() &&
            MMO->getSize().getValue() == 8)
          return PSV->getFrameIndex();
    return INT_MIN;
  }

  void step(const MachineInstr &MI, State &S) const {
    if (MI.isCall()) {
      // Caller-saved registers die; x19-x28 survive (repaired after).
      S.Regs &= 0x1FF80000u;
      return;
    }
    bool Val = false; // the value written by MI may be frame-derived
    int FI = spillSlot(MI, *MFI);
    if (MI.mayStore() && FI != INT_MIN) {
      // str Xt, [slot]: operand 0 is the stored value.
      const MachineOperand &V = MI.getOperand(0);
      if (V.isReg() && tainted(S, V.getReg()))
        S.Slots.insert(FI);
      else
        S.Slots.erase(FI);
      return;
    }
    if (MI.mayLoad()) {
      Val = FI != INT_MIN && S.Slots.count(FI);
    } else if (!MI.mayStore()) {
      unsigned Opc = MI.getOpcode();
      bool Sub = Opc == op("SUBXrr") || Opc == op("SUBXrs") || Opc == op("SUBSXrr") ||
                 Opc == op("SUBSXrs");
      unsigned NTaint = 0, NUse = 0;
      for (const MachineOperand &U : MI.uses())
        if (U.isReg() && U.getReg() && !U.isImplicit()) {
          ++NUse;
          NTaint += tainted(S, U.getReg());
        }
      // A difference of two frame addresses is a plain integer.
      Val = NTaint > 0 && !(Sub && NTaint == 2);
    }
    for (const MachineOperand &D : MI.defs()) {
      if (!D.isReg() || !D.getReg())
        continue;
      int I = xIndex(D.getReg());
      if (I < 0 || I >= 29)
        continue;
      if (Val && TRI->getRegSizeInBits(*TRI->getMinimalPhysRegClass(D.getReg())) == 64)
        S.Regs |= 1u << I;
      else
        S.Regs &= ~(1u << I);
    }
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    TII = MF.getSubtarget().getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    MFI = &MF.getFrameInfo();
    for (unsigned I = 0; I < 29; ++I)
      X[I] = findReg(*TRI, "X" + std::to_string(I));
    X[29] = FP = findReg(*TRI, "FP");
    X[30] = findReg(*TRI, "LR");
    SP = findReg(*TRI, "SP");
    XZR = findReg(*TRI, "XZR");
    G = X[28];
    if (!MFI->hasCalls())
      return false;

    DenseMap<const MachineBasicBlock *, State> Out;
    ReversePostOrderTraversal<MachineFunction *> RPO(&MF);
    auto inState = [&](const MachineBasicBlock *MBB) {
      State In;
      for (const MachineBasicBlock *P : MBB->predecessors()) {
        auto It = Out.find(P);
        if (It != Out.end())
          In.join(It->second);
      }
      return In;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (MachineBasicBlock *MBB : RPO) {
        State S = inState(MBB);
        for (const MachineInstr &MI : *MBB)
          step(MI, S);
        auto It = Out.find(MBB);
        if (It == Out.end() || It->second != S) {
          Out[MBB] = S;
          Changed = true;
        }
      }
    }

    bool Fixed = false;
    const int64_t Frame = MFI->getStackSize();
    for (MachineBasicBlock &MBB : MF) {
      DenseMap<const MachineInstr *, SmallVector<unsigned, 4>> LiveCSR;
      LivePhysRegs Live(*TRI);
      Live.addLiveOuts(MBB);
      for (MachineInstr &MI : llvm::reverse(MBB)) {
        if (MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP)
          for (unsigned I = 19; I <= 27; ++I)
            if (Live.contains(X[I]))
              LiveCSR[&MI].push_back(I);
        Live.stepBackward(MI);
      }
      State S = inState(&MBB);
      for (auto MII = MBB.begin(); MII != MBB.end(); ++MII) {
        MachineInstr &Call = *MII;
        State Before = S;
        step(Call, S);
        if (!Call.isCall() || Call.getOpcode() == TargetOpcode::STACKMAP)
          continue;
        SmallVector<unsigned, 4> Regs;
        for (unsigned I : LiveCSR.lookup(&Call))
          if (Before.Regs >> I & 1)
            Regs.push_back(I);
        SmallVector<int, 4> Slots(Before.Slots.begin(), Before.Slots.end());
        if (Regs.empty() && Slots.empty())
          continue;
        if (MFI->getMaxCallFrameSize() < 16)
          report_fatal_error("goc: " + MF.getName() + ": no LR area for the frame-address repair");
        // A scratch the call does not read: x9-x17 are dead across it.
        Register Tmp;
        for (unsigned I : {16u, 17u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
          bool Used = false;
          for (const MachineOperand &O : Call.operands())
            if (O.isReg() && O.getReg() && TRI->regsOverlap(O.getReg(), X[I]) && O.isUse())
              Used = true;
          if (!Used) {
            Tmp = X[I];
            break;
          }
        }
        DebugLoc DL = Call.getDebugLoc();
        BuildMI(MBB, MII, DL, TII->get(op("ADDXri")), Tmp).addReg(SP).addImm(0).addImm(0);
        BuildMI(MBB, MII, DL, TII->get(op("STRXui"))).addReg(Tmp).addReg(SP).addImm(1);
        auto At = std::next(MII);
        auto Emit = [&](StringRef Op) { return BuildMI(MBB, At, DL, TII->get(op(Op))); };
        Register Old = X[9], Delta = X[16], OldHi = X[17], Adj = X[10], V = X[11];
        Emit("LDRXui").addReg(Old, RegState::Define).addReg(SP).addImm(1);          // old SP
        Emit("ADDXri").addReg(Delta, RegState::Define).addReg(SP).addImm(0).addImm(0);
        Emit("SUBXrs").addReg(Delta, RegState::Define).addReg(Delta).addReg(Old).addImm(0);
        Emit("LDRXui").addReg(OldHi, RegState::Define).addReg(G).addImm(1);         // g.stack.hi
        Emit("SUBXrs").addReg(OldHi, RegState::Define).addReg(OldHi).addReg(Delta).addImm(0);
        auto Fix = [&](Register R) {
          Emit("ADDXrs").addReg(Adj, RegState::Define).addReg(R).addReg(Delta).addImm(0);
          Emit("SUBSXrs").addReg(XZR, RegState::Define).addReg(R).addReg(Old).addImm(0);
          Emit("CCMPXr").addReg(R).addReg(OldHi).addImm(2).addImm(/*HS*/ 2);
          Emit("CSELXr").addReg(R, RegState::Define).addReg(Adj).addReg(R).addImm(/*LO*/ 3);
        };
        for (unsigned I : Regs)
          Fix(X[I]);
        for (int FI : Slots) {
          int64_t Off = Frame + MFI->getObjectOffset(FI); // from SP
          if (Off < 0 || Off % 8 || Off / 8 > 4095)
            report_fatal_error("goc: " + MF.getName() + ": spill slot out of range for the repair");
          Emit("LDRXui").addReg(V, RegState::Define).addReg(SP).addImm(Off / 8);
          Fix(V);
          Emit("STRXui").addReg(V).addReg(SP).addImm(Off / 8);
        }
        Fixed = true;
      }
    }
    return Fixed;
  }
};
char GocA64FrameAddrFix::ID = 0;

} // namespace

FunctionPass *goc::createA64FrameAddrFixPass() { return new GocA64FrameAddrFix(); }
FunctionPass *goc::createA64CallFramePass() { return new GocA64CallFrame(); }
FunctionPass *goc::createA64SaveLRPass() { return new GocA64SaveLR(); }
