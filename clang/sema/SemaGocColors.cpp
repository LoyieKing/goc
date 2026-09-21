//===- SemaGocColors.cpp - goc pointer-color Sema (P28 in-tree) -----------===//
//
// Contract (goc-syntax-guide.md v0.2.1):
//   sptr store to heap/global/return = hard error; no auto-promote; no dsptr.
//
// Attr.td defines GocCPtr/SPtr/UPtr/AutoPtr/GPtr with SimpleHandler=1.
// This file:
//   1. Mirrors Goc*Attr → AnnotateAttr("goc.color.*") so CodeGen emits
//      llvm.var.annotation for P17 refine / P18 bridge (same as P27 plugin).
//   2. Runs AST escape analysis at end of TU (same rules as GocClangPlugin).
//===----------------------------------------------------------------------===//

#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/StringSwitch.h"

using namespace clang;

namespace {

enum class GocColor : uint8_t {
  None = 0,
  Auto,
  CPtr,
  SPtr,
  UPtr,
  GPtr,
};

static GocColor parseColorToken(StringRef S) {
  return llvm::StringSwitch<GocColor>(S)
      .Cases("goc.color.cptr", "cptr", "goc_cptr", GocColor::CPtr)
      .Cases("goc.color.sptr", "sptr", "goc_sptr", GocColor::SPtr)
      .Cases("goc.color.uptr", "uptr", "goc_uptr", GocColor::UPtr)
      .Cases("goc.color.auto", "auto", "auto_ptr", "goc_auto_ptr", GocColor::Auto)
      .Cases("goc.color.gptr", "gptr", "goc_gptr", GocColor::GPtr)
      .Default(GocColor::None);
}

static const char *colorAnnotate(GocColor C) {
  switch (C) {
  case GocColor::CPtr:
    return "goc.color.cptr";
  case GocColor::SPtr:
    return "goc.color.sptr";
  case GocColor::UPtr:
    return "goc.color.uptr";
  case GocColor::Auto:
    return "goc.color.auto";
  case GocColor::GPtr:
    return "goc.color.gptr";
  case GocColor::None:
    return nullptr;
  }
  return nullptr;
}

static const char *colorName(GocColor C) {
  switch (C) {
  case GocColor::CPtr:
    return "cptr";
  case GocColor::SPtr:
    return "sptr";
  case GocColor::UPtr:
    return "uptr";
  case GocColor::Auto:
    return "auto_ptr";
  case GocColor::GPtr:
    return "gptr";
  case GocColor::None:
    return "none";
  }
  return "?";
}

static GocColor colorFromGocAttr(const Decl *D) {
  if (!D)
    return GocColor::None;
  if (D->hasAttr<GocCPtrAttr>())
    return GocColor::CPtr;
  if (D->hasAttr<GocSPtrAttr>())
    return GocColor::SPtr;
  if (D->hasAttr<GocUPtrAttr>())
    return GocColor::UPtr;
  if (D->hasAttr<GocAutoPtrAttr>())
    return GocColor::Auto;
  if (D->hasAttr<GocGPtrAttr>())
    return GocColor::GPtr;
  return GocColor::None;
}

static GocColor colorFromDecl(const Decl *D) {
  if (!D)
    return GocColor::None;
  GocColor FromGoc = colorFromGocAttr(D);
  if (FromGoc != GocColor::None)
    return FromGoc;
  for (const auto *A : D->specific_attrs<AnnotateAttr>()) {
    GocColor C = parseColorToken(A->getAnnotation());
    if (C != GocColor::None)
      return C;
  }
  return GocColor::None;
}

static bool hasColorAnnotate(const Decl *D, StringRef Want) {
  for (const auto *A : D->specific_attrs<AnnotateAttr>()) {
    if (A->getAnnotation() == Want)
      return true;
  }
  return false;
}

/// Ensure Goc*Attr decls also carry AnnotateAttr for IR survival (P17/P18).
static void mirrorGocAttrsToAnnotate(ASTContext &Ctx, Decl *D) {
  GocColor C = colorFromGocAttr(D);
  if (C == GocColor::None)
    return;
  const char *Ann = colorAnnotate(C);
  if (!Ann || hasColorAnnotate(D, Ann))
    return;
  D->addAttr(AnnotateAttr::Create(Ctx, Ann, nullptr, 0, D->getSourceRange()));
}

static GocColor colorOfExpr(const Expr *E) {
  if (!E)
    return GocColor::None;
  E = E->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return colorFromDecl(DRE->getDecl());

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts())) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
          GocColor C = colorFromDecl(VD);
          if (C == GocColor::None && VD->hasLocalStorage())
            return GocColor::SPtr;
          if (C == GocColor::Auto && VD->hasLocalStorage())
            return GocColor::SPtr;
          return C != GocColor::None ? C : GocColor::SPtr;
        }
      }
    }
  }

  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return colorOfExpr(ICE->getSubExpr());
  if (const auto *CE = dyn_cast<CastExpr>(E))
    return colorOfExpr(CE->getSubExpr());
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return colorFromDecl(ME->getMemberDecl());

  return GocColor::None;
}

static bool destIsNonStack(const Expr *LHS) {
  if (!LHS)
    return false;
  LHS = LHS->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (VD->hasGlobalStorage() || VD->getTLSKind() != VarDecl::TLS_None)
        return true;
      return false;
    }
  }

  if (const auto *ME = dyn_cast<MemberExpr>(LHS)) {
    const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (VD->hasGlobalStorage())
          return true;
        return false;
      }
    }
    if (ME->isArrow() || isa<UnaryOperator>(Base))
      return true;
    return destIsNonStack(Base);
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(LHS))
    return destIsNonStack(ASE->getBase());

  if (const auto *UO = dyn_cast<UnaryOperator>(LHS)) {
    if (UO->getOpcode() == UO_Deref)
      return true;
  }

  return false;
}

static bool isSptrValue(GocColor C) { return C == GocColor::SPtr; }

class GocMirrorVisitor : public RecursiveASTVisitor<GocMirrorVisitor> {
  ASTContext &Ctx;

public:
  explicit GocMirrorVisitor(ASTContext &Ctx) : Ctx(Ctx) {}

  bool VisitVarDecl(VarDecl *VD) {
    mirrorGocAttrsToAnnotate(Ctx, VD);
    return true;
  }
  bool VisitFieldDecl(FieldDecl *FD) {
    mirrorGocAttrsToAnnotate(Ctx, FD);
    return true;
  }
  bool VisitParmVarDecl(ParmVarDecl *PD) {
    mirrorGocAttrsToAnnotate(Ctx, PD);
    return true;
  }
  bool VisitTypedefNameDecl(TypedefNameDecl *TD) {
    mirrorGocAttrsToAnnotate(Ctx, TD);
    return true;
  }
};

class GocEscapeVisitor : public RecursiveASTVisitor<GocEscapeVisitor> {
  DiagnosticsEngine &Diags;
  unsigned DiagSptrStore;
  unsigned DiagSptrReturn;
  unsigned DiagNoteColor;

public:
  explicit GocEscapeVisitor(ASTContext &Ctx) : Diags(Ctx.getDiagnostics()) {
    DiagSptrStore = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "goc: sptr escape — storing stack pointer (%0) into non-stack location "
        "(heap/global/field via pointer); encode with goc_uptr_from_sptr or keep "
        "on stack (no auto-promote; no dsptr)");
    DiagSptrReturn = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "goc: sptr escape — returning stack pointer (%0) from function");
    DiagNoteColor = Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                          "goc: value color here is '%0'");
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO->isAssignmentOp())
      return true;
    GocColor RHS = colorOfExpr(BO->getRHS());
    if (!isSptrValue(RHS))
      return true;
    if (!destIsNonStack(BO->getLHS()))
      return true;
    Diags.Report(BO->getExprLoc(), DiagSptrStore) << colorName(RHS);
    Diags.Report(BO->getRHS()->getExprLoc(), DiagNoteColor) << colorName(RHS);
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    if (!VD->hasInit())
      return true;
    if (!VD->hasGlobalStorage())
      return true;
    GocColor RHS = colorOfExpr(VD->getInit());
    if (!isSptrValue(RHS))
      return true;
    Diags.Report(VD->getLocation(), DiagSptrStore) << colorName(RHS);
    return true;
  }

  bool VisitReturnStmt(ReturnStmt *RS) {
    const Expr *RV = RS->getRetValue();
    if (!RV)
      return true;
    GocColor C = colorOfExpr(RV);
    if (!isSptrValue(C))
      return true;
    Diags.Report(RS->getReturnLoc(), DiagSptrReturn) << colorName(C);
    return true;
  }
};

} // namespace

void Sema::DiagnoseGocColorEscapes() {
  // Attr.td Goc* types are generated after tablegen; until then this TU may
  // fail to compile if Attrs are missing — they are present in this tree.
  TranslationUnitDecl *TU = Context.getTranslationUnitDecl();
  if (!TU)
    return;

  GocMirrorVisitor Mirror(Context);
  Mirror.TraverseDecl(TU);

  GocEscapeVisitor Escape(Context);
  Escape.TraverseDecl(TU);
}
