#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace llvm;

namespace {

struct GocExpandStoreGptr : public MachineFunctionPass {
  static char ID;
  std::vector<std::string> *RecipeOut = nullptr;

  GocExpandStoreGptr() : MachineFunctionPass(ID) {}
  explicit GocExpandStoreGptr(std::vector<std::string> *Out)
      : MachineFunctionPass(ID), RecipeOut(Out) {}

  StringRef getPassName() const override { return "Goc Expand Store Gptr"; }
  bool runOnMF(MachineFunction &MF) { return runOnMachineFunction(MF); }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    Function &F = MF.getFunction();
    // P18: bridge may set goc-color-wb (= gptr store) and/or goc-store-gptr.
    if (!F.hasFnAttribute("goc-store-gptr") && !F.hasFnAttribute("goc-color-wb"))
      return false;
    if (F.hasFnAttribute("goc-color-cptr-only"))
      return false;
    if (MF.empty())
      return false;

    const X86InstrInfo *TII = goc::x86TII(MF);
    const unsigned OpcMOV64rm = X86::MOV64rm;
    const unsigned OpcMOV64mr = X86::MOV64mr;
    const unsigned OpcCMP32mi = X86::CMP32mi;
    const unsigned OpcJCC_1 = X86::JCC_1;
    const unsigned OpcCALL64 = X86::CALL64pcrel32;
    const unsigned OpcADD64mi8 = X86::ADD64mi8;
    const unsigned RAX = X86::RAX;
    const unsigned RCX = X86::RCX;
    const unsigned RDX = X86::RDX;
    const unsigned R11 = X86::R11;
    const unsigned R14 = X86::R14;
    const unsigned FS = X86::FS;
    const unsigned CondE = X86::COND_E;
    const char *MiPath = "real_x86_opcodes_wb";
    const char *ApiNote = "api=X86InstrInfo";

    MachineBasicBlock &Entry = MF.front();
    MachineBasicBlock *CheckMBB = &Entry;
    MachineBasicBlock *DoWriteMBB = MF.CreateMachineBasicBlock();
    MachineBasicBlock *EnabledMBB = MF.CreateMachineBasicBlock();

    // Insert the empty blocks first: MF insertion walks each block's
    // instructions to (re)register their reg operands on the MRI use lists, so
    // instructions moved by splice (already listed) must not be present yet.
    auto Ins = CheckMBB->getIterator();
    ++Ins;
    MF.insert(Ins, EnabledMBB);
    Ins = EnabledMBB->getIterator();
    ++Ins;
    MF.insert(Ins, DoWriteMBB);

    DoWriteMBB->splice(DoWriteMBB->end(), CheckMBB, CheckMBB->begin(),
                       CheckMBB->end());
    DoWriteMBB->transferSuccessorsAndUpdatePHIs(CheckMBB);

    CheckMBB->addSuccessor(EnabledMBB);
    CheckMBB->addSuccessor(DoWriteMBB);
    EnabledMBB->addSuccessor(DoWriteMBB);

    DebugLoc DL;

    BuildMI(CheckMBB, DL, TII->get(OpcMOV64rm), R14)
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(-8)
        .addReg(FS);

    BuildMI(CheckMBB, DL, TII->get(OpcCMP32mi))
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addExternalSymbol("runtime.writeBarrier")
        .addReg(0)
        .addImm(0);

    BuildMI(CheckMBB, DL, TII->get(OpcJCC_1))
        .addMBB(DoWriteMBB)
        .addImm(CondE);

    BuildMI(EnabledMBB, DL, TII->get(OpcADD64mi8))
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addExternalSymbol("main.wbPathHits")
        .addReg(0)
        .addImm(1);

    BuildMI(EnabledMBB, DL, TII->get(OpcCALL64))
        .addExternalSymbol("runtime.gcWriteBarrier2");

    BuildMI(EnabledMBB, DL, TII->get(OpcMOV64mr))
        .addReg(R11)
        .addImm(1)
        .addReg(0)
        .addImm(0)
        .addReg(0)
        .addReg(RAX);

    BuildMI(EnabledMBB, DL, TII->get(OpcMOV64rm), RDX)
        .addReg(RCX)
        .addImm(1)
        .addReg(0)
        .addImm(0)
        .addReg(0);

    BuildMI(EnabledMBB, DL, TII->get(OpcMOV64mr))
        .addReg(R11)
        .addImm(1)
        .addReg(0)
        .addImm(8)
        .addReg(0)
        .addReg(RDX);

    BuildMI(*DoWriteMBB, DoWriteMBB->begin(), DL, TII->get(OpcMOV64mr))
        .addReg(RCX)
        .addImm(1)
        .addReg(0)
        .addImm(0)
        .addReg(0)
        .addReg(RAX);

    if (RecipeOut) {
      RecipeOut->push_back("fn=" + F.getName().str());
      RecipeOut->push_back(std::string("mi_path=") + MiPath);
      RecipeOut->push_back(ApiNote);
      if (F.hasFnAttribute("goc-color-driven"))
        RecipeOut->push_back("color_driven=1");
      if (F.hasFnAttribute("goc-color-wb"))
        RecipeOut->push_back("color_wb=gptr_store");
      RecipeOut->push_back("op=MOV64rm dst=R14 mem=FS:-8  # Go TLS → g");
      RecipeOut->push_back("op=CMP32mi mem=runtime.writeBarrier imm=0");
      RecipeOut->push_back("op=JCC_1 cond=E(4) target=dowrite  # JE");
      RecipeOut->push_back("op=ADD64mi8 mem=main.wbPathHits imm=1");
      RecipeOut->push_back("op=CALL64pcrel32 sym=runtime.gcWriteBarrier2");
      RecipeOut->push_back("op=MOV64mr mem=(R11) src=RAX  # buf[0]=new");
      RecipeOut->push_back("op=MOV64rm dst=RDX mem=(RCX)  # old");
      RecipeOut->push_back("op=MOV64mr mem=8(R11) src=RDX  # buf[1]=old");
      RecipeOut->push_back("op=MOV64mr mem=(RCX) src=RAX  # dowrite store");
      RecipeOut->push_back("wb=gcWriteBarrier2");
      RecipeOut->push_back("contract=p4_FRAME_LAYOUT_W");
    }

    errs() << "[GocExpandStoreGptr] X86 MIs via " << ApiNote << " into "
           << F.getName() << "\n";
    return true;
  }
};

char GocExpandStoreGptr::ID = 0;

struct GocExpandStoreGptrRunner : GocPassRunner {
  std::vector<std::string> *Out;
  explicit GocExpandStoreGptrRunner(std::vector<std::string> *O) : Out(O) {}
  bool runOnMF(MachineFunction &MF) override {
    GocExpandStoreGptr P(Out);
    return P.runOnMF(MF);
  }
};

} // namespace

GocPassRunner *createGocExpandStoreGptrRunner(std::vector<std::string> *Out) {
  return new GocExpandStoreGptrRunner(Out);
}

MachineFunctionPass *
createGocExpandStoreGptrPass(std::vector<std::string> *RecipeOut) {
  return new GocExpandStoreGptr(RecipeOut);
}
