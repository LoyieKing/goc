// goc_x86.h — Prefer real llvm::X86InstrInfo when local LLVM 19.1.7 Target
// headers + tablegen .inc are available (distro llvm-19-dev does NOT ship them).
// Fallback: vendor/X86InstrInfoLite.h (name-lookup opcodes).
#pragma once

#if defined(GOC_HAVE_X86INSTRINFO) && GOC_HAVE_X86INSTRINFO
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

#else

#include "vendor/X86InstrInfoLite.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

namespace goc {

// Lite path: TargetInstrInfo* only; opcodes live in InstrInfoLite after resolve.
inline const llvm::TargetInstrInfo *x86TII(const llvm::MachineFunction &MF) {
  return MF.getSubtarget().getInstrInfo();
}

} // namespace goc

#endif
