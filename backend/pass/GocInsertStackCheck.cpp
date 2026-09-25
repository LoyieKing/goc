#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/ADT/SmallVector.h"
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

constexpr unsigned kStackguard0Offset = 16;

struct GocInsertStackCheck : public MachineFunctionPass {
  static char ID;
  std::vector<std::string> *RecipeOut = nullptr;

  GocInsertStackCheck() : MachineFunctionPass(ID) {}
  explicit GocInsertStackCheck(std::vector<std::string> *Out)
      : MachineFunctionPass(ID), RecipeOut(Out) {}

  StringRef getPassName() const override { return "Goc Insert Stack Check"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Mutates CFG (morestack loop). P8: CFG only — morestack spills are done
    // by GocRebuildLISAfterStackCheck (safe liveness rebuild after this pass).
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMF(MachineFunction &MF) { return runOnMachineFunction(MF); }

  bool runOnMachineFunction(MachineFunction &MF) override {
    Function &F = MF.getFunction();
    if (!F.hasFnAttribute("goc-stackcheck"))
      return false;
    if (MF.empty())
      return false;

    const X86InstrInfo *TII = goc::x86TII(MF);
    const unsigned OpcMOV64rm = X86::MOV64rm;
    const unsigned OpcCMP64rm = X86::CMP64rm;
    const unsigned OpcJCC_1 = X86::JCC_1;
    const unsigned OpcCALL64 = X86::CALL64pcrel32;
    const unsigned OpcJMP_1 = X86::JMP_1;
    const unsigned R11 = X86::R11;
    const unsigned RSP = X86::RSP;
    const unsigned FS = X86::FS;
    const unsigned CondBE = X86::COND_BE;
    const char *MiPath = "real_x86_opcodes";
    const char *ApiNote = "api=X86InstrInfo";

    MachineBasicBlock &Entry = MF.front();
    MachineBasicBlock *CheckMBB = &Entry;
    MachineBasicBlock *OkMBB = MF.CreateMachineBasicBlock();
    MachineBasicBlock *MoreMBB = MF.CreateMachineBasicBlock();

    // Insert the empty blocks first: MF insertion walks each block's
    // instructions to (re)register their reg operands on the MRI use lists, so
    // instructions moved by splice (already listed) must not be present yet.
    auto CheckIt = CheckMBB->getIterator();
    MF.insert(++CheckIt, OkMBB);
    MF.push_back(MoreMBB);

    OkMBB->splice(OkMBB->end(), CheckMBB, CheckMBB->begin(), CheckMBB->end());
    OkMBB->transferSuccessorsAndUpdatePHIs(CheckMBB);

    CheckMBB->addSuccessor(OkMBB);
    CheckMBB->addSuccessor(MoreMBB);
    MoreMBB->addSuccessor(CheckMBB);

    DebugLoc DL;

    // R11 = g so ABIInternal arg regs (AX/BX/…) survive check.
    BuildMI(CheckMBB, DL, TII->get(OpcMOV64rm), R11)
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(-8)
        .addReg(FS);

    BuildMI(CheckMBB, DL, TII->get(OpcCMP64rm))
        .addReg(RSP)
        .addReg(R11)
        .addImm(1)
        .addReg(0)
        .addImm(static_cast<int64_t>(kStackguard0Offset))
        .addReg(0);

    BuildMI(CheckMBB, DL, TII->get(OpcJCC_1))
        .addMBB(MoreMBB)
        .addImm(CondBE);

    // P8: morestack block is CALL + JMP reentry only. Spills for the slow
    // path are inserted by GocRebuildLISAfterStackCheck after safe liveness
    // rebuild (not registry/patch-only here).
    BuildMI(MoreMBB, DL, TII->get(OpcCALL64))
        .addExternalSymbol("runtime.morestack_noctxt");

    BuildMI(MoreMBB, DL, TII->get(OpcJMP_1)).addMBB(CheckMBB);

    if (RecipeOut) {
      RecipeOut->push_back("fn=" + F.getName().str());
      RecipeOut->push_back(std::string("mi_path=") + MiPath);
      RecipeOut->push_back(ApiNote);
      RecipeOut->push_back(
          "op=MOV64rm dst=R11 mem=FS:-8  # Go TLS → g (preserve ABIInternal arg regs)");
      RecipeOut->push_back("op=CMP64rm reg=RSP mem=16(R11)  # g.stackguard0");
      RecipeOut->push_back("op=JCC_1 cond=BE(6) target=morestack  # JBE");
      RecipeOut->push_back("op=CALL64pcrel32 sym=runtime.morestack_noctxt");
      RecipeOut->push_back("op=JMP_1 target=check_reentry");
      RecipeOut->push_back("stackguard0_offset=" +
                           std::to_string(kStackguard0Offset));
      RecipeOut->push_back("contract=p4_FRAME_LAYOUT_L");
      RecipeOut->push_back(
          "stackcheck_mode=cfg_only; morestack_spill=deferred_to_rebuild_lis");
    }

    errs() << "[GocInsertStackCheck] X86 MIs via " << ApiNote << " into "
           << F.getName() << " stackguard0=" << kStackguard0Offset
           << " (morestack spill deferred)\n";
    return true;
  }
};

char GocInsertStackCheck::ID = 0;

struct GocInsertStackCheckRunner : GocPassRunner {
  std::vector<std::string> *Out;
  explicit GocInsertStackCheckRunner(std::vector<std::string> *O) : Out(O) {}
  bool runOnMF(MachineFunction &MF) override {
    GocInsertStackCheck P(Out);
    return P.runOnMF(MF);
  }
};

} // namespace

GocPassRunner *createGocInsertStackCheckRunner(std::vector<std::string> *Out) {
  return new GocInsertStackCheckRunner(Out);
}

MachineFunctionPass *
createGocInsertStackCheckPass(std::vector<std::string> *RecipeOut) {
  return new GocInsertStackCheck(RecipeOut);
}
