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
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "Goc/StackCheck.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Path.h"
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

// One STACKMAP, in the location encoding of .llvm_stackmaps (version 3).
struct SMLoc {
  uint8_t Type;
  uint16_t Size;
  uint16_t Reg;
  int32_t Off;
};
struct SMRec {
  std::string Label; // at the STACKMAP; its offset is the record's insn offset
  uint64_t ID;
  std::vector<SMLoc> Locs;
};

struct CallSite {
  std::string Pre;    // label at the call instruction
  std::string Ret;    // label right after the call instruction
  bool Morestack = false; // the cold block's call: its stub slots are the roots
  std::string Target; // empty for an indirect call
  bool Safepoint;
  std::vector<SMLoc> Roots; // locations of the STACKMAPs whose site this call is
};

// A rip-relative LEA of a function address (lea f(%rip), r64).
struct FuncAddr {
  std::string Ret;    // label after the LEA; its disp32 is the last 4 bytes
  std::string Target;
  int64_t Offset;
};

// 1.4: one row of the FILE/LINE/INL_INDEX tables, from the label on.
struct LineRow {
  std::string PC; // empty: function start
  unsigned File;
  unsigned Line;
  int Inl;
};

// 1.4: one inlined call (a .goc.inltree entry).
struct InlNode {
  int Parent;
  unsigned File, Line; // the call site, in the parent
  std::string Func;    // the inlined function (C name)
  std::string ParentPC; // label of an instruction attributed to the call site
  unsigned StartLine;
};

struct FuncInfo {
  goc::FuncStackInfo Stack;
  unsigned File = 0, StartLine = 0;
  std::vector<LineRow> Lines;
  std::vector<InlNode> Inl;
  bool A64 = false; // AArch64: BL relocations at the instruction, no stack-map roots
  std::string Name, End;
  uint64_t FrameSize;
  uint64_t Flags = 0;
  uint64_t SysvPtrRegs = 0; // bit i: SysV integer argument register i holds a pointer
  std::vector<CallSite> Calls;
  std::vector<FuncAddr> Addrs;
  std::vector<uint64_t> SptrSlots; // 1.2: BP-relative offsets, sorted
  std::vector<SMRec> Stackmaps;    // 1.3
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
  std::string Profile;
  unsigned SymABI;
  std::vector<FuncInfo> Funcs;
  // Computed on the IR as goc-llc received it: CodeGenPrepare and friends
  // later rewrite or drop instructions that carry the colors.
  StringMap<uint64_t> IRFlags, IRSysv;
  StringMap<std::vector<SptrTarget>> IRSptr;
  unsigned InlineAsmLabels = 0;
  bool A64 = false;
  // 1.4: .goc.files; index 0 is the translation unit.
  StringMap<unsigned> FileIdx;
  std::vector<std::string> FileList;

  unsigned fileIndex(const DIFile *F) {
    if (!F)
      return 0;
    SmallString<256> P(F->getFilename());
    if (!sys::path::is_absolute(P) && !F->getDirectory().empty()) {
      P = F->getDirectory();
      sys::path::append(P, F->getFilename());
    }
    auto [It, New] = FileIdx.try_emplace(P, FileList.size());
    if (New)
      FileList.push_back(P.str().str());
    return It->second;
  }

  static std::string inlName(const DISubprogram *SP) {
    if (!SP)
      return "?";
    if (!SP->getLinkageName().empty())
      return SP->getLinkageName().str();
    return SP->getName().str();
  }

  // Index in FI.Inl of the call that inlined the code at C, or -1.
  int inlIndex(FuncInfo &FI, DenseMap<const DILocation *, int> &Map, const DILocation *C) {
    const DILocation *IA = C->getInlinedAt();
    if (!IA)
      return -1;
    if (auto It = Map.find(IA); It != Map.end())
      return It->second;
    int P = inlIndex(FI, Map, IA);
    const DISubprogram *SP = C->getScope()->getSubprogram();
    FI.Inl.push_back({P, fileIndex(IA->getFile()), IA->getLine(), inlName(SP), "",
                      SP ? SP->getLine() : 0});
    int I = (int)FI.Inl.size() - 1;
    Map[IA] = I;
    return I;
  }

  // 1.4: FILE, LINE and INL_INDEX rows from the instructions' debug
  // locations (clang -gline-tables-only). Instructions without a location
  // (the stack check, spills) keep the previous row. As Go's compiler does
  // with its inline marks, the first non-call instruction of each inlined
  // body is attributed to the call site in the parent: the runtime finds
  // the call site's line through ParentPC.
  void recordLines(MachineFunction &MF, FuncInfo &FI) {
    const DISubprogram *SP = MF.getFunction().getSubprogram();
    if (!SP)
      return;
    FI.File = fileIndex(SP->getFile());
    FI.StartLine = SP->getLine();
    MCContext &Ctx = MF.getContext();
    DenseMap<const DILocation *, int> Map;
    unsigned CurF = FI.File, CurL = FI.StartLine;
    int CurI = -1;
    FI.Lines.push_back({"", CurF, CurL, CurI});
    auto Label = [&](MachineInstr &MI) {
      if (MCSymbol *S = MI.getPreInstrSymbol())
        return S->getName().str();
      MCSymbol *S = Ctx.createTempSymbol("goc_ln");
      MI.setPreInstrSymbol(MF, S);
      return S->getName().str();
    };
    for (MachineBasicBlock &MBB : MF)
      for (MachineInstr &MI : MBB) {
        if (MI.isMetaInstruction() || MI.getOpcode() == TargetOpcode::STACKMAP)
          continue;
        const DILocation *DL = MI.getDebugLoc().get();
        if (!DL || DL->getLine() == 0)
          continue;
        unsigned F = fileIndex(DL->getFile()), L = DL->getLine();
        int I = inlIndex(FI, Map, DL);
        if (I >= 0 && MI.isCall() && isDirectCall(MF, MI)) {
          // An inlined body that starts with a direct call: its interior
          // bytes are never looked up (a return address minus one is the
          // last byte), so pc+1 can carry the call site.
          int Pick = -1;
          for (int J = I; J >= 0; J = FI.Inl[J].Parent)
            if (FI.Inl[J].ParentPC.empty())
              Pick = J;
          if (Pick >= 0) {
            std::string P = Label(MI);
            if (F != CurF || L != CurL || I != CurI)
              FI.Lines.push_back({P, F, L, I});
            const InlNode &N = FI.Inl[Pick];
            FI.Lines.push_back({P + "+1", N.File, N.Line, N.Parent});
            FI.Lines.push_back({P + "+2", F, L, I});
            FI.Inl[Pick].ParentPC = P + "+1";
            CurF = F, CurL = L, CurI = I;
            continue;
          }
        }
        if (I >= 0 && !MI.isCall()) {
          // Outermost call on the chain still without a ParentPC.
          int Pick = -1;
          for (int J = I; J >= 0; J = FI.Inl[J].Parent)
            if (FI.Inl[J].ParentPC.empty())
              Pick = J;
          if (Pick >= 0) {
            FI.Inl[Pick].ParentPC = Label(MI);
            F = FI.Inl[Pick].File;
            L = FI.Inl[Pick].Line;
            I = FI.Inl[Pick].Parent;
          }
        }
        if (F == CurF && L == CurL && I == CurI)
          continue;
        FI.Lines.push_back({Label(MI), F, L, I});
        CurF = F, CurL = L, CurI = I;
      }
    // Otherwise: the last row of the parent before the body.
    for (int K = 0, E = (int)FI.Inl.size(); K < E; ++K) {
      InlNode &N = FI.Inl[K];
      if (!N.ParentPC.empty())
        continue;
      std::string Best;
      bool Found = false;
      for (const LineRow &R : FI.Lines) {
        bool In = false;
        for (int J = R.Inl; J >= 0; J = FI.Inl[J].Parent)
          if (J == K)
            In = true;
        if (In)
          break;
        if (R.Inl == N.Parent)
          Best = R.PC, Found = true;
      }
      if (Found)
        N.ParentPC = Best;
    }
  }

  static bool isDirectCall(MachineFunction &MF, const MachineInstr &MI) {
    unsigned Opc = MI.getOpcode();
    if (MF.getTarget().getTargetTriple().isAArch64())
      return MF.getSubtarget().getInstrInfo()->getName(Opc) == "BL";
    return Opc == X86::CALL64pcrel32 || Opc == X86::CALLpcrel32;
  }

  // Direct calls written in inline assembly ("call sym" on a line of its
  // own) are invisible to machine passes. Put a label pair around each one
  // in the asm text itself; labels emit no bytes, so the code is unchanged.
  // Returns false when the string was left alone.
  // STACKMAP locations not yet given to a call: a record belongs to the first
  // call at or after it in code order (elfpack's "site").
  std::vector<SMLoc> Pending;

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
            CS.Roots = std::move(Pending);
            Pending.clear();
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
    FileIdx[M.getSourceFileName()] = 0;
    FileList.push_back(M.getSourceFileName());
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

  GocObjTables(StringRef P, StringRef Prof, unsigned A)
      : MachineFunctionPass(ID), Path(P.str()), Profile(Prof.str()), SymABI(A) {}
  StringRef getPassName() const override { return "goc .gocobj tables"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  // Mirrors StackMaps::parseOperand on the post-PEI operands.
  static uint16_t dwarfReg(const TargetRegisterInfo *TRI, Register R) {
    int N = TRI->getDwarfRegNum(R, false);
    for (MCPhysReg S : TRI->superregs(R)) {
      if (N >= 0)
        break;
      N = TRI->getDwarfRegNum(S, false);
    }
    return (uint16_t)N;
  }

  bool recordStackmap(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI) {
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    StackMapOpers Opers(&MI);
    SMRec R;
    R.ID = MI.getOperand(StackMapOpers::IDPos).getImm();
    for (auto I = MI.operands_begin() + Opers.getVarIdx(), E = MI.operands_end(); I != E;) {
      if (I->isImm()) {
        switch (I->getImm()) {
        case StackMaps::DirectMemRefOp: {
          Register Reg = (++I)->getReg();
          int64_t Imm = (++I)->getImm();
          R.Locs.push_back({2, (uint16_t)MF.getDataLayout().getPointerSize(), dwarfReg(TRI, Reg), (int32_t)Imm});
          break;
        }
        case StackMaps::IndirectMemRefOp: {
          int64_t Size = (++I)->getImm();
          Register Reg = (++I)->getReg();
          int64_t Imm = (++I)->getImm();
          R.Locs.push_back({3, (uint16_t)Size, dwarfReg(TRI, Reg), (int32_t)Imm});
          break;
        }
        case StackMaps::ConstantOp: {
          int64_t Imm = (++I)->getImm();
          if (!isInt<32>(Imm))
            return false; // ConstantIndex: the pool index is module-wide
          R.Locs.push_back({4, 8, 0, (int32_t)Imm});
          break;
        }
        default:
          return false;
        }
        ++I;
        continue;
      }
      if (I->isReg()) {
        if (I->isImplicit()) {
          ++I;
          continue;
        }
        if (I->isUndef()) {
          R.Locs.push_back({4, 8, 0, (int32_t)0xFEFEFEFE});
          ++I;
          continue;
        }
        const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(I->getReg());
        uint16_t D = dwarfReg(TRI, I->getReg());
        unsigned Off = 0;
        unsigned LLVMReg = *TRI->getLLVMRegNum(D, false);
        if (unsigned Sub = TRI->getSubRegIndex(LLVMReg, I->getReg()))
          Off = TRI->getSubRegIdxOffset(Sub);
        R.Locs.push_back({1, (uint16_t)TRI->getSpillSize(*RC), D, (int32_t)Off});
        ++I;
        continue;
      }
      ++I; // register live-out masks: not used by goc
    }
    if (MI.getPreInstrSymbol())
      return false;
    MCSymbol *S = MF.getContext().createTempSymbol("goc_sm");
    MI.setPreInstrSymbol(MF, S);
    R.Label = S->getName().str();
    Pending.insert(Pending.end(), R.Locs.begin(), R.Locs.end());
    FI.Stackmaps.push_back(std::move(R));
    return true;
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
    FI.A64 = TM.getTargetTriple().isAArch64();
    A64 = A64 || FI.A64;
    const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
    FI.FrameSize = MF.getFrameInfo().getStackSize();
    FI.Flags = IRFlags.lookup(MF.getFunction().getName());
    FI.SysvPtrRegs = IRSysv.lookup(MF.getFunction().getName());
    if (auto It = goc::stackInfo().find(FI.Name); It != goc::stackInfo().end())
      FI.Stack = It->second;
    if (!FI.A64 && FI.Stack.HasCheck && FI.Stack.ABI == goc::EntryABI::C && (FI.Flags & (1u << 7)))
      report_fatal_error("goc: " + Twine(FI.Name) +
                         ": stack check needs the pointer-ness of its SysV argument "
                         "registers, and this signature is not modelled");
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
      // The BP-relative offsets above follow the x86-64 frame. arm64 has
      // no frame-address repair and records no stack maps (as on main,
      // whose meta.json never listed arm64 sptr slots): no slots, so no
      // locals bits, rather than bits computed for the wrong frame.
      if (FI.A64)
        FI.SptrSlots.clear();
    }
    MachineInstr *Last = nullptr;
    Pending.clear();
    for (MachineBasicBlock &MBB : MF) {
      bool SawStackmap = false;
      for (MachineInstr &MI : MBB) {
        if (!MI.isMetaInstruction())
          Last = &MI;
        if (MI.getOpcode() == TargetOpcode::STACKMAP) {
          SawStackmap = true;
          if (!recordStackmap(MF, MI, FI))
            return false;
          continue;
        }
        if (MI.isInlineAsm()) {
          labelInlineAsmCalls(MF, MI, FI);
          continue;
        }
        if (!FI.A64 && MI.getOpcode() == X86::LEA64r && MI.getNumOperands() >= 6 &&
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
        CS.Morestack = &MI == FI.Stack.MorestackCall;
        CS.Safepoint = SawStackmap || CS.Morestack;
        SawStackmap = false;
        CS.Roots = std::move(Pending);
        Pending.clear();
        unsigned Opc = MI.getOpcode();
        bool Direct = FI.A64 ? TII.getName(Opc) == "BL"
                             : (Opc == X86::CALL64pcrel32 || Opc == X86::CALLpcrel32);
        if (Direct && MI.getNumOperands()) {
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
    recordLines(MF, FI);
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
  void section(StringRef Name) { OS << "\t.section " << Name << ",\"\",%progbits\n"; }
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
    // 1.3: one safepoint per call, with the locals bitmap the frame has
    // there: bit i is the word at SP+8*i, SP = BP-(frame-8). Set are the
    // sptr slots, and the roots of the STACKMAPs whose site this call is
    // (Direct BP-relative slots and Constant SP offsets of stack-passed
    // pointers), as elfpack builds them.
    const FuncInfo &F = Funcs[I];
    if (F.Calls.empty())
      continue;
    int64_t Words = F.FrameSize > 8 ? (int64_t)(F.FrameSize - 8) / 8 : 0;
    // At the morestack call the frame is the entry one plus the cold
    // block's saves; the locals bitmap must reach the deepest stub slot.
    for (const goc::StubSlot &S : F.Stack.StubSlots)
      Words = std::max<int64_t>(Words, S.WordBelowVarp);
    std::vector<uint8_t> Base((Words + 7) / 8);
    auto Set = [&](std::vector<uint8_t> &B, int64_t Off) {
      if (Off < 8 || Off % 8 != 0 || Off / 8 > Words)
        return;
      int64_t Bit = Words - Off / 8;
      if (Bit >= 0 && Bit < Words)
        B[Bit / 8] |= 1u << (Bit % 8);
    };
    for (uint64_t O : F.SptrSlots)
      Set(Base, (int64_t)O);
    std::string L = W.begin();
    GC[I] = L;
    W.uleb("0");
    W.uleb(Twine(Words));
    W.uleb(Twine(F.Calls.size()));
    for (const CallSite &C : F.Calls) {
      std::vector<uint8_t> B = Base;
      if (C.Morestack) {
        // Only the spilled pointer arguments are live; the body's slots
        // are not set up yet.
        B.assign(B.size(), 0);
        for (const goc::StubSlot &S : F.Stack.StubSlots)
          if (F.SysvPtrRegs & (1ull << S.ArgReg))
            Set(B, 8 * (int64_t)S.WordBelowVarp);
      }
      for (const SMLoc &Lc : C.Roots) {
        if (F.A64)
          break; // no stack-map roots on arm64
        if (Lc.Type == 2 && Lc.Reg == 6 && Lc.Off < 0 && Lc.Size == 8)
          Set(B, -(int64_t)Lc.Off);
        else if (Lc.Type == 4 && Lc.Off >= 0 && Lc.Off % 8 == 0)
          Set(B, (int64_t)F.FrameSize - 8 - Lc.Off);
      }
      W.uleb(C.Ret + "-" + F.Name);
      for (uint8_t V : B)
        W.u8(V);
      W.uleb("0"); // ncolor
    }
    W.end(L);
  }

  std::vector<std::string> PCV(Funcs.size());
  W.section(".goc.pcvalue");
  OS << ".Lgoc_pcvbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    std::string L = W.begin();
    PCV[I] = L;
    auto Entry = [&](const std::string &PC, int64_t V) {
      W.uleb(PC.empty() ? std::string("0") : PC + "-" + F.Name);
      W.sleb(Twine(V));
    };
    int64_t Frame = (int64_t)F.FrameSize;
    unsigned NLine = F.Lines.empty() ? 0 : (F.Inl.empty() ? 2 : 3);
    if (!F.Stack.HasCheck) {
      W.uleb(Twine(1 + NLine));
      W.u8(1); // SP_DELTA: the body frame from the first instruction
      W.uleb("1");
      Entry("", Frame);
    } else {
      W.uleb(Twine(2 + NLine));
      W.u8(1); // SP_DELTA: entry, body, then the cold block's pushes
      W.uleb(Twine(3 + F.Stack.SPChanges.size()));
      Entry("", 0);
      Entry(F.Stack.BodyStart, Frame);
      Entry(F.Stack.StubStart, 0);
      for (const auto &C : F.Stack.SPChanges)
        Entry(C.first, C.second);
      W.u8(4); // UNSAFE_POINT: the check and the cold block
      W.uleb("3");
      Entry("", -2);
      Entry(F.Stack.BodyStart, -1);
      Entry(F.Stack.StubStart, -2);
    }
    if (NLine) {
      // 1.4: FILE, LINE, INL_INDEX. A row whose label lands on the same pc
      // as the next one (an instruction that emits no bytes) is replaced
      // by it.
      for (unsigned K : {2u, 3u, 5u}) {
        if (K == 5 && F.Inl.empty())
          continue;
        W.u8(K);
        W.uleb(Twine(F.Lines.size()));
        for (const LineRow &R : F.Lines)
          Entry(R.PC, K == 2 ? (int64_t)R.File : K == 3 ? (int64_t)R.Line : (int64_t)R.Inl);
      }
    }
    W.end(L);
  }

  std::vector<std::string> INL(Funcs.size());
  W.section(".goc.inltree");
  OS << ".Lgoc_inlbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    if (F.Inl.empty())
      continue;
    std::string L = W.begin();
    INL[I] = L;
    W.uleb(Twine(F.Inl.size()));
    for (const InlNode &N : F.Inl) {
      W.sleb(Twine(N.Parent));
      W.uleb(Twine(N.File));
      W.uleb(Twine(N.Line));
      W.uleb(W.ref(N.Func));
      W.uleb(N.ParentPC.empty() ? std::string("0") : N.ParentPC + "-" + F.Name);
      W.uleb(Twine(N.StartLine));
    }
    W.end(L);
  }

  W.section(".goc.files");
  for (const std::string &P : FileList) {
    std::string L = W.begin();
    W.uleb(W.ref(P));
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
    W.u8(SymABI);
    W.u8(0); // kind normal
    uint64_t Flags = F.Flags & ~uint64_t(7);
    if (F.Stack.HasCheck)
      Flags |= 1;
    if (F.Stack.NoSplit)
      Flags |= 4;
    if (F.Calls.empty())
      Flags |= 2;
    W.uleb(Twine(Flags | 8));
    W.u8(0);              // func_id
    W.uleb(Twine(F.File));
    W.uleb(Twine(F.StartLine));
    W.uleb(PCV[I] + "-.Lgoc_pcvbase+1");
    W.uleb(GC[I].empty() ? std::string("0") : GC[I] + "-.Lgoc_gcbase+1");
    W.uleb(INL[I].empty() ? std::string("0") : INL[I] + "-.Lgoc_inlbase+1"); // 1.4: inl
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
      if (F.A64) {
        // BL: CALL26 on the instruction itself, no PC bias.
        W.uleb(C.Pre + "-" + F.Name);
        W.u8(4);
        W.u8(11); // ARM64_CALL26
        W.uleb(W.ref(C.Target));
        W.sleb("0");
        W.u8(C.Safepoint ? 1 : 0);
        W.end(L);
        continue;
      }
      W.uleb(C.Ret + "-4-" + F.Name);
      W.u8(4);
      W.u8(1); // CALL
      W.uleb(W.ref(C.Target));
      W.sleb("-4");
      W.u8(C.Safepoint ? 1 : 0);
      W.end(L);
    }
  }

  W.section(".goc.stackmaps");
  for (const FuncInfo &F : Funcs)
    for (const SMRec &R : F.Stackmaps) {
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      W.uleb(R.Label + "-" + F.Name);
      W.uleb(Twine(R.ID));
      W.uleb(Twine(R.Locs.size()));
      for (const SMLoc &Lc : R.Locs) {
        W.u8(Lc.Type);
        W.uleb(Twine(Lc.Size));
        W.uleb(Twine(Lc.Reg));
        W.sleb(Twine(Lc.Off));
      }
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

  unsigned Prof = W.str(Profile);
  unsigned Prod = W.str("goc-llc (LLVM " LLVM_VERSION_STRING ")");
  W.section(".goc.header");
  OS << "\t.byte 0x47,0x4f,0x43,0x4f,0x42,0x4a,0,0\n"
     << "\t.short 1\n\t.short 4\n"
     << "\t.byte " << (A64 ? 2 : 1) << "\n\t.byte " << (goc::stackInfo().empty() ? 0 : 1) << "\n\t.short 0\n"
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

MachineFunctionPass *goc::createGocObjTablesPass(StringRef Path, StringRef Profile, unsigned SymABI) {
  return new GocObjTables(Path, Profile, SymABI);
}
