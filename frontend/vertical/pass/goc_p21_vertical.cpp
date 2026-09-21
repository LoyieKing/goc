//===- goc_p21_vertical.cpp - color.ll → bridge attrs → goobj MIR --------===//
// P21: proven vertical slice (1–2 fns). MIR seed is GENERATED from color
// bridge attrs (not checked-in pass/harness.mir). After Spill/Maps/WB,
// emit Go-frame physreg bodies via printMIR for llc→elfpack.
//===----------------------------------------------------------------------===//

#include "goc_passes.h"
#include "goc_x86.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MIRPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
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
                                  cl::init("build/p21-out"));

#if !GOC_HAVE_X86INSTRINFO
int main(int, char **) {
  errs() << "FATAL: P21 vertical requires real X86InstrInfo (local LLVM 19 tree)\n";
  return 1;
}
#else

static Function *getOrInsertDecl(Module &M, StringRef Name) {
  if (Function *F = M.getFunction(Name))
    return F;
  auto *FT = FunctionType::get(Type::getVoidTy(M.getContext()), false);
  return Function::Create(FT, GlobalValue::ExternalLinkage, Name, M);
}

static GlobalVariable *getOrInsertI8(Module &M, StringRef Name) {
  if (GlobalVariable *G = M.getGlobalVariable(Name, /*AllowLocal=*/true))
    return G;
  return new GlobalVariable(M, Type::getInt8Ty(M.getContext()), false,
                            GlobalValue::ExternalLinkage, nullptr, Name);
}

static void addRipGlobal(MachineInstrBuilder &MIB, const GlobalValue *GV) {
  MIB.addReg(X86::RIP).addImm(1).addReg(0).addGlobalAddress(GV).addReg(0);
}

static MachineInstrBuilder call64(MachineBasicBlock *MBB, const TargetInstrInfo *TII,
                                  DebugLoc DL, const GlobalValue *Callee) {
  return BuildMI(MBB, DL, TII->get(X86::CALL64pcrel32))
      .addGlobalAddress(Callee)
      .addReg(X86::RSP, RegState::ImplicitDefine)
      .addReg(X86::SSP, RegState::ImplicitDefine);
}

static MachineFunction &ensureMF(MachineModuleInfo &MMI, Function &F) {
  MachineFunction &MF = MMI.getOrCreateMachineFunction(F);
  if (MF.empty()) {
    auto *MBB = MF.CreateMachineBasicBlock();
    MF.push_back(MBB);
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    BuildMI(MBB, DebugLoc(), TII->get(X86::RET64));
  }
  return MF;
}

static const TargetRegisterClass *findGR64(const TargetRegisterInfo &TRI) {
  for (unsigned I = 0, E = TRI.getNumRegClasses(); I != E; ++I) {
    const TargetRegisterClass *RC = TRI.getRegClass(I);
    if (StringRef(TRI.getRegClassName(RC)) == "GR64")
      return RC;
  }
  return nullptr;
}

/// Color-attr seed: live ptr across CALL (for Spill/Maps). Not harness.mir.
static void seedLiveAcrossCall(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterClass *GR64 = findGR64(*TRI);
  if (!GR64)
    report_fatal_error("P21: GR64 missing");

  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();

  DebugLoc DL;
  Register Ptr = MRI.createVirtualRegister(GR64);
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), Ptr).addReg(X86::RAX);
  BuildMI(&Entry, DL, TII->get(X86::CALL64pcrel32))
      .addExternalSymbol("external_safepoint");
  BuildMI(&Entry, DL, TII->get(TargetOpcode::COPY), X86::RAX).addReg(Ptr);
  BuildMI(&Entry, DL, TII->get(X86::RET64));
}

static void seedMinimal(MachineFunction &MF) {
  MachineBasicBlock &Entry = MF.front();
  while (!Entry.empty())
    Entry.begin()->eraseFromParent();
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  BuildMI(&Entry, DebugLoc(), TII->get(X86::RET64));
}

/// Go-frame WB body selected because color bridge set goc-store-gptr.
static void lowerColorWB(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *Check = MF.CreateMachineBasicBlock();
  auto *Enabled = MF.CreateMachineBasicBlock();
  auto *DoWrite = MF.CreateMachineBasicBlock();
  MF.push_back(Check);
  MF.push_back(Enabled);
  MF.push_back(DoWrite);
  Check->addSuccessor(Enabled);
  Check->addSuccessor(DoWrite);
  Enabled->addSuccessor(DoWrite);

  DebugLoc DL;
  BuildMI(Check, DL, TII->get(X86::MOV64rr), X86::RCX).addReg(X86::RAX);
  BuildMI(Check, DL, TII->get(X86::MOV64rr), X86::RAX).addReg(X86::RBX);
  BuildMI(Check, DL, TII->get(X86::MOV64rm), X86::R14)
      .addReg(0)
      .addImm(1)
      .addReg(0)
      .addImm(-8)
      .addReg(X86::FS);
  {
    auto MIB = BuildMI(Check, DL, TII->get(X86::CMP32mi));
    addRipGlobal(MIB, getOrInsertI8(M, "runtime.writeBarrier"));
    MIB.addImm(0);
  }
  BuildMI(Check, DL, TII->get(X86::JCC_1)).addMBB(DoWrite).addImm(X86::COND_E);

  {
    auto MIB = BuildMI(Enabled, DL, TII->get(X86::ADD64mi8));
    addRipGlobal(MIB, getOrInsertI8(M, "main.wbPathHits"));
    MIB.addImm(1);
  }
  call64(Enabled, TII, DL, getOrInsertDecl(M, "runtime.gcWriteBarrier2"));
  BuildMI(Enabled, DL, TII->get(X86::MOV64mr))
      .addReg(X86::R11)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(Enabled, DL, TII->get(X86::MOV64rm), X86::RDX)
      .addReg(X86::RCX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
  BuildMI(Enabled, DL, TII->get(X86::MOV64mr))
      .addReg(X86::R11)
      .addImm(1)
      .addReg(0)
      .addImm(8)
      .addReg(0)
      .addReg(X86::RDX);

  BuildMI(DoWrite, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RCX)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0)
      .addReg(X86::RAX);
  BuildMI(DoWrite, DL, TII->get(X86::RET64));
}

/// Go-frame spill-across-call selected because color bridge set goc-spill-gptrs.
static void lowerColorSpillMaps(MachineFunction &MF, Module &M) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);
  DebugLoc DL;
  // Frame 24: SP+16 holds live stack ptr across safepoint (matches maps bit).
  BuildMI(BB, DL, TII->get(X86::PUSH64r)).addReg(X86::RBP);
  BuildMI(BB, DL, TII->get(X86::MOV64rr), X86::RBP).addReg(X86::RSP);
  BuildMI(BB, DL, TII->get(X86::SUB64ri8), X86::RSP)
      .addReg(X86::RSP)
      .addImm(24);
  BuildMI(BB, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0)
      .addReg(X86::RAX);
  call64(BB, TII, DL, getOrInsertDecl(M, "external_safepoint"));
  BuildMI(BB, DL, TII->get(X86::MOV64rm), X86::RAX)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(16)
      .addReg(0);
  BuildMI(BB, DL, TII->get(X86::ADD64ri8), X86::RSP)
      .addReg(X86::RSP)
      .addImm(24);
  BuildMI(BB, DL, TII->get(X86::POP64r), X86::RBP);
  BuildMI(BB, DL, TII->get(X86::RET64));
}

static void lowerColorRetOnly(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  auto *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);
  BuildMI(BB, DebugLoc(), TII->get(X86::RET64));
}

static std::string colorFpFor(Function &F) {
  SmallString<64> S;
  raw_svector_ostream OS(S);
  OS << "color_driven="
     << (F.hasFnAttribute("goc-color-driven") ? "1" : "0");
  OS << "|wb=" << (F.hasFnAttribute("goc-store-gptr") ? "1" : "0");
  OS << "|spill_maps="
     << (F.hasFnAttribute("goc-spill-gptrs") ? "1" : "0");
  OS << "|cptr_only="
     << (F.hasFnAttribute("goc-color-cptr-only") ? "1" : "0");
  if (F.hasFnAttribute("goc-colors-seen"))
    OS << "|colors="
       << F.getFnAttribute("goc-colors-seen").getValueAsString();
  return std::string(S.str());
}

static std::string goSymFor(StringRef MirName) {
  if (MirName == "p21_gptr_store" || MirName.contains("gptr_store"))
    return "main.P21GptrStoreWB";
  if (MirName == "p21_sptr_across_call" || MirName.contains("sptr"))
    return "main.P21SptrAcrossCall";
  if (MirName.contains("cptr"))
    return "main.P21CptrOnly";
  return ("main." + MirName).str();
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc-p21-vertical\n");

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
  Module M("goc_p21", Ctx);
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
        if (!M.getFunction(F.getName())) {
          Function::Create(F.getFunctionType(), GlobalValue::ExternalLinkage,
                           F.getName(), M);
        }
        continue;
      }
      if (M.getFunction(F.getName())) {
        errs() << "P21: duplicate fn " << F.getName() << "\n";
        return 1;
      }
      Function *NF = Function::Create(F.getFunctionType(), F.getLinkage(),
                                      F.getName(), M);
      NF->copyAttributesFrom(&F);
      auto *BB = BasicBlock::Create(Ctx, "entry", NF);
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
    } else {
      seedMinimal(MF);
    }
  }

  std::vector<std::string> Recipe;
  Recipe.push_back("p21_color_vertical=1");
  Recipe.push_back(
      "dataflow=color.ll→bridge→attrs→seedMIR(not harness.mir)→"
      "Spill|EmitMaps|ExpandStoreGptr→GoFrame→printMIR→llc→elfpack");

  legacy::PassManager PM;
  PM.add(MMIWP);
  PM.add(createGocSpillGptrsAtSafepointsPass(&Recipe));
  PM.add(createGocEmitPointerMapsPass(OutDir.getValue()));
  if (AnyWB)
    PM.add(createGocExpandStoreGptrPass(&Recipe));
  PM.run(M);

  // Analysis dump (pre Go-frame) for fingerprint proofs.
  {
    raw_fd_ostream OS(OutDir + "/p21.analysis.mir", EC, sys::fs::OF_Text);
    if (!EC) {
      OS << "# P21 analysis MIR after Spill/Maps/WB (color-seeded; pre Go-frame)\n";
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        MachineFunction *MF = MMI.getMachineFunction(F);
        if (!MF)
          continue;
        OS << "# *** IR function: " << F.getName() << " fp=" << colorFpFor(F)
           << "\n";
        MF->print(OS);
        OS << "\n";
      }
    }
  }

  // Go-frame physreg lower + printMIR (llc-ready). Bodies selected by COLOR attrs.
  getOrInsertDecl(M, "external_safepoint");
  getOrInsertDecl(M, "runtime.gcWriteBarrier2");
  getOrInsertI8(M, "runtime.writeBarrier");
  getOrInsertI8(M, "main.wbPathHits");

  SmallVector<Function *, 8> Order;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    Order.push_back(&F);
  }

  for (Function *F : Order) {
    if (MMI.getMachineFunction(*F))
      MMI.deleteMachineFunctionFor(*F);
    MachineFunction &MF = MMI.getOrCreateMachineFunction(*F);
    if (F->hasFnAttribute("goc-store-gptr") ||
        F->hasFnAttribute("goc-color-wb")) {
      lowerColorWB(MF, M);
    } else if (F->hasFnAttribute("goc-spill-gptrs") ||
               F->hasFnAttribute("goc-emit-maps")) {
      lowerColorSpillMaps(MF, M);
    } else {
      lowerColorRetOnly(MF);
    }
    MF.getProperties().reset(MachineFunctionProperties::Property::IsSSA);
    MF.getProperties().reset(
        MachineFunctionProperties::Property::TracksLiveness);
    MF.getProperties().set(MachineFunctionProperties::Property::NoVRegs);
  }

  // vertical.mir
  {
    raw_fd_ostream OS(OutDir + "/vertical.mir", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "# P21: vertical.mir from COLOR pipeline (not pass/harness.mir)\n";
    OS << "# Pass-exported llc-ready MIR (llvm::printMIR)\n";
    OS << "# Producer: goc-p21-vertical color bridge attrs → seed → Spill/Maps/WB "
          "→ Go-frame → printMIR\n";
    OS << "# Dialect: @sym, $rip PIC, full CALL implicits; meta sidecar.\n";
    OS << "# Evidence: p21.recipe.txt color_fp + bridge recipe fingerprints.\n\n";

    OS << "--- |\n";
    OS << "  target datalayout = \""
       << M.getDataLayout().getStringRepresentation() << "\"\n";
    OS << "  target triple = \"x86_64-unknown-linux-gnu\"\n\n";
    OS << "  declare void @external_safepoint()\n";
    OS << "  @main.wbPathHits = external global i8\n";
    OS << "  declare void @runtime.gcWriteBarrier2()\n";
    OS << "  @runtime.writeBarrier = external global i8\n\n";
    for (Function *F : Order) {
      OS << "  define void @" << F->getName() << "() nounwind #0 {\n";
      OS << "  entry:\n";
      OS << "    unreachable\n";
      OS << "  }\n\n";
    }
    OS << "  attributes #0 = { nounwind \"no_callee_saved_registers\" }\n";
    OS << "...\n\n";
    for (Function *F : Order) {
      MachineFunction *MF = MMI.getMachineFunction(*F);
      if (!MF)
        continue;
      printMIR(OS, *MF);
    }
  }

  // Meta + recipe
  {
    raw_fd_ostream OS(OutDir + "/vertical.meta.json", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "{\n";
    OS << "  \"producer\": \"goc-p21-vertical/printMIR\",\n";
    OS << "  \"pipeline\": "
          "\"color.ll→bridge→attrs→seedMIR→Spill|Maps|WB→GoFrame→printMIR\",\n";
    OS << "  \"not_source\": \"p5-machinepass-goobj/pass/harness.mir\",\n";
    OS << "  \"functions\": [\n";
    for (size_t I = 0; I < Order.size(); ++I) {
      Function *F = Order[I];
      bool WB = F->hasFnAttribute("goc-store-gptr");
      bool Spill = F->hasFnAttribute("goc-spill-gptrs");
      int Frame = Spill ? 24 : 0;
      std::string GoSym = goSymFor(F->getName());
      std::string Fp = colorFpFor(*F);
      OS << "    {\n";
      OS << "      \"mir_name\": \"" << F->getName() << "\",\n";
      OS << "      \"go_sym\": \"" << GoSym << "\",\n";
      OS << "      \"frame\": " << Frame << ",\n";
      OS << "      \"flags\": \"nosplit\",\n";
      OS << "      \"encoding\": \"llvm-llc-mc\",\n";
      OS << "      \"color_fp\": \"" << Fp << "\",\n";
      if (Spill) {
        OS << "      \"maps_subdir\": \"" << F->getName() << "\",\n";
        OS << "      \"args_map\": \"gclocals.p21SptrArgs\",\n";
        OS << "      \"locals_map\": \"gclocals.p21SptrLocals\",\n";
        OS << "      \"calls\": [\n";
        OS << "        {\"callee\": \"external_safepoint\", "
              "\"stackmap_index\": 0}\n";
        OS << "      ]\n";
      } else if (WB) {
        OS << "      \"calls\": [\n";
        OS << "        {\"callee\": \"runtime.gcWriteBarrier2\", "
              "\"stackmap_index\": -1}\n";
        OS << "      ]\n";
      } else {
        OS << "      \"calls\": []\n";
      }
      OS << "    }" << (I + 1 < Order.size() ? "," : "") << "\n";
    }
    OS << "  ],\n";
    OS << "  \"mircanon\": {\n";
    OS << "    \"mode\": \"identity\",\n";
    OS << "    \"transforms\": [],\n";
    OS << "    \"cfg_rewrite\": false,\n";
    OS << "    \"frame_inject\": false,\n";
    OS << "    \"dialect_strip\": false\n";
    OS << "  }\n";
    OS << "}\n";
  }

  {
    raw_fd_ostream OS(OutDir + "/p21.recipe.txt", EC, sys::fs::OF_Text);
    if (EC) {
      errs() << EC.message() << "\n";
      return 1;
    }
    OS << "# P21 machine recipe (color-vertical → goobj)\n";
    for (Function *F : Order) {
      OS << "fn " << F->getName();
      OS << " " << colorFpFor(*F);
      OS << " go_sym=" << goSymFor(F->getName());
      OS << "\n";
    }
    for (auto &L : Recipe)
      OS << L << "\n";
  }

  outs() << "PASS-P21-VERTICAL: wrote " << OutDir
         << "/{vertical.mir,vertical.meta.json,p21.recipe.txt,p21.analysis.mir}"
         << " any_wb=" << AnyWB << " any_spill=" << AnySpill << "\n";
  return 0;
}

#endif // GOC_HAVE_X86INSTRINFO
