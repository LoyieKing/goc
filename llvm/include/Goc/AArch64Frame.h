//===- AArch64Frame.h - Go-compatible arm64 frames -------------*- C++ -*-===//
//
// Go's arm64 unwinder reads a frame's return address at 0(RSP) (each frame
// saves its own LR at the bottom). LLVM keeps LR in the frame record at the
// top of the frame. These two passes give every frame that makes calls a
// 16-byte area at the bottom and copy LR there:
//
//   * GocA64CallFrame (before PEI) grows every call frame by 16 bytes and
//     moves the outgoing stack arguments up by 16, so [sp, sp+16) stays free
//     at every call; incoming stack arguments are read 16 bytes higher to
//     match. Every arm64 C body and Go entry thunk follows this.
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
llvm::FunctionPass *createA64FrameAddrFixPass();
} // namespace goc

#endif
