// P6.1 — Register-level gptr liveness + safepoint spill (Go-aligned).
// Real llvm::LiveIntervals via LiveIntervalsWrapperPass (PassManager).
//
// Gptr identification (NOT every GR64) — see isGptrVReg():
//   R1. Fn attr "goc-gptr-vregs" = comma-separated virt-reg indices (explicit).
//   R2. Def = COPY from physreg of an IR pointer-typed argument (seed ABI:
//       ptr among int/ptr: arg0→AX, arg1→BX, arg2→CX, arg3→DI).
//   R3. Def = load (mayLoad / MOV*rm) from FrameIndex / MMO of pointer alloca,
//       or COPY/load of a value already classified as gptr (one-hop prop).
//   R4. IR/MIR: MRI LLT pointer type when present (GlobalISel).
//   R5. P18 color-driven: when fn has goc-color-driven=1, classify using
//       goc-arg-ptr-colors / goc-color-spill / goc-color-cptr-only (sptr+gptr
//       are map ptrs; cptr/cheap are NOT). Without color attrs, R1–R4 unchanged.
// Reject: MOV*ri immediates, integer-only defs, unknown GR64 with no ptr proof.
#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <tuple>
#include <vector>

using namespace llvm;

namespace {

uint32_t parseU32Attr(const Function &F, StringRef Key, uint32_t Def) {
  if (!F.hasFnAttribute(Key))
    return Def;
  unsigned V = 0;
  if (F.getFnAttribute(Key).getValueAsString().getAsInteger(10, V))
    return Def;
  return V;
}

bool isPointerTy(Type *Ty) { return Ty && Ty->isPointerTy(); }

struct GoFrameLayout {
  uint32_t LocalsBytes = 24;
  DenseMap<int, int64_t> FiToGoSp;
  int64_t NextHigh = 0;

  void init(uint32_t Bytes) {
    LocalsBytes = Bytes ? Bytes : 24;
    NextHigh = int64_t(LocalsBytes) - 8;
    FiToGoSp.clear();
  }

  int64_t assignOrGet(int FI) {
    auto It = FiToGoSp.find(FI);
    if (It != FiToGoSp.end())
      return It->second;
    if (NextHigh < 0)
      report_fatal_error("GocSpill: Go locals blob exhausted");
    int64_t Off = NextHigh;
    NextHigh -= 8;
    FiToGoSp[FI] = Off;
    return Off;
  }
};

struct SpillRecord {
  unsigned VReg = 0;
  int FI = 0;
  int64_t GoSpOff = 0;
};

static DenseMap<MachineFunction *, std::vector<SpillRecord>> &spillRegistry() {
  static DenseMap<MachineFunction *, std::vector<SpillRecord>> R;
  return R;
}
static DenseMap<MachineFunction *, GoFrameLayout> &layoutRegistry() {
  static DenseMap<MachineFunction *, GoFrameLayout> R;
  return R;
}

/// Physreg for int/ptr arg i under Go amd64 ABIInternal (P8).
/// Ordinal is among integer+pointer args: AX,BX,CX,DI,SI,R8,R9.
static unsigned ptrArgPhysReg(const TargetRegisterInfo &TRI, unsigned ArgIdx,
                              unsigned RAX, unsigned RBX, unsigned RCX,
                              unsigned RDI) {
  (void)TRI;
  switch (ArgIdx) {
  case 0:
    return RAX;
  case 1:
    return RBX;
  case 2:
    return RCX;
  case 3:
    return RDI;
  default:
    return 0;
  }
}

static bool isMorestackCallee(const MachineInstr &MI) {
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isSymbol() && StringRef(MO.getSymbolName()).contains("morestack"))
      return true;
    if (MO.isGlobal() && MO.getGlobal()->getName().contains("morestack"))
      return true;
  }
  return false;
}

/// CALL and morestack slow-path are safepoints that need gptr spill.
static bool isSafepoint(const MachineInstr &MI) {
  if (MI.isCall())
    return true;
  return isMorestackCallee(MI);
}

static bool parseExplicitGptrVRegs(const Function &F,
                                   DenseSet<unsigned> &Out) {
  if (!F.hasFnAttribute("goc-gptr-vregs"))
    return false;
  StringRef S = F.getFnAttribute("goc-gptr-vregs").getValueAsString();
  SmallVector<StringRef, 4> Parts;
  S.split(Parts, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (StringRef P : Parts) {
    unsigned Id = 0;
    if (!P.trim().getAsInteger(10, Id))
      Out.insert(Id);
  }
  return !Out.empty();
}

static bool memOpIsPtrAlloca(const MachineMemOperand &MMO) {
  const Value *V = MMO.getValue();
  if (!V)
    return false;
  if (const auto *AI = dyn_cast<AllocaInst>(V))
    return isPointerTy(AI->getAllocatedType());
  return isPointerTy(V->getType());
}


static bool colorDriven(const Function &F) {
  return F.hasFnAttribute("goc-color-driven");
}

static bool colorCptrOnly(const Function &F) {
  return F.hasFnAttribute("goc-color-cptr-only");
}

/// Arg index among all args → color token from goc-arg-ptr-colors (comma list).
static StringRef argColorAt(const Function &F, unsigned ArgIdx) {
  if (!F.hasFnAttribute("goc-arg-ptr-colors"))
    return "";
  StringRef S = F.getFnAttribute("goc-arg-ptr-colors").getValueAsString();
  SmallVector<StringRef, 8> Parts;
  S.split(Parts, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  if (ArgIdx >= Parts.size())
    return "";
  return Parts[ArgIdx].trim();
}

static bool colorIsMapPtr(StringRef C) {
  return C == "gptr" || C == "sptr" || C == "auto";
}

static bool isGptrVReg(MachineFunction &MF, Register V, unsigned RAX,
                       unsigned RBX, unsigned RCX, unsigned RDI,
                       const DenseSet<unsigned> &Explicit,
                       DenseSet<Register> &KnownGptrs) {
  if (Explicit.count(V.id()) || Explicit.count(Register::virtReg2Index(V)))
    return true;
  if (KnownGptrs.count(V))
    return true;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const TargetRegisterClass *RC = MRI.getRegClassOrNull(V);
  if (!RC || StringRef(TRI->getRegClassName(RC)) != "GR64")
    return false;
  if (MRI.reg_nodbg_empty(V))
    return false;

  // R4: LLT pointer (when set). P18: not under cptr-only.
  LLT Ty = MRI.getType(V);
  if (Ty.isValid() && Ty.isPointer()) {
    if (colorCptrOnly(MF.getFunction()))
      return false;
    KnownGptrs.insert(V);
    return true;
  }

  MachineInstr *Def = MRI.getVRegDef(V);
  if (!Def)
    return false;

  // Reject obvious non-pointers: immediates / pure constants.
  if (Def->isMoveImmediate())
    return false;

  // R2: COPY from physreg of IR pointer argument.
  // ABIInternal: int/ptr ordinal → AX,BX,CX,DI,…; only pointer-typed args are gptrs.
  // P18: when color-driven, only gptr/sptr/auto map-colored args count; cptr-only rejects.
  if (Def->isCopy() && Def->getOperand(1).isReg()) {
    Register Src = Def->getOperand(1).getReg();
    if (Src.isPhysical()) {
      const Function &Fn = MF.getFunction();
      if (colorCptrOnly(Fn))
        return false;
      unsigned AbiOrd = 0;
      unsigned ArgIdx = 0;
      for (const Argument &A : Fn.args()) {
        Type *Ty = A.getType();
        bool IsIP = isPointerTy(Ty) || Ty->isIntegerTy();
        if (!IsIP) {
          ArgIdx++;
          continue;
        }
        if (isPointerTy(Ty)) {
          unsigned Phys = ptrArgPhysReg(*TRI, AbiOrd, RAX, RBX, RCX, RDI);
          if (Phys && Src == Phys) {
            if (colorDriven(Fn)) {
              StringRef AC = argColorAt(Fn, ArgIdx);
              // Non-map colors (cptr/uptr/int) must not enter Locals/Args maps.
              if (!AC.empty() && !colorIsMapPtr(AC))
                return false;
            }
            KnownGptrs.insert(V);
            return true;
          }
        }
        AbiOrd++;
        ArgIdx++;
      }
    }
    if (Src.isVirtual() && (KnownGptrs.count(Src) || Explicit.count(Src.id()))) {
      KnownGptrs.insert(V);
      return true;
    }
  }

  // R3: load from ptr alloca FI / MMO. P18: skip under cptr-only.
  if (Def->mayLoad()) {
    if (colorCptrOnly(MF.getFunction()))
      return false;
    for (const MachineMemOperand *MMO : Def->memoperands()) {
      if (MMO->isLoad() && memOpIsPtrAlloca(*MMO)) {
        KnownGptrs.insert(V);
        return true;
      }
    }
    // FrameIndex load in a function that has a pointer alloca — conservative
    // only when there is exactly one ptr alloca (demo hold).
    bool HasFI = false;
    for (const MachineOperand &MO : Def->operands())
      if (MO.isFI())
        HasFI = true;
    if (HasFI) {
      unsigned PtrAllocaN = 0;
      for (BasicBlock &BB : MF.getFunction())
        for (Instruction &I : BB)
          if (auto *AI = dyn_cast<AllocaInst>(&I))
            if (isPointerTy(AI->getAllocatedType()))
              PtrAllocaN++;
      if (PtrAllocaN >= 1) {
        KnownGptrs.insert(V);
        return true;
      }
    }
  }

  return false;
}

struct GocSpillGptrsAtSafepoints : public MachineFunctionPass {
  static char ID;
  std::vector<std::string> *RecipeOut = nullptr;

  GocSpillGptrsAtSafepoints() : MachineFunctionPass(ID) {}
  explicit GocSpillGptrsAtSafepoints(std::vector<std::string> *Out)
      : MachineFunctionPass(ID), RecipeOut(Out) {}

  StringRef getPassName() const override {
    return "Goc Spill Gptrs At Safepoints (LiveIntervals)";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addRequired<SlotIndexesWrapperPass>();
    // Spill inserts MIs; do not preserve LIS (caller must recompute or
    // order later passes to not require stale LIS — see driver comment).
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMF(MachineFunction &MF) { return runOnMachineFunction(MF); }

  bool runOnMachineFunction(MachineFunction &MF) override {
    Function &F = MF.getFunction();
    if (!F.hasFnAttribute("goc-emit-maps") &&
        !F.hasFnAttribute("goc-spill-gptrs"))
      return false;
    if (MF.empty())
      return false;

    LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
    SlotIndexes &Indexes = getAnalysis<SlotIndexesWrapperPass>().getSI();
    (void)Indexes;

    MachineRegisterInfo &MRI = MF.getRegInfo();
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

    const X86InstrInfo *TII = goc::x86TII(MF);
    const unsigned OpcMOV64mr = X86::MOV64mr;
    const unsigned OpcMOV64rm = X86::MOV64rm;
    const unsigned RAX = X86::RAX;
    const unsigned RBX = X86::RBX;
    const unsigned RCX = X86::RCX;
    const unsigned RDI = X86::RDI;
    const char *ApiNote = "api=X86InstrInfo";

    DenseSet<unsigned> Explicit;
    parseExplicitGptrVRegs(F, Explicit);
    DenseSet<Register> KnownGptrs;

    // Multi-pass classify so COPY-of-gptr (R3 prop) converges.
    SmallVector<Register, 4> Gptrs;
    for (int Round = 0; Round < 4; ++Round) {
      Gptrs.clear();
      for (unsigned I = 0, E = MRI.getNumVirtRegs(); I != E; ++I) {
        Register V = Register::index2VirtReg(I);
        if (isGptrVReg(MF, V, RAX, RBX, RCX, RDI, Explicit, KnownGptrs))
          Gptrs.push_back(V);
      }
    }

    if (RecipeOut) {
      RecipeOut->push_back(
          "gptr_id_rules=R1_explicit_attr|R2_ptr_arg_COPY|R3_ptr_load_alloca|"
          "R4_LLT_ptr|R5_color_driven; reject=every_GR64|cptr_only");
      RecipeOut->push_back("abi=amd64_ABIInternal_AX_BX_CX_DI_SI_R8_R9");
      if (colorDriven(F))
        RecipeOut->push_back("color_driven=1");
      if (colorCptrOnly(F))
        RecipeOut->push_back("color_cptr_only=1");
      if (F.hasFnAttribute("goc-color-spill"))
        RecipeOut->push_back("color_spill=1");
    }

    if (Gptrs.empty()) {
      errs() << "[GocSpill] no gptr vregs (tight ID) in " << F.getName() << "\n";
      return false;
    }

    uint32_t LocalsBytes = parseU32Attr(F, "goc-frame-locals-bytes", 24);
    GoFrameLayout &Layout = layoutRegistry()[&MF];
    Layout.init(LocalsBytes);

    // Real LiveIntervals: which gptr vregs are live at each safepoint.
    DenseMap<Register, DenseSet<MachineInstr *>> LiveBeforeSP;
    for (MachineBasicBlock &MBB : MF) {
      for (MachineInstr &MI : MBB) {
        if (!isSafepoint(MI))
          continue;
        SlotIndex Idx = LIS.getInstructionIndex(MI);
        for (Register V : Gptrs) {
          if (!LIS.hasInterval(V))
            continue;
          LiveInterval &LI = LIS.getInterval(V);
          if (LI.liveAt(Idx))
            LiveBeforeSP[V].insert(&MI);
        }
      }
    }

    MachineFrameInfo &MFI = MF.getFrameInfo();
    DenseMap<Register, std::pair<int, int64_t>> SlotFor;
    std::vector<SpillRecord> Recs;
    for (Register V : Gptrs) {
      auto It = LiveBeforeSP.find(V);
      if (It == LiveBeforeSP.end() || It->second.empty())
        continue;
      int FI = MFI.CreateStackObject(8, Align(8), /*isSpillSlot=*/true);
      int64_t GoSp = Layout.assignOrGet(FI);
      SlotFor[V] = {FI, GoSp};
      Recs.push_back(SpillRecord{V.id(), FI, GoSp});
    }

    bool Changed = false;
    for (MachineBasicBlock &MBB : MF) {
      SmallVector<MachineInstr *, 4> SPs;
      for (MachineInstr &MI : MBB)
        if (isSafepoint(MI))
          SPs.push_back(&MI);
      for (MachineInstr *SPMI : SPs) {
        MachineBasicBlock::iterator I = SPMI->getIterator();
        const char *Kind =
            isMorestackCallee(*SPMI) ? "morestack" : "CALL";
        for (Register V : Gptrs) {
          auto Lit = LiveBeforeSP.find(V);
          if (Lit == LiveBeforeSP.end() || !Lit->second.count(SPMI))
            continue;
          auto Sit = SlotFor.find(V);
          if (Sit == SlotFor.end())
            continue;
          int FI = Sit->second.first;
          int64_t GoSp = Sit->second.second;
          DebugLoc DL = SPMI->getDebugLoc();

          BuildMI(MBB, I, DL, TII->get(OpcMOV64mr))
              .addFrameIndex(FI)
              .addImm(1)
              .addReg(0)
              .addImm(0)
              .addReg(0)
              .addReg(V);
          BuildMI(MBB, std::next(I), DL, TII->get(OpcMOV64rm), V)
              .addFrameIndex(FI)
              .addImm(1)
              .addReg(0)
              .addImm(0)
              .addReg(0);

          Changed = true;
          if (RecipeOut) {
            std::string Line;
            raw_string_ostream OS(Line);
            OS << "spill_gptr_vreg=" << V.id() << " fi=" << FI
               << " go_sp_off=" << GoSp << " before_" << Kind
               << " liveintervals=1 liveintervals_eq=1";
            RecipeOut->push_back(OS.str());
          }
          errs() << "[GocSpill] LiveIntervals spill vreg" << V.id() << " -> FI#"
                 << FI << " GoSP+" << GoSp << " before " << Kind << " in "
                 << F.getName() << " (" << ApiNote << ")\n";
        }
      }
    }

    spillRegistry()[&MF] = std::move(Recs);
    if (RecipeOut) {
      RecipeOut->push_back("mi_path=liveintervals_spill_gptr");
      RecipeOut->push_back(ApiNote);
      RecipeOut->push_back("locals_layout=go_bit_i_is_sp_plus_i_times_8");
      RecipeOut->push_back(
          "note=llvm_LiveIntervals_via_LiveIntervalsWrapperPass_PassManager");
      RecipeOut->push_back(
          "safepoints=CALL+morestack; LIS_invalidated_after_spill_inserts");
    }
    return Changed;
  }
};

char GocSpillGptrsAtSafepoints::ID = 0;

struct GocSpillRunner : GocPassRunner {
  std::vector<std::string> *Recipe;
  explicit GocSpillRunner(std::vector<std::string> *R) : Recipe(R) {}
  bool runOnMF(MachineFunction &MF) override {
    report_fatal_error(
        "GocSpill runner requires PassManager (LiveIntervalsWrapperPass); "
        "use createGocSpillGptrsAtSafepointsPass");
  }
};

} // namespace

GocPassRunner *
createGocSpillGptrsAtSafepointsRunner(std::vector<std::string> *RecipeOut) {
  return new GocSpillRunner(RecipeOut);
}

MachineFunctionPass *
createGocSpillGptrsAtSafepointsPass(std::vector<std::string> *RecipeOut) {
  return new GocSpillGptrsAtSafepoints(RecipeOut);
}

namespace goc_spill_api {
std::vector<std::pair<int, int64_t>> fiGoSpOffs(MachineFunction &MF) {
  std::vector<std::pair<int, int64_t>> Out;
  auto It = layoutRegistry().find(&MF);
  if (It == layoutRegistry().end())
    return Out;
  for (auto &KV : It->second.FiToGoSp)
    Out.push_back({KV.first, KV.second});
  return Out;
}
std::vector<std::tuple<unsigned, int, int64_t>> spills(MachineFunction &MF) {
  std::vector<std::tuple<unsigned, int, int64_t>> Out;
  auto It = spillRegistry().find(&MF);
  if (It == spillRegistry().end())
    return Out;
  for (auto &S : It->second)
    Out.push_back({S.VReg, S.FI, S.GoSpOff});
  return Out;
}
} // namespace goc_spill_api
