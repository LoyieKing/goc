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
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace llvm;

namespace {

struct CallSite {
  std::string Pre;    // label at the call instruction
  std::string Ret;    // label right after the call instruction
  std::string Target; // empty for an indirect call
  bool Safepoint;
};

// A rip-relative LEA of a function address (lea f(%rip), r64).
struct FuncAddr {
  std::string Ret;    // label after the LEA; its disp32 is the last 4 bytes
  std::string Target;
  int64_t Offset;
};

struct FuncInfo {
  std::string Name, End;
  uint64_t FrameSize;
  uint64_t Flags = 0;
  uint64_t SysvPtrRegs = 0; // bit i: SysV integer argument register i holds a pointer
  std::vector<CallSite> Calls;
  std::vector<FuncAddr> Addrs;
  std::vector<uint64_t> SptrSlots; // 1.2: BP-relative offsets, sorted
};

// An alloca (by IR name) plus a byte offset inside it that holds an sptr.
struct SptrTarget {
  std::string Alloca;
  int64_t Off;
  bool operator==(const SptrTarget &O) const { return Alloca == O.Alloca && Off == O.Off; }
};

// .goc.funcs flag bits (docs/gocobj-format.md).
enum : uint64_t {
  FlagInternal = 1u << 5,
  FlagHasSptr = 1u << 6,
  FlagSysvUnmodelled = 1u << 7,
};

static bool isSptrNode(const MDNode *N) {
  if (!N || N->getNumOperands() != 1)
    return false;
  auto *S = dyn_cast<MDString>(N->getOperand(0));
  return S && S->getString() == "sptr";
}

// Whether F carries a !goc.color !{"sptr"} attachment anywhere.
static bool hasSptr(const Function &F) {
  auto IsSptr = isSptrNode;
  if (IsSptr(F.getMetadata("goc.color")))
    return true;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (IsSptr(I.getMetadata("goc.color")))
        return true;
  return false;
}

static bool compilerRoot(StringRef N) {
  return N.starts_with("goc.anchor") || N.starts_with("goc.arganchor") ||
         N.starts_with("goc.spill.root");
}

// The frame slots that hold sptr values (goc-lower meta: sptrSlots): every
// (alloca, offset) named by a !goc.frame.words tag, and every alloca that
// receives a direct store of an sptr-colored pointer.
static std::vector<SptrTarget> sptrTargets(const Function &F) {
  std::vector<SptrTarget> Out;
  auto Add = [&](SptrTarget T) {
    if (std::find(Out.begin(), Out.end(), T) == Out.end())
      Out.push_back(std::move(T));
  };
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      if (const MDNode *W = I.getMetadata("goc.frame.words")) {
        if (W->getNumOperands() < 2)
          continue;
        auto *Name = dyn_cast<MDString>(W->getOperand(1));
        if (!Name || compilerRoot(Name->getString()))
          continue;
        for (unsigned K = 2; K < W->getNumOperands(); ++K)
          if (auto *C = mdconst::dyn_extract<ConstantInt>(W->getOperand(K)))
            Add({Name->getString().str(), C->getSExtValue()});
        continue;
      }
      auto *St = dyn_cast<StoreInst>(&I);
      if (!St)
        continue;
      auto *V = dyn_cast<Instruction>(St->getValueOperand());
      auto *A = dyn_cast<AllocaInst>(St->getPointerOperand());
      if (!V || !A || !V->getType()->isPointerTy() || !V->hasName() || !A->hasName())
        continue;
      if (!isSptrNode(V->getMetadata("goc.color")) || compilerRoot(A->getName()))
        continue;
      Add({A->getName().str(), 0});
    }
  return Out;
}

// Which SysV integer argument registers (rdi rsi rdx rcx r8 r9) carry a
// pointer on entry. Returns false for a signature this model does not cover.
static bool sysvPointerRegs(const Function &F, uint64_t &Mask) {
  unsigned Used = 0;
  Mask = 0;
  for (const Argument &A : F.args()) {
    if (A.hasByValAttr())
      continue;
    if (A.hasInAllocaAttr() || A.hasPreallocatedAttr() || A.hasByRefAttr() ||
        A.hasAttribute(Attribute::InReg) || A.hasNestAttr() || A.hasSwiftSelfAttr() ||
        A.hasSwiftErrorAttr() || A.hasAttribute(Attribute::SwiftAsync))
      return false;
    Type *T = A.getType();
    if (T->isPointerTy() || (T->isIntegerTy() && T->getIntegerBitWidth() <= 64 &&
                             isPowerOf2_32(T->getIntegerBitWidth()) && T->getIntegerBitWidth() != 2 &&
                             T->getIntegerBitWidth() != 4)) {
      if (Used < 6) {
        if (T->isPointerTy())
          Mask |= 1u << Used;
        ++Used;
      }
    } else if (T->isIntegerTy(128)) {
      if (Used <= 4)
        Used += 2;
    } else if (T->isFloatTy() || T->isDoubleTy()) {
      // SSE registers; do not affect the integer sequence
    } else {
      return false;
    }
  }
  return true;
}

struct GocObjTables : MachineFunctionPass {
  static char ID;
  std::string Path;
  std::vector<FuncInfo> Funcs;
  // Computed on the IR as goc-llc received it: CodeGenPrepare and friends
  // later rewrite or drop instructions that carry the colors.
  StringMap<uint64_t> IRFlags, IRSysv;
  StringMap<std::vector<SptrTarget>> IRSptr;
  unsigned InlineAsmLabels = 0;

  // Direct calls written in inline assembly ("call sym" on a line of its
  // own) are invisible to machine passes. Put a label pair around each one
  // in the asm text itself; labels emit no bytes, so the code is unchanged.
  // Returns false when the string was left alone.
  bool labelInlineAsmCalls(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI) {
    MachineOperand &Op = MI.getOperand(0);
    if (!Op.isSymbol())
      return false;
    StringRef Asm = Op.getSymbolName();
    if (!Asm.contains("call"))
      return false;
    std::string Out;
    bool Changed = false;
    SmallVector<StringRef, 16> Lines;
    Asm.split(Lines, '\n');
    for (size_t I = 0; I < Lines.size(); ++I) {
      StringRef L = Lines[I];
      StringRef T = L.trim();
      StringRef Rest;
      if (T.consume_front("callq") || T.consume_front("call")) {
        Rest = T;
        if (!Rest.empty() && (Rest[0] == ' ' || Rest[0] == '\t')) {
          Rest = Rest.trim();
          bool Ok = !Rest.empty() && Rest.find_first_of(" \t*%$,()") == StringRef::npos &&
                    !isdigit((unsigned char)Rest[0]);
          if (Ok) {
            std::string P = ".Lgoc_iac" + std::to_string(InlineAsmLabels);
            std::string R = ".Lgoc_iar" + std::to_string(InlineAsmLabels);
            ++InlineAsmLabels;
            Out += P + ":\n" + L.str() + "\n" + R + ":";
            CallSite CS;
            CS.Pre = P;
            CS.Ret = R;
            CS.Target = Rest.str();
            CS.Safepoint = false;
            FI.Calls.push_back(std::move(CS));
            Changed = true;
            if (I + 1 < Lines.size())
              Out += "\n";
            continue;
          }
        }
      }
      Out += L.str();
      if (I + 1 < Lines.size())
        Out += "\n";
    }
    if (!Changed)
      return false;
    Op.ChangeToES(MF.createExternalSymbolName(Out), Op.getTargetFlags());
    return true;
  }

  bool doInitialization(Module &M) override {
    for (const Function &F : M) {
      if (F.isDeclaration())
        continue;
      uint64_t Fl = 0, Mask = 0;
      if (F.hasInternalLinkage())
        Fl |= FlagInternal;
      if (hasSptr(F))
        Fl |= FlagHasSptr;
      if (!sysvPointerRegs(F, Mask))
        Fl |= FlagSysvUnmodelled;
      IRFlags[F.getName()] = Fl;
      IRSysv[F.getName()] = Mask;
      if (Fl & FlagHasSptr)
        IRSptr[F.getName()] = sptrTargets(F);
    }
    return false;
  }

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
    FI.Flags = IRFlags.lookup(MF.getFunction().getName());
    FI.SysvPtrRegs = IRSysv.lookup(MF.getFunction().getName());
    if (auto It = IRSptr.find(MF.getFunction().getName()); It != IRSptr.end()) {
      const MachineFrameInfo &MFI = MF.getFrameInfo();
      StringMap<int64_t> Obj;
      for (int I = 0, E = MFI.getObjectIndexEnd(); I < E; ++I)
        if (!MFI.isDeadObjectIndex(I))
          if (const AllocaInst *A = MFI.getObjectAllocation(I))
            if (A->hasName())
              Obj.try_emplace(A->getName(), MFI.getObjectOffset(I));
      for (const SptrTarget &T : It->second) {
        auto O = Obj.find(T.Alloca);
        if (O == Obj.end())
          continue;
        int64_t Off = -O->second - 16 - T.Off;
        if (Off < 8 || Off % 8 != 0)
          continue;
        if (std::find(FI.SptrSlots.begin(), FI.SptrSlots.end(), (uint64_t)Off) == FI.SptrSlots.end())
          FI.SptrSlots.push_back(Off);
      }
      std::sort(FI.SptrSlots.begin(), FI.SptrSlots.end());
    }
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
        if (MI.isInlineAsm()) {
          labelInlineAsmCalls(MF, MI, FI);
          continue;
        }
        if (MI.getOpcode() == X86::LEA64r && MI.getNumOperands() >= 6 &&
            MI.getOperand(1).getReg() == X86::RIP && MI.getOperand(4).isGlobal() &&
            isa<Function>(MI.getOperand(4).getGlobal())) {
          if (MI.getPostInstrSymbol())
            return false;
          MCSymbol *S = Ctx.createTempSymbol("goc_lea");
          MI.setPostInstrSymbol(MF, S);
          FI.Addrs.push_back({S->getName().str(),
                              TM.getSymbol(MI.getOperand(4).getGlobal())->getName().str(),
                              MI.getOperand(4).getOffset()});
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
        if (MI.getPostInstrSymbol() || MI.getPreInstrSymbol())
          return false; // someone else owns the slot; skip this function
        MCSymbol *P = Ctx.createTempSymbol("goc_call");
        MI.setPreInstrSymbol(MF, P);
        MCSymbol *S = Ctx.createTempSymbol("goc_ret");
        MI.setPostInstrSymbol(MF, S);
        CS.Pre = P->getName().str();
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

  W.section(".goc.files");
  {
    std::string L = W.begin();
    W.uleb(W.ref(M.getSourceFileName()));
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
    W.uleb(Twine(F.Flags)); // stack-check decision still in elfpack
    W.u8(0);              // func_id
    W.uleb("0");          // file: the translation unit
    W.uleb("0");          // start_line
    W.uleb("0");          // pcv: none yet
    W.uleb(GC[I].empty() ? std::string("0") : GC[I] + "-.Lgoc_gcbase+1");
    W.uleb("0");          // inl
    W.uleb(Twine(F.SysvPtrRegs)); // 1.1: sysv_ptr_regs
    W.uleb(Twine(F.SptrSlots.size())); // 1.2: sptr_slots
    for (uint64_t O : F.SptrSlots)
      W.uleb(Twine(O));
    W.end(L);
  }

  W.section(".goc.relocs");
  for (const FuncInfo &F : Funcs) {
    for (const FuncAddr &A : F.Addrs) {
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      W.uleb(A.Ret + "-4-" + F.Name);
      W.u8(4);
      W.u8(5); // FUNC_ADDR
      W.uleb(W.ref(A.Target));
      W.sleb(Twine(A.Offset - 4));
      W.u8(0);
      W.end(L);
    }
    for (const CallSite &C : F.Calls) {
      if (C.Target.empty()) {
        // CALL_INDIRECT_MARK: offset is the instruction, width its length.
        std::string L = W.begin();
        W.uleb(W.ref(F.Name));
        W.uleb(C.Pre + "-" + F.Name);
        OS << "\t.byte " << C.Ret << "-" << C.Pre << "\n";
        W.u8(2);
        W.uleb("0");
        W.sleb("0");
        W.u8(C.Safepoint ? 1 : 0);
        W.end(L);
        continue;
      }
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
     << "\t.short 1\n\t.short 2\n"
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
