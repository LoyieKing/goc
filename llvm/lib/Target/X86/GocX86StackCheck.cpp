//===- GocX86StackCheck.cpp - x86-64 instructions for the Go stack check -===//
//
// The x86-64 hook of GocStackCheck (Goc/StackCheck.h). Only instruction
// selection lives here; the policy is in lib/CodeGen/GocStackCheck.cpp.
//
//   check (C entry; R10/R11 are free at a SysV entry):
//       movq %fs:-8, %r11              # g, unless g is fixed in R14
//     small: cmpq 16(%r11), %rsp
//     big:   leaq -(frame-StackSmall)(%rsp), %r10 ; cmpq 16(%r11), %r10
//     huge:  movq %rsp, %r10 ; subq $(frame-StackSmall), %r10 ; jb stub
//            cmpq 16(%r11), %r10
//       jbe stub
//   A Go ABIInternal entry passes arguments in R10/R11, so it uses R12/R13
//   (scratch in ABIInternal) instead.
//
//   cold block: morestack clobbers every register Go does not treat as
//   callee-saved, and C code expects RBX/R12-R15 to survive. Save the frame
//   pointer first, so the runtime sees a saved BP at varp, then RAX and the
//   six C argument registers, then RBX R12 R13 R15 R14 and XMM0-7; load g
//   into R14 for morestack; restore everything; jump back to the check.
//
//===----------------------------------------------------------------------===//
#include "Goc/StackCheck.h"

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "MCTargetDesc/X86MCTargetDesc.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

namespace {

struct X86StackCheck : goc::StackCheckTarget {
  static const TargetInstrInfo &tii(MachineFunction &MF) { return *MF.getSubtarget().getInstrInfo(); }

  // movq %fs:-8, Reg
  static void loadG(MachineFunction &MF, MachineBasicBlock &MBB, Register Reg) {
    BuildMI(&MBB, DebugLoc(), tii(MF).get(X86::MOV64rm), Reg)
        .addReg(0).addImm(1).addReg(0).addImm(-8).addReg(X86::FS);
  }

  void emitCheck(MachineFunction &MF, MachineBasicBlock &Check, MachineBasicBlock &Stub,
                 const goc::CheckPlan &P, std::vector<goc::SPChange> &) const override {
    const TargetInstrInfo &TII = tii(MF);
    bool Go = P.ABI == goc::EntryABI::GoInternal;
    Register G = Go ? X86::R13 : X86::R11;
    Register T = Go ? X86::R12 : X86::R10;
    loadG(MF, Check, G);
    int64_t Off = P.Profile->StackGuard0Off;
    int64_t Over = P.Frame - P.Profile->StackSmall;
    MachineBasicBlock *Cmp = &Check;
    switch (P.Tier) {
    case goc::FrameTier::Small:
      // cmpq 16(g), %rsp
      BuildMI(Cmp, DebugLoc(), TII.get(X86::CMP64rm)).addReg(X86::RSP)
          .addReg(G).addImm(1).addReg(0).addImm(Off).addReg(0);
      break;
    case goc::FrameTier::Big:
      BuildMI(Cmp, DebugLoc(), TII.get(X86::LEA64r), T)
          .addReg(X86::RSP).addImm(1).addReg(0).addImm(-Over).addReg(0);
      BuildMI(Cmp, DebugLoc(), TII.get(X86::CMP64rm)).addReg(T)
          .addReg(G).addImm(1).addReg(0).addImm(Off).addReg(0);
      break;
    case goc::FrameTier::Huge: {
      BuildMI(Cmp, DebugLoc(), TII.get(X86::MOV64rr), T).addReg(X86::RSP);
      BuildMI(Cmp, DebugLoc(), TII.get(X86::SUB64ri32), T).addReg(T).addImm(Over);
      BuildMI(Cmp, DebugLoc(), TII.get(X86::JCC_1)).addMBB(&Stub).addImm(X86::COND_B);
      // The compare needs a block of its own after the borrow branch.
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
      BuildMI(Cmp, DebugLoc(), TII.get(X86::CMP64rm)).addReg(T)
          .addReg(G).addImm(1).addReg(0).addImm(Off).addReg(0);
      break;
    }
    }
    BuildMI(Cmp, DebugLoc(), TII.get(X86::JCC_1)).addMBB(&Stub).addImm(X86::COND_BE);
  }

  MachineInstr *emitStub(MachineFunction &MF, MachineBasicBlock &Stub, MachineBasicBlock &Check,
                         const goc::CheckPlan &P, std::vector<goc::SPChange> &SP) const override {
    const TargetInstrInfo &TII = tii(MF);
    int64_t Delta = 0;
    auto Push = [&](Register R) {
      SP.push_back({BuildMI(&Stub, DebugLoc(), TII.get(X86::PUSH64r)).addReg(R), Delta += 8});
    };
    auto Pop = [&](Register R) {
      SP.push_back({BuildMI(&Stub, DebugLoc(), TII.get(X86::POP64r), R), Delta -= 8});
    };
    static const Register Args[] = {X86::RBP, X86::RAX, X86::RDI, X86::RSI, X86::RDX,
                                    X86::RCX, X86::R8,  X86::R9};
    static const Register Saved[] = {X86::RBX, X86::R12, X86::R13, X86::R15, X86::R14};
    static const Register Xmm[] = {X86::XMM0, X86::XMM1, X86::XMM2, X86::XMM3,
                                   X86::XMM4, X86::XMM5, X86::XMM6, X86::XMM7};
    for (Register R : Args)
      Push(R);
    for (Register R : Saved)
      Push(R);
    SP.push_back({BuildMI(&Stub, DebugLoc(), TII.get(X86::SUB64ri32), X86::RSP)
                      .addReg(X86::RSP).addImm(128),
                  Delta += 128});
    for (unsigned I = 0; I < 8; ++I)
      BuildMI(&Stub, DebugLoc(), TII.get(X86::MOVDQUmr))
          .addReg(X86::RSP).addImm(1).addReg(0).addImm(16 * I).addReg(0)
          .addReg(Xmm[I]);
    loadG(MF, Stub, X86::R14); // morestack wants g in R14
    MachineInstr *Call = BuildMI(&Stub, DebugLoc(), TII.get(X86::CALL64pcrel32))
                             .addExternalSymbol(P.Profile->Morestack);
    for (unsigned I = 0; I < 8; ++I)
      BuildMI(&Stub, DebugLoc(), TII.get(X86::MOVDQUrm), Xmm[I])
          .addReg(X86::RSP).addImm(1).addReg(0).addImm(16 * I).addReg(0);
    SP.push_back({BuildMI(&Stub, DebugLoc(), TII.get(X86::ADD64ri32), X86::RSP)
                      .addReg(X86::RSP).addImm(128),
                  Delta -= 128});
    for (int I = 4; I >= 0; --I)
      Pop(Saved[I]);
    for (int I = 7; I >= 0; --I)
      Pop(Args[I]);
    assert(Delta == 0 && "unbalanced morestack block");
    BuildMI(&Stub, DebugLoc(), TII.get(X86::JMP_1)).addMBB(&Check);
    return Call;
  }

  std::vector<goc::StubSlot> stubSlots(const goc::CheckPlan &P) const override {
    if (P.ABI != goc::EntryABI::C)
      return {};
    // Below the saved BP (varp): RAX, then RDI RSI RDX RCX R8 R9 = C
    // argument registers 0..5 at words 2..7.
    std::vector<goc::StubSlot> S;
    for (unsigned I = 0; I < 6; ++I)
      S.push_back({I, I + 2});
    return S;
  }

  void emitTrap(MachineFunction &MF, MachineBasicBlock &MBB) const override {
    BuildMI(&MBB, DebugLoc(), tii(MF).get(X86::TRAP));
  }
};

} // namespace

std::unique_ptr<goc::StackCheckTarget> goc::createX86StackCheckTarget() {
  return std::make_unique<X86StackCheck>();
}
