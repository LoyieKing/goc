//===- GocUptrLower.cpp - P19: lower goc_uptr_* to MSB protocol IR --------===//
// Replaces calls to goc_uptr_from_{sptr,cptr}[_hi] / goc_uptr_as_{sptr,cptr}[_hi]
// with explicit int64 offset / MSB checks against stack.hi.
// Evidence in IR: sub i64 (encode), icmp + and with 1<<63, add on decode.
//===----------------------------------------------------------------------===//
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <memory>
#include <string>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input.ll>"),
                                          cl::Required);
static cl::opt<std::string> OutputFilename("o", cl::desc("Output IR"),
                                           cl::init("-"));
static cl::opt<bool> KeepDecls("keep-decls",
                               cl::desc("Keep goc_uptr_* declarations"),
                               cl::init(false));

static Function *getOrInsertFatal(Module &M) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx),
                                       {PointerType::getUnqual(Ctx)}, false);
  FunctionCallee C = M.getOrInsertFunction("goc_uptr_fatal", FT);
  Function *F = cast<Function>(C.getCallee());
  F->setDoesNotReturn();
  return F;
}

static Function *getOrInsertStackHi(Module &M) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *FT = FunctionType::get(Type::getInt64Ty(Ctx), false);
  return cast<Function>(M.getOrInsertFunction("goc_stack_hi", FT).getCallee());
}

static Value *ptrToI64(IRBuilder<> &B, Value *P) {
  return B.CreatePtrToInt(P, B.getInt64Ty(), "goc.uptr.p2i");
}

static Value *i64ToPtr(IRBuilder<> &B, Value *I, Type *PtrTy) {
  return B.CreateIntToPtr(I, PtrTy, "goc.uptr.i2p");
}

static ConstantInt *msbMask(LLVMContext &Ctx) {
  return ConstantInt::get(Type::getInt64Ty(Ctx), uint64_t(1) << 63);
}

/// Insert MSB check just before CI. On failure: call goc_uptr_fatal + unreachable.
/// On success: fall through to CI's block (split). Returns builder at success path
/// (at CI).
static void insertFatalGuard(CallInst *CI, Module &M, Value *Bad,
                             const char *Msg) {
  // Split so CI (and everything after) move to Cont; Orig ends with br Cont.
  BasicBlock *Orig = CI->getParent();
  BasicBlock *Cont = Orig->splitBasicBlock(CI->getIterator(), "goc.uptr.cont");

  // Replace unconditional br with cond br to Fail / Cont.
  Instruction *OldBr = Orig->getTerminator();
  IRBuilder<> B(OldBr);
  BasicBlock *Fail =
      BasicBlock::Create(M.getContext(), "goc.uptr.fatal", Orig->getParent(), Cont);
  B.CreateCondBr(Bad, Fail, Cont);
  OldBr->eraseFromParent();

  IRBuilder<> FB(Fail);
  Value *MsgV = FB.CreateGlobalString(Msg, "goc.uptr.fatal.msg");
  FB.CreateCall(getOrInsertFatal(M), {MsgV});
  FB.CreateUnreachable();
  (void)Cont;
}

static Value *lowerFromCptr(CallInst *CI, Module &M, Value *P) {
  IRBuilder<> B(CI);
  Value *W = ptrToI64(B, P);
  Value *MSB = B.CreateAnd(W, msbMask(M.getContext()), "goc.uptr.msb");
  Value *Bad = B.CreateICmpNE(MSB, B.getInt64(0), "goc.uptr.cptr.badmsb");
  insertFatalGuard(CI, M, Bad, "goc_uptr_from_cptr: address has MSB set");
  // CI is now at start of cont block; rebuild builder.
  B.SetInsertPoint(CI);
  return i64ToPtr(B, W, CI->getType());
}

static Value *lowerFromSptr(CallInst *CI, Module &M, Value *P, Value *Hi) {
  IRBuilder<> B(CI);
  Value *Abs = ptrToI64(B, P);
  Value *Off = B.CreateSub(Abs, Hi, "goc.uptr.off");
  Value *MSB = B.CreateAnd(Off, msbMask(M.getContext()), "goc.uptr.msb");
  Value *Bad = B.CreateICmpEQ(MSB, B.getInt64(0), "goc.uptr.sptr.badmsb");
  insertFatalGuard(CI, M, Bad, "goc_uptr_from_sptr: offset cleared MSB");
  B.SetInsertPoint(CI);
  Value *Res = i64ToPtr(B, Off, CI->getType());
  if (auto *I = dyn_cast<Instruction>(Res)) {
    LLVMContext &Ctx = M.getContext();
    I->setMetadata("goc.uptr_encoded",
                   MDNode::get(Ctx, {MDString::get(Ctx, "1")}));
    I->setMetadata("goc.color", MDNode::get(Ctx, {MDString::get(Ctx, "uptr")}));
  }
  return Res;
}

static Value *lowerAsCptr(CallInst *CI, Module &M, Value *U) {
  IRBuilder<> B(CI);
  Value *W = ptrToI64(B, U);
  Value *MSB = B.CreateAnd(W, msbMask(M.getContext()), "goc.uptr.msb");
  Value *Bad = B.CreateICmpNE(MSB, B.getInt64(0), "goc.uptr.as_cptr.bad");
  insertFatalGuard(CI, M, Bad, "goc_uptr_as_cptr: MSB set");
  B.SetInsertPoint(CI);
  return i64ToPtr(B, W, CI->getType());
}

static Value *lowerAsSptr(CallInst *CI, Module &M, Value *U, Value *Hi) {
  IRBuilder<> B(CI);
  Value *W = ptrToI64(B, U);
  Value *MSB = B.CreateAnd(W, msbMask(M.getContext()), "goc.uptr.msb");
  Value *Bad = B.CreateICmpEQ(MSB, B.getInt64(0), "goc.uptr.as_sptr.bad");
  insertFatalGuard(CI, M, Bad, "goc_uptr_as_sptr: MSB clear");
  B.SetInsertPoint(CI);
  Value *Abs = B.CreateAdd(Hi, W, "goc.uptr.abs");
  return i64ToPtr(B, Abs, CI->getType());
}

static bool isUptrBuiltin(StringRef N) {
  return N == "goc_uptr_from_sptr" || N == "goc_uptr_from_cptr" ||
         N == "goc_uptr_as_sptr" || N == "goc_uptr_as_cptr" ||
         N == "goc_uptr_from_sptr_hi" || N == "goc_uptr_from_cptr_hi" ||
         N == "goc_uptr_as_sptr_hi" || N == "goc_uptr_as_cptr_hi";
}

static bool lowerCall(CallInst *CI, Module &M) {
  Function *Callee = CI->getCalledFunction();
  if (!Callee)
    return false;
  StringRef N = Callee->getName();
  if (!isUptrBuiltin(N))
    return false;

  IRBuilder<> Pre(CI);
  Value *Hi = nullptr;
  auto needHi = [&]() -> Value * {
    if (Hi)
      return Hi;
    if (N.ends_with("_hi")) {
      if (CI->arg_size() < 2)
        report_fatal_error("goc-uptr-lower: *_hi builtin missing hi arg");
      Hi = CI->getArgOperand(1);
      if (Hi->getType()->isPointerTy())
        Hi = ptrToI64(Pre, Hi);
      else if (!Hi->getType()->isIntegerTy(64))
        Hi = Pre.CreateZExtOrTrunc(Hi, Pre.getInt64Ty(), "goc.uptr.hi");
    } else {
      Hi = Pre.CreateCall(getOrInsertStackHi(M), {}, "goc.uptr.hi");
    }
    return Hi;
  };

  Value *Rep = nullptr;
  if (N == "goc_uptr_from_cptr" || N == "goc_uptr_from_cptr_hi") {
    if (N.ends_with("_hi"))
      (void)needHi();
    Rep = lowerFromCptr(CI, M, CI->getArgOperand(0));
  } else if (N == "goc_uptr_from_sptr" || N == "goc_uptr_from_sptr_hi") {
    Rep = lowerFromSptr(CI, M, CI->getArgOperand(0), needHi());
  } else if (N == "goc_uptr_as_cptr" || N == "goc_uptr_as_cptr_hi") {
    if (N.ends_with("_hi"))
      (void)needHi();
    Rep = lowerAsCptr(CI, M, CI->getArgOperand(0));
  } else if (N == "goc_uptr_as_sptr" || N == "goc_uptr_as_sptr_hi") {
    Rep = lowerAsSptr(CI, M, CI->getArgOperand(0), needHi());
  } else {
    return false;
  }

  CI->replaceAllUsesWith(Rep);
  CI->eraseFromParent();
  return true;
}

static int runOnModule(Module &M) {
  SmallVector<CallInst *, 32> Work;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *CI = dyn_cast<CallInst>(&I))
          if (Function *C = CI->getCalledFunction())
            if (isUptrBuiltin(C->getName()))
              Work.push_back(CI);
  }

  unsigned N = 0;
  for (CallInst *CI : Work) {
    if (lowerCall(CI, M))
      ++N;
  }

  if (!KeepDecls) {
    SmallVector<Function *, 8> Kill;
    for (Function &F : M)
      if (isUptrBuiltin(F.getName()) && F.isDeclaration() && F.use_empty())
        Kill.push_back(&F);
    for (Function *F : Kill)
      F->eraseFromParent();
  }

  M.addModuleFlag(Module::ModFlagBehavior::Warning, "goc.uptr.msb",
                  ConstantInt::get(Type::getInt32Ty(M.getContext()), 19));

  errs() << "goc-uptr-lower: lowered " << N << " uptr builtin call(s)\n";
  return (int)N;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "goc P19 uptr MSB IR lower\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 2;
  }

  runOnModule(*M);

  if (verifyModule(*M, &errs())) {
    errs() << "goc-uptr-lower: verifyModule failed\n";
    return 3;
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "unable to open '" << OutputFilename << "': " << EC.message()
           << "\n";
    return 2;
  }
  M->print(OS, nullptr);
  return 0;
}
