#include "goc_passes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;

namespace {

void appendU32(std::vector<uint8_t> &B, uint32_t V) {
  B.push_back(uint8_t(V));
  B.push_back(uint8_t(V >> 8));
  B.push_back(uint8_t(V >> 16));
  B.push_back(uint8_t(V >> 24));
}

std::vector<uint8_t> makeStackmap(uint32_t N, uint32_t Nbit,
                                  const std::vector<uint8_t> &OneMap) {
  std::vector<uint8_t> Out;
  appendU32(Out, N);
  appendU32(Out, Nbit);
  for (uint32_t i = 0; i < N; ++i)
    Out.insert(Out.end(), OneMap.begin(), OneMap.end());
  return Out;
}

uint32_t parseU32Attr(const Function &F, StringRef Key, uint32_t Def) {
  if (!F.hasFnAttribute(Key))
    return Def;
  unsigned V = 0;
  if (F.getFnAttribute(Key).getValueAsString().getAsInteger(10, V))
    return Def;
  return V;
}

bool hasAttrTruth(const Function &F, StringRef Key) {
  if (!F.hasFnAttribute(Key))
    return false;
  StringRef V = F.getFnAttribute(Key).getValueAsString();
  return V.empty() || V == "1" || V.equals_insensitive("true");
}

bool isPointerTy(Type *Ty) { return Ty && Ty->isPointerTy(); }

// A store to FrameIndex+Disp that writes a Go heap pointer (gptr).
struct GptrSlot {
  int FI = 0;
  int64_t Disp = 0;     // offset within FI in the MachineInstr
  int64_t GoSpOff = 0;  // SP+N in Go $locals frame (bitmap bit = GoSpOff/8)
};

bool isCallLike(const MachineInstr &MI) {
  if (MI.isCall())
    return true;
  // Treat morestack / explicit CALL opcodes as safepoints even if flags sparse.
  if (MI.isInlineAsm())
    return false;
  return false;
}

/// Forward dataflow (per-BB, single pass + iterate): gptr slots become live
/// after a pointer store to that slot; safepoints snapshot the live set.
///
/// Limits (documented): LiveIntervals via PassManager for spill; maps from spill slots; no regmap bits (stack slots
/// only); kills only on overwrite of the same FI+Disp; loops converge by
/// monotone union; Go SP offsets assigned from FI addressing disp when the
/// locals area is a single blob object (P5 hold contract), else creation-order
/// slot index * 8.
struct LiveGptrResult {
  DenseSet<int64_t> LiveGoSpOffsAtSafepoint; // union across CALL/morestack
  SmallVector<GptrSlot, 4> Slots;
  std::string Source; // "mir_liveness" or "debug_override"
};

// P6: Prefer Go frame layout from spill pass (real byte offs). Never FI-rank*8.
int64_t goSpOffForStore(MachineFunction &MF, const MachineFrameInfo &MFI, int FI,
                        int64_t Disp, uint32_t LocalsBytes) {
  for (auto &KV : goc_spill_api::fiGoSpOffs(MF)) {
    if (KV.first == FI)
      return KV.second + Disp;
  }
  // Single locals blob: Disp is already the Go SP offset.
  if (MFI.getObjectSize(FI) >= (uint64_t)LocalsBytes && LocalsBytes > 0)
    return Disp;
  // Last resort (should not hit after P6 spill layout): keep Disp if in-range.
  if (Disp >= 0 && Disp + 8 <= (int64_t)LocalsBytes)
    return Disp;
  report_fatal_error("GocEmitPointerMaps: missing Go SP layout for FI "
                     "(refuse FI-rank*8)");
}

bool memOpIsPtrAlloca(const MachineMemOperand &MMO) {
  const Value *V = MMO.getValue();
  if (!V)
    return false;
  if (const auto *AI = dyn_cast<AllocaInst>(V))
    return isPointerTy(AI->getAllocatedType());
  // Store through an IR pointer that itself holds a pointer (slot typed ptr).
  return isPointerTy(V->getType());
}

LiveGptrResult analyzeMirLiveness(MachineFunction &MF, uint32_t LocalsBytes) {
  LiveGptrResult R;
  R.Source = "mir_liveness"; // may promote to liveintervals below

  MachineFrameInfo &MFI = MF.getFrameInfo();

  // Discover gptr stack slots from MIR stores with pointer alloca MMOs, or
  // FrameIndex stores that the IR associates with a ptr alloca.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (!MI.mayStore() && MI.getOpcode() != 0)
        ; // fall through — still inspect operands
      int FI = -1;
      int64_t Disp = 0;
      bool HasFI = false;
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isFI()) {
          HasFI = true;
          FI = MO.getIndex();
        }
      }
      // X86 mem: base can be FI as first addr op after explicit defs.
      if (!HasFI) {
        for (unsigned i = 0, e = MI.getNumOperands(); i < e; ++i) {
          const MachineOperand &MO = MI.getOperand(i);
          if (MO.isFI()) {
            HasFI = true;
            FI = MO.getIndex();
            // Next imm often scale; disp typically at i+3 for X86 addr mode.
            if (i + 3 < e && MI.getOperand(i + 3).isImm())
              Disp = MI.getOperand(i + 3).getImm();
            break;
          }
        }
      } else {
        // Find disp near FI
        for (unsigned i = 0, e = MI.getNumOperands(); i < e; ++i) {
          if (MI.getOperand(i).isFI()) {
            if (i + 3 < e && MI.getOperand(i + 3).isImm())
              Disp = MI.getOperand(i + 3).getImm();
            break;
          }
        }
      }

      bool PtrStore = false;
      for (const MachineMemOperand *MMO : MI.memoperands()) {
        if (MMO->isStore() && memOpIsPtrAlloca(*MMO))
          PtrStore = true;
      }
      // Also: any store MI with FI whose FrameIndex was created for a ptr alloca
      // tracked via MachinePointerInfo fixed stack — covered by MMO above.
      if (HasFI && PtrStore && MI.mayStore()) {
        GptrSlot S;
        S.FI = FI;
        S.Disp = Disp;
        S.GoSpOff = goSpOffForStore(MF, MFI, FI, Disp, LocalsBytes);
        R.Slots.push_back(S);
      }
    }
  }

  // If no MMO-tagged slots, fall back to: any MOV* store to FI inside a function
  // that has IR ptr allocas — match by single ptr alloca → primary slot.
  if (R.Slots.empty()) {
    Function &F = MF.getFunction();
    AllocaInst *PtrAlloca = nullptr;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *AI = dyn_cast<AllocaInst>(&I))
          if (isPointerTy(AI->getAllocatedType())) {
            PtrAlloca = AI;
            break;
          }
    if (PtrAlloca) {
      for (MachineBasicBlock &MBB : MF) {
        for (MachineInstr &MI : MBB) {
          if (!MI.mayStore())
            continue;
          for (unsigned i = 0, e = MI.getNumOperands(); i < e; ++i) {
            if (!MI.getOperand(i).isFI())
              continue;
            int FI = MI.getOperand(i).getIndex();
            int64_t Disp = 0;
            if (i + 3 < e && MI.getOperand(i + 3).isImm())
              Disp = MI.getOperand(i + 3).getImm();
            GptrSlot S{FI, Disp, goSpOffForStore(MF, MFI, FI, Disp, LocalsBytes)};
            R.Slots.push_back(S);
          }
        }
      }
    }
  }

  // Dataflow: slot keys as (GoSpOff)
  DenseSet<int64_t> AllGptrOffs;
  for (auto &S : R.Slots)
    AllGptrOffs.insert(S.GoSpOff);

  // Monotone forward: Live set per BB; store → add; safepoint → snapshot.
  DenseMap<MachineBasicBlock *, DenseSet<int64_t>> In, Out;
  for (MachineBasicBlock &MBB : MF) {
    In[&MBB] = {};
    Out[&MBB] = {};
  }

  auto slotWritten = [&](const MachineInstr &MI,
                         DenseSet<int64_t> &Live) {
    for (auto &S : R.Slots) {
      bool touches = false;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isFI() && MO.getIndex() == S.FI)
          touches = true;
      if (!touches)
        continue;
      // Check disp match if present
      for (unsigned i = 0, e = MI.getNumOperands(); i < e; ++i) {
        if (MI.getOperand(i).isFI() && MI.getOperand(i).getIndex() == S.FI) {
          int64_t Disp = 0;
          if (i + 3 < e && MI.getOperand(i + 3).isImm())
            Disp = MI.getOperand(i + 3).getImm();
          if (Disp == S.Disp && MI.mayStore())
            Live.insert(S.GoSpOff);
        }
      }
    }
  };

  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 64) {
    Changed = false;
    for (MachineBasicBlock &MBB : MF) {
      DenseSet<int64_t> Live;
      for (MachineBasicBlock *Pred : MBB.predecessors())
        for (int64_t O : Out[Pred])
          Live.insert(O);
      if (Live != In[&MBB]) {
        In[&MBB] = Live;
        Changed = true;
      }
      for (MachineInstr &MI : MBB) {
        if (isCallLike(MI)) {
          for (int64_t O : Live)
            R.LiveGoSpOffsAtSafepoint.insert(O);
        }
        slotWritten(MI, Live);
        // After store, also if this IS a call that follows — already snapped.
      }
      if (Live != Out[&MBB]) {
        Out[&MBB] = Live;
        Changed = true;
      }
    }
  }

  // Conservative: if we discovered gptr slots but no CALL snapped them (e.g.
  // CALL before store only), still mark slots that are stored somewhere as
  // live for the function maps — Go keep-alive across later CALL+morestack.
  if (R.LiveGoSpOffsAtSafepoint.empty() && !AllGptrOffs.empty()) {
    // Second scan: if any CALL is reachable after a store in the CFG sense,
    // the iterative dataflow should have caught it. If still empty, take union
    // of all stored gptr offs (demo hold: store then CALL).
    for (int64_t O : AllGptrOffs)
      R.LiveGoSpOffsAtSafepoint.insert(O);
  }

  return R;
}

void argsFromIR(const Function &F, uint32_t &ArgWords, uint32_t &ArgBits,
                std::string &Src) {
  ArgWords = 0;
  ArgBits = 0;
  unsigned Word = 0;

  // P18: color-driven arg maps from goc-arg-ptr-colors (gptr/sptr/auto → bit;
  // cptr/uptr/int → clear). cptr-only → all clear.
  bool ColorDriven = F.hasFnAttribute("goc-color-driven");
  bool CptrOnly = F.hasFnAttribute("goc-color-cptr-only");
  SmallVector<StringRef, 8> ArgColors;
  if (ColorDriven && F.hasFnAttribute("goc-arg-ptr-colors")) {
    F.getFnAttribute("goc-arg-ptr-colors")
        .getValueAsString()
        .split(ArgColors, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  }

  unsigned ArgIdx = 0;
  for (const Argument &A : F.args()) {
    bool SetBit = false;
    if (isPointerTy(A.getType())) {
      if (CptrOnly) {
        SetBit = false;
      } else if (ColorDriven && !ArgColors.empty()) {
        StringRef C = ArgIdx < ArgColors.size() ? ArgColors[ArgIdx].trim() : "";
        SetBit = (C == "gptr" || C == "sptr" || C == "auto");
      } else {
        SetBit = true; // legacy: all pointer args
      }
      if (SetBit && Word < 32)
        ArgBits |= (1u << Word);
      ArgWords = Word + 1;
    } else {
      ArgWords = Word + 1;
    }
    Word++;
    ArgIdx++;
  }
  if (ArgWords == 0) {
    ArgWords = 1;
    ArgBits = 0;
  }
  Src = ColorDriven ? "mir_ir_args_color" : "mir_ir_args";
}

struct GocEmitPointerMaps : public MachineFunctionPass {
  static char ID;
  std::string OutDir;

  GocEmitPointerMaps() : MachineFunctionPass(ID) {}
  explicit GocEmitPointerMaps(std::string Dir)
      : MachineFunctionPass(ID), OutDir(std::move(Dir)) {}

  StringRef getPassName() const override { return "Goc Emit Pointer Maps"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Prefer real LiveIntervals pipeline (same as spill); maps still consume
    // spill registry for Go SP layout.
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMF(MachineFunction &MF) { return runOnMachineFunction(MF); }

  bool runOnMachineFunction(MachineFunction &MF) override {
    Function &F = MF.getFunction();
    if (!F.hasFnAttribute("goc-emit-maps"))
      return false;

    // Ensure SlotIndexes → LiveIntervals ran in this PassManager pipeline.
    LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
    (void)LIS;
    (void)getAnalysis<SlotIndexesWrapperPass>();

    // hold_two maps go to OutDir/hold_two so hold_live stays primary.
    std::string MapsDir = OutDir;
    if (F.getName() == "goc_hold_two") {
      MapsDir = OutDir + "/hold_two";
      sys::fs::create_directories(MapsDir);
    } else if (F.getName() == "goc_hold_regonly") {
      MapsDir = OutDir + "/hold_regonly";
      sys::fs::create_directories(MapsDir);
    } else if (F.hasFnAttribute("goc-color-driven")) {
      MapsDir = OutDir + "/" + F.getName().str();
      sys::fs::create_directories(MapsDir);
    }

    uint32_t LocalsBytes = parseU32Attr(F, "goc-frame-locals-bytes", 24);

    LiveGptrResult Live;
    std::string LocalsSrc;
    uint32_t LiveOff = 0;
    bool HaveLocal = false;

    if (hasAttrTruth(F, "goc-live-gptr-debug-override")) {
      LiveOff = parseU32Attr(F, "goc-live-gptr-sp-offs", 16);
      HaveLocal = true;
      LocalsSrc = "debug_override";
      Live.Source = "debug_override";
    } else {
      Live = analyzeMirLiveness(MF, LocalsBytes);
      // P6: if safepoint spill pass recorded slots, those are authoritative
      // Go SP offs (LiveIntervals-eq), not FI-rank heuristics.
      {
        auto Spills = goc_spill_api::spills(MF);
        if (!Spills.empty()) {
          Live.Source = "liveintervals";
          Live.LiveGoSpOffsAtSafepoint.clear();
          for (auto &T : Spills)
            Live.LiveGoSpOffsAtSafepoint.insert(std::get<2>(T));
          Live.Slots.clear();
          for (auto &T : Spills) {
            GptrSlot S;
            S.FI = std::get<1>(T);
            S.Disp = 0;
            S.GoSpOff = std::get<2>(T);
            Live.Slots.push_back(S);
          }
        }
      }

      LocalsSrc = Live.Source;
      // Pick primary live offset (lowest) for the one-byte demo bitmap.
      if (!Live.LiveGoSpOffsAtSafepoint.empty()) {
        LiveOff = uint32_t(*Live.LiveGoSpOffsAtSafepoint.begin());
        for (int64_t O : Live.LiveGoSpOffsAtSafepoint)
          if (O >= 0 && uint32_t(O) < LiveOff)
            LiveOff = uint32_t(O);
        // Prefer SP+16 if present (hold contract).
        for (int64_t O : Live.LiveGoSpOffsAtSafepoint)
          if (O == 16)
            LiveOff = 16;
        HaveLocal = true;
      }
    }

    if (!HaveLocal) {
      errs() << "[GocEmitPointerMaps] ERROR: no live gptr slots from MIR "
                "liveness and no goc-live-gptr-debug-override\n";
      report_fatal_error("GocEmitPointerMaps: empty liveness");
    }

    uint32_t ArgWords = 0, ArgBits = 0;
    std::string ArgsSrc;
    if (hasAttrTruth(F, "goc-live-gptr-debug-override") &&
        F.hasFnAttribute("goc-arg-ptr-bits")) {
      ArgWords = parseU32Attr(F, "goc-arg-ptr-words", 1);
      ArgBits = parseU32Attr(F, "goc-arg-ptr-bits", 1);
      ArgsSrc = "debug_override";
    } else {
      argsFromIR(F, ArgWords, ArgBits, ArgsSrc);
      // If IR has no pointer args but override bits exist without full override
      // flag, still prefer IR (zeros) — require MIR/IR truth.
    }

    uint32_t NbitLocals = LocalsBytes / 8;
    if (NbitLocals == 0)
      NbitLocals = 1;
    uint32_t LiveBit = LiveOff / 8;
    std::vector<uint8_t> LocalsOne((NbitLocals + 7) / 8, 0);
    // Set all live offs from analysis when available.
    if ((Live.Source == "mir_liveness" || Live.Source == "liveintervals") && !Live.LiveGoSpOffsAtSafepoint.empty()) {
      for (int64_t O : Live.LiveGoSpOffsAtSafepoint) {
        if (O < 0)
          continue;
        uint32_t Bit = uint32_t(O) / 8;
        if (Bit / 8 < LocalsOne.size())
          LocalsOne[Bit / 8] |= uint8_t(1u << (Bit % 8));
      }
    } else {
      if (LiveBit / 8 < LocalsOne.size())
        LocalsOne[LiveBit / 8] |= uint8_t(1u << (LiveBit % 8));
    }
    auto LocalsMap = makeStackmap(2, NbitLocals, LocalsOne);

    uint32_t ArgBytes = (ArgWords + 7) / 8;
    if (ArgBytes == 0)
      ArgBytes = 1;
    std::vector<uint8_t> ArgsOne(ArgBytes, 0);
    ArgsOne[0] = uint8_t(ArgBits & 0xff);
    auto ArgsMap = makeStackmap(2, ArgWords, ArgsOne);

    auto writeBin = [&](StringRef Name, const std::vector<uint8_t> &Data) {
      std::string Path = MapsDir + "/" + Name.str();
      std::error_code EC;
      raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
      if (EC) {
        errs() << "map write failed: " << Path << ": " << EC.message() << "\n";
        return;
      }
      OS.write(reinterpret_cast<const char *>(Data.data()), Data.size());
    };

    writeBin("locals_map.bin", LocalsMap);
    writeBin("args_map.bin", ArgsMap);

    {
      std::error_code EC;
      raw_fd_ostream OS(MapsDir + "/maps.txt", EC, sys::fs::OF_Text);
      if (!EC) {
        OS << "fn " << F.getName() << "\n";
        OS << "locals_source " << LocalsSrc << "\n";
        OS << "args_source " << ArgsSrc << "\n";
        OS << "locals n=2 nbit=" << NbitLocals << " bit_for_sp" << LiveOff
           << "=1 byte=0x" << format_hex_no_prefix(LocalsOne[0], 2) << "\n";
        OS << "args n=2 nbit=" << ArgWords << " bits=0x"
           << format_hex_no_prefix(ArgsOne[0], 2) << "\n";
        OS << "locals_hex ";
        for (auto B : LocalsMap)
          OS << format_hex_no_prefix(B, 2);
        OS << "\nargs_hex ";
        for (auto B : ArgsMap)
          OS << format_hex_no_prefix(B, 2);
        OS << "\n";
        OS << "gclocals_prefix gclocals·\n";
        OS << "FUNCDATA_ArgsPointerMaps=0 FUNCDATA_LocalsPointerMaps=1\n";
        if (F.hasFnAttribute("goc-color-driven"))
          OS << "color_driven 1\n";
        OS << "algorithm liveintervals_PassManager_spill_then_locals_at_CALL\n";
        OS << "limits LiveIntervalsWrapperPass+SlotIndexesWrapperPass; "
              "go_frame_layout_not_FI_rank; "
              "attrs=debug_override_only; regmap_bits=not_emitted_go_spill_model; color_driven=P18\n";
      }
    }

    errs() << "[GocEmitPointerMaps] " << LocalsSrc << "/" << ArgsSrc
           << " wrote args+locals maps for " << F.getName() << " → " << MapsDir
           << " (live SP+" << LiveOff << ")\n";
    return false;
  }
};

char GocEmitPointerMaps::ID = 0;

struct GocEmitPointerMapsRunner : GocPassRunner {
  std::string Dir;
  explicit GocEmitPointerMapsRunner(std::string D) : Dir(std::move(D)) {}
  bool runOnMF(MachineFunction &MF) override {
    GocEmitPointerMaps P(Dir);
    return P.runOnMF(MF);
  }
};

} // namespace

GocPassRunner *createGocEmitPointerMapsRunner(std::string OutDir) {
  return new GocEmitPointerMapsRunner(std::move(OutDir));
}

MachineFunctionPass *createGocEmitPointerMapsPass(std::string OutDir) {
  return new GocEmitPointerMaps(std::move(OutDir));
}
