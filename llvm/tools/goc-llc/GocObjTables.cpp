//===- GocObjTables.cpp - write the .goc.* tables of a .gocobj ------------===//
//
// The last machine pass before the AsmPrinter. It puts a label after every
// call and at the end of every function, and at the end of the module writes
// the .goc.* sections (docs/gocobj-format.md) as assembler directives that
// refer to those labels. The driver appends that file to the function
// assembly, so llvm-mc fills in every offset and the one ELF object it
// writes is the .gocobj.
//
// Transitional content (plan row 4): functions (text size, frame size),
// direct-call relocations, and one gc record per safepoint call with empty
// bitmaps. The pointer bits still come from .llvm_stackmaps in the same
// object; Go names, ABI and the stack-check decision still come from
// meta.json. Later rows move those into these tables.
//
//===----------------------------------------------------------------------===//
#include "GocObjTables.h"

#include "X86.h"
#include "MCTargetDesc/X86MCTargetDesc.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <string>
#include <vector>

using namespace llvm;

namespace {

struct CallSite {
  std::string Ret;    // label right after the call instruction
  std::string Target; // empty for an indirect call
  bool Safepoint;
};

struct FuncInfo {
  std::string Name, End;
  uint64_t FrameSize;
  std::vector<CallSite> Calls;
};

struct GocObjTables : MachineFunctionPass {
  static char ID;
  std::string Path;
  std::vector<FuncInfo> Funcs;

  explicit GocObjTables(StringRef P) : MachineFunctionPass(ID), Path(P.str()) {}
  StringRef getPassName() const override { return "goc .gocobj tables"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  static bool isRealCall(const MachineInstr &MI) {
    return MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP &&
           MI.getOpcode() != TargetOpcode::PATCHPOINT;
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const TargetMachine &TM = MF.getTarget();
    MCContext &Ctx = MF.getContext();
    FuncInfo FI;
    FI.Name = TM.getSymbol(&MF.getFunction())->getName().str();
    FI.FrameSize = MF.getFrameInfo().getStackSize();
    MachineInstr *Last = nullptr;
    for (MachineBasicBlock &MBB : MF) {
      bool SawStackmap = false;
      for (MachineInstr &MI : MBB) {
        if (!MI.isMetaInstruction())
          Last = &MI;
        if (MI.getOpcode() == TargetOpcode::STACKMAP) {
          SawStackmap = true;
          continue;
        }
        if (!isRealCall(MI))
          continue;
        CallSite CS;
        CS.Safepoint = SawStackmap;
        SawStackmap = false;
        unsigned Opc = MI.getOpcode();
        if ((Opc == X86::CALL64pcrel32 || Opc == X86::CALLpcrel32) && MI.getNumOperands()) {
          const MachineOperand &MO = MI.getOperand(0);
          if (MO.isGlobal())
            CS.Target = TM.getSymbol(MO.getGlobal())->getName().str();
          else if (MO.isSymbol())
            CS.Target = MO.getSymbolName();
          else if (MO.isMCSymbol())
            CS.Target = MO.getMCSymbol()->getName().str();
        }
        if (MI.getPostInstrSymbol())
          return false; // someone else owns the slot; skip this function
        MCSymbol *S = Ctx.createTempSymbol("goc_ret");
        MI.setPostInstrSymbol(MF, S);
        CS.Ret = S->getName().str();
        FI.Calls.push_back(std::move(CS));
      }
    }
    if (!Last)
      return false;
    if (MCSymbol *S = Last->getPostInstrSymbol()) {
      FI.End = S->getName().str();
    } else {
      MCSymbol *E = Ctx.createTempSymbol("goc_end");
      Last->setPostInstrSymbol(MF, E);
      FI.End = E->getName().str();
    }
    Funcs.push_back(std::move(FI));
    return true;
  }

  bool doFinalization(Module &M) override;
};

char GocObjTables::ID = 0;

// Assembler text for the .goc.* sections.
struct Writer {
  raw_ostream &OS;
  StringMap<unsigned> Strs;
  std::vector<std::string> StrList;
  unsigned Rec = 0;

  unsigned str(StringRef S) {
    auto [It, New] = Strs.try_emplace(S, StrList.size());
    if (New)
      StrList.push_back(S.str());
    return It->second;
  }
  std::string ref(StringRef S) {
    return ".Lgoc_s" + std::to_string(str(S)) + "-.Lgoc_strtab";
  }
  void section(StringRef Name) { OS << "\t.section " << Name << ",\"\",@progbits\n"; }
  // A record: uleb length, then the body between two labels.
  std::string begin() {
    std::string L = ".Lgoc_r" + std::to_string(Rec++);
    OS << L << ":\n\t.uleb128 " << L << "e-" << L << "b\n" << L << "b:\n";
    return L;
  }
  void end(const std::string &L) { OS << L << "e:\n"; }
  void uleb(const Twine &E) { OS << "\t.uleb128 " << E << "\n"; }
  void sleb(const Twine &E) { OS << "\t.sleb128 " << E << "\n"; }
  void u8(unsigned V) { OS << "\t.byte " << V << "\n"; }
};

bool GocObjTables::doFinalization(Module &M) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "goc-llc: " << Path << ": " << EC.message() << "\n";
    return false;
  }
  Writer W{OS};
  OS << "# .goc.* tables written by goc-llc (docs/gocobj-format.md)\n";

  // gcmaps first: funcs records point at them.
  std::vector<std::string> GC(Funcs.size());
  W.section(".goc.gcmaps");
  OS << ".Lgoc_gcbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    unsigned N = 0;
    for (const CallSite &C : F.Calls)
      N += C.Safepoint;
    if (!N)
      continue;
    std::string L = W.begin();
    GC[I] = L;
    W.uleb("0");
    W.uleb("0");
    W.uleb(Twine(N));
    for (const CallSite &C : F.Calls)
      if (C.Safepoint) {
        W.uleb(C.Ret + "-" + F.Name);
        W.uleb("0");
      }
    W.end(L);
  }

  W.section(".goc.funcs");
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    std::string L = W.begin();
    W.uleb(W.ref(F.Name));
    W.uleb(F.End + "-" + F.Name);
    W.uleb(Twine(F.FrameSize));
    W.uleb("0");          // arg_size: still in meta.json
    W.u8(1);              // ABIInternal; meta.json lists the ABI0 exceptions
    W.u8(0);              // kind normal
    W.uleb("0");          // flags: stack-check decision still in elfpack
    W.u8(0);              // func_id
    W.uleb("0");          // file
    W.uleb("0");          // start_line
    W.uleb("0");          // pcv: none yet
    W.uleb(GC[I].empty() ? std::string("0") : GC[I] + "-.Lgoc_gcbase+1");
    W.uleb("0");          // inl
    W.end(L);
  }

  W.section(".goc.relocs");
  for (const FuncInfo &F : Funcs)
    for (const CallSite &C : F.Calls) {
      if (C.Target.empty())
        continue;
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      W.uleb(C.Ret + "-4-" + F.Name);
      W.u8(4);
      W.u8(1); // CALL
      W.uleb(W.ref(C.Target));
      W.sleb("-4");
      W.u8(C.Safepoint ? 1 : 0);
      W.end(L);
    }

  const DataLayout &DL = M.getDataLayout();
  W.section(".goc.data");
  for (const GlobalVariable &G : M.globals()) {
    if (G.isDeclaration() || G.hasPrivateLinkage() || G.getName().starts_with("llvm."))
      continue;
    std::string Name = G.getName().str();
    std::string L = W.begin();
    W.uleb(W.ref(Name));
    W.uleb(Twine(DL.getTypeAllocSize(G.getValueType())));
    W.u8(Log2(DL.getPreferredAlign(&G)));
    uint64_t Flags = (G.isConstant() ? 1 : 0) | (G.hasLocalLinkage() ? 4 : 0);
    W.uleb(Twine(Flags));
    W.uleb("0"); // pointer bitmap: not yet
    W.end(L);
  }

  unsigned Prof = W.str("go1.24-amd64");
  unsigned Prod = W.str("goc-llc (LLVM " LLVM_VERSION_STRING ")");
  W.section(".goc.header");
  OS << "\t.byte 0x47,0x4f,0x43,0x4f,0x42,0x4a,0,0\n"
     << "\t.short 1\n\t.short 0\n"
     << "\t.byte 1\n\t.byte 0\n\t.short 0\n"
     << "\t.long .Lgoc_s" << Prof << "-.Lgoc_strtab\n"
     << "\t.long .Lgoc_s" << Prod << "-.Lgoc_strtab\n"
     << "\t.long 0\n";

  W.section(".goc.strtab");
  OS << ".Lgoc_strtab:\n\t.byte 0\n";
  for (size_t I = 0; I < W.StrList.size(); ++I) {
    OS << ".Lgoc_s" << I << ":\n\t.asciz \"";
    OS.write_escaped(W.StrList[I]);
    OS << "\"\n";
  }
  return false;
}

} // namespace

MachineFunctionPass *goc::createGocObjTablesPass(StringRef Path) {
  return new GocObjTables(Path);
}
