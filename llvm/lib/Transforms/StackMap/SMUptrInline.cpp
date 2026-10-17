//===- SMUptrInline.cpp - inline uptr helpers, block naming ----------===//

#include "Goc/Internal/StackMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

// --fast-stack-alloca of the driver: goc_dynalloc bumps a per-object pool.
static cl::opt<bool> GocFastStackAlloca(
    "goc-fast-stack-alloca",
    cl::desc("goc-uptr-inline: goc_dynalloc bumps a 16 MiB per-object pool "
             "(single goroutine only) instead of malloc/free per call"));

namespace goc::sm {

// Runs before O3 (and at O0: there is no runtime TU to call into). Every
// uptr / stack-bound / dynamic-alloca helper that C code or goc-color-escape
// can call gets an internal alwaysinline body here, so a goc object never
// references an out-of-line helper. Call-bearing functions may inline. Maps
// are recorded on the merged frame afterwards. A stack-pointer argument kept
// in a callee-saved register, and a local that stores &s->token, are
// reanchored / tagged so copystack adjusts them. Source noinline is left
// untouched.
//
// g is invariant for the goroutine. The FS:-8 / x28 read is readnone, and
// hoistTlsG (in goc-stackmap, after O3) leaves one read per function.
// stack.lo and stack.hi change only inside a call. The morestack prologue
// that writes them is inserted after this IR, so a call O3 proved readonly
// can still move the stack. Those loads stay volatile.
// The module's target triple says which goroutine-g convention applies.
bool gocArchArm64(const Module &M) { return Triple(M.getTargetTriple()).isAArch64(); }

Value *tlsGWord(IRBuilder<> &B, uint64_t Off) {
  FunctionType *FT = FunctionType::get(B.getInt64Ty(), false);
  // arm64 keeps g in x28 (llc -mattr=+reserve-x28). amd64 reads FS:-8.
  const char *Asm = gocArchArm64(*B.GetInsertBlock()->getModule()) ? "mov $0, x28"
                                                                   : "movq %fs:-8, $0";
  InlineAsm *IA = InlineAsm::get(FT, Asm, "=r", /*hasSideEffects=*/false);
  CallInst *C = B.CreateCall(IA);
  // g does not change for the life of the goroutine, and FS/x28 is not
  // LLVM memory. readnone + willreturn is what lets O3 treat the read as a
  // pure value (nounwind alone still counts as a side effect).
  C->setDoesNotAccessMemory();
  C->setDoesNotThrow();
  C->addFnAttr(Attribute::WillReturn);
  Value *G = B.CreateIntToPtr(C, B.getPtrTy());
  // g.stack.lo is at g+0, g.stack.hi at g+8 (stackguard0 at g+16).
  LoadInst *L = B.CreateLoad(
      B.getInt64Ty(), B.CreateGEP(B.getInt8Ty(), G, B.getInt64(Off)));
  L->setVolatile(true);
  return L;
}

// goc.uptr.fatal(msg, len): write(2, msg, len), then trap. Freestanding: no
// libc, and the message must survive a trap that the Go traceback cannot
// attribute. One cold internal copy per module.
Function *uptrFatal(Module &M) {
  if (Function *F = M.getFunction("goc.uptr.fatal"))
    return F;
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *FT = FunctionType::get(Type::getVoidTy(C), {Ptr, I64}, false);
  auto *F = Function::Create(FT, GlobalValue::InternalLinkage, "goc.uptr.fatal", M);
  F->setDoesNotReturn();
  F->setDoesNotThrow();
  F->addFnAttr(Attribute::NoInline);
  F->addFnAttr(Attribute::Cold);
  IRBuilder<> B(BasicBlock::Create(C, "entry", F));
  auto *AFT = FunctionType::get(I64, {I64, I64, Ptr, I64}, false);
  InlineAsm *IA;
  if (gocArchArm64(M))
    // Linux arm64 write is syscall 64 (x8).
    IA = InlineAsm::get(AFT, "svc #0", "={x0},{x8},{x0},{x1},{x2},~{memory}",
                        /*hasSideEffects=*/true);
  else
    // Linux amd64 write is syscall 1; the kernel clobbers rcx and r11.
    IA = InlineAsm::get(AFT, "syscall",
                        "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory},"
                        "~{dirflag},~{fpsr},~{flags}",
                        /*hasSideEffects=*/true);
  B.CreateCall(IA, {B.getInt64(gocArchArm64(M) ? 64 : 1), B.getInt64(2),
                    F->getArg(0), F->getArg(1)});
  B.CreateCall(Intrinsic::getDeclaration(&M, Intrinsic::trap));
  B.CreateUnreachable();
  return F;
}

void emitUptrFatal(IRBuilder<> &B, Module &M, const char *Msg) {
  std::string Text = std::string("goc FATAL uptr: ") + Msg + "\n";
  Value *Str = B.CreateGlobalString(Text, "goc.uptr.fatal.msg");
  B.CreateCall(uptrFatal(M), {Str, B.getInt64(Text.size())});
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

// The explicit tag conversions of goc_uptr.h. The *_hi forms take stack.hi
// as an argument; the others read the current g. A wrong tag is fatal.
Function *inlineUptrConv(Module &M, UptrConv K, bool ExplicitHi) {
  static const char *const Names[] = {"from_cptr", "as_cptr", "from_sptr", "as_sptr"};
  static const char *const Msgs[] = {
      "goc_uptr_from_cptr: address has MSB set (not encodeable as cptr)",
      "goc_uptr_as_cptr: MSB set (expected absolute cptr encoding)",
      "goc_uptr_from_sptr: offset cleared MSB (abs not below stack.hi?)",
      "goc_uptr_as_sptr: MSB clear (expected stack-offset encoding)"};
  unsigned I = static_cast<unsigned>(K);
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  FunctionType *FT = ExplicitHi ? FunctionType::get(Ptr, {Ptr, I64}, false)
                                : FunctionType::get(Ptr, {Ptr}, false);
  std::string Name = std::string("goc.inline.uptr.") + Names[I] + (ExplicitHi ? "_hi" : "");
  Function *F = alwaysInlineUptr(M, Name, FT);
  if (!F->empty())
    return F;
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Ok = BasicBlock::Create(C, "ok", F);
  IRBuilder<> B(Entry);
  Value *Arg = F->getArg(0);
  Value *W = B.CreatePtrToInt(Arg, I64);
  Value *MSB = ConstantInt::get(I64, 1ULL << 63);
  auto stackHi = [&]() -> Value * { return ExplicitHi ? F->getArg(1) : tlsGWord(B, 8); };
  auto msbSet = [&](Value *V) { return B.CreateICmpNE(B.CreateAnd(V, MSB), B.getInt64(0)); };
  Value *Result = Arg;
  switch (K) {
  case UptrConv::FromCptr:
  case UptrConv::AsCptr:
    // An absolute cptr word must have MSB clear.
    B.CreateCondBr(msbSet(W), Bad, Ok);
    B.SetInsertPoint(Ok);
    break;
  case UptrConv::FromSptr: {
    // Offset from stack.hi: negative, so MSB set.
    Value *Off = B.CreateSub(W, stackHi());
    B.CreateCondBr(msbSet(Off), Ok, Bad);
    B.SetInsertPoint(Ok);
    Result = B.CreateIntToPtr(Off, Ptr);
    break;
  }
  case UptrConv::AsSptr:
    B.CreateCondBr(msbSet(W), Ok, Bad);
    B.SetInsertPoint(Ok);
    Result = B.CreateIntToPtr(B.CreateAdd(stackHi(), W), Ptr);
    break;
  }
  B.CreateRet(Result);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, Msgs[I]);
  return F;
}

// Alloca pool: 16 MiB, one per module, bumped in-line. The pool lives in the
// object's BSS, so it needs no init call and no allocator. Each module's
// allocations nest LIFO by function return, so a pool per object is as
// correct as one per process. The cursor is a plain global: one goroutine
// only (QuickJS). JS_DEFAULT_STACK_SIZE is 1 MiB, but the goc CLI sets
// --stack-size 16384 KiB (EarleyBoyer and deep recursion exceed 1 MiB of
// nested frames), so the pool matches that limit and does not trap before
// QuickJS's own stack check.
constexpr uint64_t DynPoolBytes = 16ULL << 20;

GlobalVariable *dynPool(Module &M) {
  if (GlobalVariable *G = M.getGlobalVariable("goc.alloca.pool", true))
    return G;
  auto *Ty = ArrayType::get(Type::getInt8Ty(M.getContext()), DynPoolBytes);
  auto *G = new GlobalVariable(M, Ty, false, GlobalValue::InternalLinkage,
                               ConstantAggregateZero::get(Ty), "goc.alloca.pool");
  G->setAlignment(Align(16));
  return G;
}

GlobalVariable *dynPoolOff(Module &M) {
  if (GlobalVariable *G = M.getGlobalVariable("goc.alloca.off", true))
    return G;
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *G = new GlobalVariable(M, I64, false, GlobalValue::InternalLinkage,
                               ConstantInt::get(I64, 0), "goc.alloca.off");
  G->setAlignment(Align(8));
  return G;
}

// --fast-stack-alloca (GOC_INLINE_DYNALLOC=1): pool bump, not a call.
Function *inlineDynAllocPool(Module &M) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *Null = ConstantPointerNull::get(Ptr);
  auto *FT = FunctionType::get(Ptr, {I64, Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynalloc", FT);
  if (!F->empty())
    return F;
  GlobalVariable *Pool = dynPool(M);
  GlobalVariable *Off = dynPoolOff(M);

  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Bump = BasicBlock::Create(C, "bump", F);
  BasicBlock *Exhaust = BasicBlock::Create(C, "exhaust", F);
  BasicBlock *Mark = BasicBlock::Create(C, "mark", F);
  IRBuilder<> B(Entry);
  Value *Bytes = F->getArg(0);
  Value *Scope = F->getArg(1);
  Value *Overflow = B.CreateICmpUGT(Bytes, ConstantInt::get(I64, ~uint64_t{15}));
  B.CreateCondBr(B.CreateOr(B.CreateICmpEQ(Scope, Null), Overflow), Bad, Bump);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "dynamic alloca size overflow");
  B.SetInsertPoint(Bump);
  Value *Cur = B.CreateLoad(I64, Off);
  Value *Aligned = B.CreateAnd(B.CreateAdd(Bytes, B.getInt64(15)), B.getInt64(~15));
  Value *Left = B.CreateSub(B.getInt64(DynPoolBytes), Cur);
  B.CreateCondBr(B.CreateICmpUGT(Aligned, Left), Exhaust, Mark);
  B.SetInsertPoint(Exhaust);
  emitUptrFatal(B, M, "alloca pool exhausted");
  B.SetInsertPoint(Mark);
  // The first bump in a function saves the watermark. A pool address is
  // never NULL, so a later alloca in the same function leaves it alone.
  // Native alloca does not zero, and neither does the pool.
  Value *Base = B.CreateGEP(B.getInt8Ty(), Pool, Cur);
  Value *Mark0 = B.CreateLoad(Ptr, Scope);
  B.CreateStore(B.CreateSelect(B.CreateICmpEQ(Mark0, Null), Base, Mark0), Scope);
  B.CreateStore(B.CreateAdd(Cur, Aligned), Off);
  B.CreateRet(Base);
  return F;
}

Function *inlineDynReleasePool(Module &M) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *Null = ConstantPointerNull::get(Ptr);
  auto *FT = FunctionType::get(Type::getVoidTy(C), {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynrelease", FT);
  if (!F->empty())
    return F;
  GlobalVariable *Pool = dynPool(M);
  GlobalVariable *Off = dynPoolOff(M);
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Empty = BasicBlock::Create(C, "empty", F);
  BasicBlock *Check = BasicBlock::Create(C, "check", F);
  BasicBlock *Corrupt = BasicBlock::Create(C, "corrupt", F);
  BasicBlock *Drop = BasicBlock::Create(C, "drop", F);
  BasicBlock *Done = BasicBlock::Create(C, "done", F);
  IRBuilder<> B(Entry);
  Value *Scope = F->getArg(0);
  B.CreateCondBr(B.CreateICmpEQ(Scope, Null), Bad, Empty);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "missing dynamic alloca scope");
  B.SetInsertPoint(Empty);
  Value *Mark = B.CreateLoad(Ptr, Scope);
  B.CreateCondBr(B.CreateICmpEQ(Mark, Null), Done, Check);
  B.SetInsertPoint(Check);
  Value *Cur = B.CreateLoad(I64, Off);
  Value *End = B.CreateGEP(B.getInt8Ty(), Pool, Cur);
  Value *Ok = B.CreateAnd(B.CreateICmpUGE(Mark, Pool), B.CreateICmpULE(Mark, End));
  B.CreateCondBr(Ok, Drop, Corrupt);
  B.SetInsertPoint(Corrupt);
  emitUptrFatal(B, M, "alloca pool watermark corrupt");
  B.SetInsertPoint(Drop);
  // Drop every byte this function bumped.
  Value *Delta = B.CreateSub(B.CreatePtrToInt(Mark, I64), B.CreatePtrToInt(Pool, I64));
  B.CreateStore(Delta, Off);
  B.CreateStore(Null, Scope);
  B.CreateBr(Done);
  B.SetInsertPoint(Done);
  B.CreateRetVoid();
  return F;
}

// --no-fast-stack-alloca: one malloc per alloca, chained on the scope and
// freed at return. Off the goroutine stack either way: Go pcsp cannot
// describe a mid-frame SP change, and the pointer must stay a cptr across
// stack growth. Block header: {previous, size}, 16 bytes, so the payload
// keeps malloc's 16-byte alignment.
Function *inlineDynAllocHeap(Module &M) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *Ptr = PointerType::getUnqual(C);
  auto *Null = ConstantPointerNull::get(Ptr);
  auto *FT = FunctionType::get(Ptr, {I64, Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynalloc.heap", FT);
  if (!F->empty())
    return F;
  FunctionCallee Malloc = M.getOrInsertFunction("malloc", FunctionType::get(Ptr, {I64}, false));
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Alloc = BasicBlock::Create(C, "alloc", F);
  BasicBlock *Oom = BasicBlock::Create(C, "oom", F);
  BasicBlock *Link = BasicBlock::Create(C, "link", F);
  IRBuilder<> B(Entry);
  Value *Bytes = F->getArg(0);
  Value *Scope = F->getArg(1);
  Value *Overflow = B.CreateICmpUGT(Bytes, ConstantInt::get(I64, ~uint64_t{15}));
  B.CreateCondBr(B.CreateOr(B.CreateICmpEQ(Scope, Null), Overflow), Bad, Alloc);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "dynamic alloca size overflow");
  B.SetInsertPoint(Alloc);
  Value *Size = B.CreateAdd(Bytes, B.getInt64(16));
  Value *Block = B.CreateCall(Malloc, {Size});
  B.CreateCondBr(B.CreateICmpEQ(Block, Null), Oom, Link);
  B.SetInsertPoint(Oom);
  emitUptrFatal(B, M, "malloc failed for dynamic alloca");
  B.SetInsertPoint(Link);
  B.CreateStore(B.CreateLoad(Ptr, Scope), Block);
  B.CreateStore(Size, B.CreateGEP(B.getInt8Ty(), Block, B.getInt64(8)));
  B.CreateStore(Block, Scope);
  B.CreateRet(B.CreateGEP(B.getInt8Ty(), Block, B.getInt64(16)));
  return F;
}

Function *inlineDynReleaseHeap(Module &M) {
  LLVMContext &C = M.getContext();
  auto *Ptr = PointerType::getUnqual(C);
  auto *Null = ConstantPointerNull::get(Ptr);
  auto *FT = FunctionType::get(Type::getVoidTy(C), {Ptr}, false);
  Function *F = alwaysInlineUptr(M, "goc.inline.dynrelease.heap", FT);
  if (!F->empty())
    return F;
  FunctionCallee Free = M.getOrInsertFunction(
      "free", FunctionType::get(Type::getVoidTy(C), {Ptr}, false));
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *Bad = BasicBlock::Create(C, "bad", F);
  BasicBlock *Start = BasicBlock::Create(C, "start", F);
  BasicBlock *Loop = BasicBlock::Create(C, "loop", F);
  BasicBlock *Body = BasicBlock::Create(C, "body", F);
  BasicBlock *Done = BasicBlock::Create(C, "done", F);
  IRBuilder<> B(Entry);
  Value *Scope = F->getArg(0);
  B.CreateCondBr(B.CreateICmpEQ(Scope, Null), Bad, Start);
  B.SetInsertPoint(Bad);
  emitUptrFatal(B, M, "missing dynamic alloca scope");
  B.SetInsertPoint(Start);
  Value *Head = B.CreateLoad(Ptr, Scope);
  B.CreateStore(Null, Scope);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  PHINode *Cur = B.CreatePHI(Ptr, 2);
  Cur->addIncoming(Head, Start);
  B.CreateCondBr(B.CreateICmpEQ(Cur, Null), Done, Body);
  B.SetInsertPoint(Body);
  Value *Prev = B.CreateLoad(Ptr, Cur);
  B.CreateCall(Free, {Cur});
  Cur->addIncoming(Prev, Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Done);
  B.CreateRetVoid();
  return F;
}

bool fastStackAlloca() { return GocFastStackAlloca; }

Function *uptrHelperFor(Module &M, StringRef N) {
  if (N == "goc_stack_hi")
    return inlineStackBound(M, "goc.inline.stack_hi", 8);
  if (N == "goc_stack_lo")
    return inlineStackBound(M, "goc.inline.stack_lo", 0);
  if (N == "goc_uptr_decode")
    return inlineUptrDecode(M);
  if (N == "goc_uptr_from_ptr")
    return inlineUptrFromPtr(M);
  if (N == "goc_uptr_require_cptr")
    return inlineUptrRequireCptr(M);
  static const struct {
    const char *Name;
    UptrConv K;
    bool Hi;
  } Convs[] = {
      {"goc_uptr_from_cptr", UptrConv::FromCptr, false},
      {"goc_uptr_from_cptr_hi", UptrConv::FromCptr, true},
      {"goc_uptr_as_cptr", UptrConv::AsCptr, false},
      {"goc_uptr_as_cptr_hi", UptrConv::AsCptr, true},
      {"goc_uptr_from_sptr", UptrConv::FromSptr, false},
      {"goc_uptr_from_sptr_hi", UptrConv::FromSptr, true},
      {"goc_uptr_as_sptr", UptrConv::AsSptr, false},
      {"goc_uptr_as_sptr_hi", UptrConv::AsSptr, true},
  };
  for (const auto &Cv : Convs)
    if (N == Cv.Name)
      return inlineUptrConv(M, Cv.K, Cv.Hi);
  if (N == "goc_dynalloc")
    return fastStackAlloca() ? inlineDynAllocPool(M) : inlineDynAllocHeap(M);
  if (N == "goc_dynrelease")
    return fastStackAlloca() ? inlineDynReleasePool(M) : inlineDynReleaseHeap(M);
  return nullptr;
}

bool rewriteUptrCall(CallInst *CI) {
  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;
  StringRef N = Callee->getName();
  Module &M = *CI->getModule();
  Function *Helper = uptrHelperFor(M, N);
  if (!Helper)
    return false;
  if (CI->arg_size() != Helper->arg_size())
    report_fatal_error(Twine("goc: call to ") + N + " has " + Twine(CI->arg_size()) +
                       " arguments; goc_uptr.h declares " + Twine(Helper->arg_size()));
  IRBuilder<> B(CI);
  SmallVector<Value *, 2> Args(CI->args());
  CallInst *New = B.CreateCall(Helper, Args);
  New->setCallingConv(CI->getCallingConv());
  CI->replaceAllUsesWith(New);
  CI->eraseFromParent();
  return true;
}

struct GocUptrInline : PassInfoMixin<GocUptrInline> {
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

void addGocUptrInline(FunctionPassManager &FPM) { FPM.addPass(GocUptrInline()); }
void addGocNameBlocks(FunctionPassManager &FPM) { FPM.addPass(GocNameBlocks()); }

} // namespace goc::sm
