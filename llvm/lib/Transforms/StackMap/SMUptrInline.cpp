//===- SMUptrInline.cpp - inline uptr helpers, block naming ----------===//

#include "Goc/Internal/StackMap.h"

using namespace llvm;

namespace goc::sm {

// Runs before O3. Call-bearing functions may inline. Maps are recorded on the
// merged frame afterwards. A stack-pointer argument kept in a callee-saved
// register, and a local that stores &s->token, are reanchored / tagged so
// copystack adjusts them. Source noinline is left untouched.
//
// goc_stack_hi / goc_uptr_decode / goc_uptr_from_ptr are out of line in
// another TU, so every JS entry pays a PLT call to read g and add.
// Give each an alwaysinline body here. goc-inline-gate runs before O3, and
// that pipeline inlines the body. g is invariant for the goroutine. The
// FS:-8 / x28 read is readnone, and hoistTlsG (in goc-stackmap, after O3)
// leaves one read per function. stack.lo and stack.hi change only inside a
// call. The morestack prologue that writes them is inserted after this IR,
// so a call O3 proved readonly can still move the stack. Those loads stay
// volatile.
bool gocArchArm64() {
  static int Arm = -1;
  if (Arm < 0) {
    const char *Arch = std::getenv("GOC_ARCH");
    Arm = (Arch && std::strcmp(Arch, "arm64") == 0) ? 1 : 0;
  }
  return Arm == 1;
}

Value *tlsGWord(IRBuilder<> &B, uint64_t Off) {
  FunctionType *FT = FunctionType::get(B.getInt64Ty(), false);
  // arm64 keeps g in x28 (llc -mattr=+reserve-x28). amd64 reads FS:-8.
  // Do not reserve r14 here: the installed llc has no -reserve-goc-r14.
  const char *Asm = gocArchArm64() ? "mov $0, x28" : "movq %fs:-8, $0";
  InlineAsm *IA = InlineAsm::get(FT, Asm, "=r", /*hasSideEffects=*/false);
  CallInst *C = B.CreateCall(IA);
  // g does not change for the life of the goroutine, and FS/x28 is not
  // LLVM memory. readnone + willreturn is what lets O3 treat the read as a
  // pure value (nounwind alone still counts as a side effect).
  C->setDoesNotAccessMemory();
  C->setDoesNotThrow();
  C->addFnAttr(Attribute::WillReturn);
  Value *G = B.CreateIntToPtr(C, B.getPtrTy());
  LoadInst *L = B.CreateLoad(
      B.getInt64Ty(), B.CreateGEP(B.getInt8Ty(), G, B.getInt64(Off)));
  L->setVolatile(true);
  return L;
}

Function *uptrFatal(Module &M) {
  if (Function *F = M.getFunction("goc_uptr_fatal"))
    return F;
  auto *FT = FunctionType::get(Type::getVoidTy(M.getContext()),
                               {PointerType::getUnqual(M.getContext())}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "goc_uptr_fatal", M);
  F->setDoesNotReturn();
  F->setDoesNotThrow();
  return F;
}

void emitUptrFatal(IRBuilder<> &B, Module &M, const char *Msg) {
  B.CreateCall(uptrFatal(M), {B.CreateGlobalString(Msg)});
  B.CreateUnreachable();
}

Function *alwaysInlineUptr(Module &M, StringRef Name, FunctionType *FT) {
  if (Function *F = M.getFunction(Name))
    return F;
  auto *F = Function::Create(FT, GlobalValue::InternalLinkage, Name, M);
  F->addFnAttr(Attribute::AlwaysInline);
  F->setDoesNotThrow();
  return F;
}

Function *inlineStackBound(Module &M, StringRef Name, uint64_t Off) {
  auto *FT = FunctionType::get(Type::getInt64Ty(M.getContext()), false);
  Function *F = alwaysInlineUptr(M, Name, FT);
  if (!F->empty())
    return F;
  IRBuilder<> B(BasicBlock::Create(M.getContext(), "entry", F));
  B.CreateRet(tlsGWord(B, Off));
  return F;
}

Function *inlineUptrDecode(Module &M) {
  LLVMContext &C = M.getContext();
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Ptr, {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.uptr.decode", FT);
  if (!F->empty())
    return F;
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Stack = BasicBlock::Create(C, "stack", F);
  BasicBlock *Join = BasicBlock::Create(C, "join", F);
  IRBuilder<> B(Entry);
  Value *Arg = F->getArg(0);
  Value *W = B.CreatePtrToInt(Arg, B.getInt64Ty());
  Value *MSB = B.CreateAnd(W, ConstantInt::get(B.getInt64Ty(), 1ULL << 63));
  B.CreateCondBr(B.CreateICmpNE(MSB, B.getInt64(0)), Stack, Join);
  B.SetInsertPoint(Stack);
  Value *Abs = B.CreateIntToPtr(B.CreateAdd(tlsGWord(B, 8), W), Ptr);
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  PHINode *Phi = B.CreatePHI(Ptr, 2);
  Phi->addIncoming(Arg, Entry);
  Phi->addIncoming(Abs, Stack);
  B.CreateRet(Phi);
  return F;
}

Function *inlineUptrFromPtr(Module &M) {
  LLVMContext &C = M.getContext();
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Ptr, {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.uptr.from_ptr", FT);
  if (!F->empty())
    return F;
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Check = BasicBlock::Create(C, "check", F);
  BasicBlock *Encode = BasicBlock::Create(C, "encode", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Pass = BasicBlock::Create(C, "cptr", F);
  IRBuilder<> B(Entry);
  Value *Arg = F->getArg(0);
  Value *W = B.CreatePtrToInt(Arg, B.getInt64Ty());
  B.CreateCondBr(B.CreateICmpEQ(W, B.getInt64(0)), Pass, Check);
  B.SetInsertPoint(Check);
  Value *Lo = tlsGWord(B, 0);
  Value *Hi = tlsGWord(B, 8);
  Value *In = B.CreateAnd(B.CreateICmpNE(Lo, B.getInt64(0)),
                          B.CreateICmpNE(Hi, B.getInt64(0)));
  In = B.CreateAnd(In, B.CreateICmpUGE(W, Lo));
  In = B.CreateAnd(In, B.CreateICmpULT(W, Hi));
  B.CreateCondBr(In, Encode, Pass);
  B.SetInsertPoint(Encode);
  Value *Off = B.CreateSub(W, Hi);
  Value *OffMSB = B.CreateAnd(Off, ConstantInt::get(B.getInt64Ty(), 1ULL << 63));
  Value *Encoded = B.CreateIntToPtr(Off, Ptr);
  B.CreateCondBr(B.CreateICmpEQ(OffMSB, B.getInt64(0)), Bad, Pass);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "goc_uptr_from_sptr: offset cleared MSB");
  B.SetInsertPoint(Pass);
  PHINode *Phi = B.CreatePHI(Ptr, 2);
  Phi->addIncoming(Arg, Entry);
  Phi->addIncoming(Arg, Check);
  Phi->addIncoming(Encoded, Encode);
  B.CreateRet(Phi);
  return F;
}

Function *inlineUptrRequireCptr(Module &M) {
  LLVMContext &C = M.getContext();
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Ptr, {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.uptr.require_cptr", FT);
  if (!F->empty())
    return F;
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Ok = BasicBlock::Create(C, "ok", F);
  IRBuilder<> B(Entry);
  Value *Arg = F->getArg(0);
  Value *W = B.CreatePtrToInt(Arg, B.getInt64Ty());
  Value *Lo = tlsGWord(B, 0);
  Value *Hi = tlsGWord(B, 8);
  Value *In = B.CreateAnd(B.CreateICmpNE(W, B.getInt64(0)),
                          B.CreateICmpNE(Lo, B.getInt64(0)));
  In = B.CreateAnd(In, B.CreateICmpNE(Hi, B.getInt64(0)));
  In = B.CreateAnd(In, B.CreateICmpUGE(W, Lo));
  In = B.CreateAnd(In, B.CreateICmpULT(W, Hi));
  B.CreateCondBr(In, Bad, Ok);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "sptr cannot be returned as cptr");
  B.SetInsertPoint(Ok);
  B.CreateRet(Arg);
  return F;
}

GlobalVariable *externGlobal(Module &M, StringRef Name, Type *Ty) {
  if (GlobalVariable *G = M.getGlobalVariable(Name))
    return G;
  return new GlobalVariable(M, Ty, false, GlobalValue::ExternalLinkage,
                            nullptr, Name);
}

// Pool bump, not a call. Only sound when the linked runtime was built with
// GOC_DYNALLOC_POOL (the exported cursor). QJS sets GOC_INLINE_DYNALLOC=1.
Function *inlineDynAlloc(Module &M) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Ptr, {I64, Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynalloc", FT);
  if (!F->empty())
    return F;
  GlobalVariable *Pool = externGlobal(M, "goc_alloca_pool", Ptr);
  GlobalVariable *Off = externGlobal(M, "goc_alloca_off", I64);
  GlobalVariable *Sz = externGlobal(M, "goc_alloca_pool_size", I64);
  FunctionCallee Init = M.getOrInsertFunction(
      "goc_alloca_pool_init", FunctionType::get(Type::getVoidTy(C), false));

  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Ready = BasicBlock::Create(C, "ready", F);
  BasicBlock *DoInit = BasicBlock::Create(C, "doinit", F);
  BasicBlock *Bump = BasicBlock::Create(C, "bump", F);
  BasicBlock *Exhaust = BasicBlock::Create(C, "exhaust", F);
  BasicBlock *Mark = BasicBlock::Create(C, "mark", F);
  IRBuilder<> B(Entry);
  Value *Bytes = F->getArg(0);
  Value *Scope = F->getArg(1);
  Value *Overflow = B.CreateICmpUGT(Bytes, ConstantInt::get(I64, ~uint64_t{15}));
  Value *BadArg = B.CreateOr(
      B.CreateICmpEQ(Scope, ConstantPointerNull::get(cast<PointerType>(Ptr))),
      Overflow);
  B.CreateCondBr(BadArg, Bad, Ready);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "dynamic alloca size overflow");
  B.SetInsertPoint(Ready);
  Value *Pool0 = B.CreateLoad(Ptr, Pool);
  B.CreateCondBr(
      B.CreateICmpEQ(Pool0, ConstantPointerNull::get(cast<PointerType>(Ptr))),
      DoInit, Bump);
  B.SetInsertPoint(DoInit);
  B.CreateCall(Init);
  Value *Pool1 = B.CreateLoad(Ptr, Pool);
  B.CreateBr(Bump);
  B.SetInsertPoint(Bump);
  PHINode *PoolV = B.CreatePHI(Ptr, 2);
  PoolV->addIncoming(Pool0, Ready);
  PoolV->addIncoming(Pool1, DoInit);
  Value *Cur = B.CreateLoad(I64, Off);
  Value *Aligned = B.CreateAnd(B.CreateAdd(Bytes, B.getInt64(15)), B.getInt64(~15));
  Value *Limit = B.CreateLoad(I64, Sz);
  Value *Left = B.CreateSub(Limit, Cur);
  B.CreateCondBr(B.CreateICmpUGT(Aligned, Left), Exhaust, Mark);
  B.SetInsertPoint(Exhaust);
  emitUptrFatal(B, M, "alloca pool exhausted");
  B.SetInsertPoint(Mark);
  Value *Base = B.CreateGEP(B.getInt8Ty(), PoolV, Cur);
  Value *Mark0 = B.CreateLoad(Ptr, Scope);
  Value *NeedMark = B.CreateICmpEQ(Mark0, ConstantPointerNull::get(cast<PointerType>(Ptr)));
  Value *NewMark = B.CreateSelect(NeedMark, Base, Mark0);
  B.CreateStore(NewMark, Scope);
  B.CreateStore(B.CreateAdd(Cur, Aligned), Off);
  B.CreateRet(Base);
  return F;
}

Function *inlineDynRelease(Module &M) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Type::getVoidTy(C), {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynrelease", FT);
  if (!F->empty())
    return F;
  GlobalVariable *Pool = externGlobal(M, "goc_alloca_pool", Ptr);
  GlobalVariable *Off = externGlobal(M, "goc_alloca_off", I64);
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Empty = BasicBlock::Create(C, "empty", F);
  BasicBlock *Check = BasicBlock::Create(C, "check", F);
  BasicBlock *Corrupt = BasicBlock::Create(C, "corrupt", F);
  BasicBlock *Drop = BasicBlock::Create(C, "drop", F);
  IRBuilder<> B(Entry);
  Value *Scope = F->getArg(0);
  B.CreateCondBr(B.CreateICmpEQ(Scope, ConstantPointerNull::get(cast<PointerType>(Ptr))),
                 Bad, Empty);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "missing dynamic alloca scope");
  B.SetInsertPoint(Empty);
  Value *Mark = B.CreateLoad(Ptr, Scope);
  BasicBlock *Done = BasicBlock::Create(C, "done", F);
  B.CreateCondBr(B.CreateICmpEQ(Mark, ConstantPointerNull::get(cast<PointerType>(Ptr))),
                 Done, Check);
  B.SetInsertPoint(Check);
  Value *PoolV = B.CreateLoad(Ptr, Pool);
  Value *Cur = B.CreateLoad(I64, Off);
  Value *End = B.CreateGEP(B.getInt8Ty(), PoolV, Cur);
  Value *Ok = B.CreateAnd(B.CreateICmpUGE(Mark, PoolV), B.CreateICmpULE(Mark, End));
  B.CreateCondBr(Ok, Drop, Corrupt);
  B.SetInsertPoint(Corrupt);
  emitUptrFatal(B, M, "alloca pool watermark corrupt");
  B.SetInsertPoint(Drop);
  Value *Delta = B.CreateSub(B.CreatePtrToInt(Mark, I64), B.CreatePtrToInt(PoolV, I64));
  B.CreateStore(Delta, Off);
  B.CreateStore(ConstantPointerNull::get(cast<PointerType>(Ptr)), Scope);
  B.CreateBr(Done);
  B.SetInsertPoint(Done);
  B.CreateRetVoid();
  return F;
}

bool rewriteUptrCall(CallInst *CI) {
  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;
  StringRef N = Callee->getName();
  Module &M = *CI->getModule();
  Function *Helper = nullptr;
  if (N == "goc_stack_hi")
    Helper = inlineStackBound(M, "goc.inline.stack_hi", 8);
  else if (N == "goc_stack_lo")
    Helper = inlineStackBound(M, "goc.inline.stack_lo", 0);
  else if (N == "goc_uptr_decode")
    Helper = inlineUptrDecode(M);
  else if (N == "goc_uptr_from_ptr")
    Helper = inlineUptrFromPtr(M);
  else if (N == "goc_uptr_require_cptr")
    Helper = inlineUptrRequireCptr(M);
  else if (const char *pool = std::getenv("GOC_INLINE_DYNALLOC")) {
    if (std::strcmp(pool, "1") == 0 && N == "goc_dynalloc")
      Helper = inlineDynAlloc(M);
    else if (std::strcmp(pool, "1") == 0 && N == "goc_dynrelease")
      Helper = inlineDynRelease(M);
    else
      return false;
  }
  else
    return false;
  if (CI->arg_size() != Helper->arg_size())
    return false;
  IRBuilder<> B(CI);
  SmallVector<Value *, 2> Args(CI->args());
  CallInst *New = B.CreateCall(Helper, Args);
  New->setCallingConv(CI->getCallingConv());
  CI->replaceAllUsesWith(New);
  CI->eraseFromParent();
  return true;
}

struct GocInlineGate : PassInfoMixin<GocInlineGate> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    bool Changed = false;
    SmallVector<CallInst *, 16> calls;
    for (Instruction &I : instructions(F))
      if (auto *CI = dyn_cast<CallInst>(&I))
        calls.push_back(CI);
    for (CallInst *CI : calls)
      Changed |= rewriteUptrCall(CI);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

// Name every unnamed address-taken block. LLVM 19's .ll parser keys forward
// blockaddress references by ValID, whose ordering ignores the kind, so a
// numbered and a named target in one table can collapse into one entry
// (quickjs dispatch_table: OP_pow jumped to OP_or). Clang without value
// names leaves blocks numbered while the inliner names its ".exit" blocks;
// naming them all keeps the textual hand-off between tools exact.
struct GocNameBlocks : PassInfoMixin<GocNameBlocks> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    unsigned n = 0;
    for (BasicBlock &BB : F)
      if (!BB.hasName() && BB.hasAddressTaken())
        BB.setName("goc.ba" + Twine(n++));
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

void addGocInlineGate(FunctionPassManager &FPM) { FPM.addPass(GocInlineGate()); }
void addGocNameBlocks(FunctionPassManager &FPM) { FPM.addPass(GocNameBlocks()); }

} // namespace goc::sm
