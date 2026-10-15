//===- GocAArch64StackCheck.cpp - AArch64 instructions for the Go stack check ===//
//
// The AArch64 hook of GocStackCheck (Goc/StackCheck.h). Only instruction
// selection lives here; the policy is in lib/CodeGen/GocStackCheck.cpp.
//
//   check (C entry; X16/X17 are the intra-procedure scratch registers and
//   are free at an AAPCS64 entry; g is X28, reserved for the whole TU):
//       ldr x16, [x28, #16]            # g.stackguard0
//     small: cmp sp, x16
//     big:   sub x17, sp, #(frame-StackSmall) ; cmp x17, x16
//     huge:  movz/movk x17, #(frame-StackSmall) ; subs x17, sp, x17 ; b.lo stub
//            cmp x17, x16
//       b.ls stub
//
//   cold block: save X0-X9 in pairs
//   with pre-indexed STP, reserve 128 bytes and save Q0-Q7, hand the
//   return address to morestack in X3 (Go's arm64 prologue does the same),
//   BL runtime.morestack_noctxt, restore everything, B back to the check.
//
// Opcodes and registers are looked up by name in the target's own tables,
// so this file needs no private AArch64 headers or generated .inc files and
// works with whichever libLLVM goc-llc links (it must include AArch64).
//
//===----------------------------------------------------------------------===//
#include "Goc/StackCheck.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

// AArch64CC condition codes (AArch64BaseInfo.h); the ISA encoding.
enum : int64_t { CondLO = 3, CondLS = 9 };
// UXTX #0 as an extended-register operand (AArch64_AM::getArithExtendImm).
constexpr int64_t ExtUXTX = 3 << 3;

struct Names {
  const TargetInstrInfo &TII;
  const TargetRegisterInfo &TRI;
  explicit Names(MachineFunction &MF)
      : TII(*MF.getSubtarget().getInstrInfo()), TRI(*MF.getSubtarget().getRegisterInfo()) {}

  const MCInstrDesc &op(StringRef N) const {
    static StringMap<unsigned> Cache;
    auto It = Cache.find(N);
    if (It == Cache.end()) {
      unsigned Found = 0;
      for (unsigned I = 0, E = TII.getNumOpcodes(); I < E && !Found; ++I)
        if (TII.getName(I) == N)
          Found = I;
      if (!Found)
        report_fatal_error("goc: AArch64 opcode " + Twine(N) + " not found");
      It = Cache.try_emplace(N, Found).first;
    }
    return TII.get(It->second);
  }
  Register reg(StringRef N) const {
    static StringMap<unsigned> Cache;
    auto It = Cache.find(N);
    if (It == Cache.end()) {
      unsigned Found = 0;
      for (unsigned I = 1, E = TRI.getNumRegs(); I < E && !Found; ++I)
        if (StringRef(TRI.getName(I)) == N)
          Found = I;
      if (!Found)
        report_fatal_error("goc: AArch64 register " + Twine(N) + " not found");
      It = Cache.try_emplace(N, Found).first;
    }
    return It->second;
  }
  Register x(unsigned I) const { return reg(I == 29 ? "FP" : I == 30 ? "LR" : "X" + std::to_string(I)); }
  Register q(unsigned I) const { return reg("Q" + std::to_string(I)); }
};

struct AArch64StackCheck : goc::StackCheckTarget {
  void emitCheck(MachineFunction &MF, MachineBasicBlock &Check, MachineBasicBlock &Stub,
                 const goc::CheckPlan &P, std::vector<goc::SPChange> &) const override {
    Names N(MF);
    if (P.ABI != goc::EntryABI::C)
      report_fatal_error("goc: the AArch64 stack check supports C entries only");
    Register SP = N.reg("SP"), XZR = N.reg("XZR"), G = N.x(28), X16 = N.x(16), X17 = N.x(17);
    int64_t Off = P.Profile->StackGuard0Off;
    int64_t Over = P.Frame - P.Profile->StackSmall;
    DebugLoc DL;
    // ldr x16, [x28, #Off] (unsigned offset, scaled by 8)
    BuildMI(&Check, DL, N.op("LDRXui"), X16).addReg(G).addImm(Off / 8);
    MachineBasicBlock *Cmp = &Check;
    switch (P.Tier) {
    case goc::FrameTier::Small:
      // cmp sp, x16 = subs xzr, sp, x16, uxtx
      BuildMI(Cmp, DL, N.op("SUBSXrx64"), XZR).addReg(SP).addReg(X16).addImm(ExtUXTX);
      break;
    case goc::FrameTier::Big:
      // Over <= StackBig-StackSmall = 3968: fits imm12.
      BuildMI(Cmp, DL, N.op("SUBXri"), X17).addReg(SP).addImm(Over).addImm(0);
      BuildMI(Cmp, DL, N.op("SUBSXrs"), XZR).addReg(X17).addReg(X16).addImm(0);
      break;
    case goc::FrameTier::Huge: {
      bool First = true;
      for (unsigned HW = 0; HW < 4; ++HW) {
        int64_t Chunk = (Over >> (16 * HW)) & 0xFFFF;
        if (!Chunk && !(First && HW == 3))
          continue;
        if (First)
          BuildMI(Cmp, DL, N.op("MOVZXi"), X17).addImm(Chunk).addImm(16 * HW);
        else
          BuildMI(Cmp, DL, N.op("MOVKXi"), X17).addReg(X17).addImm(Chunk).addImm(16 * HW);
        First = false;
      }
      // subs x17, sp, x17 ; b.lo stub (SP-(frame-StackSmall) wrapped)
      BuildMI(Cmp, DL, N.op("SUBSXrx64"), X17).addReg(SP).addReg(X17).addImm(ExtUXTX);
      BuildMI(Cmp, DL, N.op("Bcc")).addImm(CondLO).addMBB(&Stub);
      MachineBasicBlock *Next = MF.CreateMachineBasicBlock();
      MF.insert(std::next(Check.getIterator()), Next);
      MachineBasicBlock *Body = *llvm::find_if(Check.successors(),
                                               [&](MachineBasicBlock *S) { return S != &Stub; });
      Check.replaceSuccessor(Body, Next);
      Next->addSuccessor(Body);
      Next->addSuccessor(&Stub);
      for (const auto &L : Check.liveins())
        Next->addLiveIn(L);
      Cmp = Next;
      BuildMI(Cmp, DL, N.op("SUBSXrs"), XZR).addReg(X17).addReg(X16).addImm(0);
      break;
    }
    }
    BuildMI(Cmp, DL, N.op("Bcc")).addImm(CondLS).addMBB(&Stub);
  }

  MachineInstr *emitStub(MachineFunction &MF, MachineBasicBlock &Stub, MachineBasicBlock &Check,
                         const goc::CheckPlan &P, std::vector<goc::SPChange> &SP) const override {
    Names N(MF);
    Register Sp = N.reg("SP"), XZR = N.reg("XZR");
    DebugLoc DL;
    int64_t Delta = 0;
    // x0-x9 first (stubSlots counts words from the top), then the AAPCS
    // callee-saved x19-x28: Go's newstack/gogo preserve none of them.
    static const unsigned Pairs[10][2] = {{0, 1},   {2, 3},   {4, 5},   {6, 7},   {8, 9},
                                          {19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}};
    auto R = [&](unsigned I) { return N.x(I); };
    for (const auto &Pr : Pairs)
      // stp xA, xB, [sp, #-16]!
      SP.push_back({BuildMI(&Stub, DL, N.op("STPXpre"), Sp)
                        .addReg(R(Pr[0])).addReg(R(Pr[1])).addReg(Sp).addImm(-2),
                    Delta += 16});
    // q0-q7 above a 16-byte LR area: the unwinder reads this frame's
    // return address at 0(RSP) (AArch64Frame.h). x30 is still the caller's.
    // [sp] LR, [16,144) q0-q7, [144,208) d8-d15 (callee-saved halves).
    SP.push_back({BuildMI(&Stub, DL, N.op("SUBXri"), Sp).addReg(Sp).addImm(208).addImm(0),
                  Delta += 208});
    BuildMI(&Stub, DL, N.op("STRXui")).addReg(N.x(30)).addReg(Sp).addImm(0);
    for (unsigned I = 0; I < 8; I += 2)
      BuildMI(&Stub, DL, N.op("STPQi")).addReg(N.q(I)).addReg(N.q(I + 1)).addReg(Sp).addImm(I + 1);
    for (unsigned I = 8; I < 16; I += 2) // stp dI, dI+1, [sp, #144+8*(I-8)] (scaled by 8)
      BuildMI(&Stub, DL, N.op("STPDi")).addReg(N.reg("D" + std::to_string(I)))
          .addReg(N.reg("D" + std::to_string(I + 1))).addReg(Sp).addImm(18 + (I - 8));
    // mov x3, x30 = orr x3, xzr, x30
    BuildMI(&Stub, DL, N.op("ORRXrs"), N.x(3)).addReg(XZR).addReg(N.x(30)).addImm(0);
    MachineInstr *Call = BuildMI(&Stub, DL, N.op("BL")).addExternalSymbol(P.Profile->Morestack);
    for (int I = 6; I >= 0; I -= 2)
      BuildMI(&Stub, DL, N.op("LDPQi")).addReg(N.q(I), RegState::Define)
          .addReg(N.q(I + 1), RegState::Define).addReg(Sp).addImm(I + 1);
    for (unsigned I = 8; I < 16; I += 2)
      BuildMI(&Stub, DL, N.op("LDPDi")).addReg(N.reg("D" + std::to_string(I)), RegState::Define)
          .addReg(N.reg("D" + std::to_string(I + 1)), RegState::Define).addReg(Sp).addImm(18 + (I - 8));
    SP.push_back({BuildMI(&Stub, DL, N.op("ADDXri"), Sp).addReg(Sp).addImm(208).addImm(0),
                  Delta -= 208});
    for (int I = 9; I >= 0; --I)
      // ldp xA, xB, [sp], #16
      SP.push_back({BuildMI(&Stub, DL, N.op("LDPXpost"), Sp)
                        .addReg(R(Pairs[I][0]), RegState::Define)
                        .addReg(R(Pairs[I][1]), RegState::Define)
                        .addReg(Sp).addImm(2),
                    Delta -= 16});
    assert(Delta == 0 && "unbalanced morestack block");
    BuildMI(&Stub, DL, N.op("B")).addMBB(&Check);
    return Call;
  }

  std::vector<goc::StubSlot> stubSlots(const goc::CheckPlan &) const override {
    // The first STP (x0, x1) lands at entry SP - 16: x0 is word 1 below
    // varp (entry SP - 8) and x1 is varp itself, which copystack adjusts on
    // its own ("saved bp"). Pair k (x2k, x2k+1) is words 2k+1 and 2k.
    std::vector<goc::StubSlot> S;
    for (unsigned I = 0; I < 8; ++I)
      if (I != 1)
        S.push_back({I, I % 2 == 0 ? I + 1 : I - 1});
    return S;
  }

  void emitTrap(MachineFunction &MF, MachineBasicBlock &MBB) const override {
    Names N(MF);
    BuildMI(&MBB, DebugLoc(), N.op("BRK")).addImm(1);
  }
};

} // namespace

std::unique_ptr<goc::StackCheckTarget> goc::createAArch64StackCheckTarget() {
  return std::make_unique<AArch64StackCheck>();
}
