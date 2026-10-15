//===- GocX86FrameAddrFix.h - shared state of the frame-address fix -*- C++ -*-===//
//
// Internal to lib/Target/X86: the lattice (GocX86FrameAddrLattice.cpp) and the
// repair pass (GocX86FrameAddrFix.cpp). See the latter for the design.
//===----------------------------------------------------------------------===//
#ifndef GOC_X86FRAMEADDRFIX_H
#define GOC_X86FRAMEADDRFIX_H

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "Goc/AArch64Frame.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "Goc/StackCheck.h"
#include "Goc/SlotMD.h"
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <cstring>

namespace goc::fa {
using namespace llvm;


// Abstract value of a register unit / spill slot.
//   FA(b,c) exactly b+c, b in {RBP,RSP}               -> re-derive: LEA c(b)
//   D1      moves with the stack: a frame address plus an integer (&a[i]
//           hoisted by LICM), or a merge of different frame addresses
//                                                      -> rebase: += delta
//   MS      "maybe stack": a D1 value on some paths and a value that is not
//           a stack address on others (p = small ? buf : malloc(n); p++)
//                                                      -> rebase iff the
//           rebased value lies in this frame: [RSP, frame top)
//   NotFA   not computed from RBP/RSP in this function
//   Taint   frame-derived with unknown coefficient (p - x with x unknown,
//           p * k, ...). Cannot be repaired; counted and reported.
//   Bot     undefined / unvisited (join identity)
// delta = RBP after the call - RBP before it (goc.oldfp, below).
enum Kind : uint8_t { Bot = 0, NotFA, FA, D1, MS, Taint };
struct Val {
  Kind K = Bot;
  unsigned Base = 0;
  int64_t Off = 0;
  const MachineInstr *Org = nullptr; // diagnostics only
  // Exact copy of tracked slot Alias (>= 0), grouped by CopyId. Go adjusts the
  // slot; a CSR that still holds the copy does not, so the fix pass reloads it.
  int Alias = -1;
  uintptr_t CopyId = 0;
  bool operator==(const Val &O) const {
    if (K != O.K || Alias != O.Alias || CopyId != O.CopyId)
      return false;
    if (K == FA)
      return Base == O.Base && Off == O.Off;
    return true;
  }
  bool operator!=(const Val &O) const { return !(*this == O); }
  bool frameish() const { return K == FA || K == D1 || K == MS || K == Taint; }
  bool moves() const { return K == FA || K == D1; } // coefficient exactly 1
  bool repairable() const { return K == FA || K == D1 || K == MS; }
};
inline Val mk(Kind K) { Val V; V.K = K; return V; }
inline Val fa(unsigned B, int64_t O) { Val V; V.K = FA; V.Base = B; V.Off = O; return V; }
inline Val taint(const MachineInstr *Org) { Val V; V.K = Taint; V.Org = Org; return V; }
inline Val join(const Val &A, const Val &B) {
  if (A.K == Bot) return B;
  if (B.K == Bot) return A;
  Val R;
  if (A.K == B.K && (A.K != FA || (A.Base == B.Base && A.Off == B.Off)))
    R = A;
  else if (A.moves() && B.moves())
    R = mk(D1); // both move with the stack by the same delta
  else if ((A.repairable() || A.K == NotFA) && (B.repairable() || B.K == NotFA))
    R = mk(MS);
  else {
    R = mk(Taint);
    R.Org = A.K == Taint ? A.Org : B.Org;
  }
  // Different paths (or a kind change) are not one exact spill copy.
  if (A.K != B.K || A.Alias != B.Alias || A.CopyId != B.CopyId ||
      (A.K == FA && (A.Base != B.Base || A.Off != B.Off))) {
    R.Alias = -1;
    R.CopyId = 0;
  }
  return R;
}

struct State {
  std::vector<Val> Units; // per register unit
  std::vector<Val> Slots; // per tracked spill slot
  bool operator==(const State &O) const { return Units == O.Units && Slots == O.Slots; }
};

extern unsigned TotalUnrepairable, FnsUnrepairable, TotalRepairCalls;

struct GocFrameAddrFix : MachineFunctionPass {
  static char ID;
  GocFrameAddrFix() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "goc frame-address fix after calls"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  MachineFrameInfo *MFI = nullptr;
  DenseMap<int, unsigned> SlotIdx; // tracked FI -> index
  // Volatile reloads of root allocas keep the IR alloca on the memoperand
  // instead of a FixedStack pseudo-value. Map those allocas to the same index.
  DenseMap<const AllocaInst *, unsigned> AllocaSlot;
  SmallVector<int, 16> SlotFI;
  // Parallel to SlotFI. Root allocas are stack-map slots Go rewrites; the
  // repair walk reloads CSR copies and must not add delta to the slot again.
  SmallVector<uint8_t, 16> SlotIsRoot;
  unsigned NU = 0;
  struct Stats { unsigned Calls = 0, Lea = 0, Rebase = 0, Range = 0, Slot = 0, Derived = 0, Split = 0, Reload = 0; } St;

  static bool isFrameBase(unsigned R) { return R == X86::RBP || R == X86::RSP; }
  // 8-byte stack-map roots whose address does not escape to a callee. A CSR
  // that is an exact copy has to be reloaded: the morestack stub restores the
  // register, and Go has already adjusted the slot.
  static bool isCopiedRoot(const AllocaInst *AI) { return goc::isCopiedRoot(AI); }
  bool aliasesCopiedRoot(const Val &V) const {
    return V.Alias >= 0 && (unsigned)V.Alias < SlotIsRoot.size() &&
           SlotIsRoot[V.Alias];
  }
  static bool isSafepoint(const MachineInstr &MI) {
    return MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP;
  }

  // Tracked-slot index touched by MI through its memory operands, or -1.
  // Spill slots use a FixedStack pseudo-value. Root allocas (goc.arganchor
  // and the like) stay as the IR alloca on volatile loads and stores.
  int spillSlotOf(const MachineInstr &MI, uint64_t *Size = nullptr) const;

  // A load from a goc-stackmap/goc-reanchor root slot (goc.spill.root*,
  // goc.anchor*): the slot holds a stack address (or, for an argument anchor,
  // any pointer). "frame address - such a value" is a pointer difference.
  static bool loadsGocRoot(const MachineInstr &MI);

  Val readReg(const State &S, unsigned R) const;
  void writeReg(State &S, unsigned R, const Val &V) const;
  void invalidateBase(State &S, unsigned B, const MachineInstr &MI) const;

  // Memory address register operand indices (base, index) of MI, if any.
  void addrOps(const MachineInstr &MI, int &BaseOp, int &IndexOp) const;

  // Frame-ish value operands (not memory address registers) of MI.
  bool anyFrameValueOperand(const State &S, const MachineInstr &MI) const;

  // Transfer function for one non-safepoint instruction.
  void step(State &S, const MachineInstr &MI) const;

  // Call transfer: regmask clobbers become garbage, return values NotFA.
  void stepCall(State &S, const MachineInstr &MI) const;

  void derivedLive(MachineFunction &MF, StringRef What, const MachineInstr &Call,
                   const Val &V);

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace goc::fa

#endif
