// P16: Lower analysis MachineFunctions to Go-frame physreg, then printMIR.
#pragma once

#include <string>

namespace llvm {
class ModulePass;
}

/// Final PM pass: in-place Go-frame physreg lower of analysis MFs, then
/// write OutDir/harness.mir + harness.meta.json via llvm::printMIR.
/// Requires MachineModuleInfoWrapperPass. No parallel Module/seed export.
llvm::ModulePass *createGocLowerGoFrameEmitPass(std::string OutDir, bool WithWB);
