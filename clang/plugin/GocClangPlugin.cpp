//===- GocClangPlugin.cpp - P27 goc color attrs + Sema escape --------------===//
// Out-of-tree Clang plugin that meaningfully extends Clang:
//   1. ParsedAttrInfo: goc_cptr / goc_sptr / goc_uptr / goc_auto_ptr / goc_gptr
//      (attaches AnnotateAttr "goc.color.*" for IR survival)
//   2. PluginASTAction: AST/Sema-level escape analysis — sptr into heap/global
//      or returned = hard error (no auto-promote; no dsptr).
// Loaded via: clang-19 -fplugin=libGocClang.so …
// In-tree patches (Attr.td etc.) live in ../patches/ for eventual clang rebuild.
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

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
  case GocColor::CPtr: return "goc.color.cptr";
  case GocColor::SPtr: return "goc.color.sptr";
  case GocColor::UPtr: return "goc.color.uptr";
  case GocColor::Auto: return "goc.color.auto";
  case GocColor::GPtr: return "goc.color.gptr";
  case GocColor::None: return nullptr;
  }
  return nullptr;
}

static const char *colorName(GocColor C) {
  switch (C) {
  case GocColor::CPtr: return "cptr";
  case GocColor::SPtr: return "sptr";
  case GocColor::UPtr: return "uptr";
  case GocColor::Auto: return "auto_ptr";
  case GocColor::GPtr: return "gptr";
  case GocColor::None: return "none";
  }
  return "?";
}

static GocColor colorFromDecl(const Decl *D) {
  if (!D)
    return GocColor::None;
  for (const auto *A : D->specific_attrs<AnnotateAttr>()) {
    GocColor C = parseColorToken(A->getAnnotation());
    if (C != GocColor::None)
      return C;
  }
  return GocColor::None;
}

static GocColor colorFromType(QualType QT) {
  // Attributes on the pointer declarator land on the Decl, not always the type.
  // Still check AttributedType / AnnotatedType if present.
  QualType T = QT;
  while (const auto *AT = T->getAs<AttributedType>()) {
    // Fall through — color mainly on Decl.
    T = AT->getModifiedType();
  }
  (void)T;
  return GocColor::None;
}

static GocColor colorOfExpr(const Expr *E) {
  if (!E)
    return GocColor::None;
  E = E->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return colorFromDecl(DRE->getDecl());

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      // &local → provenance stack → treat as sptr for escape checks
      if (const auto *DRE = dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts())) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
          if (VD->hasLocalStorage() && !VD->hasAttr<AnnotateAttr>())
            return GocColor::SPtr; // bare &stack → sptr provenance
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

  return colorFromType(E->getType());
}

/// True if the destination of a store is non-stack (global / static / heap field).
static bool destIsNonStack(const Expr *LHS) {
  if (!LHS)
    return false;
  LHS = LHS->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (VD->hasGlobalStorage() || VD->getTLSKind() != VarDecl::TLS_None)
        return true;
      // Local slot: stack OK
      return false;
    }
  }

  if (const auto *ME = dyn_cast<MemberExpr>(LHS)) {
    // struct field store: if base is global or through pointer, treat as non-stack
    const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (VD->hasGlobalStorage())
          return true;
        // local struct field — stack OK for the field itself
        return false;
      }
    }
    // x->field or (*p).field through pointer → heap/global-ish
    if (ME->isArrow() || isa<UnaryOperator>(Base))
      return true;
    return destIsNonStack(Base);
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(LHS)) {
    return destIsNonStack(ASE->getBase());
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(LHS)) {
    if (UO->getOpcode() == UO_Deref)
      return true; // *p = … — pointee storage; conservatively non-stack
  }

  return false;
}

static GocColor colorOfLValue(const Expr *LHS) {
  if (!LHS)
    return GocColor::None;
  LHS = LHS->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS))
    return colorFromDecl(DRE->getDecl());
  if (const auto *ME = dyn_cast<MemberExpr>(LHS))
    return colorFromDecl(ME->getMemberDecl());
  return GocColor::None;
}

static bool acceptsImplicitUptrStorage(const Expr *LHS) {
  if (!LHS || !LHS->getType()->isPointerType())
    return false;
  GocColor C = colorOfLValue(LHS);
  return C == GocColor::None || C == GocColor::Auto || C == GocColor::CPtr;
}

static bool isSptrValue(GocColor C) { return C == GocColor::SPtr; }

// ---- ParsedAttrInfo for each color ----

template <GocColor C>
struct GocColorAttrInfo : public ParsedAttrInfo {
  GocColorAttrInfo() {
    OptArgs = 0;
    static constexpr Spelling SpellingsCPtr[] = {
        {ParsedAttr::AS_GNU, "goc_cptr"},
        {ParsedAttr::AS_C23, "goc::cptr"},
        {ParsedAttr::AS_CXX11, "goc::cptr"},
    };
    static constexpr Spelling SpellingsSPtr[] = {
        {ParsedAttr::AS_GNU, "goc_sptr"},
        {ParsedAttr::AS_C23, "goc::sptr"},
        {ParsedAttr::AS_CXX11, "goc::sptr"},
    };
    static constexpr Spelling SpellingsUPtr[] = {
        {ParsedAttr::AS_GNU, "goc_uptr"},
        {ParsedAttr::AS_C23, "goc::uptr"},
        {ParsedAttr::AS_CXX11, "goc::uptr"},
    };
    static constexpr Spelling SpellingsAuto[] = {
        {ParsedAttr::AS_GNU, "goc_auto_ptr"},
        {ParsedAttr::AS_C23, "goc::auto_ptr"},
        {ParsedAttr::AS_CXX11, "goc::auto_ptr"},
    };
    static constexpr Spelling SpellingsGPtr[] = {
        {ParsedAttr::AS_GNU, "goc_gptr"},
        {ParsedAttr::AS_C23, "goc::gptr"},
        {ParsedAttr::AS_CXX11, "goc::gptr"},
    };
    switch (C) {
    case GocColor::CPtr: Spellings = SpellingsCPtr; break;
    case GocColor::SPtr: Spellings = SpellingsSPtr; break;
    case GocColor::UPtr: Spellings = SpellingsUPtr; break;
    case GocColor::Auto: Spellings = SpellingsAuto; break;
    case GocColor::GPtr: Spellings = SpellingsGPtr; break;
    case GocColor::None: break;
    }
  }

  bool diagAppertainsToDecl(Sema &S, const ParsedAttr &Attr,
                            const Decl *D) const override {
    if (!isa<VarDecl>(D) && !isa<FieldDecl>(D) && !isa<TypedefNameDecl>(D) &&
        !isa<FunctionDecl>(D) && !isa<ParmVarDecl>(D)) {
      unsigned ID = S.getDiagnostics().getCustomDiagID(
          DiagnosticsEngine::Warning,
          "goc color attribute ignored on this declaration");
      S.Diag(Attr.getLoc(), ID);
      return false;
    }
    return true;
  }

  AttrHandling handleDeclAttribute(Sema &S, Decl *D,
                                   const ParsedAttr &Attr) const override {
    const char *Ann = colorAnnotate(C);
    if (!Ann)
      return AttributeNotApplied;
    D->addAttr(AnnotateAttr::Create(S.Context, Ann, nullptr, 0, Attr.getRange()));
    return AttributeApplied;
  }
};

// ---- Sema / AST escape visitor ----

class GocEscapeVisitor : public RecursiveASTVisitor<GocEscapeVisitor> {
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  unsigned DiagSptrStore;
  unsigned DiagSptrReturn;
  unsigned DiagNoteColor;
  unsigned ErrorCount = 0;

public:
  explicit GocEscapeVisitor(ASTContext &Ctx)
      : Ctx(Ctx), Diags(Ctx.getDiagnostics()) {
    DiagSptrStore = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "goc: sptr escape — destination is not a cptr/auto T* storage eligible "
        "for implicit uptr encoding (%0)");
    DiagSptrReturn = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "goc: sptr escape — returning stack pointer (%0) from function");
    DiagNoteColor = Diags.getCustomDiagID(
        DiagnosticsEngine::Note, "goc: value color here is '%0'");
  }

  unsigned errors() const { return ErrorCount; }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO->isAssignmentOp())
      return true;
    GocColor RHS = colorOfExpr(BO->getRHS());
    if (!isSptrValue(RHS))
      return true;
    bool NonStack = destIsNonStack(BO->getLHS());
    GocColor DestColor = colorOfLValue(BO->getLHS());
    if (acceptsImplicitUptrStorage(BO->getLHS()) &&
        (NonStack || DestColor == GocColor::CPtr))
      return true; // the IR pass encodes this store and decodes T* loads
    if (!NonStack)
      return true;
    Diags.Report(BO->getExprLoc(), DiagSptrStore) << colorName(RHS);
    Diags.Report(BO->getRHS()->getExprLoc(), DiagNoteColor) << colorName(RHS);
    ++ErrorCount;
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    // global/static init from sptr
    if (!VD->hasInit())
      return true;
    if (!VD->hasGlobalStorage())
      return true;
    GocColor RHS = colorOfExpr(VD->getInit());
    if (!isSptrValue(RHS))
      return true;
    Diags.Report(VD->getLocation(), DiagSptrStore) << colorName(RHS);
    ++ErrorCount;
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
    ++ErrorCount;
    return true;
  }
};

class GocSemaConsumer : public ASTConsumer {
  bool Verbose;
public:
  explicit GocSemaConsumer(bool Verbose) : Verbose(Verbose) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    GocEscapeVisitor V(Context);
    V.TraverseDecl(Context.getTranslationUnitDecl());
    if (Verbose) {
      llvm::errs() << "goc-clang: Sema escape scan done; errors=" << V.errors()
                   << "\n";
    }
    // Errors already emitted via DiagnosticsEngine; clang will fail the compile
    // if -Werror or if we set them as Error (we did).
  }
};

class GocClangAction : public PluginASTAction {
  bool Verbose = false;

public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    return std::make_unique<GocSemaConsumer>(Verbose);
  }

  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &args) override {
    for (const auto &A : args) {
      if (A == "help") {
        llvm::errs()
            << "goc-clang plugin: color attrs goc_cptr/sptr/uptr/auto_ptr/gptr; "
               "Sema sptr-escape errors\n";
      } else if (A == "verbose") {
        Verbose = true;
      }
    }
    return true;
  }

  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<GocClangAction>
    X("goc-clang", "goc P27: pointer-color attrs + Sema sptr escape");

static ParsedAttrInfoRegistry::Add<GocColorAttrInfo<GocColor::CPtr>>
    A1("goc_cptr", "goc cptr color attribute");
static ParsedAttrInfoRegistry::Add<GocColorAttrInfo<GocColor::SPtr>>
    A2("goc_sptr", "goc sptr color attribute");
static ParsedAttrInfoRegistry::Add<GocColorAttrInfo<GocColor::UPtr>>
    A3("goc_uptr", "goc uptr color attribute");
static ParsedAttrInfoRegistry::Add<GocColorAttrInfo<GocColor::Auto>>
    A4("goc_auto_ptr", "goc auto_ptr color attribute");
static ParsedAttrInfoRegistry::Add<GocColorAttrInfo<GocColor::GPtr>>
    A5("goc_gptr", "goc gptr color attribute");
