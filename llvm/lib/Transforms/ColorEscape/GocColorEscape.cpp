//===- GocColorEscape.cpp - pointer color + escape analysis ---------------===//
// Frontend vertical slice: refine auto_ptr, encode stack pointers stored in
// cptr T* slots as uptr, and annotate IR for later backend hooks.
//===----------------------------------------------------------------------===//
//
// The pass is split by phase; ColorEscapeInternal.h holds the shared state:
//   CERewrite.cpp   implicit uptr encodes/decodes and guarded returns
//   CECheck.cpp     escape/mix checks, metadata, formals, default color
//   lib/Analysis/GocColor: ColorAnalysis.cpp (seeding, propagation) and
//   ColorPrinter.cpp (the print<goc-color> table)

#include "Goc/Internal/ColorEscape.h"
#include "Goc/ColorEscape.h"

using namespace llvm;
using namespace goc::ce;

int goc::runColorEscape(Module &M, raw_ostream *Report) {
  // Register goc metadata kinds up front: printed attachment order follows
  // kind IDs, and later passes would otherwise register them in DenseMap
  // (pointer-hash) order, making the output IR nondeterministic.
  for (const char *Kind : {"goc.color", "goc.prov", "goc.uptr_encoded"})
    M.getContext().getMDKindID(Kind);
  unsigned DynamicAllocas = lowerDynamicAllocas(M);
  ColorEscapeState S;
  seedAnnotations(M, S);
  seedProducers(M, S);
  proveStackOnlyFormals(M, S);
  applyDefaultPtrColor(M, S);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
  }

  // A decoded uptr can be assigned to a different cptr field, promoting its
  // storage in turn. Continue until every newly encoded slot's loads have also
  // been decoded; a fixed two-pass sequence silently returned encoded words.
  unsigned EncodedStores = 0, DecodedLoads = 0;
  for (;;) {
    unsigned NewEncoded = encodeEscapingSptrStores(M, S);
    unsigned NewDecoded = decodeAutoUptrLoads(M, S);
    EncodedStores += NewEncoded;
    DecodedLoads += NewDecoded;
    if (NewEncoded == 0 && NewDecoded == 0)
      break;
    for (Function &F : M) {
      if (!F.isDeclaration())
        propagate(F, S);
    }
  }
  unsigned GuardedReturns = guardDynamicUptrReturns(M, S);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    checkEscapesAndMix(F, S);
  }
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    propagate(F, S);
  }
  attachMetadata(M, S);

  if (Report) {
    emitColorReport(M, S, *Report);
    return (int)S.ErrorCount;
  }
  errs() << "goc-color-escape: summary: " << S.ErrorCount
         << " error(s), scoped dynamic allocas=" << DynamicAllocas
         << ", implicit uptr stores=" << EncodedStores
         << ", decoded pointer loads=" << DecodedLoads
         << ", guarded returns=" << GuardedReturns << " in module '"
         << M.getModuleIdentifier() << "'\n";
  return (int)S.ErrorCount;
}
