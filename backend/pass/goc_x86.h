// goc_x86.h — requires real llvm::X86InstrInfo from the local LLVM 19.1.7
// source tree (Target/X86 headers + tablegen .inc). The X86InstrInfoLite
// fallback has been removed: builds fail without the real headers.
#pragma once

#include "X86InstrInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

namespace goc {

inline const llvm::X86InstrInfo *x86TII(const llvm::MachineFunction &MF) {
  return static_cast<const llvm::X86InstrInfo *>(
      MF.getSubtarget().getInstrInfo());
}

// Re-export commonly used opcodes/regs/conds under a stable alias when needed.
namespace X86 = llvm::X86;

} // namespace goc
