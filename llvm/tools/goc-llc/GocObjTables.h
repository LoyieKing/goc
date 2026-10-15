//===- GocObjTables.h - .goc.* table writer for goc-llc --------*- C++ -*-===//
#ifndef GOC_LLC_GOCOBJTABLES_H
#define GOC_LLC_GOCOBJTABLES_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
class MachineFunctionPass;
}

namespace goc {
/// Last pass before the AsmPrinter: labels calls and function ends and, at
/// module end, writes the .goc.* sections as assembler text to Path.
llvm::MachineFunctionPass *createGocObjTablesPass(llvm::StringRef Path);
} // namespace goc

#endif
