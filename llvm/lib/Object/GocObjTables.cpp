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
#include "GocObjTablesImpl.h"

namespace goc::objtab {

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

// The frame slots that hold sptr values (goc-lower meta: sptrSlots): every
// (alloca, offset) named by a !goc.frame.words tag, and every alloca that
// receives a direct store of an sptr-colored pointer.
static std::vector<SptrTarget> sptrTargets(const Function &F) {
  std::vector<SptrTarget> Out;
  auto Add = [&](SptrTarget T) {
    if (std::find(Out.begin(), Out.end(), T) == Out.end())
      Out.push_back(std::move(T));
  };
  auto Slots = goc::slotMap(F);
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      if (const MDNode *W = I.getMetadata("goc.frame.words")) {
        const AllocaInst *A = goc::frameWordsSlot(Slots, W);
        if (!A || goc::isCopiedRoot(A))
          continue;
        for (unsigned K = 2; K < W->getNumOperands(); ++K)
          if (auto *C = mdconst::dyn_extract<ConstantInt>(W->getOperand(K)))
            Add({A, C->getSExtValue()});
        continue;
      }
      auto *St = dyn_cast<StoreInst>(&I);
      if (!St)
        continue;
      auto *V = dyn_cast<Instruction>(St->getValueOperand());
      auto *A = dyn_cast<AllocaInst>(St->getPointerOperand());
      if (!V || !A || !V->getType()->isPointerTy())
        continue;
      if (!isSptrNode(V->getMetadata("goc.color")) || goc::isCopiedRoot(A))
        continue;
      Add({A, 0});
    }
  return Out;
}

// Which SysV integer argument registers (rdi rsi rdx rcx r8 r9) carry a
// pointer on entry. Returns false for a signature this model does not cover.
static bool sysvPointerRegs(const Function &F, uint64_t &Mask) {
  // AAPCS64 has x0-x7; an i128 takes an even-numbered pair.
  bool A64 = Triple(F.getParent()->getTargetTriple()).isAArch64();
  unsigned NRegs = A64 ? 8 : 6;
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
      if (Used < NRegs) {
        if (T->isPointerTy())
          Mask |= 1u << Used;
        ++Used;
      }
    } else if (T->isIntegerTy(128)) {
      if (A64 && Used % 2)
        ++Used;
      if (Used + 2 <= NRegs)
        Used += 2;
    } else if (T->isFloatTy() || T->isDoubleTy()) {
      // SSE registers; do not affect the integer sequence
    } else {
      return false;
    }
  }
  return true;
}

unsigned GocObjTables::fileIndex(const DIFile *F) {
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

int GocObjTables::inlIndex(FuncInfo &FI, DenseMap<const DILocation *, int> &Map, const DILocation *C) {
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

void GocObjTables::recordLines(MachineFunction &MF, FuncInfo &FI) {
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

bool GocObjTables::labelInlineAsmCalls(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI) {
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

bool GocObjTables::doInitialization(Module &M) {
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

uint16_t GocObjTables::dwarfReg(const TargetRegisterInfo *TRI, Register R) {
  int N = TRI->getDwarfRegNum(R, false);
  for (MCPhysReg S : TRI->superregs(R)) {
    if (N >= 0)
      break;
    N = TRI->getDwarfRegNum(S, false);
  }
  return (uint16_t)N;
}

bool GocObjTables::recordStackmap(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI) {
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

bool GocObjTables::runOnMachineFunction(MachineFunction &MF) {
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
    DenseMap<const AllocaInst *, int64_t> Obj;
    for (int I = 0, E = MFI.getObjectIndexEnd(); I < E; ++I)
      if (!MFI.isDeadObjectIndex(I))
        if (const AllocaInst *A = MFI.getObjectAllocation(I))
          Obj.try_emplace(A, MFI.getObjectOffset(I));
    for (const SptrTarget &T : It->second) {
      auto O = Obj.find(T.Alloca);
      if (O == Obj.end())
        continue;
      // Bytes below varp: x86-64 varp is the saved RBP (entry SP - 16
      // with the return address); arm64 varp is entry SP - 8.
      int64_t Off = -O->second - (FI.A64 ? 8 : 16) - T.Off;
      if (Off < 8 || Off % 8 != 0)
        continue;
      if (std::find(FI.SptrSlots.begin(), FI.SptrSlots.end(), (uint64_t)Off) == FI.SptrSlots.end())
        FI.SptrSlots.push_back(Off);
    }
  }
  // arm64: the saved X29 is a raw address in the caller's frame. Mark it
  // as a pointer slot so a stack copy adjusts it (GC ignores stack
  // addresses); epilogues and FP-relative accesses read it back.
  if (FI.A64) {
    const MachineFrameInfo &MFI = MF.getFrameInfo();
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
    for (const CalleeSavedInfo &CS : MFI.getCalleeSavedInfo())
      if (StringRef(TRI.getName(CS.getReg())) == "FP") {
        FI.A64FPSave = MFI.getObjectOffset(CS.getFrameIdx());
        FI.A64HasFP = true;
        int64_t Off = -FI.A64FPSave - 8;
        if (Off >= 8 && Off % 8 == 0 &&
            std::find(FI.SptrSlots.begin(), FI.SptrSlots.end(), (uint64_t)Off) == FI.SptrSlots.end())
          FI.SptrSlots.push_back(Off);
      }
  }
  std::sort(FI.SptrSlots.begin(), FI.SptrSlots.end());
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

char GocObjTables::ID = 0;

} // namespace goc::objtab

llvm::MachineFunctionPass *goc::createGocObjTablesPass(llvm::StringRef Path, llvm::StringRef Profile,
                                                  unsigned SymABI) {
  return new objtab::GocObjTables(Path, Profile, SymABI);
}
