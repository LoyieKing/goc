//===- SlotMD.h - goc stack-slot metadata -----------------------*- C++ -*-===//
//
// The goc passes and goc-llc identify the stack slots they care about by
// metadata, never by IR value name: a Release (no-assertions) clang discards
// value names, and the IR must mean the same either way.
//
//   !goc.root !{!"spill" | !"anchor" | !"arganchor"}
//       on an alloca a goc pass created, naming its role.
//   !goc.slot !{i64 N}
//       a function-unique id of an alloca that a !goc.frame.words tag refers
//       to. The next free id is kept on the function as !goc.nextslot.
//   !goc.frame.words !{!"goc.frame.words", i64 N, i64 off...}
//       on a writer (or on the alloca itself): the 8-byte words at those
//       offsets of slot N hold pointers.
//
//===----------------------------------------------------------------------===//
#ifndef GOC_SLOTMD_H
#define GOC_SLOTMD_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

namespace goc {

inline void markRoot(llvm::AllocaInst *A, llvm::StringRef Kind) {
  llvm::LLVMContext &C = A->getContext();
  A->setMetadata("goc.root", llvm::MDNode::get(C, {llvm::MDString::get(C, Kind)}));
}

// The role a goc pass gave this alloca, or "" for a C variable.
inline llvm::StringRef rootKind(const llvm::Value *V) {
  auto *A = llvm::dyn_cast_or_null<llvm::AllocaInst>(V);
  if (!A)
    return "";
  const llvm::MDNode *N = A->getMetadata("goc.root");
  if (!N || N->getNumOperands() != 1)
    return "";
  auto *S = llvm::dyn_cast<llvm::MDString>(N->getOperand(0));
  return S ? S->getString() : "";
}

// A stack-map root slot of goc-stackmap/goc-reanchor: one pointer, adjusted
// by the stack copier, whose loads give a pointer the code may hold in a
// register (goc.spill.root, goc.anchor, goc.arganchor).
inline bool isCopiedRoot(const llvm::Value *V) {
  llvm::StringRef K = rootKind(V);
  return K == "spill" || K == "anchor" || K == "arganchor";
}

inline bool slotIdOf(const llvm::AllocaInst *A, uint64_t &Id) {
  const llvm::MDNode *N = A->getMetadata("goc.slot");
  if (!N || N->getNumOperands() != 1)
    return false;
  auto *C = llvm::mdconst::dyn_extract<llvm::ConstantInt>(N->getOperand(0));
  if (!C)
    return false;
  Id = C->getZExtValue();
  return true;
}

// The id of A, assigning the next free one if it has none.
inline uint64_t slotId(llvm::AllocaInst *A) {
  uint64_t Id;
  if (slotIdOf(A, Id))
    return Id;
  llvm::Function *F = A->getFunction();
  llvm::LLVMContext &C = A->getContext();
  llvm::Type *I64 = llvm::Type::getInt64Ty(C);
  Id = 0;
  if (const llvm::MDNode *N = F->getMetadata("goc.nextslot"))
    if (auto *V = llvm::mdconst::dyn_extract<llvm::ConstantInt>(N->getOperand(0)))
      Id = V->getZExtValue();
  auto Num = [&](uint64_t V) {
    return llvm::MDNode::get(C, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(I64, V))});
  };
  A->setMetadata("goc.slot", Num(Id));
  F->setMetadata("goc.nextslot", Num(Id + 1));
  return Id;
}

inline llvm::MDNode *frameWordsTag(llvm::AllocaInst *Slot, llvm::ArrayRef<uint64_t> Offs) {
  llvm::LLVMContext &C = Slot->getContext();
  llvm::Type *I64 = llvm::Type::getInt64Ty(C);
  llvm::SmallVector<llvm::Metadata *, 8> Ops{
      llvm::MDString::get(C, "goc.frame.words"),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(I64, slotId(Slot)))};
  for (uint64_t O : Offs)
    Ops.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(I64, O)));
  return llvm::MDNode::get(C, Ops);
}

// Slot ids of F's allocas.
inline llvm::DenseMap<uint64_t, llvm::AllocaInst *> slotMap(const llvm::Function &F) {
  llvm::DenseMap<uint64_t, llvm::AllocaInst *> M;
  for (const llvm::Instruction &I : llvm::instructions(F))
    if (auto *A = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
      uint64_t Id;
      if (slotIdOf(A, Id))
        M[Id] = const_cast<llvm::AllocaInst *>(A);
    }
  return M;
}

// The slot a !goc.frame.words tag refers to, or null.
inline llvm::AllocaInst *frameWordsSlot(const llvm::DenseMap<uint64_t, llvm::AllocaInst *> &M,
                                        const llvm::MDNode *W) {
  if (!W || W->getNumOperands() < 2)
    return nullptr;
  auto *C = llvm::mdconst::dyn_extract<llvm::ConstantInt>(W->getOperand(1));
  return C ? M.lookup(C->getZExtValue()) : nullptr;
}

} // namespace goc

#endif
