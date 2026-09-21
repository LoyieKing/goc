//===- goc_p18_driver.cpp - P18 vertical: color attrs → Spill/Maps/WB -----===//
// Loads bridged.ll (attrs from goc-color-bridge), seeds MIR like P5, runs
// existing GocSpill / EmitMaps / ExpandStoreGptr. Proves color-driven behavior.
//===----------------------------------------------------------------------===//

#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <string>
#include <vector>

using namespace llvm;

static cl::list<std::string> InputFiles(cl::Positional, cl::desc("<bridged.ll>..."),
                                        cl::OneOrMore);
static cl::opt<std::string> OutDir("outdir", cl::desc("Output directory"),
                                  cl::init("build/p18-out"));

static unsigned x86RetOpcode(const TargetInstrInfo &TII,
                             const TargetRegisterInfo &TRI) {
#if GOC_HAVE_X86INSTRINFO
  (void)TII;
  (void)TRI;
  return X86::RET64;
#else
  for (unsigned I = 0, E = TII.getNumOpcodes(); I != E; ++I)
    if (TII.getName(I) == "RET64")
      return I;
  report_fatal_error("RET64 not found");
#endif
}

static const TargetRegisterClass *findGR64(const TargetRegisterInfo &TRI) {
  for (unsigned I = 0, E = TRI.getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI.getRegClass(I);
    if (StringRef(TRI.getRegClassName(RC)) == "GR64")
      return RC;
  }
  return nullptr;
}

static MachineFunction &ensureMF(MachineModuleInfo &MMI, Function &F) {
  MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
  if (MF.empty()) {
    auto *MBB = MF.CreateMachineBasicBlock();
    MF.push_back(MBB);
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    BuildMI(MBB, DebugLoc(), TII->get(TargetOpcode::IMPLICIT_DEF));
    BuildMI(MBB, DebugLoc(), TII->get(x86RetOpcode(*TII, *TRI)));
  }
  return MF;
}

static void seedLiveAcrossCall(MachineFunction &MF) {
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
  const TargetRegisterClass *GR64 = findGR64(*TRI);
  if (!GR64)
    report_fatal_error("P18: GR64 missing");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register Ptr = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), Ptr).addReg(RAX);
  BuildMI(&Entry, DL, TII->get(OpcCALL64)).addExternalSymbol("external_safepoint");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), RAX).addReg(Ptr);
  BuildMI(&Entry, DL, TII->get(x86RetOpcode(*TII, *TRI)));
}

static void seedMinimal(MachineFunction &MF) {
  // Leave terminated empty-ish body for WB expand (splices entry).
  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  BuildMI(&Entry, DebugLoc(), TII->get(x86RetOpcode(*TII, *TRI)));
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc-p18-driver\n");

  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeCore(PR);
  initializeCodeGen(PR);

  std::error_code EC;
  sys::fs::create_directories(OutDir);

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
  // Merge all bridged inputs into one Module for a single PM run.
  Module M("goc_p18", Ctx);
  M.setDataLayout(TM->createDataLayout());

  SMDiagnostic Diag;
  for (const std::string &Path : InputFiles) {
    std::unique_ptr<Module> Part = parseIRFile(Path, Diag, Ctx);
    if (!Part) {
      Diag.print(argv[0], errs());
      return 1;
    }
    for (Function &F : *Part) {
      if (F.isDeclaration()) {
        // Materialize decls used by bodies (external_safepoint etc.)
        if (!M.getFunction(F.getName())) {
          Function::Create(F.getFunctionType(), GlobalValue::ExternalLinkage,
                           F.getName(), M);
        }
        continue;
      }
      if (M.getFunction(F.getName())) {
        errs() << "P18: duplicate fn " << F.getName() << "\n";
        return 1;
      }
      // Clone function into M by parsing: simplest — value-map clone via
      // redefine: create empty + copy attrs + recreate signature.
      Function *NF = Function::Create(F.getFunctionType(), F.getLinkage(),
                                      F.getName(), M);
      NF->copyAttributesFrom(&F);
      // Body not needed for MIR seed path; attrs drive passes.
      auto *BB = BasicBlock::Create(Ctx, "entry", NF);
      // Minimal IR body so pointer args / allocas exist for R2/R3 where useful.
      IRBuilder<> IRB(BB);
      if (NF->arg_size() > 0 && NF->getArg(0)->getType()->isPointerTy() &&
          NF->hasFnAttribute("goc-spill-gptrs")) {
        Type *PtrTy = PointerType::getUnqual(Ctx);
        AllocaInst *Slot = IRB.CreateAlloca(PtrTy, nullptr, "local_map_ptr");
        IRB.CreateStore(NF->getArg(0), Slot);
      }
      if (NF->getReturnType()->isVoidTy())
        IRB.CreateRetVoid();
      else if (NF->getReturnType()->isIntegerTy(32))
        IRB.CreateRet(ConstantInt::get(NF->getReturnType(), 0));
      else
        IRB.CreateRet(UndefValue::get(NF->getReturnType()));
    }
  }

  auto *MMIWP = new MachineModuleInfoWrapperPass(TM);
  MachineModuleInfo &MMI = MMIWP->getMMI();

  bool AnyWB = false;
  bool AnySpill = false;
  bool AnyCptr = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    MachineFunction &MF = ensureMF(MMI, F);
    if (F.hasFnAttribute("goc-spill-gptrs") || F.hasFnAttribute("goc-emit-maps")) {
      seedLiveAcrossCall(MF);
      AnySpill = true;
    } else if (F.hasFnAttribute("goc-store-gptr") ||
               F.hasFnAttribute("goc-color-wb")) {
      seedMinimal(MF);
      AnyWB = true;
    } else if (F.hasFnAttribute("goc-color-cptr-only")) {
      seedMinimal(MF);
      AnyCptr = true;
    } else {
      seedMinimal(MF);
    }
  }

  std::vector<std::string> Recipe;
  Recipe.push_back("p18_color_driven=1");
  Recipe.push_back("dataflow=bridged.ll→attrs→Spill|EmitMaps|ExpandStoreGptr");

  legacy::PassManager PM;
  PM.add(MMIWP);
  PM.add(createGocSpillGptrsAtSafepointsPass(&Recipe));
  PM.add(createGocEmitPointerMapsPass(OutDir.getValue()));
  if (AnyWB)
    PM.add(createGocExpandStoreGptrPass(&Recipe));
  PM.run(M);

  {
    raw_fd_ostream OS(OutDir + "/p18.recipe.txt", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "# P18 machine recipe (color-driven)\n";
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      OS << "fn " << F.getName();
      OS << " color_driven="
         << (F.hasFnAttribute("goc-color-driven") ? 1 : 0);
      OS << " wb=" << (F.hasFnAttribute("goc-store-gptr") ? 1 : 0);
      OS << " spill_maps="
         << (F.hasFnAttribute("goc-spill-gptrs") ? 1 : 0);
      OS << " cptr_only="
         << (F.hasFnAttribute("goc-color-cptr-only") ? 1 : 0);
      if (F.hasFnAttribute("goc-colors-seen"))
        OS << " colors="
           << F.getFnAttribute("goc-colors-seen").getValueAsString();
      OS << "\n";
    }
    for (auto &L : Recipe)
      OS << L << "\n";
  }

  // Dump MIR for proofs
  {
    raw_fd_ostream OS(OutDir + "/p18.mir", EC, sys::fs::OF_Text);
    if (!EC) {
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
        OS << "# *** IR function: " << F.getName() << "\n";
        MF.print(OS);
        OS << "\n";
      }
    }
  }

  outs() << "PASS-P18-DRIVER: wrote " << OutDir
         << "/{p18.recipe.txt,p18.mir,maps…}"
         << " any_wb=" << AnyWB << " any_spill=" << AnySpill
         << " any_cptr=" << AnyCptr << "\n";
  return 0;
}
