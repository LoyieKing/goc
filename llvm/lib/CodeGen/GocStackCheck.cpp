//===- GocStackCheck.cpp - target-independent Go stack check -------------===//
//
// See Goc/StackCheck.h. This pass decides, the target hook emits.
//
//===----------------------------------------------------------------------===//
#include "Goc/StackCheck.h"

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

namespace goc {

static const AbiProfile Profiles[] = {
#define GOC_ABI_PROFILE(NAME, ARCH, PTR, GREG, SG0, SMALL, BIG, MORESTACK, INTR, FPR)            \
  {NAME, ARCH, PTR, GREG, SG0, SMALL, BIG, MORESTACK},
#include "Goc/AbiProfiles.def"
};

const AbiProfile *lookupProfile(StringRef Name) {
  for (const AbiProfile &P : Profiles)
    if (Name == P.Name)
      return &P;
  return nullptr;
}

StackCheckTarget::~StackCheckTarget() = default;

StringMap<FuncStackInfo> &stackInfo() {
  static StringMap<FuncStackInfo> M;
  return M;
}

// A call the function makes: a real call instruction, or a "call" written in
// inline assembly (the goc shims call Go and libc helpers that way).
static bool makesCalls(const MachineFunction &MF) {
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      if (MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP &&
          MI.getOpcode() != TargetOpcode::PATCHPOINT)
        return true;
      if (MI.isInlineAsm() && MI.getOperand(0).isSymbol() &&
          StringRef(MI.getOperand(0).getSymbolName()).contains("call"))
        return true;
    }
  return false;
}

namespace {

struct GocStackCheck : MachineFunctionPass {
  static char ID;
  StackCheckOptions Opts;
  std::unique_ptr<StackCheckTarget> Target;

  GocStackCheck(const StackCheckOptions &O, std::unique_ptr<StackCheckTarget> T)
      : MachineFunctionPass(ID), Opts(O), Target(std::move(T)) {}
  StringRef getPassName() const override { return "goc stack check"; }

  // A label emitted where it stands (EH_LABEL is a meta instruction).
  static std::string label(MachineFunction &MF, MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator At, StringRef Prefix) {
    MCSymbol *S = MF.getContext().createTempSymbol(Prefix);
    BuildMI(MBB, At, DebugLoc(), MF.getSubtarget().getInstrInfo()->get(TargetOpcode::EH_LABEL))
        .addSym(S);
    return S->getName().str();
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    FuncStackInfo Info;
    Info.ABI = Opts.CABI;
    int64_t Frame = MF.getFrameInfo().getStackSize();
    const AbiProfile &Prof = *Opts.Profile;
    // A C-entered function without calls whose frame fits StackSmall is
    // covered by its caller's guard (StackGuard leaves room for it): the
    // linker's nosplit check charges it instead. A frameless function needs
    // no check either.
    bool Leaf = Info.ABI == EntryABI::C && Frame <= Prof.StackSmall && !makesCalls(MF);
    std::string Name = MF.getTarget().getSymbol(&MF.getFunction())->getName().str();
    if (Frame <= 0 || Leaf) {
      Info.NoSplit = true;
      stackInfo()[Name] = std::move(Info);
      return false;
    }

    CheckPlan P;
    P.ABI = Info.ABI;
    P.Frame = Frame;
    P.FixedG = Opts.FixedG;
    P.Profile = &Prof;
    P.Tier = Frame <= Prof.StackSmall ? FrameTier::Small
             : Frame <= Prof.StackBig ? FrameTier::Big
                                      : FrameTier::Huge;

    MachineBasicBlock *Body = &MF.front();
    // The cold block goes last. If the last block could run into it, end
    // that block with a trap.
    MachineBasicBlock &Last = MF.back();
    MachineBasicBlock::reverse_iterator LI = Last.rbegin();
    while (LI != Last.rend() && LI->isMetaInstruction())
      ++LI;
    if (LI == Last.rend() || !LI->isBarrier())
      Target->emitTrap(MF, Last);

    MachineBasicBlock *Check = MF.CreateMachineBasicBlock();
    MachineBasicBlock *Stub = MF.CreateMachineBasicBlock();
    MF.insert(MF.begin(), Check);
    MF.push_back(Stub);
    for (const auto &LIn : Body->liveins()) {
      Check->addLiveIn(LIn);
      Stub->addLiveIn(LIn);
    }
    Check->addSuccessor(Body);
    Check->addSuccessor(Stub);
    Stub->addSuccessor(Check);

    std::vector<SPChange> SP;
    Target->emitCheck(MF, *Check, *Stub, P, SP);
    Info.MorestackCall = Target->emitStub(MF, *Stub, *Check, P, SP);
    Info.StubSlots = Target->stubSlots(P);
    for (const SPChange &C : SP) {
      MachineBasicBlock &MBB = *C.After->getParent();
      Info.SPChanges.push_back({label(MF, MBB, std::next(C.After->getIterator()), "goc_sp"), C.Delta});
    }
    Info.BodyStart = label(MF, *Body, Body->begin(), "goc_body");
    Info.StubStart = label(MF, *Stub, Stub->begin(), "goc_stub");
    Info.HasCheck = true;
    MF.RenumberBlocks();
    stackInfo()[Name] = std::move(Info);
    return true;
  }
};

char GocStackCheck::ID = 0;

} // namespace

MachineFunctionPass *createGocStackCheckPass(const StackCheckOptions &O,
                                             std::unique_ptr<StackCheckTarget> T) {
  return new GocStackCheck(O, std::move(T));
}

} // namespace goc
