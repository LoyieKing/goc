// goc-llc: llc-19 plus two machine passes: GocStackmapPlacement (drops
// STACKMAP records not followed by a CALL in their block, e.g. before an
// intrinsic that was expanded inline) and the post-RA GocFrameAddrFix.
//
// Why a driver: stock llc has no hook to insert a plugin MachineFunction pass
// into its codegen pipeline. This file rebuilds exactly what llc's
// LLVMTargetMachine::addPassesToEmitFile does (same TargetPassConfig, same
// cl::opt flags, same TargetOptions) and appends the pass as the very last
// machine pass before the AsmPrinter. With GOC_FRAMEADDR_FIX=0 the fix pass
// is not added (the placement pass still runs, and -enable-tail-merge=false
// is the default unless given explicitly).
//
// The problem it solves (see GocStackMap.cpp, rematFrameAddress): a goroutine
// stack is copied during a call (the callee's morestack prologue, or the Go
// runtime). A value "RBP+c" (the address of a local) that the register
// allocator kept in a callee-saved register, or in a spill slot, across that
// call still points into the old stack afterwards. The morestack stub and the
// callee epilogues restore callee-saved registers verbatim; Go's copier only
// rewrites stack words that a stack map marks. Previously goc-reanchor
// re-derived every such address with an opaque `asm sideeffect "leaq"` at each
// use after a safepoint; that blocked addressing-mode folding everywhere.
// goc-reanchor now emits a plain GEP and this pass restores the invariant on
// the final machine code:
//
//   After every call, no callee-saved register and no spill slot that is live
//   holds a frame address computed before the call.
//
// Safety argument.
//   1. The stack only moves inside a call: morestack runs in a callee's
//      prologue, and the runtime copies/shrinks a goroutine stack only while
//      it is stopped at a call. Between two calls RBP/RSP-relative addresses
//      are stable. (Same assumption the leaq asm made: it re-derived from RBP
//      at the use, not at the call.)
//   2. After a call returns, RBP and RSP are correct for the (possibly moved)
//      frame: Go adjusts saved frame pointers when it copies, and RSP comes
//      back from the new stack. So "LEA c(%rbp)" executed after the call is
//      the rebased value of the same local's address.
//   3. Caller-saved registers do not survive a call (regmask), so only
//      callee-saved GPRs (RBX, R12-R15) and spill slots can carry a stale
//      address across it. XMM registers are all caller-saved in SysV.
//   4. A forward analysis over the lattice below (Val/Kind) tracks, per
//      register unit and per spill slot, whether it holds exactly
//      FA(base, c) = base+c with base in {RBP, RSP} on every path (LEA
//      base+disp, MOV copies, ADD/SUB imm, full-width spill/reload), a value
//      that moves with the stack by an unknown offset (D1: &a[i], merges of
//      different frame addresses), or "maybe a stack address" (MS). After each
//      call, every live callee-saved register and live spill slot in state FA
//      is re-derived with LEA (a slot via the dead caller-saved R11 and a
//      store). D1/MS locations are rebased by delta = RBP_after - RBP_before
//      (RBP_before is saved in goc.oldfp, which is in no stack map, so a copy
//      moves it verbatim); MS only if the rebased value lies in this frame.
//      The rebase runs in a cold block entered only when delta != 0, so the
//      hot path costs one store and one compare per such call. Re-deriving or
//      rebasing a value a location already holds correctly is harmless even
//      where liveness is imprecise.
//   5. A value derived from a frame address with an unknown coefficient
//      (p - x, p * k) is "tainted" and cannot be repaired here. It is counted
//      and reported in a note (GOC_FRAMEADDR_TAINT=report lists them,
//      =fatal aborts the compile). For quickjs these are libregexp's
//      `sp - stack_buf` style differences that are re-added to a reloaded
//      base, i.e. offsets, not addresses; the same values are unrepaired in
//      the legacy asm mode too. goc-reanchor keeps frame addresses that must
//      survive a call in stack-map-adjusted anchor slots.
//   6. Any non-call write to RSP (dynamic alloca) or RBP invalidates FA
//      states based on it (they become tainted).
//   7. Addresses loaded from memory (a pointer stored in a local, a pointer
//      argument into the caller's frame) are not FA values; they remain the
//      job of goc-stackmap / goc-reanchor exactly as before.
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

#include <climits>
#include <cstdlib>
#include <optional>
#include <cstring>

using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;
static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input>"), cl::init("-"));
static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));
static cl::opt<char> OptLevel("O", cl::desc("Optimization level"), cl::Prefix, cl::init('2'));

namespace {

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
  bool operator==(const Val &O) const {
    if (K != O.K)
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
Val mk(Kind K) { Val V; V.K = K; return V; }
Val fa(unsigned B, int64_t O) { Val V; V.K = FA; V.Base = B; V.Off = O; return V; }
Val taint(const MachineInstr *Org) { Val V; V.K = Taint; V.Org = Org; return V; }
Val join(const Val &A, const Val &B) {
  if (A.K == Bot) return B;
  if (B.K == Bot) return A;
  if (A == B) return A;
  if (A.moves() && B.moves())
    return mk(D1); // both move with the stack by the same delta
  if ((A.repairable() || A.K == NotFA) && (B.repairable() || B.K == NotFA))
    return mk(MS);
  Val T = mk(Taint);
  T.Org = A.K == Taint ? A.Org : B.Org;
  return T;
}

struct State {
  std::vector<Val> Units; // per register unit
  std::vector<Val> Slots; // per tracked spill slot
  bool operator==(const State &O) const { return Units == O.Units && Slots == O.Slots; }
};

unsigned TotalUnrepairable = 0, FnsUnrepairable = 0, TotalRepairCalls = 0;

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
  DenseMap<int, unsigned> SlotIdx; // spill FI -> index
  SmallVector<int, 16> SlotFI;
  unsigned NU = 0;
  struct Stats { unsigned Calls = 0, Lea = 0, Rebase = 0, Range = 0, Slot = 0, Derived = 0, Split = 0; } St;

  static bool isFrameBase(unsigned R) { return R == X86::RBP || R == X86::RSP; }
  static bool isSafepoint(const MachineInstr &MI) {
    return MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP;
  }

  // Spill-slot index touched by MI through its memory operands, or -1.
  int spillSlotOf(const MachineInstr &MI, uint64_t *Size = nullptr) const {
    for (const MachineMemOperand *MMO : MI.memoperands()) {
      const PseudoSourceValue *PSV = MMO->getPseudoValue();
      if (!PSV)
        continue;
      if (auto *FS = dyn_cast<FixedStackPseudoSourceValue>(PSV)) {
        auto It = SlotIdx.find(FS->getFrameIndex());
        if (It != SlotIdx.end()) {
          if (Size)
            *Size = MMO->getSize().hasValue() ? MMO->getSize().getValue() : 0;
          return (int)It->second;
        }
      }
    }
    return -1;
  }

  // A load from a goc-stackmap/goc-reanchor root slot (goc.spill.root*,
  // goc.anchor*): the slot holds a stack address (or, for an argument anchor,
  // any pointer). "frame address - such a value" is a pointer difference.
  static bool loadsGocRoot(const MachineInstr &MI) {
    for (const MachineMemOperand *MMO : MI.memoperands())
      if (const Value *V = MMO->getValue())
        if (V->hasName() && (V->getName().starts_with("goc.spill.root") ||
                             V->getName().starts_with("goc.anchor") ||
                             V->getName().starts_with("goc.arganchor")))
          return true;
    return false;
  }

  Val readReg(const State &S, unsigned R) const {
    if (!R || R == X86::RIP || R == X86::EFLAGS)
      return mk(NotFA);
    if (isFrameBase(R))
      return fa(R, 0);
    Val V;
    bool First = true;
    for (MCRegUnit U : TRI->regunits(R)) {
      V = First ? S.Units[U] : join(V, S.Units[U]);
      First = false;
    }
    if (First || V.K == Bot)
      return mk(NotFA);
    return V;
  }
  void writeReg(State &S, unsigned R, const Val &V) const {
    if (!R || isFrameBase(R))
      return;
    for (MCRegUnit U : TRI->regunits(R))
      S.Units[U] = V;
  }
  void invalidateBase(State &S, unsigned B, const MachineInstr &MI) const {
    // RSP/RBP itself changed (dynamic alloca, epilogue): b+c can no longer be
    // re-derived from b, but the value still moves with the stack.
    for (Val &V : S.Units)
      if (V.K == FA && V.Base == B)
        V = mk(D1);
    for (Val &V : S.Slots)
      if (V.K == FA && V.Base == B)
        V = mk(D1);
  }

  // Memory address register operand indices (base, index) of MI, if any.
  void addrOps(const MachineInstr &MI, int &BaseOp, int &IndexOp) const {
    BaseOp = IndexOp = -1;
    const MCInstrDesc &D = MI.getDesc();
    int M = X86II::getMemoryOperandNo(D.TSFlags);
    if (M < 0)
      return;
    M += X86II::getOperandBias(D);
    BaseOp = M + X86::AddrBaseReg;
    IndexOp = M + X86::AddrIndexReg;
  }

  // Frame-ish value operands (not memory address registers) of MI.
  bool anyFrameValueOperand(const State &S, const MachineInstr &MI) const {
    int BaseOp, IndexOp;
    addrOps(MI, BaseOp, IndexOp);
    for (unsigned i = 0, e = MI.getNumOperands(); i != e; ++i) {
      const MachineOperand &MO = MI.getOperand(i);
      if (!MO.isReg() || !MO.isUse() || MO.isUndef() || !MO.getReg())
        continue;
      if ((int)i == BaseOp || (int)i == IndexOp)
        continue;
      if (MO.isImplicit() && isFrameBase(MO.getReg()))
        continue; // push/pop adjust RSP; they do not copy its value
      if (readReg(S, MO.getReg()).frameish())
        return true;
    }
    return false;
  }

  // Transfer function for one non-safepoint instruction.
  void step(State &S, const MachineInstr &MI) const {
    if (MI.isDebugInstr() || MI.isCFIInstruction() || MI.isLabel())
      return;
    unsigned Op = MI.getOpcode();
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.isDef() && MO.getReg().isPhysical()) {
        unsigned R = MO.getReg();
        if (TRI->regsOverlap(R, X86::RSP))
          invalidateBase(S, X86::RSP, MI);
        if (TRI->regsOverlap(R, X86::RBP))
          invalidateBase(S, X86::RBP, MI);
      }

    Val V = mk(NotFA);
    bool Exact = true;
    if (Op == TargetOpcode::IMPLICIT_DEF) {
      V = mk(Bot);
    } else if (MI.isInlineAsm() &&
               StringRef(MI.getOperand(InlineAsm::MIOp_AsmString).getSymbolName()) ==
                   "leaq $1, $0") {
      // GOC_FRAMEADDR_MODE=asm remat: def, then a 5-operand memory operand.
      V = taint(&MI);
      for (unsigned i = InlineAsm::MIOp_FirstOperand; i + 5 < MI.getNumOperands(); ++i) {
        const MachineOperand &F = MI.getOperand(i);
        if (!F.isImm())
          continue;
        InlineAsm::Flag Fl(F.getImm());
        if (!Fl.isMemKind())
          continue;
        const MachineOperand &Bs = MI.getOperand(i + 1 + X86::AddrBaseReg);
        const MachineOperand &Ix = MI.getOperand(i + 1 + X86::AddrIndexReg);
        const MachineOperand &Dp = MI.getOperand(i + 1 + X86::AddrDisp);
        if (Bs.isReg() && Ix.isReg() && !Ix.getReg() && Dp.isImm()) {
          Val VB = readReg(S, Bs.getReg());
          if (VB.K == FA)
            V = fa(VB.Base, VB.Off + Dp.getImm());
          else if (VB.repairable())
            V = VB.K == MS ? mk(MS) : mk(D1);
        }
        break;
      }
    } else if (Op == X86::LEA64r) {
      unsigned B = MI.getOperand(1 + X86::AddrBaseReg).getReg();
      int64_t Sc = MI.getOperand(1 + X86::AddrScaleAmt).getImm();
      unsigned I = MI.getOperand(1 + X86::AddrIndexReg).getReg();
      unsigned Sg = MI.getOperand(1 + X86::AddrSegmentReg).getReg();
      const MachineOperand &Disp = MI.getOperand(1 + X86::AddrDisp);
      Val VB = B ? readReg(S, B) : mk(NotFA);
      Val VI = I ? readReg(S, I) : mk(NotFA);
      if (!VB.frameish() && !VI.frameish())
        V = mk(NotFA);
      else if (Sg || !Disp.isImm())
        V = taint(&MI);
      else if (VB.K == FA && !I)
        V = fa(VB.Base, VB.Off + Disp.getImm());
      else if (VB.moves() && !VI.frameish())
        V = mk(D1); // &a[i]
      else if (!VB.frameish() && VI.moves() && Sc == 1)
        V = mk(D1);
      else if (VB.K == MS && !VI.frameish())
        V = mk(MS);
      else if (!VB.frameish() && VI.K == MS && Sc == 1)
        V = mk(MS);
      else
        V = taint(&MI);
    } else if (Op == X86::MOV64rr) {
      V = readReg(S, MI.getOperand(1).getReg());
    } else if ((Op == X86::ADD64ri32 || Op == X86::ADD64ri8 ||
                Op == X86::SUB64ri32 || Op == X86::SUB64ri8) &&
               MI.getOperand(2).isImm()) {
      Val VS = readReg(S, MI.getOperand(1).getReg());
      int64_t C = MI.getOperand(2).getImm();
      if (Op == X86::SUB64ri32 || Op == X86::SUB64ri8)
        C = -C;
      if (VS.K == FA)
        V = fa(VS.Base, VS.Off + C);
      else
        V = VS.frameish() ? VS : mk(NotFA);
    } else if (Op == X86::INC64r || Op == X86::DEC64r) {
      Val VS = readReg(S, MI.getOperand(1).getReg());
      if (VS.K == FA)
        V = fa(VS.Base, VS.Off + (Op == X86::INC64r ? 1 : -1));
      else
        V = VS.frameish() ? VS : mk(NotFA);
    } else if (Op == X86::ADD64rr || Op == X86::ADD64rm) {
      Val A = readReg(S, MI.getOperand(1).getReg());
      Val B = Op == X86::ADD64rr ? readReg(S, MI.getOperand(2).getReg()) : mk(NotFA);
      if (Op == X86::ADD64rm) {
        int Sl = spillSlotOf(MI);
        if (Sl >= 0 && S.Slots[Sl].frameish())
          B = S.Slots[Sl];
      }
      if (!A.frameish() && !B.frameish())
        V = mk(NotFA);
      else if ((A.moves() && !B.frameish()) || (!A.frameish() && B.moves()))
        V = mk(D1); // pointer + integer
      else if ((A.K == MS && !B.frameish()) || (!A.frameish() && B.K == MS))
        V = mk(MS);
      else
        V = taint(&MI);
    } else if (Op == X86::CMOV64rr || Op == X86::CMOV64rm) {
      // dst = cond ? src : dst -- one of the two values.
      Val A = readReg(S, MI.getOperand(1).getReg());
      Val B = mk(NotFA);
      if (Op == X86::CMOV64rr) {
        B = readReg(S, MI.getOperand(2).getReg());
      } else {
        int Sl = spillSlotOf(MI);
        if (Sl >= 0 && S.Slots[Sl].K != Bot)
          B = S.Slots[Sl];
      }
      V = join(A, B);
      if (V.K == Taint && !V.Org)
        V.Org = &MI;
    } else if (Op == X86::SUB64rr || Op == X86::SUB64rm) {
      Val A = readReg(S, MI.getOperand(1).getReg());
      Val B = mk(NotFA);
      bool BStackPtr = false;
      if (Op == X86::SUB64rr) {
        B = readReg(S, MI.getOperand(2).getReg());
      } else {
        int Sl = spillSlotOf(MI);
        if (Sl >= 0)
          B = S.Slots[Sl];
        BStackPtr = loadsGocRoot(MI);
      }
      if (!A.frameish() && !B.frameish())
        V = mk(NotFA);
      else if (A.repairable() && (B.repairable() || BStackPtr))
        V = mk(NotFA); // difference of two (maybe-)stack addresses: invariant
      else
        V = taint(&MI); // p - x: pointer or pointer difference?
    } else {
      Exact = false;
      int FI = 0;
      if (Register R = TII->isLoadFromStackSlotPostFE(MI, FI)) {
        int Sl = spillSlotOf(MI);
        if (Op == X86::MOV64rm && Sl >= 0 && MI.getOperand(0).getReg() == R) {
          V = S.Slots[Sl];
          if (V.K == Bot)
            V = mk(NotFA);
          Exact = true;
        }
      }
    }
    if (!Exact) {
      bool T = anyFrameValueOperand(S, MI);
      if (MI.mayLoad()) {
        int Sl = spillSlotOf(MI);
        if (Sl >= 0 && S.Slots[Sl].frameish())
          T = true;
      }
      V = T ? taint(&MI) : mk(NotFA);
    }
    // Spill slot writes.
    if (MI.mayStore()) {
      uint64_t Size = 0;
      int Sl = spillSlotOf(MI, &Size);
      if (Sl >= 0) {
        int FI = 0;
        Register R = TII->isStoreToStackSlotPostFE(MI, FI);
        if (R && Op == X86::MOV64mr && Size == 8 &&
            MFI->getObjectSize(SlotFI[Sl]) == 8) {
          S.Slots[Sl] = readReg(S, R);
        } else {
          // Partial or non-MOV write: tainted if it already was or if any
          // value operand (not the store's own address) is frame-derived.
          bool T = S.Slots[Sl].frameish() || anyFrameValueOperand(S, MI);
          S.Slots[Sl] = T ? taint(&MI) : mk(NotFA);
        }
      }
    }
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.isDef() && MO.getReg() && MO.getReg().isPhysical())
        writeReg(S, MO.getReg(), V);
  }

  // Call transfer: regmask clobbers become garbage, return values NotFA.
  void stepCall(State &S, const MachineInstr &MI) const {
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isRegMask()) {
        for (unsigned R = 1, e = TRI->getNumRegs(); R != e; ++R)
          if (MO.clobbersPhysReg(R) && !isFrameBase(R))
            writeReg(S, R, mk(Bot));
      } else if (MO.isReg() && MO.isDef() && MO.getReg() &&
                 !isFrameBase(MO.getReg())) {
        writeReg(S, MO.getReg(), mk(NotFA));
      }
    }
  }

  void derivedLive(MachineFunction &MF, StringRef What, const MachineInstr &Call,
                   const Val &V) {
    ++St.Derived;
    const char *Mode = std::getenv("GOC_FRAMEADDR_TAINT");
    bool Fatal = Mode && std::strcmp(Mode, "fatal") == 0;
    bool Quiet = !Mode || std::strcmp(Mode, "quiet") == 0;
    if (Quiet && !Fatal)
      return;
    std::string Msg;
    raw_string_ostream OS(Msg);
    OS << "goc-frameaddr-fix: " << MF.getName() << ": " << What
       << " holds an unrepairable frame-derived value across call to ";
    for (const MachineOperand &MO : Call.operands())
      if (MO.isGlobal()) OS << MO.getGlobal()->getName();
      else if (MO.isSymbol()) OS << MO.getSymbolName();
    OS << " in bb." << Call.getParent()->getNumber() << "\n";
    if (V.Org) {
      OS << "    tainted by: ";
      V.Org->print(OS, /*IsStandalone=*/true, /*SkipOpers=*/false, /*SkipDebugLoc=*/true);
    } else if (V.Base) {
      OS << "    merge of " << TRI->getName(V.Base) << "+" << V.Off << " with another value\n";
    } else {
      OS << "    (merge)\n";
    }
    if (!Fatal) {
      errs() << OS.str();
      return;
    }
    if (std::getenv("GOC_FRAMEADDR_DEBUG"))
      MF.print(errs());
    report_fatal_error(Twine(OS.str()));
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const char *E = std::getenv("GOC_FRAMEADDR_FIX");
    if (E && std::strcmp(E, "0") == 0)
      return false;
    TRI = MF.getSubtarget().getRegisterInfo();
    TII = MF.getSubtarget().getInstrInfo();
    MFI = &MF.getFrameInfo();
    NU = TRI->getNumRegUnits();
    SlotIdx.clear();
    SlotFI.clear();
    St = Stats();
    int OldFP = INT_MIN;
    for (int FI = MFI->getObjectIndexBegin(), FE = MFI->getObjectIndexEnd(); FI != FE; ++FI) {
      if (MFI->isDeadObjectIndex(FI))
        continue;
      if (MFI->isSpillSlotObjectIndex(FI)) {
        SlotIdx[FI] = SlotFI.size();
        SlotFI.push_back(FI);
      } else if (const AllocaInst *AI = MFI->getObjectAllocation(FI)) {
        if (AI->getName() == "goc.oldfp")
          OldFP = FI;
      }
    }
    unsigned NS = SlotFI.size();

    // ---- Backward liveness (register units + spill slots). ----
    unsigned NB = MF.getNumBlockIDs();
    std::vector<BitVector> LiveInU(NB, BitVector(NU)), LiveInS(NB, BitVector(NS));
    auto stepBack = [&](const MachineInstr &MI, BitVector &LU, BitVector &LS) {
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isRegMask()) {
          for (unsigned R = 1, e = TRI->getNumRegs(); R != e; ++R)
            if (MO.clobbersPhysReg(R))
              for (MCRegUnit U : TRI->regunits(R))
                LU.reset(U);
        } else if (MO.isReg() && MO.isDef() && MO.getReg()) {
          for (MCRegUnit U : TRI->regunits(MO.getReg()))
            LU.reset(U);
        }
      }
      if (MI.mayStore() && !MI.mayLoad()) {
        uint64_t Size = 0;
        int Sl = spillSlotOf(MI, &Size);
        if (Sl >= 0 && Size >= (uint64_t)MFI->getObjectSize(SlotFI[Sl]))
          LS.reset(Sl);
      }
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.isUse() && !MO.isUndef() && MO.getReg())
          for (MCRegUnit U : TRI->regunits(MO.getReg()))
            LU.set(U);
      if (MI.mayLoad()) {
        int Sl = spillSlotOf(MI);
        if (Sl >= 0)
          LS.set(Sl);
      }
    };
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (MachineBasicBlock *MBB : post_order(&MF)) {
        BitVector LU(NU), LS(NS);
        for (MachineBasicBlock *Succ : MBB->successors()) {
          LU |= LiveInU[Succ->getNumber()];
          LS |= LiveInS[Succ->getNumber()];
        }
        for (auto It = MBB->rbegin(); It != MBB->rend(); ++It)
          stepBack(*It, LU, LS);
        if (LU != LiveInU[MBB->getNumber()] || LS != LiveInS[MBB->getNumber()]) {
          LiveInU[MBB->getNumber()] = LU;
          LiveInS[MBB->getNumber()] = LS;
          Changed = true;
        }
      }
    }
    // Live sets right after each safepoint.
    DenseMap<const MachineInstr *, std::pair<BitVector, BitVector>> LiveAfter;
    for (MachineBasicBlock &MBB : MF) {
      BitVector LU(NU), LS(NS);
      for (MachineBasicBlock *Succ : MBB.successors()) {
        LU |= LiveInU[Succ->getNumber()];
        LS |= LiveInS[Succ->getNumber()];
      }
      for (auto It = MBB.rbegin(); It != MBB.rend(); ++It) {
        if (isSafepoint(*It))
          LiveAfter[&*It] = {LU, LS};
        stepBack(*It, LU, LS);
      }
    }
    if (LiveAfter.empty())
      return false;

    // ---- Forward must-analysis. ----
    State Init;
    Init.Units.assign(NU, mk(NotFA));
    // Spill slots are uninitialized at entry: identity for the join (a path
    // that reaches a reload without a spill reads garbage either way).
    Init.Slots.assign(NS, mk(Bot));
    State BotS;
    BotS.Units.assign(NU, mk(Bot));
    BotS.Slots.assign(NS, mk(Bot));
    std::vector<State> In(NB, BotS), Out(NB, BotS);
    std::vector<bool> Seen(NB, false);
    ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
    Changed = true;
    while (Changed) {
      Changed = false;
      for (MachineBasicBlock *MBB : RPOT) {
        unsigned N = MBB->getNumber();
        State S = MBB == &MF.front() ? Init : BotS;
        for (MachineBasicBlock *P : MBB->predecessors()) {
          if (!Seen[P->getNumber()])
            continue;
          const State &PO = Out[P->getNumber()];
          for (unsigned i = 0; i < NU; ++i) S.Units[i] = join(S.Units[i], PO.Units[i]);
          for (unsigned i = 0; i < NS; ++i) S.Slots[i] = join(S.Slots[i], PO.Slots[i]);
        }
        In[N] = S;
        for (MachineInstr &MI : *MBB) {
          if (isSafepoint(MI))
            stepCall(S, MI);
          else
            step(S, MI);
        }
        if (!Seen[N] || !(S == Out[N])) {
          Out[N] = std::move(S);
          Seen[N] = true;
          Changed = true;
        }
      }
    }

    // ---- Repair. ----
    static const unsigned CSRs[] = {X86::RBX, X86::R12, X86::R13, X86::R14, X86::R15};
    static const unsigned Scratch[] = {X86::R11, X86::R10, X86::R9, X86::R8,
                                       X86::RCX, X86::RSI, X86::RDI};
    const MachineRegisterInfo &MRI = MF.getRegInfo();
    const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
    auto frameRef = [&](int FI, Register &FrameReg) {
      return TFL->getFrameIndexReference(MF, FI, FrameReg).getFixed();
    };
    auto imm32 = [&](int64_t V) {
      if (V < INT32_MIN || V > INT32_MAX)
        report_fatal_error("goc-frameaddr-fix: offset out of range");
      return V;
    };
    // Top of this frame's addressable area relative to RBP: the return
    // address and the incoming stack arguments (byval, varargs area).
    // Fixed-object offsets are relative to the entry SP (= RBP + 8).
    int64_t TopOff = 16;
    for (int FI = MFI->getObjectIndexBegin(); FI < 0; ++FI)
      if (!MFI->isDeadObjectIndex(FI))
        TopOff = std::max<int64_t>(TopOff, 8 + MFI->getObjectOffset(FI) + MFI->getObjectSize(FI));
    bool AnalyzeOnly = E && std::strcmp(E, "analyze") == 0;
    const char *TraceFn = std::getenv("GOC_FRAMEADDR_TRACE");
    bool Trace = TraceFn && MF.getName() == TraceFn;
    bool Any = false;
    std::vector<MachineBasicBlock *> Order(RPOT.begin(), RPOT.end());
    for (MachineBasicBlock *Orig : Order) {
      State S = In[Orig->getNumber()];
      MachineBasicBlock *MBB = Orig;
      for (auto It = MBB->begin(); It != MBB->end();) {
        MachineInstr &MI = *It;
        if (!isSafepoint(MI)) {
          step(S, MI);
          if (Trace) {
            errs() << "  [bb." << MBB->getNumber() << "] ";
            for (const MachineOperand &MO : MI.operands())
              if (MO.isReg() && MO.isDef() && MO.getReg() && MO.getReg().isPhysical() &&
                  !MO.isImplicit()) {
                Val V = readReg(S, MO.getReg());
                errs() << TRI->getName(MO.getReg()) << "=" << "BNFDMT"[V.K] << " ";
              }
            MI.print(errs(), true, false, true);
          }
          ++It;
          continue;
        }
        stepCall(S, MI);
        auto &LA = LiveAfter[&MI];
        DebugLoc DL = MI.getDebugLoc();
        SmallVector<std::pair<unsigned, Val>, 4> Regs;
        SmallVector<std::pair<unsigned, Val>, 4> Slots;
        bool NeedDelta = false, NeedRange = false;
        for (unsigned R : CSRs) {
          if (MRI.isReserved(R))
            continue;
          bool Live = false, AnyFrame = false;
          for (MCRegUnit U : TRI->regunits(R)) {
            Live |= LA.first.test(U);
            AnyFrame |= S.Units[U].frameish();
          }
          if (!Live || !AnyFrame)
            continue;
          Val V = readReg(S, R);
          if (!V.repairable()) {
            derivedLive(MF, TRI->getName(R), MI, V);
            continue;
          }
          Regs.push_back({R, V});
          NeedDelta |= V.K != FA;
          NeedRange |= V.K == MS;
        }
        for (unsigned Sl = 0; Sl < NS; ++Sl) {
          const Val &V = S.Slots[Sl];
          if (!LA.second.test(Sl) || !V.frameish())
            continue;
          if (!V.repairable()) {
            derivedLive(MF, "spill slot", MI, V);
            continue;
          }
          Slots.push_back({Sl, V});
          NeedDelta |= V.K != FA;
          NeedRange |= V.K == MS;
        }
        if (Regs.empty() && Slots.empty()) {
          ++It;
          continue;
        }
        if (AnalyzeOnly) { // GOC_FRAMEADDR_FIX=analyze: count, do not rewrite
          for (auto &P : Regs)
            (P.second.K == FA ? St.Lea : P.second.K == D1 ? St.Rebase : St.Range)++;
          for (auto &P : Slots)
            (void)P, ++St.Slot;
          ++St.Calls;
          ++It;
          continue;
        }
        // Scratch registers: clobbered by the call, not a result, dead after.
        SmallVector<unsigned, 5> Free;
        for (unsigned R : Scratch) {
          bool Ok = false;
          for (const MachineOperand &MO : MI.operands())
            if (MO.isRegMask() && MO.clobbersPhysReg(R))
              Ok = true;
          for (const MachineOperand &MO : MI.operands())
            if (MO.isReg() && MO.isDef() && MO.getReg() && TRI->regsOverlap(MO.getReg(), R))
              Ok = false;
          for (MCRegUnit U : TRI->regunits(R))
            if (LA.first.test(U))
              Ok = false;
          if (Ok)
            Free.push_back(R);
        }
        if (Free.size() < 5)
          report_fatal_error(Twine("goc-frameaddr-fix: ") + MF.getName() +
                             ": not enough dead scratch registers after a call");
        const unsigned D = Free[0], SZ = Free[1], T1 = Free[2], T2 = Free[3], T3 = Free[4];
        // FA-only: re-derive inline right after the call (1-2 instructions
        // per location). Otherwise the hot path is: store RBP to goc.oldfp
        // before the call, compare after it, and branch to an out-of-line
        // block that rebases everything only if the stack actually moved.
        MachineBasicBlock *FixMBB = MBB;
        MachineBasicBlock::iterator InsertAt = std::next(It);
        MachineBasicBlock *Cont = nullptr;
        Register OFR;
        int64_t OOff = 0;
        if (NeedDelta) {
          if (OldFP == INT_MIN)
            report_fatal_error(Twine("goc-frameaddr-fix: ") + MF.getName() +
                               ": needs a rebase but has no goc.oldfp slot");
          // goc.oldfp is in no stack map, so a stack copy moves it verbatim:
          // after the call it still holds the pre-call RBP.
          OOff = frameRef(OldFP, OFR);
          auto oMMO = [&](MachineMemOperand::Flags F) {
            return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, OldFP), F, 8, Align(8));
          };
          BuildMI(*MBB, It, DL, TII->get(X86::MOV64mr))
              .addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0).addReg(X86::RBP)
              .addMemOperand(oMMO(MachineMemOperand::MOStore));
          Cont = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
          MF.insert(std::next(MBB->getIterator()), Cont);
          Cont->splice(Cont->end(), MBB, std::next(It), MBB->end());
          Cont->transferSuccessors(MBB);
          FixMBB = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
          MF.push_back(FixMBB);
          MBB->addSuccessor(Cont);
          MBB->addSuccessor(FixMBB);
          FixMBB->addSuccessor(Cont);
          // Flags are dead right after a call.
          BuildMI(*MBB, MBB->end(), DL, TII->get(X86::CMP64mr))
              .addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0).addReg(X86::RBP)
              .addMemOperand(oMMO(MachineMemOperand::MOLoad));
          BuildMI(*MBB, MBB->end(), DL, TII->get(X86::JCC_1)).addMBB(FixMBB).addImm(X86::COND_NE);
          InsertAt = FixMBB->end();
        }
        auto B = [&](unsigned Opc) { return BuildMI(*FixMBB, InsertAt, DL, TII->get(Opc)); };
        auto Bd = [&](unsigned Opc, unsigned Dst) { return BuildMI(*FixMBB, InsertAt, DL, TII->get(Opc), Dst); };
        auto leaTo = [&](unsigned Dst, unsigned Base, unsigned Index, int64_t Off) {
          Bd(X86::LEA64r, Dst).addReg(Base).addImm(1).addReg(Index).addImm(imm32(Off)).addReg(0);
        };
        if (NeedDelta) {
          // delta = RBP - goc.oldfp
          Bd(X86::MOV64rr, D).addReg(X86::RBP);
          Bd(X86::SUB64rm, D).addReg(D).addReg(OFR).addImm(1).addReg(0).addImm(OOff).addReg(0)
              .addMemOperand(MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, OldFP),
                                                     MachineMemOperand::MOLoad, 8, Align(8)));
        }
        if (NeedRange) {
          // SZ = frame top - RSP: a rebased value w is in this frame iff
          // (w - RSP) <u SZ. Only this frame's addresses can be MS here.
          leaTo(SZ, X86::RBP, 0, TopOff);
          Bd(X86::SUB64rr, SZ).addReg(SZ).addReg(X86::RSP);
        }
        // v := v rebased, for v in register V.
        auto fixReg = [&](unsigned V, const Val &Vl) {
          if (Vl.K == FA) {
            leaTo(V, Vl.Base, 0, Vl.Off);
            ++St.Lea;
          } else if (Vl.K == D1) {
            Bd(X86::ADD64rr, V).addReg(V).addReg(D);
            ++St.Rebase;
          } else { // MS
            leaTo(T1, V, D, 0);                 // T1 = v + delta
            Bd(X86::MOV64rr, T2).addReg(T1);
            Bd(X86::SUB64rr, T2).addReg(T2).addReg(X86::RSP);
            B(X86::CMP64rr).addReg(T2).addReg(SZ);
            Bd(X86::CMOV64rr, V).addReg(V).addReg(T1).addImm(X86::COND_B);
            ++St.Range;
          }
        };
        for (auto &P : Regs)
          fixReg(P.first, P.second);
        for (auto &P : Slots) {
          int FI = SlotFI[P.first];
          Register FR;
          int64_t SOff = frameRef(FI, FR);
          auto mmo = [&](MachineMemOperand::Flags F) {
            return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI), F, 8,
                                           MFI->getObjectAlign(FI));
          };
          if (P.second.K != FA)
            Bd(X86::MOV64rm, T3).addReg(FR).addImm(1).addReg(0).addImm(SOff).addReg(0)
                .addMemOperand(mmo(MachineMemOperand::MOLoad));
          fixReg(T3, P.second);
          B(X86::MOV64mr).addReg(FR).addImm(1).addReg(0).addImm(SOff).addReg(0).addReg(T3)
              .addMemOperand(mmo(MachineMemOperand::MOStore));
          ++St.Slot;
        }
        ++St.Calls;
        Any = true;
        if (Cont) {
          B(X86::JMP_1).addMBB(Cont);
          ++St.Split;
          MBB = Cont; // continue the walk in the split-off tail
          It = MBB->begin();
          continue;
        }
        It = InsertAt; // skip the inserted instructions
      }
    }
    TotalUnrepairable += St.Derived;
    FnsUnrepairable += St.Derived ? 1 : 0;
    TotalRepairCalls += St.Calls;
    if (std::getenv("GOC_FRAMEADDR_STATS") && (St.Calls || St.Derived))
      errs() << "goc-frameaddr-fix: " << MF.getName() << " calls=" << St.Calls
             << " lea=" << St.Lea << " rebase=" << St.Rebase << " range=" << St.Range
             << " slot=" << St.Slot << " split=" << St.Split << " unrepairable=" << St.Derived << "\n";
    return Any;
  }
};
char GocFrameAddrFix::ID = 0;

// Each llvm.experimental.stackmap goc-stackmap/goc-reanchor emit names the
// roots live across the CALL that follows it; elfpack attaches the record to
// the first CALL after it in layout order. That is only sound when the
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

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();
  PassRegistry *Registry = PassRegistry::getPassRegistry();
  initializeCore(*Registry);
  initializeCodeGen(*Registry);
  initializeLoopStrengthReducePass(*Registry);
  initializeLowerIntrinsicsPass(*Registry);
  initializePostInlineEntryExitInstrumenterPass(*Registry);
  initializeUnreachableBlockElimLegacyPassPass(*Registry);
  initializeConstantHoistingLegacyPassPass(*Registry);
  initializeScalarOpts(*Registry);
  initializeVectorization(*Registry);
  initializeScalarizeMaskedMemIntrinLegacyPassPass(*Registry);
  initializeExpandReductionsPass(*Registry);
  initializeExpandVectorPredicationPass(*Registry);
  initializeHardwareLoopsLegacyPass(*Registry);
  initializeTransformUtils(*Registry);
  initializeReplaceWithVeclibLegacyPass(*Registry);
  initializeTLSVariableHoistLegacyPassPass(*Registry);
  // Stack maps are attached by layout position (see GocStackmapPlacement):
  // tail merging must stay off whatever the caller passes.
  std::vector<const char *> Args(argv, argv + argc);
  bool UserTailMerge = false;
  for (const char *A : Args)
    if (StringRef(A).starts_with("-enable-tail-merge") ||
        StringRef(A).starts_with("--enable-tail-merge"))
      UserTailMerge = true;
  if (!UserTailMerge)
    Args.push_back("-enable-tail-merge=false");
  cl::ParseCommandLineOptions(Args.size(), Args.data(),
                              "goc-llc: llc-19 + goc frame-address fix\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::string CPUStr = codegen::getCPUStr(), FeaturesStr = codegen::getFeaturesStr();
  auto OLvl = CodeGenOpt::parseLevel(OptLevel);
  if (!OLvl) {
    errs() << "goc-llc: invalid optimization level\n";
    return 1;
  }
  std::unique_ptr<TargetMachine> Target;
  Triple TheTriple;
  const llvm::Target *TheTarget = nullptr;
  auto SetDataLayout = [&](StringRef DLTriple, StringRef) -> std::optional<std::string> {
    TheTriple = Triple(DLTriple.str());
    if (TheTriple.getTriple().empty())
      TheTriple.setTriple(sys::getDefaultTargetTriple());
    std::string Error;
    TheTarget = TargetRegistry::lookupTarget(codegen::getMArch(), TheTriple, Error);
    if (!TheTarget) {
      errs() << "goc-llc: " << Error << "\n";
      exit(1);
    }
    TargetOptions Options = codegen::InitTargetOptionsFromCodeGenFlags(TheTriple);
    Options.MCOptions.AsmVerbose = true;
    Options.MCOptions.PreserveAsmComments = true;
    Target.reset(TheTarget->createTargetMachine(
        TheTriple.getTriple(), CPUStr, FeaturesStr, Options,
        codegen::getExplicitRelocModel(), codegen::getExplicitCodeModel(), *OLvl));
    return Target->createDataLayout().getStringRepresentation();
  };
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context, ParserCallbacks(SetDataLayout));
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }
  if (auto CM = M->getCodeModel(); CM && !codegen::getExplicitCodeModel())
    Target->setCodeModel(*CM);
  if (codegen::getFloatABIForCalls() != FloatABI::Default)
    Target->Options.FloatABIType = codegen::getFloatABIForCalls();
  std::error_code EC;
  std::string OutName = OutputFilename.empty() ? std::string("-") : std::string(OutputFilename);
  auto Out = std::make_unique<ToolOutputFile>(
      OutName, EC, codegen::getFileType() == CodeGenFileType::AssemblyFile ? sys::fs::OF_Text : sys::fs::OF_None);
  if (EC) {
    errs() << "goc-llc: " << EC.message() << "\n";
    return 1;
  }
  Target->Options.ObjectFilenameForDebug = Out->outputFilename();
  TargetLibraryInfoImpl TLII(Triple(M->getTargetTriple()));
  if (verifyModule(*M, &errs())) {
    errs() << "goc-llc: input module cannot be verified\n";
    return 1;
  }
  codegen::setFunctionAttributes(CPUStr, FeaturesStr, *M);

  legacy::PassManager PM;
  PM.add(new TargetLibraryInfoWrapperPass(TLII));
  LLVMTargetMachine &LLVMTM = static_cast<LLVMTargetMachine &>(*Target);
  auto *MMIWP = new MachineModuleInfoWrapperPass(&LLVMTM);
  SmallVector<char, 0> Buffer;
  raw_svector_ostream BOS(Buffer);
  raw_pwrite_stream *OS = &BOS;
  // Same as LLVMTargetMachine::addPassesToEmitFile, plus the fix pass.
  TargetPassConfig *PassConfig = LLVMTM.createPassConfig(PM);
  PassConfig->setDisableVerify(false);
  PM.add(PassConfig);
  PM.add(MMIWP);
  if (PassConfig->addISelPasses()) {
    errs() << "goc-llc: addISelPasses failed\n";
    return 1;
  }
  PassConfig->addMachinePasses();
  PassConfig->setInitialized();
  if (TargetPassConfig::willCompleteCodeGenPipeline()) {
    PM.add(new GocStackmapPlacement());
    PM.add(new GocFrameAddrFix());
    if (LLVMTM.addAsmPrinter(PM, *OS, nullptr, codegen::getFileType(),
                             MMIWP->getMMI().getContext())) {
      errs() << "goc-llc: target does not support this file type\n";
      return 1;
    }
  } else if (codegen::getFileType() != CodeGenFileType::Null) {
    PM.add(createPrintMIRPass(*OS));
  }
  PM.add(createFreeMachineFunctionPass());
  const_cast<TargetLoweringObjectFile *>(LLVMTM.getObjFileLowering())
      ->Initialize(MMIWP->getMMI().getContext(), *Target);
  PM.run(*M);
  if (Context.getDiagHandlerPtr()->HasErrors)
    return 1;
  Out->os() << Buffer;
  Out->keep();
  if (TotalUnrepairable)
    errs() << "goc-llc: note: " << TotalUnrepairable
           << " frame-derived values of unknown provenance stay live across calls in "
           << FnsUnrepairable << " functions (not rebased; GOC_FRAMEADDR_TAINT=report lists them)\n";
  return 0;
}
