//===- ColorPrinter.cpp - print<goc-color>: the resolved-color table ------===//
//
// One row per source-level pointer (field, global, param, return, local):
// its resolved color, whether it was explicit or inferred, and the spelling
// in the source. Debug info (-g) supplies names and lines.

#include "Goc/Internal/ColorEscape.h"

using namespace llvm;

namespace goc::ce {

const char *spellingOf(Color C) {
  switch (C) {
  case Color::Auto:
    return "auto_ptr";
  case Color::CPtr:
    return "cptr";
  case Color::SPtr:
    return "sptr";
  case Color::UPtr:
    return "uptr";
  case Color::GPtr:
    return "gptr";
  case Color::None:
    return "T*";
  }
  return "T*";
}

const PtrInfo *findInfo(ColorEscapeState &S, Value *V) {
  auto It = S.Info.find(V);
  return It == S.Info.end() ? nullptr : &It->second;
}

std::string filePath(const DIFile *File) {
  if (!File)
    return {};
  StringRef Name = File->getFilename();
  StringRef Dir = File->getDirectory();
  if (Name.empty())
    return {};
  if (Name.starts_with("/") || Dir.empty())
    return Name.str();
  SmallString<256> P(Dir);
  sys::path::append(P, Name);
  return std::string(P);
}

void setLoc(ReportRow &R, const DIFile *File, unsigned Line, unsigned Col) {
  R.File = filePath(File);
  R.Line = Line;
  R.Col = Col;
  if (R.File.empty() || !Line) {
    R.Loc = "-";
    return;
  }
  R.Loc = R.File + ":" + std::to_string(Line);
  if (Col)
    R.Loc += ":" + std::to_string(Col);
}

DIType *peelQual(DIType *T) {
  while (auto *D = dyn_cast_or_null<DIDerivedType>(T)) {
    switch (D->getTag()) {
    case dwarf::DW_TAG_typedef:
    case dwarf::DW_TAG_const_type:
    case dwarf::DW_TAG_volatile_type:
    case dwarf::DW_TAG_restrict_type:
    case dwarf::DW_TAG_atomic_type:
      T = D->getBaseType();
      break;
    default:
      return T;
    }
  }
  return T;
}

bool isObjectPointerType(DIType *T) {
  T = peelQual(T);
  auto *P = dyn_cast_or_null<DIDerivedType>(T);
  return P && P->getTag() == dwarf::DW_TAG_pointer_type;
}

const DISubprogram *enclosingFn(const DIScope *S) {
  while (S) {
    if (auto *SP = dyn_cast<DISubprogram>(S))
      return SP;
    auto *LS = dyn_cast<DILocalScope>(S);
    if (!LS)
      return nullptr;
    S = LS->getScope();
  }
  return nullptr;
}

std::string fieldKeyOf(StructType *ST, unsigned Elem) {
  std::string Key;
  raw_string_ostream OS(Key);
  ST->print(OS);
  OS << ":0." << Elem << ".";
  OS.flush();
  return Key;
}

// A zero-offset field store is often just the struct address. Clang drops
// the GEP, so the field key is empty and the pass records the object in
// AutoUptrSlots instead of AutoUptrFields.
std::string addressFieldKey(Value *Address) {
  std::string Key = pointerFieldKey(Address);
  if (!Key.empty())
    return Key;
  if (pointerFieldGEP(Address))
    return {};
  Value *Base = Address;
  if (auto *II = dyn_cast<IntrinsicInst>(Address))
    if (II->getIntrinsicID() == Intrinsic::ptr_annotation)
      Base = II->getArgOperand(0);
  Base = Base->stripPointerCasts();
  Type *Ty = nullptr;
  if (auto *G = dyn_cast<GlobalVariable>(Base))
    Ty = G->getValueType();
  else if (auto *AI = dyn_cast<AllocaInst>(Base))
    Ty = AI->getAllocatedType();
  else
    return {};
  auto *ST = dyn_cast<StructType>(Ty);
  if (!ST || ST->isOpaque() || ST->getNumElements() == 0 ||
      !ST->getElementType(0)->isPointerTy())
    return {};
  return fieldKeyOf(ST, 0);
}

Color valueColor(Value *V, ColorEscapeState &S) {
  if (!V)
    return Color::Auto;
  V = V->stripPointerCasts();
  if (isa<ConstantPointerNull>(V) || isa<UndefValue>(V))
    return Color::Auto;
  if (isa<AllocaInst>(V))
    return Color::SPtr;
  if (isa<Function>(V) || isa<GlobalVariable>(V))
    return Color::CPtr;
  const PtrInfo *PI = findInfo(S, V);
  if (!PI)
    return S.effective(V) == Color::None ? Color::Auto : S.effective(V);
  if (PI->EncodedUPtr || PI->Declared == Color::UPtr || PI->Refined == Color::UPtr)
    return Color::UPtr;
  if (PI->GoHeapProv || PI->Declared == Color::GPtr || PI->Refined == Color::GPtr)
    return Color::GPtr;
  if (PI->Declared == Color::SPtr || PI->Refined == Color::SPtr ||
      (PI->StackProv && !PI->HeapProv && !PI->GoHeapProv && !PI->EncodedUPtr))
    return Color::SPtr;
  if (PI->Declared == Color::CPtr || PI->Refined == Color::CPtr || PI->HeapProv)
    return Color::CPtr;
  Color E = S.effective(V);
  return E == Color::None ? Color::Auto : E;
}

void noteFieldStore(std::map<std::string, Color> &Seen, StringRef Key,
                           Color C) {
  if (Key.empty())
    return;
  auto It = Seen.find(Key.str());
  if (It == Seen.end())
    Seen.emplace(Key.str(), C);
  else if (It->second != C)
    It->second = Color::None; // mixed stores: do not invent one color
}

ReportAnn mergedAnn(ReportAnn A, ReportAnn B) {
  if (A.C == Color::None)
    return B;
  if (B.C == Color::None)
    return A;
  if (A.C == Color::Auto)
    return B;
  return A;
}

bool stillDefault(Value *Slot, ColorEscapeState &S) {
  Color Def = parseColorToken(DefaultPtrColor);
  if (Def == Color::None || Def == Color::Auto)
    return false;
  if (!S.DefaultColoredSlots.count(Slot))
    return false;
  const PtrInfo *PI = findInfo(S, Slot);
  return PI && PI->Declared == Def;
}

// Final color of a pointer slot. Explicit annotations win unless analysis
// rewrote the storage as uptr. A default that analysis put back to auto
// (stack slot holding an sptr) is reported as the refined color.
void resolveSlot(Value *Slot, Color Ann, ColorEscapeState &S, Color &Out,
                        const char *&Source, const char *&Spelling) {
  Spelling = spellingOf(Ann);
  bool Promoted = Slot && S.AutoUptrSlots.count(Slot);
  if (Promoted && Ann != Color::SPtr && Ann != Color::UPtr &&
      Ann != Color::GPtr) {
    Out = Color::UPtr;
    Source = "inferred";
    return;
  }
  if (Ann == Color::CPtr || Ann == Color::SPtr || Ann == Color::UPtr ||
      Ann == Color::GPtr) {
    Out = Ann;
    Source = "explicit";
    return;
  }
  if (Slot && stillDefault(Slot, S)) {
    Out = parseColorToken(DefaultPtrColor);
    Source = "default-ptr-color";
    Spelling = "T*";
    return;
  }
  Color Eff = Color::Auto;
  if (Slot) {
    if (const PtrInfo *PI = findInfo(S, Slot)) {
      if (PI->Refined != Color::None && PI->Refined != Color::Auto)
        Eff = PI->Refined;
      else {
        Color E = S.effective(Slot);
        if (E != Color::None)
          Eff = E;
      }
    }
  }
  Out = Eff;
  Source = "inferred";
  if (Ann == Color::Auto && Out == Color::Auto)
    Source = "explicit";
}

void resolveField(Color Ann, bool Promoted, Color SeenStore, Color &Out,
                         const char *&Source, const char *&Spelling) {
  Spelling = spellingOf(Ann);
  if (Promoted && Ann != Color::SPtr && Ann != Color::UPtr &&
      Ann != Color::GPtr) {
    Out = Color::UPtr;
    Source = "inferred";
    return;
  }
  if (Ann == Color::CPtr || Ann == Color::SPtr || Ann == Color::UPtr ||
      Ann == Color::GPtr) {
    Out = Ann;
    Source = "explicit";
    return;
  }
  if (SeenStore == Color::SPtr) {
    Out = Color::SPtr;
    Source = "inferred";
    return;
  }
  Out = Color::Auto;
  Source = Ann == Color::Auto ? "explicit" : "inferred";
}

void addRow(std::vector<ReportRow> &Rows, std::set<std::string> &Seen,
                   ReportRow R) {
  std::string Id = R.Kind + "\t" + R.Name + "\t" + R.Loc;
  if (!Seen.insert(Id).second)
    return;
  Rows.push_back(std::move(R));
}

void emitColorReport(Module &M, ColorEscapeState &S, raw_ostream &OS) {
  DenseMap<Value *, ReportAnn> ValueAnn;
  std::map<std::string, ReportAnn> FieldAnn;
  auto readAnn = [](Value *StrV, Value *FileV, Value *LineV) {
    ReportAnn A;
    A.C = parseColorToken(annotationString(StrV));
    A.File = annotationString(FileV).str();
    if (auto *CI = dyn_cast_or_null<ConstantInt>(LineV))
      A.Line = (unsigned)CI->getZExtValue();
    return A;
  };

  if (GlobalVariable *GA = M.getGlobalVariable("llvm.global.annotations")) {
    if (auto *Init = dyn_cast<ConstantArray>(GA->getInitializer())) {
      for (unsigned i = 0, e = Init->getNumOperands(); i != e; ++i) {
        auto *Rec = dyn_cast<ConstantStruct>(Init->getOperand(i));
        if (!Rec || Rec->getNumOperands() < 4)
          continue;
        Value *Target = Rec->getOperand(0)->stripPointerCasts();
        ReportAnn A =
            readAnn(Rec->getOperand(1), Rec->getOperand(2), Rec->getOperand(3));
        if (A.C == Color::None)
          continue;
        ValueAnn[Target] = mergedAnn(ValueAnn.lookup(Target), A);
      }
    }
  }

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *Callee = CB->getCalledFunction();
      if (!isAnnotationIntrinsic(Callee) || CB->arg_size() < 4)
        continue;
      ReportAnn A =
          readAnn(CB->getArgOperand(1), CB->getArgOperand(2), CB->getArgOperand(3));
      if (A.C == Color::None)
        continue;
      if (Callee->getIntrinsicID() == Intrinsic::ptr_annotation) {
        std::string Key = addressFieldKey(CB->getArgOperand(0));
        if (!Key.empty()) {
          FieldAnn[Key] = mergedAnn(FieldAnn[Key], A);
          continue;
        }
      }
      Value *Target = CB->getArgOperand(0)->stripPointerCasts();
      ValueAnn[Target] = mergedAnn(ValueAnn.lookup(Target), A);
    }
  }

  std::set<std::string> Promoted(S.AutoUptrFields.begin(), S.AutoUptrFields.end());
  for (Value *V : S.AutoUptrSlots) {
    Type *Ty = nullptr;
    if (auto *G = dyn_cast<GlobalVariable>(V))
      Ty = G->getValueType();
    else if (auto *AI = dyn_cast<AllocaInst>(V))
      Ty = AI->getAllocatedType();
    auto *ST = dyn_cast_or_null<StructType>(Ty);
    if (!ST || ST->isOpaque() || ST->getNumElements() == 0 ||
        !ST->getElementType(0)->isPointerTy())
      continue;
    Promoted.insert(fieldKeyOf(ST, 0));
  }

  std::map<std::string, Color> FieldStores;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || !SI->getValueOperand()->getType()->isPointerTy())
        continue;
      noteFieldStore(FieldStores, addressFieldKey(SI->getPointerOperand()),
                     valueColor(SI->getValueOperand(), S));
    }
  }

  std::vector<ReportRow> Rows;
  std::set<std::string> SeenRows;
  DebugInfoFinder Finder;
  Finder.processModule(M);
  bool HaveDI = Finder.compile_unit_count() > 0;

  struct LocalSite {
    Value *Addr = nullptr;
    unsigned Col = 0;
  };
  DenseMap<const DILocalVariable *, LocalSite> LocalAddr;
  if (HaveDI) {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (Instruction &I : instructions(F)) {
        for (DbgRecord &DR : I.getDbgRecordRange()) {
          auto *DVR = dyn_cast<DbgVariableRecord>(&DR);
          if (!DVR || !DVR->isDbgDeclare() || !DVR->getVariable())
            continue;
          if (DVR->getNumVariableLocationOps() < 1)
            continue;
          Value *Addr = DVR->getVariableLocationOp(0);
          if (!Addr)
            continue;
          LocalSite Site;
          Site.Addr = Addr->stripPointerCasts();
          Site.Col = DVR->getDebugLoc().getCol();
          LocalAddr[DVR->getVariable()] = Site;
        }
      }
    }
  }

  auto annOf = [&](Value *V) -> Color {
    if (!V)
      return Color::None;
    auto It = ValueAnn.find(V);
    return It == ValueAnn.end() ? Color::None : It->second.C;
  };

  if (HaveDI) {
    std::set<const StructType *> UsedStructs;
    for (DIType *Ty : Finder.types()) {
      auto *CT = dyn_cast<DICompositeType>(Ty);
      if (!CT || CT->getTag() != dwarf::DW_TAG_structure_type || CT->isForwardDecl())
        continue;
      if (CT->getElements().empty())
        continue;
      std::string TypeName = CT->getName().str();
      StructType *ST = nullptr;
      if (!TypeName.empty()) {
        ST = StructType::getTypeByName(M.getContext(), "struct." + TypeName);
        if (!ST) {
          std::string Prefix = "struct." + TypeName + ".";
          for (StructType *Cand : M.getIdentifiedStructTypes()) {
            if (Cand->getName().starts_with(Prefix) && !Cand->isOpaque()) {
              ST = Cand;
              break;
            }
          }
        }
      } else {
        uint64_t Size = CT->getSizeInBits() / 8;
        StructType *Found = nullptr;
        bool Unique = true;
        for (StructType *Cand : M.getIdentifiedStructTypes()) {
          if (UsedStructs.count(Cand) || Cand->isOpaque() ||
              !Cand->getName().starts_with("struct.anon"))
            continue;
          if (M.getDataLayout().getTypeAllocSize(Cand) != Size)
            continue;
          if (Found)
            Unique = false;
          Found = Cand;
        }
        if (Unique)
          ST = Found;
      }
      if (ST)
        UsedStructs.insert(ST);
      unsigned MemberIndex = 0;
      for (DINode *Elt : CT->getElements()) {
        auto *Mem = dyn_cast<DIDerivedType>(Elt);
        if (!Mem || Mem->getTag() != dwarf::DW_TAG_member)
          continue;
        unsigned ThisIndex = MemberIndex++;
        if (!isObjectPointerType(Mem->getBaseType()))
          continue;
        std::string FName = Mem->getName().str();
        if (FName.empty())
          FName = "#" + std::to_string(ThisIndex);
        std::string StructLabel = TypeName.empty() ? "anon" : TypeName;
        ReportRow R;
        R.Kind = "field";
        R.Name = StructLabel + "." + FName;
        setLoc(R, Mem->getFile(), Mem->getLine(), 0);
        Color Ann = Color::None;
        bool Prom = false;
        Color SeenStore = Color::Auto;
        bool SawStore = false;
        if (ST && !ST->isOpaque()) {
          unsigned Elem = (unsigned)-1;
          const StructLayout *SL = M.getDataLayout().getStructLayout(ST);
          uint64_t Off = Mem->getOffsetInBits() / 8;
          for (unsigned i = 0, e = ST->getNumElements(); i != e; ++i) {
            if (SL->getElementOffset(i) == Off &&
                ST->getElementType(i)->isPointerTy()) {
              Elem = i;
              break;
            }
          }
          if (Elem != (unsigned)-1) {
            std::string Key = fieldKeyOf(ST, Elem);
            auto AIt = FieldAnn.find(Key);
            if (AIt != FieldAnn.end())
              Ann = AIt->second.C;
            Prom = Promoted.count(Key) != 0;
            auto SIt = FieldStores.find(Key);
            if (SIt != FieldStores.end()) {
              SawStore = true;
              SeenStore = SIt->second;
            }
          }
        }
        if (!SawStore)
          SeenStore = Color::Auto;
        // A missing store must not look like a unanimous auto store.
        Color StoreForResolve = SawStore ? SeenStore : Color::None;
        resolveField(Ann, Prom, StoreForResolve, R.C, R.Source, R.Spelling);
        addRow(Rows, SeenRows, std::move(R));
      }
    }

    for (DISubprogram *SP : Finder.subprograms()) {
      if (!SP->isDefinition())
        continue;
      Function *F = M.getFunction(SP->getName());
      if (!F || F->isDeclaration())
        continue;
      DIType *RetTy = nullptr;
      if (auto *SR = SP->getType()) {
        DITypeRefArray Ts = SR->getTypeArray();
        if (Ts.size() > 0)
          RetTy = Ts[0];
      }
      if (isObjectPointerType(RetTy)) {
        ReportRow R;
        R.Kind = "return";
        R.Name = SP->getName().str();
        setLoc(R, SP->getFile(), SP->getLine(), 0);
        Color Ann = annOf(F);
        if (Ann == Color::CPtr || Ann == Color::SPtr || Ann == Color::UPtr ||
            Ann == Color::GPtr) {
          R.C = Ann;
          R.Source = "explicit";
          R.Spelling = spellingOf(Ann);
        } else {
          Color Join = Color::None;
          bool Any = false;
          bool AllDefault = true;
          for (Instruction &I : instructions(*F)) {
            auto *RI = dyn_cast<ReturnInst>(&I);
            if (!RI || !RI->getReturnValue() ||
                !RI->getReturnValue()->getType()->isPointerTy())
              continue;
            Any = true;
            Color C = valueColor(RI->getReturnValue(), S);
            Join = Join == Color::None ? C : (Join == C ? Join : Color::Auto);
            Value *Origin = RI->getReturnValue()->stripPointerCasts();
            if (auto *LI = dyn_cast<LoadInst>(Origin))
              Origin = LI->getPointerOperand()->stripPointerCasts();
            if (!(stillDefault(Origin, S) && C == parseColorToken(DefaultPtrColor)))
              AllDefault = false;
          }
          R.C = Any && Join != Color::None ? Join : Color::Auto;
          R.Spelling = spellingOf(Ann);
          if (Ann == Color::Auto && R.C == Color::Auto)
            R.Source = "explicit";
          else if (Any && AllDefault && R.C == parseColorToken(DefaultPtrColor) &&
                   R.C != Color::Auto && R.C != Color::None)
            R.Source = "default-ptr-color";
          else
            R.Source = "inferred";
        }
        addRow(Rows, SeenRows, std::move(R));
      }
    }

    // Clang 19 leaves subprogram retainedNodes empty and hangs locals off
    // #dbg_declare records. Those records are the declaration list.
    for (const auto &KV : LocalAddr) {
      const DILocalVariable *Var = KV.first;
      Value *Addr = KV.second.Addr;
      unsigned Col = KV.second.Col;
      if (!Var || Var->isArtificial() || !isObjectPointerType(Var->getType()))
        continue;
      const DISubprogram *Owner = enclosingFn(Var->getScope());
      Function *Fn = Owner ? M.getFunction(Owner->getName()) : nullptr;
      if (!Fn)
        if (auto *I = dyn_cast<Instruction>(Addr))
          Fn = I->getFunction();
      std::string FnName = Owner ? Owner->getName().str()
                                 : (Fn ? Fn->getName().str() : "anon");
      std::string VName = Var->getName().str();
      if (VName.empty())
        VName = "#" + std::to_string(Var->getArg());
      Value *Slot = Addr;
      if (Fn && Var->getArg() > 0 && Var->getArg() - 1 < Fn->arg_size())
        Slot = Fn->getArg(Var->getArg() - 1);
      Color Ann = annOf(Addr);
      if (Ann == Color::None)
        Ann = annOf(Slot);
      ReportRow R;
      R.Kind = Var->getArg() > 0 ? "param" : "local";
      R.Name = FnName + "." + VName;
      setLoc(R, Var->getFile(), Var->getLine(), Col);
      resolveSlot(Slot, Ann, S, R.C, R.Source, R.Spelling);
      addRow(Rows, SeenRows, std::move(R));
    }

    DenseMap<const DIGlobalVariable *, GlobalVariable *> GlobalByDI;
    for (GlobalVariable &G : M.globals()) {
      SmallVector<DIGlobalVariableExpression *, 1> Exprs;
      G.getDebugInfo(Exprs);
      for (DIGlobalVariableExpression *E : Exprs)
        if (E->getVariable())
          GlobalByDI[E->getVariable()] = &G;
    }
    for (const DIGlobalVariableExpression *GVE : Finder.global_variables()) {
      const DIGlobalVariable *DG = GVE->getVariable();
      if (!DG || !DG->isDefinition() || !isObjectPointerType(DG->getType()))
        continue;
      GlobalVariable *GV = GlobalByDI.lookup(DG);
      std::string GName = DG->getName().str();
      if (GName.empty())
        GName = GV ? GV->getName().str() : "anon";
      if (auto *SP = dyn_cast_or_null<DISubprogram>(DG->getScope()))
        GName = SP->getName().str() + "." + GName;
      ReportRow R;
      R.Kind = "global";
      R.Name = GName;
      setLoc(R, DG->getFile(), DG->getLine(), 0);
      resolveSlot(GV, annOf(GV), S, R.C, R.Source, R.Spelling);
      addRow(Rows, SeenRows, std::move(R));
    }
  } else {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (Argument &A : F.args()) {
        if (!A.getType()->isPointerTy())
          continue;
        ReportRow R;
        R.Kind = "param";
        std::string Arg = A.getName().str();
        if (Arg.empty())
          Arg = "#" + std::to_string(A.getArgNo());
        R.Name = F.getName().str() + "." + Arg;
        R.Loc = "-";
        resolveSlot(&A, Color::None, S, R.C, R.Source, R.Spelling);
        addRow(Rows, SeenRows, std::move(R));
      }
      if (F.getReturnType()->isPointerTy()) {
        ReportRow R;
        R.Kind = "return";
        R.Name = F.getName().str();
        R.Loc = "-";
        Color Ann = annOf(&F);
        Color Join = Color::None;
        bool Any = false;
        for (Instruction &I : instructions(F)) {
          auto *RI = dyn_cast<ReturnInst>(&I);
          if (!RI || !RI->getReturnValue())
            continue;
          Any = true;
          Color C = valueColor(RI->getReturnValue(), S);
          Join = Join == Color::None ? C : (Join == C ? Join : Color::Auto);
        }
        if (Ann == Color::CPtr || Ann == Color::SPtr || Ann == Color::UPtr ||
            Ann == Color::GPtr) {
          R.C = Ann;
          R.Source = "explicit";
          R.Spelling = spellingOf(Ann);
        } else {
          R.C = Any && Join != Color::None ? Join : Color::Auto;
          R.Source = "inferred";
          R.Spelling = spellingOf(Ann);
        }
        addRow(Rows, SeenRows, std::move(R));
      }
      for (Instruction &I : instructions(F)) {
        auto *AI = dyn_cast<AllocaInst>(&I);
        if (!AI || !AI->getAllocatedType()->isPointerTy())
          continue;
        // The entry spill of a formal is the same declaration as the param row.
        bool FormalSpill = false;
        if (AI->getParent() == &F.getEntryBlock()) {
          for (User *U : AI->users()) {
            auto *SI = dyn_cast<StoreInst>(U);
            if (!SI || SI->getParent() != &F.getEntryBlock())
              continue;
            if (SI->getPointerOperand()->stripPointerCasts() == AI &&
                isa<Argument>(SI->getValueOperand()))
              FormalSpill = true;
          }
        }
        if (FormalSpill)
          continue;
        ReportRow R;
        R.Kind = "local";
        std::string N = AI->getName().str();
        if (N.empty())
          N = "anon";
        R.Name = F.getName().str() + "." + N;
        auto It = ValueAnn.find(AI);
        if (It != ValueAnn.end() && It->second.Line) {
          R.Loc = It->second.File + ":" + std::to_string(It->second.Line);
          R.Line = It->second.Line;
        } else
          R.Loc = "-";
        resolveSlot(AI, annOf(AI), S, R.C, R.Source, R.Spelling);
        addRow(Rows, SeenRows, std::move(R));
      }
    }
    for (GlobalVariable &G : M.globals()) {
      if (!G.getValueType()->isPointerTy() || G.getName().starts_with("llvm."))
        continue;
      ReportRow R;
      R.Kind = "global";
      R.Name = G.getName().str();
      auto It = ValueAnn.find(&G);
      if (It != ValueAnn.end() && It->second.Line) {
        R.Loc = It->second.File + ":" + std::to_string(It->second.Line);
        R.Line = It->second.Line;
      } else
        R.Loc = "-";
      resolveSlot(&G, annOf(&G), S, R.C, R.Source, R.Spelling);
      addRow(Rows, SeenRows, std::move(R));
    }
    for (StructType *ST : M.getIdentifiedStructTypes()) {
      if (ST->isOpaque() || !ST->getName().starts_with("struct."))
        continue;
      StringRef Nice = ST->getName();
      if (Nice.starts_with("struct."))
        Nice = Nice.drop_front(strlen("struct."));
      for (unsigned i = 0, e = ST->getNumElements(); i != e; ++i) {
        if (!ST->getElementType(i)->isPointerTy())
          continue;
        std::string Key = fieldKeyOf(ST, i);
        ReportRow R;
        R.Kind = "field";
        R.Name = Nice.str() + ".#" + std::to_string(i);
        auto AIt = FieldAnn.find(Key);
        Color Ann = AIt == FieldAnn.end() ? Color::None : AIt->second.C;
        if (AIt != FieldAnn.end() && AIt->second.Line) {
          R.Loc = AIt->second.File + ":" + std::to_string(AIt->second.Line);
          R.Line = AIt->second.Line;
        } else
          R.Loc = "-";
        bool Prom = Promoted.count(Key) != 0;
        Color SeenStore = Color::None;
        auto SIt = FieldStores.find(Key);
        if (SIt != FieldStores.end())
          SeenStore = SIt->second;
        resolveField(Ann, Prom, SeenStore, R.C, R.Source, R.Spelling);
        addRow(Rows, SeenRows, std::move(R));
      }
    }
  }

  std::sort(Rows.begin(), Rows.end(), [](const ReportRow &A, const ReportRow &B) {
    return std::tie(A.File, A.Line, A.Col, A.Kind, A.Name) <
           std::tie(B.File, B.Line, B.Col, B.Kind, B.Name);
  });

  OS << "goc-color-report\tkind\tname\tloc\tcolor\tsource\tspelling\n";
  for (const ReportRow &R : Rows) {
    OS << "goc-color-report\t" << R.Kind << '\t' << R.Name << '\t' << R.Loc
           << '\t' << colorName(R.C) << '\t' << R.Source << '\t' << R.Spelling
           << '\n';
  }
}

} // namespace goc::ce
