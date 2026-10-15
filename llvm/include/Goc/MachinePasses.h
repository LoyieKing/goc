//===- Goc/MachinePasses.h - goc's machine passes for goc-llc ---*- C++ -*-===//
//
// Factories for the MachineFunction passes goc-llc appends after LLVM's own
// machine pipeline. The stack check lives in Goc/StackCheck.h and the arm64
// frame passes in Goc/AArch64Frame.h.
//
//===----------------------------------------------------------------------===//
#ifndef GOC_MACHINEPASSES_H
#define GOC_MACHINEPASSES_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
class MachineFunctionPass;
class raw_ostream;
} // namespace llvm

namespace goc {
/// lib/CodeGen: deletes STACKMAP records with no CALL after them in their
/// block (an intrinsic goc-stackmap counted as a safepoint was expanded).
llvm::MachineFunctionPass *createGocStackmapPlacementPass();

/// lib/Target/X86: after every call, re-derives or rebases callee-saved
/// registers and spill slots that hold a frame address computed before it.
llvm::MachineFunctionPass *createGocX86FrameAddrFixPass();
/// Prints the module-wide count of frame-derived values the fix could not
/// repair, if any.
void printFrameAddrFixNote(llvm::raw_ostream &OS);

/// lib/Object: last pass before the AsmPrinter. Labels calls and function
/// ends and, at module end, writes the .goc.* sections as assembler text to
/// Path.
llvm::MachineFunctionPass *createGocObjTablesPass(llvm::StringRef Path,
                                                  llvm::StringRef Profile,
                                                  unsigned SymABI);
} // namespace goc

#endif
