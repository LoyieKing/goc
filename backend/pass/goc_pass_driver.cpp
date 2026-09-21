#include "goc_passes.h"
#include "goc_x86.h"
#include "goc_mir_export.h"

#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MIRPrinter.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string>
    OutDir("outdir", cl::desc("Output directory for MIR/recipe/maps"),
           cl::init("build/pass-out"));
static cl::opt<bool> DoWB("wb", cl::desc("Also run store_gptr WB expand"),
                          cl::init(false));

static Function *makeVoidTagged(
    Module &M, StringRef Name,
    std::initializer_list<std::pair<StringRef, StringRef>> Attrs) {
  LLVMContext &Ctx = M.getContext();
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, Name, M);
  for (auto &KV : Attrs)
    F->addFnAttr(KV.first, KV.second);
  auto *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(BB);
  IRB.CreateRetVoid();
  return F;
}

static Function *makeHoldTwo(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), {PtrTy, PtrTy}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "goc_hold_two", M);
  F->addFnAttr("goc-stackcheck", "1");
  F->addFnAttr("goc-emit-maps", "1");
  F->addFnAttr("goc-spill-gptrs", "1");
  F->addFnAttr("goc-frame-locals-bytes", "32");
  F->getArg(0)->setName("p");
  F->getArg(1)->setName("q");
  auto *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(BB);
  IRB.CreateRetVoid();
  return F;
}


static Function *makeHoldRegOnly(Module &M) {
  // Register-only hard case: ptr arg, NO alloca/store in IR. Heap gptr lives
  // only in a vreg across CALL; without spill, maps are empty → FAIL.
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), {PtrTy}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "goc_hold_regonly", M);
  F->addFnAttr("goc-stackcheck", "1");
  F->addFnAttr("goc-emit-maps", "1");
  F->addFnAttr("goc-spill-gptrs", "1");
  F->addFnAttr("goc-frame-locals-bytes", "24");
  F->getArg(0)->setName("p");
  auto *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(BB);
  IRB.CreateRetVoid();
  return F;
}

static Function *makeHoldLive(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), {PtrTy}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "goc_hold_live", M);
  F->addFnAttr("goc-stackcheck", "1");
  F->addFnAttr("goc-emit-maps", "1");
  F->addFnAttr("goc-spill-gptrs", "1");
  F->addFnAttr("goc-frame-locals-bytes", "24");
  Argument *P = F->getArg(0);
  P->setName("p");
  auto *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(BB);
  AllocaInst *Slot = IRB.CreateAlloca(PtrTy, nullptr, "local_gptr");
  IRB.CreateStore(P, Slot);
  IRB.CreateRetVoid();
  return F;
}

static Function *makeHoldArg(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), {PtrTy}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "goc_hold_arg", M);
  F->addFnAttr("goc-stackcheck", "1");
  F->addFnAttr("goc-spill-gptrs", "1");
  F->addFnAttr("goc-frame-locals-bytes", "24");
  Argument *P = F->getArg(0);
  P->setName("p");
  auto *BB = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(BB);
  AllocaInst *Slot = IRB.CreateAlloca(PtrTy, nullptr, "local_gptr");
  IRB.CreateStore(P, Slot);
  IRB.CreateRetVoid();
  return F;
}

static unsigned x86RetOpcode(const TargetInstrInfo &TII,
                             const TargetRegisterInfo &TRI) {
#if GOC_HAVE_X86INSTRINFO
  (void)TII; (void)TRI;
  return X86::RET64;
#else
  goc::X86::InstrInfoLite Lite;
  Lite.resolve(TII, TRI);
  // Lite may not cache RET64 — look up by name.
  for (unsigned I = 0, E = TII.getNumOpcodes(); I != E; ++I)
    if (TII.getName(I) == "RET64")
      return I;
  report_fatal_error("RET64 not found");
#endif
}


static Function *makeLeaf(Module &M) {
  return makeVoidTagged(M, "goc_leaf", {});
}
static Function *makeFadd64(Module &M) {
  return makeVoidTagged(M, "goc_fadd64", {});
}
static Function *makeFadd32(Module &M) {
  return makeVoidTagged(M, "goc_fadd32", {});
}

static MachineFunction &ensureMF(MachineModuleInfo &MMI, Function &F) {
  MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
  if (MF.empty()) {
    auto *MBB = MF.CreateMachineBasicBlock();
    MF.push_back(MBB);
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    BuildMI(MBB, DebugLoc(), TII->get(TargetOpcode::IMPLICIT_DEF));
    // DominatorTree / LiveIntervals require a terminated MBB.
    BuildMI(MBB, DebugLoc(), TII->get(x86RetOpcode(*TII, *TRI)));
  }
  return MF;
}

static void seedHoldLiveMIR(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

#if GOC_HAVE_X86INSTRINFO
  const unsigned OpcCALL64 = X86::CALL64pcrel32;
  const unsigned RAX = X86::RAX;
#else
  goc::X86::InstrInfoLite Lite;
  Lite.resolve(*TII, *TRI);
  const unsigned OpcCALL64 = Lite.CALL64pcrel32;
  const unsigned RAX = Lite.RAX;
#endif

  const TargetRegisterClass *GR64 = nullptr;
  for (unsigned I = 0, E = TRI->getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI->getRegClass(I);
    if (StringRef(TRI->getRegClassName(RC)) == "GR64") {
      GR64 = RC;
      break;
    }
  }
  if (!GR64)
    report_fatal_error("seedHoldLiveMIR: GR64 not found");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register Gptr = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), Gptr).addReg(RAX);
  BuildMI(&Entry, DL, TII->get(OpcCALL64)).addExternalSymbol("main.HugeFrameVoid");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(Gptr);
  BuildMI(&Entry, DL, TII->get(x86RetOpcode(*TII, *TRI)));
}

static void seedHoldArgMIR(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

#if GOC_HAVE_X86INSTRINFO
  const unsigned OpcCALL64 = X86::CALL64pcrel32;
  const unsigned RAX = X86::RAX;
#else
  goc::X86::InstrInfoLite Lite;
  Lite.resolve(*TII, *TRI);
  const unsigned OpcCALL64 = Lite.CALL64pcrel32;
  const unsigned RAX = Lite.RAX;
#endif

  const TargetRegisterClass *GR64 = nullptr;
  for (unsigned I = 0, E = TRI->getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI->getRegClass(I);
    if (StringRef(TRI->getRegClassName(RC)) == "GR64") {
      GR64 = RC;
      break;
    }
  }
  if (!GR64)
    report_fatal_error("seedHoldArgMIR: GR64 not found");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register Gptr = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), Gptr).addReg(RAX);
  BuildMI(&Entry, DL, TII->get(OpcCALL64)).addExternalSymbol("main.HugeFrameVoid");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(Gptr);
  BuildMI(&Entry, DL, TII->get(x86RetOpcode(*TII, *TRI)));
}


static void seedHoldRegOnlyMIR(MachineFunction &MF) {
  // Heap gptr in vreg across CALL; also a live GR64 immediate (NOT a gptr).
  // Tight ID must spill only the ptr; integer must not get a Locals bit.
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

#if GOC_HAVE_X86INSTRINFO
  const unsigned OpcCALL64 = X86::CALL64pcrel32;
  const unsigned OpcMOV64ri = X86::MOV64ri;
  const unsigned RAX = X86::RAX;
#else
  goc::X86::InstrInfoLite Lite;
  Lite.resolve(*TII, *TRI);
  const unsigned OpcCALL64 = Lite.CALL64pcrel32;
  const unsigned OpcMOV64ri = Lite.MOV64ri;
  const unsigned RAX = Lite.RAX;
#endif

  const TargetRegisterClass *GR64 = nullptr;
  for (unsigned I = 0, E = TRI->getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI->getRegClass(I);
    if (StringRef(TRI->getRegClassName(RC)) == "GR64") {
      GR64 = RC;
      break;
    }
  }
  if (!GR64)
    report_fatal_error("seedHoldRegOnlyMIR: GR64 not found");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register Gptr = MRI.createVirtualRegister(GR64);
  Register NotGptr = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), Gptr).addReg(RAX);
  BuildMI(&Entry, DL, TII->get(OpcMOV64ri), NotGptr).addImm(0xdeadbeef);
  BuildMI(&Entry, DL, TII->get(OpcCALL64)).addExternalSymbol("main.HugeFrameVoid");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(Gptr);
  // Keep NotGptr live across CALL without treating it as gptr.
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(NotGptr);
  BuildMI(&Entry, DL, TII->get(x86RetOpcode(*TII, *TRI)));
}

static void seedHoldTwoLiveMIR(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

#if GOC_HAVE_X86INSTRINFO
  const unsigned OpcCALL64 = X86::CALL64pcrel32;
  const unsigned RAX = X86::RAX;
  const unsigned RBX = X86::RBX; // ABIInternal arg1
#else
  goc::X86::InstrInfoLite Lite;
  Lite.resolve(*TII, *TRI);
  const unsigned OpcCALL64 = Lite.CALL64pcrel32;
  const unsigned RAX = Lite.RAX;
  const unsigned RBX = Lite.RBX;
#endif

  const TargetRegisterClass *GR64 = nullptr;
  for (unsigned I = 0, E = TRI->getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI->getRegClass(I);
    if (StringRef(TRI->getRegClassName(RC)) == "GR64") {
      GR64 = RC;
      break;
    }
  }
  if (!GR64)
    report_fatal_error("seedHoldTwoLiveMIR: GR64 not found");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register G0 = MRI.createVirtualRegister(GR64);
  Register G1 = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), G0).addReg(RAX);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), G1).addReg(RBX);
  BuildMI(&Entry, DL, TII->get(OpcCALL64)).addExternalSymbol("main.HugeFrameVoid");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(G0);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RBX).addReg(G1);
  BuildMI(&Entry, DL, TII->get(x86RetOpcode(*TII, *TRI)));
}

namespace {

/// Final pass: dump all MachineFunctions to goc.mir while MMI is still alive.
struct GocDumpMirPass : public ModulePass {
  static char ID;
  std::string Dir;
  explicit GocDumpMirPass(std::string D) : ModulePass(ID), Dir(std::move(D)) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    MachineModuleInfo &MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
    std::error_code EC;
    raw_fd_ostream OS(Dir + "/goc.mir", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return false;
    }
    OS << "# P15 analysis MIR dump via llvm::printMIR (PassManager + LiveIntervals)\n";
#if GOC_HAVE_X86INSTRINFO
    OS << "# X86InstrInfo: real headers\n";
#else
    OS << "# X86InstrInfo: Lite fallback\n";
#endif
    // YAML MIR serialization (not MF.print debug text).
    printMIR(OS, M);
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      if (!MMI.getMachineFunction(F))
        continue;
      MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
      printMIR(OS, MF);
    }
    return false;
  }
};
char GocDumpMirPass::ID = 0;

} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "goc P8 MachineFunctionPass driver\n");

  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();

  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeCore(PR);
  initializeCodeGen(PR);

  std::error_code EC = sys::fs::create_directories(OutDir);
  if (EC) {
    errs() << "mkdir " << OutDir << ": " << EC.message() << "\n";
    return 1;
  }

  std::string TripleStr = "x86_64-unknown-linux-gnu";
  std::string Err;
  const Target *T = TargetRegistry::lookupTarget(TripleStr, Err);
  if (!T) {
    errs() << Err << "\n";
    return 1;
  }
  TargetOptions Opt;
  std::unique_ptr<TargetMachine> TMBase(
      T->createTargetMachine(TripleStr, "generic", "", Opt, Reloc::Static));
  auto *TM = static_cast<LLVMTargetMachine *>(TMBase.get());

  LLVMContext Ctx;
  Module M("goc_p5", Ctx);
  M.setDataLayout(TM->createDataLayout());

  Function *Checked = makeVoidTagged(
      M, "goc_checked_add",
      {{"goc-stackcheck", "1"}, {"goc-frame-locals-bytes", "0"}});
  Function *Hold = makeHoldLive(M);
  Function *HoldArg = makeHoldArg(M);
  Function *HoldTwo = makeHoldTwo(M);
  Function *HoldReg = makeHoldRegOnly(M);
  Function *Leaf = makeLeaf(M);
  Function *Fadd64 = makeFadd64(M);
  Function *Fadd32 = makeFadd32(M);
  Function *Store = nullptr;
  if (DoWB)
    Store = makeVoidTagged(M, "goc_store_gptr", {{"goc-store-gptr", "1"}});

  auto *MMIWP = new MachineModuleInfoWrapperPass(TM);
  MachineModuleInfo &MMI = MMIWP->getMMI();

  {
    MachineFunction &MF = ensureMF(MMI, *Checked);
    (void)MF;
  }
  {
    MachineFunction &MF = ensureMF(MMI, *Hold);
    seedHoldLiveMIR(MF);
  }
  {
    MachineFunction &MF = ensureMF(MMI, *HoldArg);
    seedHoldArgMIR(MF);
  }
  {
    MachineFunction &MF = ensureMF(MMI, *HoldTwo);
    seedHoldTwoLiveMIR(MF);
  }
  {
    MachineFunction &MF = ensureMF(MMI, *HoldReg);
    seedHoldRegOnlyMIR(MF);
  }
  ensureMF(MMI, *Leaf);
  ensureMF(MMI, *Fadd64);
  ensureMF(MMI, *Fadd32);
  if (Store)
    ensureMF(MMI, *Store);

  std::vector<std::string> Recipe;

  legacy::PassManager PM;
  PM.add(MMIWP);
  // P8 pipeline order (documented):
  //   Spill(CALL, primary LIS) → EmitMaps → StackCheck(CFG only)
  //     → RebuildSafeLiveness+MorestackSpill → WB → Dump
  // Primary LiveIntervals on straight-line MIR. After StackCheck's cyclic
  // morestack CFG, GocRebuildLISAfterStackCheck does a safe fixed-point
  // liveness rebuild (not blind LIS) and spills morestack slow path.
  PM.add(createGocSpillGptrsAtSafepointsPass(&Recipe));
  PM.add(createGocEmitPointerMapsPass(OutDir.getValue()));
  PM.add(createGocInsertStackCheckPass(&Recipe));
  PM.add(createGocRebuildLISAfterStackCheckPass(&Recipe));
  if (Store)
    PM.add(createGocExpandStoreGptrPass(&Recipe));
  PM.add(new GocDumpMirPass(OutDir.getValue()));
  // P16: in-place Go-frame lower + printMIR harness (same MMI/MFs; no parallel seed Module)
  PM.add(createGocLowerGoFrameEmitPass(OutDir.getValue(), DoWB));
  PM.run(M);

  {
    raw_fd_ostream OS(OutDir + "/stackcheck.recipe.txt", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "# MachineInstr recipe from GocInsertStackCheck / ExpandStoreGptr / Spill\n";
    OS << "stackguard0_offset 16\n";
    OS << "morestack runtime.morestack_noctxt\n";
    OS << "gclocals_prefix gclocals·\n";
#if GOC_HAVE_X86INSTRINFO
    OS << "x86_instr_info real\n";
#else
    OS << "x86_instr_info lite_fallback\n";
#endif
    OS << "liveintervals via=LiveIntervalsWrapperPass\n";
    for (auto &L : Recipe)
      OS << L << "\n";
  }

  // P8 §2: structured MI lower list for goobj (not hand-authored Prog body).
  {
    raw_fd_ostream OS(OutDir + "/mi_lower.txt", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "# MIR→goobj lower input (from pass recipe / real X86 MIs)\n";
    OS << "format goc-mi-lower-1\n";
    OS << "abi amd64_ABIInternal\n";
    OS << "arch amd64\n";
    for (auto &L : Recipe)
      OS << L << "\n";
    // Full per-fn MI bodies (P11: build.sh overwrites from pass/harness.mir via mirparse;
    // mi_full_bodies.txt is documented fallback only).
    {
      const std::string PassDir = std::string(__FILE__).substr(0, std::string(__FILE__).find_last_of("/\\") + 1);
      std::ifstream Bodies(PassDir + "mi_full_bodies.txt");
      if (!Bodies) {
        // Fallback: same-dir relative to binary CWD during `make run`.
        Bodies.open("mi_full_bodies.txt");
      }
      if (!Bodies) {
        Bodies.open("pass/mi_full_bodies.txt");
      }
      if (!Bodies) {
        errs() << "FATAL: cannot open mi_full_bodies.txt for MIR→goobj export\n";
        return 1;
      }
      OS << "\n# ---- full MI bodies (MIR→goobj; no binwriter templates) ----\n";
      std::string line;
      while (std::getline(Bodies, line))
        OS << line << "\n";
    }
  }

  // P16: harness.mir already written inside PM by GocLowerGoFrameEmitPass.\n\n
    outs() << "PASS-DRIVER: wrote " << OutDir
         << "/{goc.mir,harness.mir,harness.meta.json,stackcheck.recipe.txt,mi_lower.txt,args_map.bin,locals_map.bin,maps.txt}"
         << " (PassManager + LiveIntervals + RebuildLIS + GoFrameLower"
#if GOC_HAVE_X86INSTRINFO
         << " + real X86InstrInfo"
#endif
         << ")\n";
  return 0;
}
