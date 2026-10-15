//===- GocObjTablesImpl.h - internal types of the .goc.* writer -*- C++ -*-===//
//
// Shared by GocObjTables.cpp (per-function collection) and
// GocObjTablesWriter.cpp (module-end assembler output).
//===----------------------------------------------------------------------===//
#ifndef GOC_OBJTABLESIMPL_H
#define GOC_OBJTABLESIMPL_H

#include "Goc/MachinePasses.h"

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
#include "Goc/SlotMD.h"
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

namespace goc::objtab {
using namespace llvm;


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
  bool A64 = false; // AArch64: BL relocations at the instruction
  int64_t A64FPSave = 0; // AArch64: entry-SP offset of the saved X29 (X29 points there)
  bool A64HasFP = false;
  std::string Name, End;
  uint64_t FrameSize;
  uint64_t Flags = 0;
  uint64_t SysvPtrRegs = 0; // bit i: SysV integer argument register i holds a pointer
  std::vector<CallSite> Calls;
  std::vector<FuncAddr> Addrs;
  std::vector<uint64_t> SptrSlots; // 1.2: BP-relative offsets, sorted
  std::vector<SMRec> Stackmaps;    // 1.3
};

// An alloca plus a byte offset inside it that holds an sptr.
struct SptrTarget {
  const AllocaInst *Alloca;
  int64_t Off;
  bool operator==(const SptrTarget &O) const { return Alloca == O.Alloca && Off == O.Off; }
};

// .goc.funcs flag bits (docs/gocobj-format.md).
enum : uint64_t {
  FlagInternal = 1u << 5,
  FlagHasSptr = 1u << 6,
  FlagSysvUnmodelled = 1u << 7,
};

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

  unsigned fileIndex(const DIFile *F);

  static std::string inlName(const DISubprogram *SP) {
    if (!SP)
      return "?";
    if (!SP->getLinkageName().empty())
      return SP->getLinkageName().str();
    return SP->getName().str();
  }

  // Index in FI.Inl of the call that inlined the code at C, or -1.
  int inlIndex(FuncInfo &FI, DenseMap<const DILocation *, int> &Map, const DILocation *C);

  // 1.4: FILE, LINE and INL_INDEX rows from the instructions' debug
  // locations (clang -gline-tables-only). Instructions without a location
  // (the stack check, spills) keep the previous row. As Go's compiler does
  // with its inline marks, the first non-call instruction of each inlined
  // body is attributed to the call site in the parent: the runtime finds
  // the call site's line through ParentPC.
  void recordLines(MachineFunction &MF, FuncInfo &FI);

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
  // call at or after it in code order (its "site").
  std::vector<SMLoc> Pending;

  bool labelInlineAsmCalls(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI);

  bool doInitialization(Module &M) override;

  GocObjTables(StringRef P, StringRef Prof, unsigned A)
      : MachineFunctionPass(ID), Path(P.str()), Profile(Prof.str()), SymABI(A) {}
  StringRef getPassName() const override { return "goc .gocobj tables"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  // Mirrors StackMaps::parseOperand on the post-PEI operands.
  static uint16_t dwarfReg(const TargetRegisterInfo *TRI, Register R);

  bool recordStackmap(MachineFunction &MF, MachineInstr &MI, FuncInfo &FI);

  static bool isRealCall(const MachineInstr &MI) {
    return MI.isCall() && MI.getOpcode() != TargetOpcode::STACKMAP &&
           MI.getOpcode() != TargetOpcode::PATCHPOINT;
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  bool doFinalization(Module &M) override;
};

} // namespace goc::objtab

#endif
