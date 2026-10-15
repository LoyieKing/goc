//===- AArch64Frame.h - Go-compatible arm64 frames -------------*- C++ -*-===//
//
// Go's arm64 unwinder reads a frame's return address at 0(RSP) (each frame
// saves its own LR at the bottom). LLVM keeps LR in the frame record at the
// top of the frame. These two passes give every frame that makes calls a
// 16-byte area at the bottom and copy LR there:
//
//   * GocA64CallFrame (before PEI) sizes every call frame pseudo to 16 bytes,
//     so the reserved outgoing-argument area is at least [sp, sp+16). Calls
//     that pass arguments on the stack would share that area and are refused.
//   * GocA64SaveLR (after PEI) stores x30 at [sp] once the prologue has set
//     up the frame.
//
//===----------------------------------------------------------------------===//
#ifndef GOC_AARCH64FRAME_H
#define GOC_AARCH64FRAME_H

namespace llvm {
class FunctionPass;
}

namespace goc {
llvm::FunctionPass *createA64CallFramePass();
llvm::FunctionPass *createA64SaveLRPass();
} // namespace goc

#endif
