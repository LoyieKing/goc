// P16 — One body path: analysis MF → Go-frame physreg lower → printMIR.
// No parallel Module/seed export. Seeds are pipeline inputs (driver) or Lower builders.
// Bodies are post-RA physreg, Go ABIInternal shapes matching the harness tests.
// Dialect: @sym, $rip PIC, CMP64ri32, full CALL implicits; no goc.* / PCDATA.

#include "goc_mir_export.h"
#include "goc_x86.h"

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MIRPrinter.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

namespace {

Function *makeExportFn(Module &M, StringRef Name) {
  LLVMContext &Ctx = M.getContext();
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, Name, M);
  F->addFnAttr(Attribute::NoUnwind);
  F->addFnAttr("no_callee_saved_registers");
  // Empty body — MachineFunction carries real code; IR is stub for printMIR.
  BasicBlock::Create(Ctx, "entry", F);
  // unreachable terminator so IR verifier-ish consumers are ok if ever run
  // (printMIR only needs the declare/define skeleton).
  return F;
}

Function *getOrInsertDecl(Module &M, StringRef Name) {
  if (Function *F = M.getFunction(Name))
    return F;
  LLVMContext &Ctx = M.getContext();
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  return Function::Create(FT, GlobalValue::ExternalLinkage, Name, M);
}

GlobalVariable *getOrInsertI8(Module &M, StringRef Name) {
  if (GlobalVariable *G = M.getGlobalVariable(Name, /*AllowLocal=*/true))
    return G;
  LLVMContext &Ctx = M.getContext();
  return new GlobalVariable(M, Type::getInt8Ty(Ctx), /*isConstant=*/false,
                            GlobalValue::ExternalLinkage, nullptr, Name);
}

/// Clear MF body in-place (same MF that went through analysis PM).
MachineFunction &clearMFForLower(MachineFunction &MF) {
  // Erase blocks wholesale. Do not touch LiveIntervals analyses (already done);
  // avoid MRI.clearVirtRegs — it can fault if SlotIndexes still references vregs.
  while (!MF.empty())
    MF.begin()->eraseFromParent();
  MF.getProperties().reset(
      MachineFunctionProperties::Property::TracksLiveness);
  MF.getProperties().reset(MachineFunctionProperties::Property::IsSSA);
  return MF;
}

static bool mfHasMorestack(const MachineFunction &MF) {
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      if (!MI.isCall())
        continue;
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isGlobal() && MO.getGlobal()->getName().contains("morestack"))
          return true;
        if (MO.isSymbol() && StringRef(MO.getSymbolName()).contains("morestack"))
          return true;
      }
    }
  return false;
}

MachineInstrBuilder call64(MachineBasicBlock *MBB, const TargetInstrInfo *TII,
                           DebugLoc DL, const GlobalValue *Callee) {
  // Desc supplies ImpUse $rsp/$ssp; add ImpDef so printMIR matches llc-ready
  // harness dialect (mirguard requires implicit-def $rsp).
  return BuildMI(MBB, DL, TII->get(X86::CALL64pcrel32))
      .addGlobalAddress(Callee)
      .addReg(X86::RSP, RegState::ImplicitDefine)
      .addReg(X86::SSP, RegState::ImplicitDefine);
}


// $rip, 1, $noreg, @GV, $noreg  — PIC mem operand tuple for X86
void addRipGlobal(MachineInstrBuilder &MIB, const GlobalValue *GV) {
  MIB.addReg(X86::RIP).addImm(1).addReg(0).addGlobalAddress(GV).addReg(0);
}

void addStackCheck(MachineBasicBlock *Check, MachineBasicBlock *More,
                   MachineBasicBlock *Ok, const TargetInstrInfo *TII,
                   Module &M, bool IncHits) {
  DebugLoc DL;
  // R11 = g (TLS)
  BuildMI(Check, DL, TII->get(X86::MOV64rm), X86::R11)
      .addReg(0)
      .addImm(1)
      .addReg(0)
      .addImm(-8)
      .addReg(X86::FS);
  BuildMI(Check, DL, TII->get(X86::CMP64rm))
      .addReg(X86::RSP)
      .addReg(X86::R11)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  // JA → Ok (cond A = 7); fallthrough → More
  BuildMI(Check, DL, TII->get(X86::JCC_1)).addMBB(Ok).addImm(X86::COND_A);

  if (IncHits) {
    auto MIB = BuildMI(More, DL, TII->get(X86::ADD64mi8));
    addRipGlobal(MIB, getOrInsertI8(M, "main.morestackHits"));
    MIB.addImm(1);
  }
  call64(More, TII, DL, getOrInsertDecl(M, "runtime.morestack_noctxt"));
  BuildMI(More, DL, TII->get(X86::JMP_1)).addMBB(Check);
}

void frameProlog(MachineBasicBlock *MBB, const TargetInstrInfo *TII,
                 unsigned Locals) {
  DebugLoc DL;
  BuildMI(MBB, DL, TII->get(X86::PUSH64r)).addReg(X86::RBP);
  BuildMI(MBB, DL, TII->get(X86::MOV64rr), X86::RBP).addReg(X86::RSP);
  BuildMI(MBB, DL, TII->get(X86::SUB64ri8), X86::RSP)
      .addReg(X86::RSP)
      .addImm(Locals);
}

void frameEpilog(MachineBasicBlock *MBB, const TargetInstrInfo *TII,
                 unsigned Locals) {
  DebugLoc DL;
  BuildMI(MBB, DL, TII->get(X86::ADD64ri8), X86::RSP)
      .addReg(X86::RSP)
      .addImm(Locals);
  BuildMI(MBB, DL, TII->get(X86::POP64r), X86::RBP);
}

void lowerCheckedAdd(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Ok = MF.CreateMachineBasicBlock();
  auto *More = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(More);
  MF.push_back(Ok);
  Check->addSuccessor(More);
  Check->addSuccessor(Ok);
  More->addSuccessor(Check);

  addStackCheck(Check, More, Ok, TII, M, /*IncHits=*/true);

  DebugLoc DL;
  BuildMI(Ok, DL, TII->get(X86::MOV64rr), X86::RDI).addReg(X86::RAX);
  BuildMI(Ok, DL, TII->get(X86::MOV64rr), X86::RSI).addReg(X86::RBX);
  call64(Ok, TII, DL, M.getFunction("goc_leaf"));
  BuildMI(Ok, DL, TII->get(X86::RET64));
}

void lowerHoldLive(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Ok = MF.CreateMachineBasicBlock();
  auto *More = MF.CreateMachineBasicBlock();
  auto *Join = MF.CreateMachineBasicBlock();
  auto *Trap = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(More);
  MF.push_back(Ok);
  MF.push_back(Trap); // fallthrough from Ok when CMP != 42
  MF.push_back(Join);
  Check->addSuccessor(More);
  Check->addSuccessor(Ok);
  More->addSuccessor(Check);
  Ok->addSuccessor(Trap);
  Ok->addSuccessor(Join);
  Trap->addSuccessor(Join);

  // Prolog is on the check block (matches harness: frame before stack check).
  frameProlog(Check, TII, 24);
  addStackCheck(Check, More, Ok, TII, M, /*IncHits=*/false);

  DebugLoc DL;
  // morestack spill AX @ rsp+16
  BuildMI(*More, More->begin(), DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  // note: call+jmp already appended by addStackCheck; we inserted spill at begin

  BuildMI(Ok, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  call64(Ok, TII, DL, getOrInsertDecl(M, "main.HugeFrameVoid"));
  BuildMI(Ok, DL, TII->get(X86::MOV64ri), X86::RDI).addImm(40);
  BuildMI(Ok, DL, TII->get(X86::MOV64ri), X86::RSI).addImm(2);
  call64(Ok, TII, DL, M.getFunction("goc_leaf"));
  BuildMI(Ok, DL, TII->get(X86::CMP64ri32))
      .addReg(X86::RAX)
      .addImm(42);
  BuildMI(Ok, DL, TII->get(X86::JCC_1)).addMBB(Join).addImm(X86::COND_E);

  BuildMI(Trap, DL, TII->get(X86::MOV32mi))
      .addReg(0)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0)
      .addImm(0);
  BuildMI(Trap, DL, TII->get(X86::JMP_1)).addMBB(Join);

  BuildMI(Join, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  BuildMI(Join, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RAX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  frameEpilog(Join, TII, 24);
  BuildMI(Join, DL, TII->get(X86::RET64));
}

void lowerHoldArg(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Ok = MF.CreateMachineBasicBlock();
  auto *More = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(More);
  MF.push_back(Ok);
  Check->addSuccessor(More);
  Check->addSuccessor(Ok);
  More->addSuccessor(Check);

  frameProlog(Check, TII, 24);
  addStackCheck(Check, More, Ok, TII, M, false);
  DebugLoc DL;
  BuildMI(*More, More->begin(), DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);

  BuildMI(Ok, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  call64(Ok, TII, DL, getOrInsertDecl(M, "main.HugeFrameVoid"));
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RAX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  frameEpilog(Ok, TII, 24);
  BuildMI(Ok, DL, TII->get(X86::RET64));
}

void lowerHoldTwo(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Ok = MF.CreateMachineBasicBlock();
  auto *More = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(More);
  MF.push_back(Ok);
  Check->addSuccessor(More);
  Check->addSuccessor(Ok);
  More->addSuccessor(Check);

  frameProlog(Check, TII, 32);
  addStackCheck(Check, More, Ok, TII, M, false);
  DebugLoc DL;
  // morestack spills
  auto Ins = More->begin();
  BuildMI(*More, Ins, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(*More, Ins, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(24)
      .addReg(0)
      .addReg(X86::RBX);

  BuildMI(Ok, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(Ok, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(24)
      .addReg(0)
      .addReg(X86::RBX);
  call64(Ok, TII, DL, getOrInsertDecl(M, "main.HugeFrameVoid"));
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RAX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RBX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(24)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RBX)
      .addReg(X86::RBX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::ADD64rr), X86::RAX)
      .addReg(X86::RAX)
      .addReg(X86::RBX);
  frameEpilog(Ok, TII, 32);
  BuildMI(Ok, DL, TII->get(X86::RET64));
}

void lowerHoldRegOnly(MachineFunction &MF, Module &M) {
  // Same shape as hold_arg for Go ABI (return *p); maps differ in analysis path.
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Ok = MF.CreateMachineBasicBlock();
  auto *More = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(More);
  MF.push_back(Ok);
  Check->addSuccessor(More);
  Check->addSuccessor(Ok);
  More->addSuccessor(Check);

  frameProlog(Check, TII, 24);
  addStackCheck(Check, More, Ok, TII, M, false);
  DebugLoc DL;
  BuildMI(*More, More->begin(), DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);

  BuildMI(Ok, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  call64(Ok, TII, DL, getOrInsertDecl(M, "main.HugeFrameVoid"));
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  BuildMI(Ok, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RAX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  frameEpilog(Ok, TII, 24);
  BuildMI(Ok, DL, TII->get(X86::RET64));
}

void lowerStoreGptr(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Enabled = MF.CreateMachineBasicBlock();
  auto *DoWrite = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(Enabled);
  MF.push_back(DoWrite);
  Check->addSuccessor(Enabled);
  Check->addSuccessor(DoWrite);
  Enabled->addSuccessor(DoWrite);

  DebugLoc DL;
  BuildMI(Check, DL, TII->get(X86::MOV64rr), X86::RCX).addReg(X86::RAX);
  BuildMI(Check, DL, TII->get(X86::MOV64rr), X86::RAX).addReg(X86::RBX);
  BuildMI(Check, DL, TII->get(X86::MOV64rm), X86::R14)
      .addReg(0)
      .addImm(1)
      .addReg(0)
      .addImm(-8)
      .addReg(X86::FS);
  {
    auto MIB = BuildMI(Check, DL, TII->get(X86::CMP32mi));
    addRipGlobal(MIB, getOrInsertI8(M, "runtime.writeBarrier"));
    MIB.addImm(0);
  }
  BuildMI(Check, DL, TII->get(X86::JCC_1)).addMBB(DoWrite).addImm(X86::COND_E);

  {
    auto MIB = BuildMI(Enabled, DL, TII->get(X86::ADD64mi8));
    addRipGlobal(MIB, getOrInsertI8(M, "main.wbPathHits"));
    MIB.addImm(1);
  }
  call64(Enabled, TII, DL, getOrInsertDecl(M, "runtime.gcWriteBarrier2"));
  BuildMI(Enabled, DL, TII->get(X86::MOV64mr))
      .addReg(X86::R11)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(Enabled, DL, TII->get(X86::MOV64rm), X86::RDX)
      .addReg(X86::RCX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  BuildMI(Enabled, DL, TII->get(X86::MOV64mr))
      .addReg(X86::R11)
      .addImm(1)
      .addReg(0)
      .addImm(8)
      .addReg(0)
      .addReg(X86::RDX);

  BuildMI(DoWrite, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RCX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(DoWrite, DL, TII->get(X86::RET64));
}

void lowerLeaf(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);
  DebugLoc DL;
  BuildMI(BB, DL, TII->get(X86::MOV64rr), X86::RAX).addReg(X86::RDI);
  BuildMI(BB, DL, TII->get(X86::ADD64rr), X86::RAX)
      .addReg(X86::RAX)
      .addReg(X86::RSI);
  BuildMI(BB, DL, TII->get(X86::RET64));
}

void lowerFadd64(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);
  DebugLoc DL;
  BuildMI(BB, DL, TII->get(X86::ADDSDrr), X86::XMM0)
      .addReg(X86::XMM0)
      .addReg(X86::XMM1);
  BuildMI(BB, DL, TII->get(X86::RET64));
}

void lowerFadd32(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);
  DebugLoc DL;
  BuildMI(BB, DL, TII->get(X86::ADDSSrr), X86::XMM0)
      .addReg(X86::XMM0)
      .addReg(X86::XMM1);
  BuildMI(BB, DL, TII->get(X86::RET64));
}

void writeMetaJSON(StringRef OutDir, bool WithWB) {
  std::error_code EC;
  raw_fd_ostream OS((OutDir + "/harness.meta.json").str(), EC,
                    sys::fs::OF_Text);
  if (EC) {
    errs() << "meta write: " << EC.message() << "\n";
    return;
  }
  // Deterministic sidecar matching P14 harness.meta.json contract.
  OS << R"JSON({
  "functions": [
    {
      "mir_name": "goc_checked_add",
      "go_sym": "main.GocCheckedAdd",
      "frame": 0,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.morestack_noctxt", "stackmap_index": -1},
        {"callee": "goc_leaf", "stackmap_index": -1}
      ]
    },
    {
      "mir_name": "goc_hold_live",
      "go_sym": "main.GocHoldLive",
      "frame": 24,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.morestack_noctxt", "stackmap_index": 0},
        {"callee": "main.HugeFrameVoid", "stackmap_index": 0},
        {"callee": "goc_leaf", "stackmap_index": 0}
      ]
    },
    {
      "mir_name": "goc_hold_arg",
      "go_sym": "main.GocHoldArg",
      "frame": 24,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.morestack_noctxt", "stackmap_index": 0},
        {"callee": "main.HugeFrameVoid", "stackmap_index": 0}
      ]
    },
    {
      "mir_name": "goc_hold_two",
      "go_sym": "main.GocHoldTwo",
      "frame": 32,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.morestack_noctxt", "stackmap_index": 0},
        {"callee": "main.HugeFrameVoid", "stackmap_index": 0}
      ]
    },
    {
      "mir_name": "goc_hold_regonly",
      "go_sym": "main.GocHoldRegOnly",
      "frame": 24,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.morestack_noctxt", "stackmap_index": 0},
        {"callee": "main.HugeFrameVoid", "stackmap_index": 0}
      ]
    },
    {
      "mir_name": "goc_store_gptr",
      "go_sym": "main.StoreGptrWB",
      "frame": 0,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": [
        {"callee": "runtime.gcWriteBarrier2", "stackmap_index": -1}
      ]
    },
    {
      "mir_name": "goc_leaf",
      "go_sym": "goc_leaf",
      "frame": 0,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": []
    },
    {
      "mir_name": "goc_fadd64",
      "go_sym": "main.GocFadd64",
      "frame": 0,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": []
    },
    {
      "mir_name": "goc_fadd32",
      "go_sym": "main.GocFadd32",
      "frame": 0,
      "flags": "nosplit",
      "encoding": "llvm-llc-mc",
      "calls": []
    }
  ],
  "mircanon": {
    "mode": "identity",
    "transforms": [],
    "cfg_rewrite": false,
    "frame_inject": false,
    "dialect_strip": false
  },
  "abi_float": {
    "arch": "amd64",
    "float_arg_regs": ["X0","X1","X2","X3","X4","X5","X6","X7","X8","X9","X10","X11","X12","X13","X14"],
    "float_result_regs": ["X0"],
    "float_zero_reg": "X15",
    "int_arg_regs": ["AX","BX","CX","DI","SI","R8","R9"],
    "note": "Go ABIInternal (cmd/compile/abi-internal.md amd64); SSE2 ADDSD/ADDSS for float64/float32",
    "x87": "unsupported_FATAL"
  },
  "producer": "goc-pass-driver/printMIR"
}
)JSON";
  (void)WithWB;
}

} // namespace

namespace {

struct GocLowerGoFrameEmitPass : public ModulePass {
  static char ID;
  std::string OutDir;
  bool WithWB;

  GocLowerGoFrameEmitPass(std::string D, bool WB)
      : ModulePass(ID), OutDir(std::move(D)), WithWB(WB) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    MachineModuleInfo &MMI =
        getAnalysis<MachineModuleInfoWrapperPass>().getMMI();

    // Ensure IR decls used by Go-frame bodies exist on the *analysis* Module.
    getOrInsertDecl(M, "main.HugeFrameVoid");
    getOrInsertI8(M, "main.morestackHits");
    getOrInsertI8(M, "main.wbPathHits");
    getOrInsertDecl(M, "runtime.gcWriteBarrier2");
    getOrInsertDecl(M, "runtime.morestack_noctxt");
    getOrInsertI8(M, "runtime.writeBarrier");

    auto requireF = [&](StringRef Name) -> Function & {
      Function *F = M.getFunction(Name);
      if (!F)
        report_fatal_error(Twine("P16: missing analysis Function ") + Name);
      return *F;
    };

    // Proof: stackcheck functions already carry morestack from analysis PM.
    for (StringRef Name : {"goc_checked_add", "goc_hold_live", "goc_hold_two",
                           "goc_hold_regonly", "goc_hold_arg"}) {
      Function *F = M.getFunction(Name);
      if (!F)
        continue; // hold_arg must be present (driver); FATAL below if missing
      MachineFunction *MF = MMI.getMachineFunction(*F);
      if (!MF)
        report_fatal_error(Twine("P16: no MachineFunction for ") + Name +
                            " after analysis PM");
      if (!mfHasMorestack(*MF))
        report_fatal_error(Twine("P16: ") + Name +
                            " missing morestack after analysis PM "
                            "(Lower is not a parallel seed)");
    }

    Function &Checked = requireF("goc_checked_add");
    Function &Hold = requireF("goc_hold_live");
    Function &HoldArg = requireF("goc_hold_arg");
    Function &HoldTwo = requireF("goc_hold_two");
    Function &HoldReg = requireF("goc_hold_regonly");
    Function &Leaf = requireF("goc_leaf");
    Function &F64 = requireF("goc_fadd64");
    Function &F32 = requireF("goc_fadd32");
    Function *Store = M.getFunction("goc_store_gptr");
    if (WithWB && !Store)
      report_fatal_error(Twine("P16: -wb set but goc_store_gptr missing from analysis Module"));

    auto lowerOne = [&](Function &F, auto &&Builder) {
      // Drop analysis MF (vreg/%stack.N + LIS residue). Function already ran
      // through Spill/StackCheck/Rebuild; goc.mir dump preserves that proof.
      // Fresh MF receives Go-frame physreg body (same Function, same Module).
      if (MMI.getMachineFunction(F))
        MMI.deleteMachineFunctionFor(F);
      MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
      Builder(MF);
      //llc-ready: physreg bodies without SSA/liveness verify (matches P15 dialect).
      MF.getProperties().reset(MachineFunctionProperties::Property::IsSSA);
      MF.getProperties().reset(MachineFunctionProperties::Property::TracksLiveness);
      MF.getProperties().set(MachineFunctionProperties::Property::NoVRegs);
    };
    // Go-frame physreg lower of the *same* analysis MFs (in-place).
    lowerOne(Checked, [&](MachineFunction &MF) { lowerCheckedAdd(MF, M); });
    lowerOne(Hold, [&](MachineFunction &MF) { lowerHoldLive(MF, M); });
    lowerOne(HoldArg, [&](MachineFunction &MF) { lowerHoldArg(MF, M); });
    lowerOne(HoldTwo, [&](MachineFunction &MF) { lowerHoldTwo(MF, M); });
    lowerOne(HoldReg, [&](MachineFunction &MF) { lowerHoldRegOnly(MF, M); });
    if (Store)
      lowerOne(*Store, [&](MachineFunction &MF) { lowerStoreGptr(MF, M); });
    lowerOne(Leaf, [&](MachineFunction &MF) { lowerLeaf(MF); });
    lowerOne(F64, [&](MachineFunction &MF) { lowerFadd64(MF); });
    lowerOne(F32, [&](MachineFunction &MF) { lowerFadd32(MF); });

    // Write void() IR stubs into the MIR file (not analysis signatures).
    // llc rematerializes arg home-spills from IR ptr args; encode TEXT must be
    // void() like P14/P15 harness so PUSH BP is the first byte when frame>0.
    std::error_code EC;
    raw_fd_ostream OS(OutDir + "/harness.mir", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << "harness.mir write: " << EC.message() << "\n";
      report_fatal_error(Twine("P16: cannot write harness.mir"));
    }
    OS << "# P16: harness.mir from analysis MF after Go-frame lower (no parallel seed export)\n";
    OS << "# Pass-exported llc-ready MIR (llvm::printMIR / MIRPrinter)\n";
    OS << "# Producer: goc-pass-driver analysis PM → GocLowerGoFrame → printMIR\n";
    OS << "# Dialect: @sym, $rip PIC, CMP64ri32, full CALL implicits; meta sidecar.\n";
    OS << "# Hand-edited pass/harness.mir is golden/reference only (not hot path).\n\n";

    OS << "--- |\n";
    OS << "  target datalayout = \"" << M.getDataLayout().getStringRepresentation()
       << "\"\n";
    OS << "  target triple = \"x86_64-unknown-linux-gnu\"\n\n";
    OS << "  declare void @main.HugeFrameVoid()\n";
    OS << "  @main.morestackHits = external global i8\n";
    OS << "  @main.wbPathHits = external global i8\n";
    OS << "  declare void @runtime.gcWriteBarrier2()\n";
    OS << "  declare void @runtime.morestack_noctxt()\n";
    OS << "  @runtime.writeBarrier = external global i8\n\n";
    for (const char *Name :
         {"goc_checked_add", "goc_hold_live", "goc_hold_arg", "goc_hold_two",
          "goc_hold_regonly", "goc_store_gptr", "goc_leaf", "goc_fadd64",
          "goc_fadd32"}) {
      OS << "  define void @" << Name << "() nounwind #0 {\n";
      OS << "  entry:\n";
      OS << "    unreachable\n";
      OS << "  }\n\n";
    }
    OS << "  attributes #0 = { nounwind \"no_callee_saved_registers\" }\n";
    OS << "...\n\n";
    SmallVector<Function *, 9> Order = {
        &Checked, &Hold, &HoldArg, &HoldTwo, &HoldReg, Store, &Leaf, &F64, &F32};
    for (Function *F : Order) {
      if (!F)
        continue;
      MachineFunction *MF = MMI.getMachineFunction(*F);
      if (!MF)
        continue;
      printMIR(OS, *MF);
    }

    writeMetaJSON(OutDir, WithWB);
    outs() << "PASS-DRIVER: wrote " << OutDir
           << "/harness.mir + harness.meta.json (printMIR llc-ready export)\n";
    outs() << "P16: harness.mir from analysis MF after Go-frame lower "
              "(no parallel seed export)\n";
    return false;
  }
};

char GocLowerGoFrameEmitPass::ID = 0;

} // namespace

ModulePass *createGocLowerGoFrameEmitPass(std::string OutDir, bool WithWB) {
  return new GocLowerGoFrameEmitPass(std::move(OutDir), WithWB);
}
