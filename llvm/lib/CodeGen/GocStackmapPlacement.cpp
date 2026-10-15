//===- GocStackmapPlacement.cpp - drop STACKMAPs not followed by a CALL ----===//
//
#include "Goc/MachinePasses.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace llvm;

namespace {
// Each llvm.experimental.stackmap goc-stackmap/goc-reanchor emit names the
// roots live across the CALL that follows it; GocObjTables attaches the record
// to the first CALL after it in layout order. That is only sound when the
// STACKMAP and its CALL stay in one machine block. Two things break it:
//  - Tail merging (BranchFolding) hoists a CALL shared by two blocks into a
//    common tail and leaves each STACKMAP before a JMP; the pcdata at the
//    merged CALL would then name another path's roots. goc-llc forces
//    -enable-tail-merge=false (see main), so this cannot happen.
//  - goc-stackmap treats some intrinsics as safepoints that ISel expands
//    inline (llvm.umul.with.overflow, ...). No CALL follows, so the record
//    would be attached to whatever CALL comes next in *layout*, possibly on
//    a path where its roots are uninitialized. Nothing can grow the stack
//    between such a STACKMAP and the end of its block, so it is deleted.
unsigned TotalStrayStackmaps = 0;
struct GocStackmapPlacement : MachineFunctionPass {
  static char ID;
  GocStackmapPlacement() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "goc stackmap placement"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override {
    SmallVector<MachineInstr *, 8> Stray;
    for (MachineBasicBlock &MBB : MF)
      for (auto It = MBB.begin(), E = MBB.end(); It != E; ++It) {
        if (It->getOpcode() != TargetOpcode::STACKMAP)
          continue;
        bool Ok = false;
        for (auto J = std::next(It); J != E; ++J) {
          if (J->isTerminator() || J->isReturn())
            break;
          if (J->isCall() && J->getOpcode() != TargetOpcode::STACKMAP) {
            Ok = true;
            break;
          }
        }
        if (!Ok)
          Stray.push_back(&*It);
      }
    for (MachineInstr *MI : Stray) {
      if (std::getenv("GOC_STACKMAP_PLACEMENT_TRACE"))
        errs() << "goc-llc: drop stray STACKMAP id " << MI->getOperand(0).getImm()
               << " in " << MF.getName() << "\n";
      MI->eraseFromParent();
    }
    TotalStrayStackmaps += Stray.size();
    return !Stray.empty();
  }
};
char GocStackmapPlacement::ID = 0;
} // namespace

MachineFunctionPass *goc::createGocStackmapPlacementPass() { return new GocStackmapPlacement(); }
