//===- Goc/Internal/StackMap.h - shared helpers of the stack-map passes -===//

#ifndef GOC_STACKMAP_INTERNAL_H
#define GOC_STACKMAP_INTERNAL_H

#include <algorithm>
#include <optional>
#include <memory>
#include <cstdlib>
#include <cstring>
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "Goc/StackMap.h"
#include "Goc/SlotMD.h"

namespace goc::sm {
using namespace llvm;

// PC 0's bitmap must not include compiler spill slots. A function-wide bit
// forced a volatile null store on every entry so the copier would not see
// garbage. The slot is recorded only at calls the real store dominates.
constexpr uint64_t GocStackMapTag = 0x474f430000000000ULL;

// Liveness of one value across safepoints, by backward propagation from its
// use points (a PHI operand is used at its incoming block's terminator).
// Def is the defining instruction (nullptr: defined on entry, e.g. an
// argument anchor stored in the entry block). A value is live across CB iff
// some use is reachable from CB without passing through Def. Dominance
// (DT.dominates(CB, Use)) is *not* enough: after a call on one arm of an if,
// or inside a loop, the later use is reachable but not dominated, and a root
// omitted there is left stale by a stack copy. Live across CB also implies
// Def dominates CB (a path CB->use avoiding Def plus a path entry->CB
// avoiding Def would reach the use undefined), so every slot listed is
// initialized.
struct CrossCallLiveness {
  const Instruction *Def;
  DenseSet<const BasicBlock *> liveOut;
  DenseMap<const BasicBlock *, SmallVector<const Instruction *, 4>> usesIn;

  CrossCallLiveness(const Instruction *D, ArrayRef<Instruction *> UsePoints)
      : Def(D) {
    const BasicBlock *DefBB = D ? D->getParent() : nullptr;
    SmallVector<const BasicBlock *, 32> work;
    DenseSet<const BasicBlock *> liveIn;
    for (const Instruction *U : UsePoints) {
      const BasicBlock *B = U->getParent();
      usesIn[B].push_back(U);
      // A use after Def in Def's own block is satisfied locally.
      if (B == DefBB && D != U && D->comesBefore(U))
        continue;
      if (liveIn.insert(B).second)
        work.push_back(B);
    }
    while (!work.empty()) {
      const BasicBlock *B = work.pop_back_val();
      for (const BasicBlock *P : predecessors(B)) {
        liveOut.insert(P);
        if (P == DefBB)
          continue; // Def (re)defines the value at the end of this path
        if (liveIn.insert(P).second)
          work.push_back(P);
      }
    }
  }

  bool liveAcross(const Instruction *CB) const {
    const BasicBlock *B = CB->getParent();
    if (Def && Def->getParent() == B && CB->comesBefore(Def))
      return false; // reaches Def before any use in this block
    if (auto It = usesIn.find(B); It != usesIn.end())
      for (const Instruction *U : It->second)
        if (U != CB && CB->comesBefore(U))
          return true;
    return liveOut.count(B) != 0;
  }
};

bool isSptrColored(const Instruction &I);
bool isNonSafepointIntrinsic(const CallBase &CB);
CallBase *stackmapBefore(Instruction *Safepoint);
void ensureStackmapRoot(CallBase *Safepoint, Value *Root, uint64_t &NextId);
AllocaInst *frameAlloca(Value *Address, int64_t &Offset,
                        const DataLayout &DL);
bool outgoingPointerWords(const CallBase &CB, const DataLayout &DL,
                          SmallVectorImpl<int64_t> &Offsets);
bool hoistTlsG(Function &F);
bool isSafepoint(const Instruction &I);
bool ignoreUse(const Instruction *User);
bool separatedBySafepoint(const Instruction *Def, const Instruction *UsePoint);
Instruction *usePoint(Use &U);
std::optional<std::pair<AllocaInst *, int64_t>>
pureFrameAddress(Value *V, const DataLayout &DL);
Value *rematFrameAddress(IRBuilder<> &B, Value *V, const DataLayout &DL);
bool rematerializableFrameAddress(Value *V, const DataLayout &DL);
unsigned rematCrossSafepointUses(Value *V, const DataLayout &DL, Function &F);
bool operandIsKnownStack(const Value *V, const DataLayout &DL,
                         const DenseSet<const Argument *> &Known, unsigned Depth);
const DenseSet<const Argument *> &stackPointerArguments(const Module &M,
                                                        const DataLayout &DL);
DenseSet<Value *> stackAddresses(Function &F, const DataLayout &DL);
bool unmergeReportedAllocas(Function &F);
bool gocArchArm64();
Value *tlsGWord(IRBuilder<> &B, uint64_t Off);
Function *uptrFatal(Module &M);
void emitUptrFatal(IRBuilder<> &B, Module &M, const char *Msg);
Function *alwaysInlineUptr(Module &M, StringRef Name, FunctionType *FT);
Function *inlineStackBound(Module &M, StringRef Name, uint64_t Off);
Function *inlineUptrDecode(Module &M);
Function *inlineUptrFromPtr(Module &M);
Function *inlineUptrRequireCptr(Module &M);
enum class UptrConv { FromCptr, AsCptr, FromSptr, AsSptr };
Function *inlineUptrConv(Module &M, UptrConv K, bool ExplicitHi);
Function *inlineDynAllocPool(Module &M);
Function *inlineDynReleasePool(Module &M);
Function *inlineDynAllocHeap(Module &M);
Function *inlineDynReleaseHeap(Module &M);
bool fastStackAlloca();
Function *uptrHelperFor(Module &M, StringRef Name);
bool rewriteUptrCall(CallInst *CI);

// Pass factories, for parseStackMapPass.
void addGocStackMap(FunctionPassManager &FPM);
void addGocReanchor(FunctionPassManager &FPM);
void addGocInlineGate(FunctionPassManager &FPM);
void addGocNameBlocks(FunctionPassManager &FPM);

} // namespace goc::sm

#endif
