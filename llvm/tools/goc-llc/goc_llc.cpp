// goc-llc: llc-19 plus goc's machine passes, appended as the last machine
// passes before the AsmPrinter:
//   GocStackmapPlacement (lib/CodeGen)   drop STACKMAPs not followed by a CALL
//   GocFrameAddrFix (lib/Target/X86)      re-derive frame addresses after calls
//   GocA64* (lib/Target/AArch64)          arm64 LR slot and frame-address fix
//   GocStackCheck (lib/CodeGen + Target)  Go stack-growth prologue
//   GocObjTables (lib/Object)             the .goc.* tables of a .gocobj
//
// Why a driver: stock llc has no hook to insert a plugin MachineFunction pass
// into its codegen pipeline. This file rebuilds exactly what llc's
// LLVMTargetMachine::addPassesToEmitFile does (same TargetPassConfig, same
// cl::opt flags, same TargetOptions). -enable-tail-merge=false is the
// default unless given explicitly.
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
#include "Goc/MachinePasses.h"

using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;
static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input>"), cl::init("-"));
static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));
static cl::opt<std::string> TargetTriple("mtriple", cl::desc("Override target triple for module"));
static cl::opt<std::string> GocTables(
    "goc-tables", cl::desc("Write the .goc.* sections (assembler text) to this file"),
    cl::value_desc("filename"));
static cl::opt<std::string> GocProfile(
    "goc-profile", cl::desc("Go ABI profile (Goc/AbiProfiles.def) the code targets"),
    cl::init("goabi1-amd64"));
static cl::opt<unsigned> GocSymABI(
    "goc-sym-abi",
    cl::desc("Go symbol ABI of the C functions in the tables: 0 ABI0 (Go ABI thunks wrap "
             "them, or a C-only object), 1 ABIInternal"),
    cl::init(0));
static cl::opt<bool> GocNoStackCheck("goc-no-stack-check",
                                     cl::desc("Do not emit the Go stack check (tests only)"));
static cl::opt<char> OptLevel("O", cl::desc("Optimization level"), cl::Prefix, cl::init('2'));

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
    TheTriple = Triple(TargetTriple.empty() ? DLTriple.str() : Triple::normalize(TargetTriple));
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
  if (!TargetTriple.empty())
    M->setTargetTriple(Triple::normalize(TargetTriple));
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
  bool GocA64 = Triple(M->getTargetTriple()).getArch() == Triple::aarch64;
  // arm64: reserve 0(RSP) for the LR copy Go unwinds through (AArch64Frame.h).
  if (GocA64)
    PM.add(goc::createA64CallFramePass());
  PassConfig->addMachinePasses();
  if (GocA64) {
    PM.add(goc::createA64FrameAddrFixPass());
    PM.add(goc::createA64SaveLRPass());
  }
  PassConfig->setInitialized();
  if (TargetPassConfig::willCompleteCodeGenPipeline()) {
    PM.add(goc::createGocStackmapPlacementPass());
    Triple::ArchType Arch = Triple(M->getTargetTriple()).getArch();
    bool IsA64 = Arch == Triple::aarch64;
    // The frame-address repair rewrites x86 instructions.
    if (Arch == Triple::x86_64)
      PM.add(goc::createGocX86FrameAddrFixPass());
    // The profile follows the target unless one was named.
    if (IsA64 && GocProfile.getNumOccurrences() == 0)
      GocProfile = "goabi1-arm64";
    if (!GocNoStackCheck && (Arch == Triple::x86_64 || IsA64)) {
      const goc::AbiProfile *Prof = goc::lookupProfile(GocProfile);
      StringRef Want = IsA64 ? "arm64" : "amd64";
      if (!Prof || StringRef(Prof->Arch) != Want) {
        errs() << "goc-llc: unknown or non-" << Want << " profile " << GocProfile << "\n";
        return 1;
      }
      // The bodies are C: they are entered with the platform convention
      // whatever their Go symbol ABI says.
      goc::StackCheckOptions SO{Prof, goc::EntryABI::C};
      PM.add(goc::createGocStackCheckPass(
          SO, IsA64 ? goc::createAArch64StackCheckTarget() : goc::createX86StackCheckTarget()));
    }
    if (!GocTables.empty())
      PM.add(goc::createGocObjTablesPass(GocTables, GocProfile, GocSymABI));
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
  goc::printFrameAddrFixNote(errs());
  return 0;
}
