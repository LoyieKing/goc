//===- Goc/Internal/ColorEscape.h - shared state of the color passes -===//

#ifndef GOC_COLORESCAPE_INTERNAL_H
#define GOC_COLORESCAPE_INTERNAL_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>
#include "Goc/ColorEscape.h"

namespace goc::ce {
using namespace llvm;

extern cl::opt<bool> Verbose;
extern cl::opt<std::string> DefaultPtrColor;

enum class Color : uint8_t {
  None = 0,
  Auto,
  CPtr,
  SPtr,
  UPtr,
  GPtr,
};

struct PtrInfo {
  Color Declared = Color::None; // from annotate, if any
  Color Refined = Color::Auto;  // after analysis
  bool StackProv = false;       // derived from alloca / stack
  bool HeapProv = false;        // malloc / global / goc_alloc
  bool GoHeapProv = false;      // gptr track
  bool EncodedUPtr = false;     // through goc_uptr_from_*
  bool DynamicUPtr = false;     // decoded cptr|sptr value; may be stack-backed
  bool Escapes = false;         // stored to non-stack or returned
};

struct Diagnostic {
  std::string Msg;
  Instruction *At = nullptr;
};

struct ColorEscapeState {
  DenseMap<Value *, PtrInfo> Info;
  DenseMap<Value *, Color> SlotColor; // alloca/global/ptr.annotation slot → color
  DenseSet<Value *> DefaultColoredSlots;
  DenseSet<const Argument *> StackOnlyFormals;
  DenseSet<Value *> AutoUptrSlots;
  DenseMap<Value *, SmallVector<std::string, 2>> AutoUptrRootFields;
  SmallVector<std::string, 8> AutoUptrFields;
  SmallVector<Diagnostic, 8> Diags;
  unsigned ErrorCount = 0;

  void error(Instruction *I, const Twine &T) {
    Diags.push_back({T.str(), I});
    ++ErrorCount;
    errs() << "goc-color-escape: error: " << T << "\n";
    if (I) {
      errs() << "  at: ";
      I->print(errs());
      errs() << "\n";
      if (I->getFunction())
        errs() << "  in function '" << I->getFunction()->getName() << "'\n";
    }
  }

  void note(const Twine &T) {
    if (Verbose)
      errs() << "goc-color-escape: note: " << T << "\n";
  }

  PtrInfo &info(Value *V) { return Info[V]; }

  Color effective(Value *V) {
    auto It = Info.find(V);
    if (It == Info.end())
      return Color::Auto;
    if (It->second.Declared != Color::None && It->second.Declared != Color::Auto)
      return It->second.Declared;
    return It->second.Refined;
  }
};

struct ReportAnn {
  Color C = Color::None;
  std::string File;
  unsigned Line = 0;
};

struct ReportRow {
  std::string Kind;
  std::string Name;
  std::string Loc;
  std::string File;
  unsigned Line = 0;
  unsigned Col = 0;
  Color C = Color::Auto;
  const char *Source = "inferred";
  const char *Spelling = "T*";
};

const char *colorName(Color C);
Color parseColorToken(StringRef S);
StringRef annotationString(Value *V);
bool isAnnotationIntrinsic(Function *F);
bool isUptrFromSptr(StringRef N);
bool isUptrFromPtr(StringRef N) ;
bool isUptrDecode(StringRef N) ;
bool isUptrFromCptr(StringRef N);
bool isUptrAsSptr(StringRef N);
bool isUptrAsCptr(StringRef N);
bool isGocAlloc(StringRef N);
bool isGptrFrom(StringRef N) ;
bool isUptrRuntimeReturn(StringRef N);
Value *stripAC(Value *V);
bool isStackLocation(Value *Dest);
bool isGlobalLocation(Value *Dest);
void seedAnnotations(Module &M, ColorEscapeState &S);
void seedProducers(Module &M, ColorEscapeState &S);
Color joinColor(Color A, Color B);
void applyColorProv(PtrInfo &PI, Color C);
void propagate(Function &F, ColorEscapeState &S);
bool isStackDest(Value *Dest, ColorEscapeState &S);
bool isEscapingDest(Value *Dest, ColorEscapeState &S);
Color pointerSlotColor(Value *Address, ColorEscapeState &S);
SmallVector<Value *, 4> pointerStorageRoots(Value *Address);
const GEPOperator *pointerFieldGEP(Value *Address);
std::string pointerFieldKey(Value *Address);
void markAutoUptrSlot(Value *Address, ColorEscapeState &S);
bool isAutoUptrSlot(Value *Address, ColorEscapeState &S);
bool addressMayEscape(Value *Address);
FunctionCallee getUnaryPtrHelper(Module &M, StringRef Name);
bool mayDirectlyNameStack(Value *V, DenseSet<Value *> &Seen);
unsigned encodeEscapingSptrStores(Module &M, ColorEscapeState &S);
unsigned decodeAutoUptrLoads(Module &M, ColorEscapeState &S);
unsigned guardDynamicUptrReturns(Module &M, ColorEscapeState &S);
bool borrowedFromStackFormal(Value *V, ColorEscapeState &S,
                                    DenseSet<Value *> &Seen);
void checkEscapesAndMix(Function &F, ColorEscapeState &S);
void attachMetadata(Module &M, ColorEscapeState &S);
bool provenStackActual(Value *Actual,
                              const DenseSet<const Argument *> &Candidates);
void proveStackOnlyFormals(Module &M, ColorEscapeState &S);
void applyDefaultPtrColor(Module &M, ColorEscapeState &S);
unsigned lowerDynamicAllocas(Module &M);
const char *spellingOf(Color C);
const PtrInfo *findInfo(ColorEscapeState &S, Value *V);
std::string filePath(const DIFile *File);
void setLoc(ReportRow &R, const DIFile *File, unsigned Line, unsigned Col);
DIType *peelQual(DIType *T);
bool isObjectPointerType(DIType *T);
const DISubprogram *enclosingFn(const DIScope *S);
std::string fieldKeyOf(StructType *ST, unsigned Elem);
std::string addressFieldKey(Value *Address);
Color valueColor(Value *V, ColorEscapeState &S);
void noteFieldStore(std::map<std::string, Color> &Seen, StringRef Key,
                           Color C);
ReportAnn mergedAnn(ReportAnn A, ReportAnn B);
bool stillDefault(Value *Slot, ColorEscapeState &S);
void resolveSlot(Value *Slot, Color Ann, ColorEscapeState &S, Color &Out,
                        const char *&Source, const char *&Spelling);
void resolveField(Color Ann, bool Promoted, Color SeenStore, Color &Out,
                         const char *&Source, const char *&Spelling);
void addRow(std::vector<ReportRow> &Rows, std::set<std::string> &Seen,
                   ReportRow R);
void emitColorReport(Module &M, ColorEscapeState &S, raw_ostream &OS);

} // namespace goc::ce

#endif
