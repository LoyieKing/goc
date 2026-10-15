//===- Goc/ColorEscape.h - pointer-color escape transform ------*- C++ -*-===//
//
// Refines pointer colors (cptr/sptr/uptr/gptr) over a module, reports color
// errors, inserts uptr encode/decode and attaches !goc.color metadata.
// Exposed by libGocPasses.so as "goc-color-escape" and "print<goc-color>".
//
//===----------------------------------------------------------------------===//
#ifndef GOC_COLORESCAPE_H
#define GOC_COLORESCAPE_H

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace goc {
/// Runs the color/escape transform on M. Returns the number of color errors.
/// With Report, prints the resolved-color table there instead of the summary
/// line (print<goc-color> runs this on a clone of the module).
int runColorEscape(llvm::Module &M, llvm::raw_ostream *Report = nullptr);
} // namespace goc

#endif
