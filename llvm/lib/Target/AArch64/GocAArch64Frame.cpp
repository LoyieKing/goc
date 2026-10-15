//===- GocAArch64Frame.cpp - LR at 0(RSP) for Go's arm64 unwinder --------===//
//
// See Goc/AArch64Frame.h.
//
//===----------------------------------------------------------------------===//
#include "Goc/AArch64Frame.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
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
  bool runOnMachineFunction(MachineFunction &MF) override {
    const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
    unsigned Setup = TII.getCallFrameSetupOpcode();
    unsigned Destroy = TII.getCallFrameDestroyOpcode();
    bool Changed = false;
    for (MachineBasicBlock &MBB : MF)
      for (MachineInstr &MI : MBB) {
        unsigned Opc = MI.getOpcode();
        if (Opc != Setup && Opc != Destroy)
          continue;
        int64_t Amt = MI.getOperand(0).getImm();
        if (Amt != 0)
          report_fatal_error("goc: " + MF.getName() +
                             ": arm64 calls with stack arguments are not supported "
                             "(0(RSP) holds the return address Go unwinds through)");
        MI.getOperand(0).setImm(LRArea);
        Changed = true;
      }
    if (Changed && MF.getFrameInfo().hasVarSizedObjects())
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

} // namespace

FunctionPass *goc::createA64CallFramePass() { return new GocA64CallFrame(); }
FunctionPass *goc::createA64SaveLRPass() { return new GocA64SaveLR(); }
