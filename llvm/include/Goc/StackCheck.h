//===- Goc/StackCheck.h - Go stack check and morestack in LLVM --*- C++ -*-===//
//
// Every goc function runs on a goroutine stack, so it must check
// g.stackguard0 on entry and call runtime.morestack_noctxt when the frame it
// is about to allocate does not fit, exactly like Go's own prologue.
//
// The work is split in two:
//
//  - GocStackCheck (lib/CodeGen/GocStackCheck.cpp) is target independent. It
//    decides which functions need a check (leaf / small-frame skip), picks
//    the frame tier (small, big, huge: the AbiProfiles StackSmall/StackBig
//    constants), adds a check block in front of the entry and a cold block at
//    the end, and records what the .goc.* tables need about them: where the
//    body starts, the SP delta at every point of the check and the cold
//    block, the unsafe-point ranges, and which call is the morestack call
//    and what its pointer map is.
//
//  - A StackCheckTarget per architecture only emits instructions into the
//    blocks it is handed: load g, compare, branch to the cold block; in the
//    cold block save what morestack would clobber, call morestack, restore,
//    jump back to the check.
//
// Nothing here is in the IR: an IR-level check would be copied into callers
// by the inliner and see the wrong SP.
//
//===----------------------------------------------------------------------===//
#ifndef GOC_STACKCHECK_H
#define GOC_STACKCHECK_H

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class MachineBasicBlock;
class MachineFunction;
class MachineFunctionPass;
class MachineInstr;
} // namespace llvm

namespace goc {

/// The Go runtime constants a check depends on (Goc/AbiProfiles.def).
struct AbiProfile {
  const char *Name, *Arch;
  unsigned PtrSize;
  const char *GReg;
  int64_t StackGuard0Off, StackSmall, StackBig;
  const char *Morestack;
};
const AbiProfile *lookupProfile(llvm::StringRef Name);

/// How a function is entered.
enum class EntryABI {
  C,          ///< SysV/AAPCS C entry (Go ABI0 symbol): no Go spill area
  GoInternal, ///< Go ABIInternal: arguments in registers, caller spill area
};

enum class FrameTier {
  Small, ///< frame <= StackSmall: compare SP with stackguard0
  Big,   ///< frame <= StackBig: compare SP-(frame-StackSmall)
  Huge,  ///< larger: SP-(frame-StackSmall) may wrap; check the borrow too
};

struct CheckPlan {
  EntryABI ABI;
  FrameTier Tier;
  int64_t Frame;   ///< bytes the body allocates below the return address
  const AbiProfile *Profile;
};

/// Where the SP delta (bytes below the entry SP, return address excluded)
/// changes inside the check or cold block: after instruction MI it is Delta.
struct SPChange {
  llvm::MachineInstr *After;
  int64_t Delta;
};

/// What the cold block leaves on the stack at the morestack call, for its
/// pointer map: word K below varp (the saved frame pointer) holds C argument
/// register ArgReg (0-based, in the C calling convention's order).
struct StubSlot {
  unsigned ArgReg;
  unsigned WordBelowVarp;
};

class StackCheckTarget {
public:
  virtual ~StackCheckTarget();
  /// Emit the check into Check (which falls through to the body) with a
  /// conditional branch to Stub. Record SP changes in SP.
  virtual void emitCheck(llvm::MachineFunction &MF, llvm::MachineBasicBlock &Check,
                         llvm::MachineBasicBlock &Stub, const CheckPlan &P,
                         std::vector<SPChange> &SP) const = 0;
  /// Emit the cold block: undo what the check left pushed, save what
  /// morestack clobbers, call the profile's morestack, restore, and branch
  /// back to Check. Return the morestack call; record SP changes in SP.
  virtual llvm::MachineInstr *emitStub(llvm::MachineFunction &MF, llvm::MachineBasicBlock &Stub,
                                       llvm::MachineBasicBlock &Check, const CheckPlan &P,
                                       std::vector<SPChange> &SP) const = 0;
  /// The saved argument registers visible to the GC at the morestack call
  /// (empty when nothing pointer-bearing is saved below varp).
  virtual std::vector<StubSlot> stubSlots(const CheckPlan &P) const = 0;
  /// A trap instruction, used when the last body block could fall through.
  virtual void emitTrap(llvm::MachineFunction &MF, llvm::MachineBasicBlock &MBB) const = 0;
};

std::unique_ptr<StackCheckTarget> createX86StackCheckTarget();
std::unique_ptr<StackCheckTarget> createAArch64StackCheckTarget();

/// What GocStackCheck decided for one function, read by the tables writer.
struct FuncStackInfo {
  bool HasCheck = false;
  bool NoSplit = false;          ///< no check: the linker's nosplit budget applies
  EntryABI ABI = EntryABI::C;
  std::string BodyStart;         ///< label at the first body instruction
  std::string StubStart;         ///< label at the cold block
  std::vector<std::pair<std::string, int64_t>> SPChanges; ///< label after which the delta holds
  llvm::MachineInstr *MorestackCall = nullptr;
  std::vector<StubSlot> StubSlots;
};

/// Per-module results, keyed by function symbol name.
llvm::StringMap<FuncStackInfo> &stackInfo();

struct StackCheckOptions {
  const AbiProfile *Profile;
  EntryABI CABI;   ///< entry ABI of the C functions (C when Go ABI thunks wrap them)
};

/// Runs after prologue/epilogue insertion, before the tables writer.
llvm::MachineFunctionPass *createGocStackCheckPass(const StackCheckOptions &O,
                                                   std::unique_ptr<StackCheckTarget> T);

} // namespace goc

#endif
