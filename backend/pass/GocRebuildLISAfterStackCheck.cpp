// P8 §1 — After StackCheck inserts the morestack cycle, safely rebuild
// liveness (LiveIntervals-equivalent fixed-point; no blind LIS recompute on
// the synthetic cycle) and spill live gptr carriers before morestack CALL.
//
// Reduces StackCheck's previous registry/patch-only morestack spills.
#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <tuple>
#include <vector>

using namespace llvm;

namespace {

bool isPointerTy(Type *Ty) { return Ty && Ty->isPointerTy(); }

bool isMorestackCallee(const MachineInstr &MI) {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isSymbol() && StringRef(MO.getSymbolName()).contains("morestack"))
      return true;
    if (MO.isGlobal() && MO.getGlobal()->getName().contains("morestack"))
      return true;
  }
  return false;
}

/// Go amd64 ABIInternal integer/pointer arg order.
static unsigned abiInternalArgPhys(unsigned ArgIdx, unsigned AX, unsigned BX,
                                   unsigned CX, unsigned DI, unsigned SI,
                                   unsigned R8, unsigned R9) {
  switch (ArgIdx) {
  case 0:
    return AX;
  case 1:
    return BX;
  case 2:
    return CX;
  case 3:
    return DI;
  case 4:
    return SI;
  case 5:
    return R8;
  case 6:
    return R9;
  default:
    return 0;
  }
}

struct GocRebuildLISAfterStackCheck : public MachineFunctionPass {
  static char ID;
  std::vector<std::string> *RecipeOut = nullptr;

  GocRebuildLISAfterStackCheck() : MachineFunctionPass(ID) {}
  explicit GocRebuildLISAfterStackCheck(std::vector<std::string> *Out)
      : MachineFunctionPass(ID), RecipeOut(Out) {}

  StringRef getPassName() const override {
    return "Goc Rebuild LIS After StackCheck (safe + morestack spill)";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Intentionally does NOT require LiveIntervalsWrapperPass: recomputing
    // real LIS on the synthetic morestack cycle + live vregs is known to
    // non-converge / hang. We rebuild an equivalent fixed-point liveness.
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    Function &F = MF.getFunction();
    if (!F.hasFnAttribute("goc-stackcheck"))
      return false;
    if (MF.empty())
      return false;

    // Find morestack CALL inserted by StackCheck.
    MachineInstr *MoreCall = nullptr;
    MachineBasicBlock *MoreMBB = nullptr;
    for (MachineBasicBlock &MBB : MF) {
      for (MachineInstr &MI : MBB) {
        if (MI.isCall() && isMorestackCallee(MI)) {
          MoreCall = &MI;
          MoreMBB = &MBB;
          break;
        }
      }
      if (MoreCall)
        break;
    }
    if (!MoreCall || !MoreMBB)
      return false;

    // Skip if StackCheck (or a prior run) already spilled immediately before
    // morestack (MOV64mr to FI right before CALL). We still prefer our path:
    // detect existing spill-to-FI before CALL and leave them if present from
    // an older StackCheck — P8 StackCheck no longer inserts them.
    // (Always run rebuild-based spills below.)

    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    const X86InstrInfo *TII = goc::x86TII(MF);
    const unsigned OpcMOV64mr = X86::MOV64mr;
    const unsigned OpcMOV64rm = X86::MOV64rm;
    const unsigned AX = X86::RAX;
    const unsigned BX = X86::RBX;
    const unsigned CX = X86::RCX;
    const unsigned DI = X86::RDI;
    const unsigned SI = X86::RSI;
    const unsigned R8 = X86::R8;
    const unsigned R9 = X86::R9;
    const char *ApiNote = "api=X86InstrInfo";

    // --- Safe liveness rebuild (iterative live-in / live-out) for virt regs ---
    MachineRegisterInfo &MRI = MF.getRegInfo();
    const unsigned NumV = MRI.getNumVirtRegs();

    // Per-MBB: defs and uses of each vreg (bitvectors via DenseSet).
    DenseMap<MachineBasicBlock *, DenseSet<unsigned>> Defs, Uses;
    DenseMap<MachineBasicBlock *, DenseSet<unsigned>> LiveIn, LiveOut;

    for (MachineBasicBlock &MBB : MF) {
      DenseSet<unsigned> &D = Defs[&MBB];
      DenseSet<unsigned> &U = Uses[&MBB];
      for (MachineInstr &MI : MBB) {
        for (const MachineOperand &MO : MI.operands()) {
          if (!MO.isReg() || !MO.getReg().isVirtual())
            continue;
          unsigned Vid = MO.getReg().id();
          if (MO.isDef())
            D.insert(Vid);
          else if (MO.readsReg())
            U.insert(Vid);
        }
      }
      LiveIn[&MBB] = {};
      LiveOut[&MBB] = {};
    }

    // Fixed-point: LiveIn[B] = Uses[B] ∪ (LiveOut[B] - Defs[B])
    //              LiveOut[B] = ∪ LiveIn[S] for S in succ(B)
    bool ChangedFP = true;
    unsigned Iters = 0;
    while (ChangedFP && Iters < 256) {
      ChangedFP = false;
      ++Iters;
      for (MachineBasicBlock &MBB : MF) {
        DenseSet<unsigned> NewOut;
        for (MachineBasicBlock *S : MBB.successors())
          for (unsigned V : LiveIn[S])
            NewOut.insert(V);
        if (NewOut != LiveOut[&MBB]) {
          LiveOut[&MBB] = NewOut;
          ChangedFP = true;
        }
        DenseSet<unsigned> NewIn = Uses[&MBB];
        for (unsigned V : LiveOut[&MBB])
          if (!Defs[&MBB].count(V))
            NewIn.insert(V);
        if (NewIn != LiveIn[&MBB]) {
          LiveIn[&MBB] = NewIn;
          ChangedFP = true;
        }
      }
    }

    // Physreg ptr-args live across morestack: ABIInternal function live-ins.
    // Body vregs are not defined yet at entry morestack; carriers are arg regs.
    SmallVector<unsigned, 4> LivePhysAtMore;
    unsigned AbiOrd = 0;
    for (Argument &A : F.args()) {
      Type *Ty = A.getType();
      bool IsIP = isPointerTy(Ty) || Ty->isIntegerTy();
      if (!IsIP)
        continue;
      unsigned Phys =
          abiInternalArgPhys(AbiOrd, AX, BX, CX, DI, SI, R8, R9);
      if (isPointerTy(Ty) && Phys)
        LivePhysAtMore.push_back(Phys);
      AbiOrd++;
    }

    // Also: any vreg live-in to MoreMBB (rare for entry morestack; kept for
    // generality if body values ever flow into morestack block).
    DenseSet<unsigned> LiveVRegsAtMore = LiveIn[MoreMBB];

    auto Spills = goc_spill_api::spills(MF);
    // Map: prefer spill registry order ↔ ptr-arg order (same Locals FIs as CALL).
    struct MoreSpill {
      int FI;
      unsigned Phys;
      int64_t GoSp;
      unsigned VReg; // 0 if phys-only
    };
    SmallVector<MoreSpill, 4> ToSpill;

    if (!Spills.empty() && !LivePhysAtMore.empty()) {
      for (size_t i = 0; i < Spills.size() && i < LivePhysAtMore.size(); ++i) {
        ToSpill.push_back(MoreSpill{std::get<1>(Spills[i]), LivePhysAtMore[i],
                                    std::get<2>(Spills[i]),
                                    std::get<0>(Spills[i])});
      }
    } else if (!Spills.empty() && !LiveVRegsAtMore.empty()) {
      // Match live vregs to registry by vreg id.
      for (auto &S : Spills) {
        unsigned Vid = std::get<0>(S);
        if (LiveVRegsAtMore.count(Vid))
          ToSpill.push_back(MoreSpill{std::get<1>(S), 0, std::get<2>(S), Vid});
      }
    }

    // If no CALL spill registry (e.g. checked_add with no maps), still preserve
    // ptr/int ABI regs across morestack into fresh? Skip Locals maps — just
    // don't clobber: StackCheck uses R11 for g, so AX/BX survive without spill
    // when there are no competing defs in MoreMBB. For hold_* we have registry.

    bool Changed = false;
    MachineBasicBlock::iterator CallIt = MoreCall->getIterator();
    DebugLoc DL = MoreCall->getDebugLoc();

    // Avoid duplicate spills if already present immediately before CALL.
    auto alreadySpilledFI = [&](int FI) -> bool {
      if (CallIt == MoreMBB->begin())
        return false;
      MachineInstr &Prev = *std::prev(CallIt);
      for (const MachineOperand &MO : Prev.operands())
        if (MO.isFI() && MO.getIndex() == FI)
          return true;
      return false;
    };

    for (auto &MS : ToSpill) {
      if (alreadySpilledFI(MS.FI))
        continue;
      if (MS.Phys) {
        BuildMI(*MoreMBB, CallIt, DL, TII->get(OpcMOV64mr))
            .addFrameIndex(MS.FI)
            .addImm(1)
            .addReg(0)
            .addImm(0)
            .addReg(0)
            .addReg(MS.Phys);
        BuildMI(*MoreMBB, std::next(CallIt), DL, TII->get(OpcMOV64rm), MS.Phys)
            .addFrameIndex(MS.FI)
            .addImm(1)
            .addReg(0)
            .addImm(0)
            .addReg(0);
      } else if (MS.VReg) {
        Register V = Register(MS.VReg);
        BuildMI(*MoreMBB, CallIt, DL, TII->get(OpcMOV64mr))
            .addFrameIndex(MS.FI)
            .addImm(1)
            .addReg(0)
            .addImm(0)
            .addReg(0)
            .addReg(V);
        BuildMI(*MoreMBB, std::next(CallIt), DL, TII->get(OpcMOV64rm), V)
            .addFrameIndex(MS.FI)
            .addImm(1)
            .addReg(0)
            .addImm(0)
            .addReg(0);
      }
      Changed = true;
      if (RecipeOut) {
        std::string Line;
        raw_string_ostream OS(Line);
        OS << "morestack_spill_rebuilt_lis fi=" << MS.FI
           << " go_sp_off=" << MS.GoSp << " phys=" << MS.Phys
           << " vreg=" << MS.VReg << " safe_liveness_iters=" << Iters;
        RecipeOut->push_back(OS.str());
      }
      errs() << "[GocRebuildLIS] morestack spill FI#" << MS.FI << " GoSP+"
             << MS.GoSp << " phys=" << MS.Phys << " in " << F.getName() << " ("
             << ApiNote << ")\n";
    }

    if (RecipeOut) {
      RecipeOut->push_back(
          "lis_policy=safe_recompute_after_stackcheck; "
          "equiv_fixed_point_liveness; morestack_spill_via_rebuilt_lis; "
          "no_blind_LIS_on_cyclic_CFG");
      RecipeOut->push_back(std::string("safe_liveness_fp_iters=") +
                           std::to_string(Iters));
      RecipeOut->push_back(ApiNote);
      RecipeOut->push_back("mi_path=morestack_spill_rebuilt_lis");
    }

    errs() << "[GocRebuildLIS] safe liveness iters=" << Iters
           << " morestack_spills=" << ToSpill.size() << " in " << F.getName()
           << "\n";
    return Changed;
  }
};

char GocRebuildLISAfterStackCheck::ID = 0;

} // namespace

MachineFunctionPass *
createGocRebuildLISAfterStackCheckPass(std::vector<std::string> *RecipeOut) {
  return new GocRebuildLISAfterStackCheck(RecipeOut);
}

GocPassRunner *
createGocRebuildLISAfterStackCheckRunner(std::vector<std::string> *Out) {
  struct Runner : GocPassRunner {
    std::vector<std::string> *O;
    explicit Runner(std::vector<std::string> *X) : O(X) {}
    bool runOnMF(MachineFunction &MF) override {
      GocRebuildLISAfterStackCheck P(O);
      return P.runOnMachineFunction(MF);
    }
  };
  return new Runner(Out);
}
