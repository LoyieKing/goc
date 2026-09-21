// X86InstrInfoLite — FALLBACK only when local LLVM 19.1.7 Target/X86
// headers + X86GenInstrInfo.inc are missing (distro llvm-19-dev never ships
// them). Prefer real X86InstrInfo.h when GOC_HAVE_X86INSTRINFO=1 (see
// pass/Makefile LLVM_X86_SRC_INCLUDE / LLVM_X86_BUILD_INCLUDE).
#pragma once
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/ErrorHandling.h"

namespace goc {
namespace X86 {

struct InstrInfoLite {
  unsigned MOV64rm = 0, MOV64mr = 0, MOV64ri = 0, CMP64rm = 0, CMP32mi = 0;
  unsigned JCC_1 = 0, JMP_1 = 0, CALL64pcrel32 = 0, ADD64mi8 = 0;
  unsigned RAX = 0, RCX = 0, RDX = 0, RBX = 0, RSP = 0, RBP = 0;
  unsigned RSI = 0, RDI = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;
  unsigned R12 = 0, R13 = 0, R14 = 0, R15 = 0, FS = 0;
  bool Resolved = false;

  static unsigned findOpcode(const llvm::MCInstrInfo &MCII, llvm::StringRef N) {
    for (unsigned I = 0, E = MCII.getNumOpcodes(); I != E; ++I)
      if (MCII.getName(I) == N)
        return I;
    llvm::report_fatal_error(llvm::Twine("X86InstrInfoLite missing opcode ") + N);
  }
  static unsigned findReg(const llvm::MCRegisterInfo &MRI, llvm::StringRef N) {
    for (unsigned R = 1, E = MRI.getNumRegs(); R != E; ++R)
      if (llvm::StringRef(MRI.getName(R)) == N)
        return R;
    llvm::report_fatal_error(llvm::Twine("X86InstrInfoLite missing reg ") + N);
  }
  void resolve(const llvm::MCInstrInfo &MCII, const llvm::MCRegisterInfo &MRI) {
    if (Resolved) return;
    MOV64rm = findOpcode(MCII, "MOV64rm");
    MOV64mr = findOpcode(MCII, "MOV64mr");
    MOV64ri = findOpcode(MCII, "MOV64ri");
    CMP64rm = findOpcode(MCII, "CMP64rm");
    CMP32mi = findOpcode(MCII, "CMP32mi");
    JCC_1 = findOpcode(MCII, "JCC_1");
    JMP_1 = findOpcode(MCII, "JMP_1");
    CALL64pcrel32 = findOpcode(MCII, "CALL64pcrel32");
    ADD64mi8 = findOpcode(MCII, "ADD64mi8");
    RAX = findReg(MRI, "RAX"); RCX = findReg(MRI, "RCX");
    RDX = findReg(MRI, "RDX"); RBX = findReg(MRI, "RBX");
    RSP = findReg(MRI, "RSP"); RBP = findReg(MRI, "RBP");
    RSI = findReg(MRI, "RSI"); RDI = findReg(MRI, "RDI");
    R8 = findReg(MRI, "R8"); R9 = findReg(MRI, "R9");
    R10 = findReg(MRI, "R10"); R11 = findReg(MRI, "R11");
    R12 = findReg(MRI, "R12"); R13 = findReg(MRI, "R13");
    R14 = findReg(MRI, "R14"); R15 = findReg(MRI, "R15");
    FS = findReg(MRI, "FS");
    Resolved = true;
  }
};
constexpr unsigned COND_E = 4;
constexpr unsigned COND_BE = 6;
} // namespace X86
} // namespace goc
