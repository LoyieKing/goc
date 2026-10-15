//===- Goc/ColorEscape.h - pointer-color escape transform ------*- C++ -*-===//
//
// Refines pointer colors (cptr/sptr/uptr/gptr) over a module, reports color
// errors, inserts uptr encode/decode and attaches !goc.color metadata.
// Used by the goc-color-escape tool and by libGocPasses.so ("goc-color-escape").
//
//===----------------------------------------------------------------------===//
#ifndef GOC_COLORESCAPE_H
#define GOC_COLORESCAPE_H

namespace llvm {
class Module;
}

namespace goc {
/// Runs the color/escape transform on M. Returns the number of color errors.
int runColorEscape(llvm::Module &M);
} // namespace goc

#endif
