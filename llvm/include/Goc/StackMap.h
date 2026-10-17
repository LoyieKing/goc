//===- Goc/StackMap.h - stack-map and inline-gate passes -------*- C++ -*-===//
//
// goc-inline-gate, goc-stackmap, and goc-reanchor: IR function
// passes that prepare call sites so LLVM records where live sptr/gptr values
// sit, for the Go stack maps.
//
//===----------------------------------------------------------------------===//
#ifndef GOC_STACKMAP_H
#define GOC_STACKMAP_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Passes/PassBuilder.h"

namespace goc {
/// Pipeline-parsing hook for the stack-map function passes.
bool parseStackMapPass(llvm::StringRef Name, llvm::FunctionPassManager &FPM,
                       llvm::ArrayRef<llvm::PassBuilder::PipelineElement>);
} // namespace goc

#endif
