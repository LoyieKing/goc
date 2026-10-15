//===- CECheck.cpp - colors: escape checks, metadata, formals, allocas -===//

#include "ColorEscapeInternal.h"

using namespace llvm;

namespace goc::ce {

void checkEscapesAndMix(Function &F, ColorEscapeState &S) {
  for (Instruction &I : instructions(F)) {
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      Value *Val = SI->getValueOperand();
      Value *Dest = SI->getPointerOperand();
      if (!Val->getType()->isPointerTy())
        continue;

      Color Eff = S.effective(Val);
      auto &VI = S.info(Val);

      // Resolve dest field annotation.
      Color DestField = Color::None;
      Value *AnnotDest = Dest;
      if (auto *II = dyn_cast<IntrinsicInst>(Dest)) {
        if (II->getIntrinsicID() == Intrinsic::ptr_annotation) {
          DestField =
              parseColorToken(annotationString(II->getArgOperand(1)));
          AnnotDest = II;
        }
      }
      Value *DestBase = Dest->stripPointerCasts();
      if (auto *II = dyn_cast<IntrinsicInst>(Dest))
        if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
          DestBase = II->getArgOperand(0)->stripPointerCasts();
      auto SlotIt = S.SlotColor.find(DestBase);
      if (SlotIt != S.SlotColor.end())
        DestField = SlotIt->second;

      bool DestStack = isStackDest(Dest, S);
      bool EscDest = isEscapingDest(Dest, S);

      // Refine Auto at store edge.
      if (Eff == Color::Auto || VI.Declared == Color::Auto ||
          VI.Declared == Color::None) {
        if (VI.StackProv && !VI.EncodedUPtr && !VI.GoHeapProv) {
          if (EscDest && !DestStack) {
            // Must have been uptr from the start — raw stack store to heap.
            Eff = Color::SPtr; // treat as nailed sptr for error path
            VI.Refined = Color::SPtr;
          } else if (DestStack) {
            VI.Refined = Color::SPtr;
            Eff = Color::SPtr;
          }
        } else if (VI.GoHeapProv) {
          VI.Refined = Color::GPtr;
          Eff = Color::GPtr;
        } else if (VI.HeapProv && !VI.StackProv) {
          VI.Refined = Color::CPtr;
          Eff = Color::CPtr;
        } else if (VI.EncodedUPtr) {
          VI.Refined = Color::UPtr;
          Eff = Color::UPtr;
        }
      }

      // gptr must not mix into cptr/sptr/uptr/auto slots.
      if (Eff == Color::GPtr || VI.GoHeapProv) {
        if (DestField == Color::CPtr || DestField == Color::SPtr ||
            DestField == Color::UPtr || DestField == Color::Auto) {
          S.error(&I,
                  "gptr cannot be stored into a " + Twine(colorName(DestField)) +
                      " field/slot (no implicit convert between Go-heap and "
                      "C/stack pointer colors)");
        }
        // Also: storing gptr into unannotated non-gptr global/cptr-ish
        if (DestField == Color::None && EscDest && !DestStack)
          S.error(&I, "gptr cannot be stored into an uncolored heap/global "
                      "slot; declare a gptr destination (no implicit "
                      "convert to cptr/auto)");
      }
      if ((Eff == Color::CPtr || Eff == Color::SPtr || Eff == Color::UPtr ||
           Eff == Color::Auto) &&
          DestField == Color::GPtr) {
        S.error(&I, Twine(colorName(Eff)) +
                        " cannot be stored into a gptr field/slot (no "
                        "implicit convert)");
      }

      // Dest expects uptr → value must be uptr / encoded.
      if (DestField == Color::UPtr && Eff != Color::UPtr && !VI.EncodedUPtr) {
        S.error(&I, "uptr field requires an encoded uptr value (use "
                    "goc_uptr_from_sptr / goc_uptr_from_cptr); refusing silent "
                    "sptr→uptr promote");
      }

      // Core escape rule: sptr / raw stack absolute must not enter heap.
      bool IsSPtrLike =
          (Eff == Color::SPtr) ||
          (VI.Declared == Color::SPtr) ||
          (VI.StackProv && !VI.EncodedUPtr && Eff != Color::UPtr &&
           Eff != Color::CPtr && Eff != Color::GPtr);

      if (IsSPtrLike && EscDest && !DestStack) {
        S.error(&I,
                "sptr escape: stack pointer color must not be stored into a "
                "heap/global field; encode with goc_uptr_from_sptr or declare "
                "the value as auto_ptr/uptr before escape (no automatic "
                "promote to uptr)");
        VI.Escapes = true;
      }

      // Explicit sptr annotation nailed then escape — always error (even if
      // somehow encoded missing).
      if (VI.Declared == Color::SPtr && EscDest && !DestStack &&
          !VI.EncodedUPtr) {
        // already errored above; ensure refined stays sptr
        VI.Refined = Color::SPtr;
      }

      (void)AnnotDest;
      continue;
    }

    if (auto *RI = dyn_cast<ReturnInst>(&I)) {
      Value *V = RI->getReturnValue();
      if (!V || !V->getType()->isPointerTy())
        continue;
      Color Eff = S.effective(V);
      auto &VI = S.info(V);
      bool IsSPtrLike =
          (Eff == Color::SPtr) || (VI.Declared == Color::SPtr) ||
          (VI.StackProv && !VI.EncodedUPtr && Eff != Color::UPtr);
      DenseSet<Value *> Seen;
      if (IsSPtrLike && !isUptrRuntimeReturn(F.getName()) &&
          !borrowedFromStackFormal(V, S, Seen)) {
        S.error(RI,
                "sptr escape: returning a stack pointer is forbidden; use "
                "uptr encoding or keep the pointer on-stack (no automatic "
                "promote to uptr)");
      }
      continue;
    }

    // Call args: if callee is known to sink to heap globals — conservative
    // check for gptr/cptr mix on annotated params is limited.
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee || isAnnotationIntrinsic(Callee))
        continue;
      StringRef N = Callee->getName();
      // Passing gptr to goc_uptr_from_sptr etc. is wrong.
      if (isUptrFromSptr(N) || isUptrFromCptr(N) || isUptrFromPtr(N)) {
        Value *Arg = CB->getArgOperand(0);
        auto &AI = S.info(Arg);
        if (AI.GoHeapProv || S.effective(Arg) == Color::GPtr) {
          S.error(&I, "gptr cannot be passed to " + Twine(N) +
                          " (gptr is a separate track)");
        }
      }
      if (isGptrFrom(N) == false && N.starts_with("goc_") == false) {
        // Detect direct call that stores-like: skip.
      }
    }
  }
}

void attachMetadata(Module &M, ColorEscapeState &S) {
  LLVMContext &Ctx = M.getContext();
  auto getMD = [&](Color C) -> MDNode * {
    return MDNode::get(Ctx, {MDString::get(Ctx, colorName(C))});
  };

  // Module-level schema note.
  M.addModuleFlag(Module::Warning, "goc.color.schema",
                  MDString::get(Ctx, "annotate+!goc.color;v0.2.2"));

  for (auto &KV : S.Info) {
    Value *V = KV.first;
    Color C = S.effective(V);
    if (C == Color::None || C == Color::Auto) {
      if (KV.second.StackProv && !KV.second.EncodedUPtr)
        C = Color::SPtr;
      else if (KV.second.EncodedUPtr)
        C = Color::UPtr;
      else if (KV.second.GoHeapProv)
        C = Color::GPtr;
      else if (KV.second.HeapProv)
        C = Color::CPtr;
      else if (C == Color::Auto) {
        // leave auto
      } else
        continue;
    }
    if (auto *I = dyn_cast<Instruction>(V)) {
      // Only pointer-typed results (or void calls that produce encoded ptrs).
      if (!I->getType()->isPointerTy() && !isa<CallBase>(I))
        continue;
      if (!I->getType()->isPointerTy() && isa<CallBase>(I)) {
        // Keep metadata on annotation intrinsics / builders only if ptr-typed.
        continue;
      }
      I->setMetadata("goc.color", getMD(C));
      if (KV.second.StackProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "stack")}));
      else if (KV.second.GoHeapProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "goheap")}));
      else if (KV.second.HeapProv)
        I->setMetadata("goc.prov",
                       MDNode::get(Ctx, {MDString::get(Ctx, "cheap")}));
      if (KV.second.EncodedUPtr)
        I->setMetadata("goc.uptr_encoded",
                       MDNode::get(Ctx, {MDString::get(Ctx, "1")}));
    }
  }

  // Also tag annotated allocas/slots.
  for (auto &KV : S.SlotColor) {
    if (auto *I = dyn_cast<Instruction>(KV.first))
      I->setMetadata("goc.color", getMD(KV.second));
    else if (auto *G = dyn_cast<GlobalVariable>(KV.first)) {
      // Globals cannot take inst metadata; use named md attachment via
      // !goc.global.colors later if needed. Skip.
      (void)G;
    }
  }
}

bool provenStackActual(Value *Actual,
                              const DenseSet<const Argument *> &Candidates) {
  SmallVector<Value *, 8> Work{Actual};
  DenseSet<Value *> Seen;
  bool HasOrigin = false;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val()->stripPointerCasts();
    if (!Seen.insert(V).second)
      continue;
    if (isa<ConstantPointerNull>(V) || isa<AllocaInst>(V)) {
      HasOrigin = true;
      continue;
    }
    if (auto *A = dyn_cast<Argument>(V)) {
      if (!Candidates.count(A))
        return false;
      HasOrigin = true;
      continue;
    }
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      Work.push_back(GEP->getPointerOperand());
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
      if (!Slot)
        return false;
      bool HasStore = false;
      for (User *U : Slot->users())
        if (auto *S = dyn_cast<StoreInst>(U))
          if (S->getPointerOperand() == Slot) {
            HasStore = true;
            Work.push_back(S->getValueOperand());
          }
      if (!HasStore)
        return false;
      continue;
    }
    if (auto *Sel = dyn_cast<SelectInst>(V)) {
      Work.push_back(Sel->getTrueValue());
      Work.push_back(Sel->getFalseValue());
      continue;
    }
    if (auto *Phi = dyn_cast<PHINode>(V)) {
      for (Value *In : Phi->incoming_values())
        Work.push_back(In);
      continue;
    }
    return false;
  }
  return HasOrigin;
}

// A file-local function with no address-taken uses and only stack-alloca
// actual arguments has a stack-only formal. Unlike a generic T* parameter,
// dereferencing it is a stack destination (e.g. dtoa_malloc(&mptr),
// js_inner_module_linking(&stack_top)). Recursive forwarding preserves this
// proof; unknown or external callsites keep the regular cptr default.
void proveStackOnlyFormals(Module &M, ColorEscapeState &S) {
  DenseMap<Function *, SmallVector<CallBase *, 8>> Callers;
  SmallVector<Argument *, 32> Candidates;
  for (Function &F : M) {
    if (F.isDeclaration() || !F.hasLocalLinkage())
      continue;
    SmallVector<CallBase *, 8> Calls;
    bool AddressTaken = false;
    for (User *U : F.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &F) {
        AddressTaken = true;
        break;
      }
      Calls.push_back(CB);
    }
    if (AddressTaken || Calls.empty())
      continue;
    Callers[&F] = std::move(Calls);
    for (Argument &A : F.args()) {
      if (!A.getType()->isPointerTy() ||
          (S.info(&A).Declared != Color::None &&
           S.info(&A).Declared != Color::Auto))
        continue;
      Candidates.push_back(&A);
      S.StackOnlyFormals.insert(&A);
    }
  }
  bool Changed;
  do {
    Changed = false;
    for (Argument *A : Candidates) {
      if (!S.StackOnlyFormals.count(A))
        continue;
      for (CallBase *CB : Callers[A->getParent()]) {
        if (A->getArgNo() >= CB->arg_size() ||
            !provenStackActual(CB->getArgOperand(A->getArgNo()),
                               S.StackOnlyFormals)) {
          S.StackOnlyFormals.erase(A);
          Changed = true;
          break;
        }
      }
    }
  } while (Changed);
  for (Argument *A : Candidates)
    if (S.StackOnlyFormals.count(A)) {
      auto &PI = S.info(A);
      PI.Refined = Color::SPtr;
      PI.StackProv = true;
      PI.HeapProv = false;
    }
}

// Bulk coloring for large unannotated codebases (e.g. quickjs-ng): treat
// ordinary unannotated pointer slots/formals as the given color. Proven
// stack-only formals and their spill slots retain their source provenance.
// Explicit annotations (goc_sptr/uptr/gptr/...) still win.
void applyDefaultPtrColor(Module &M, ColorEscapeState &S) {
  Color C = parseColorToken(DefaultPtrColor);
  if (C == Color::None || C == Color::Auto)
    return;
  auto apply = [&](Value *V, PtrInfo &PI) {
    if (PI.Declared != Color::None && PI.Declared != Color::Auto)
      return;
    PI.Declared = C;
    if (PI.Refined == Color::None || PI.Refined == Color::Auto)
      PI.Refined = C;
    switch (C) {
    case Color::CPtr:
      PI.HeapProv = true;
      break;
    case Color::SPtr:
      PI.StackProv = true;
      break;
    case Color::GPtr:
      PI.GoHeapProv = true;
      break;
    case Color::UPtr:
      PI.EncodedUPtr = true;
      break;
    default:
      break;
    }
    S.SlotColor[V] = C;
    S.DefaultColoredSlots.insert(V);
  };
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    DenseSet<AllocaInst *> stackFormalSlots;
    for (Instruction &I : F.getEntryBlock())
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (auto *A = dyn_cast<Argument>(SI->getValueOperand()))
          if (S.StackOnlyFormals.count(A))
            if (auto *AI = dyn_cast<AllocaInst>(SI->getPointerOperand()->stripPointerCasts()))
              stackFormalSlots.insert(AI);
    for (Instruction &I : instructions(F))
      if (auto *AI = dyn_cast<AllocaInst>(&I))
        if (AI->getAllocatedType()->isPointerTy() && !stackFormalSlots.count(AI))
          apply(AI, S.info(AI));
    for (Argument &A : F.args())
      if (A.getType()->isPointerTy() && !S.StackOnlyFormals.count(&A))
        apply(&A, S.info(&A));
  }
  errs() << "goc-color-escape: default-ptr-color=" << DefaultPtrColor << "\n";
}

// Go's pcsp table cannot describe an arbitrary change to SP in the middle of
// a C frame. Lower C alloca(size) to a per-invocation scoped allocation before
// coloring, so its result is a cptr and nested calls can grow the g stack.
// As with C alloca, repeated executions stay allocated until function return.
unsigned lowerDynamicAllocas(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *SizeTy = Type::getInt64Ty(Ctx);
  unsigned Count = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    SmallVector<AllocaInst *, 8> Dynamic;
    SmallVector<ReturnInst *, 8> Returns;
    bool StackRestore = false;
    bool NonlocalExit = false;
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!AI->isStaticAlloca())
          Dynamic.push_back(AI);
      } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        Returns.push_back(RI);
      } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        StackRestore |= II->getIntrinsicID() == Intrinsic::stacksave ||
                        II->getIntrinsicID() == Intrinsic::stackrestore;
      } else if (isa<ResumeInst>(I) || isa<CleanupReturnInst>(I) ||
                 isa<CatchReturnInst>(I) || isa<InvokeInst>(I)) {
        NonlocalExit = true;
      }
    }
    if (Dynamic.empty())
      continue;
    if (StackRestore || NonlocalExit)
      report_fatal_error(Twine("goc: unsupported dynamic alloca lifetime in ") + F.getName());
    for (AllocaInst *AI : Dynamic)
      if (!AI->getAllocatedType()->isIntegerTy(8) ||
          AI->getAlign().value() > 16 ||
          !AI->getArraySize()->getType()->isIntegerTy() ||
          AI->getArraySize()->getType()->getIntegerBitWidth() > 64)
        report_fatal_error(Twine("goc: unsupported dynamic alloca element/alignment in ") + F.getName());
    FunctionCallee Allocate = M.getOrInsertFunction(
        "goc_dynalloc", FunctionType::get(Ptr, {SizeTy, Ptr}, false));
    FunctionCallee Release = M.getOrInsertFunction(
        "goc_dynrelease", FunctionType::get(Type::getVoidTy(Ctx), {Ptr}, false));
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    auto *Head = Entry.CreateAlloca(Ptr, nullptr, "goc.dynalloc.head");
    Entry.CreateStore(ConstantPointerNull::get(cast<PointerType>(Ptr)), Head);
    for (AllocaInst *AI : Dynamic) {
      IRBuilder<> B(AI);
      Value *Bytes = B.CreateZExtOrTrunc(AI->getArraySize(), SizeTy);
      auto *Storage = B.CreateCall(Allocate, {Bytes, Head}, "goc.dynalloc");
      AI->replaceAllUsesWith(Storage);
      AI->eraseFromParent();
      ++Count;
    }
    for (ReturnInst *RI : Returns) {
      IRBuilder<> B(RI);
      B.CreateCall(Release, {Head});
    }
  }
  return Count;
}

} // namespace goc::ce
