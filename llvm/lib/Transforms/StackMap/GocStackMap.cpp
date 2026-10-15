// GocStackMap: IR pass that records sptr-colored values at every call site via
// llvm.experimental.stackmap.
//
// Why: a goroutine stack growth copies frames, and the runtime adjusts the
// slots its stack map marks. goc's coloring marks *values* (!goc.color), not
// slots, so frame roots come from existing pointer allocas and the dedicated
// pointer-typed root allocas created here for SSA values live across calls.
// Make live sptr values reside in pointer-typed frame slots and reload them
// after possible stack growth. The stackmap operands are the *addresses of
// these root slots*, never the raw SSA pointer values: LLVM Direct locations
// then identify precisely which frame words hold pointers. Ordinary Register
// locations are not enough; Go's stack copier cannot adjust a value kept only
// in a C register. The post-PEI MIR also reports these named slots.
//
// Do not report dead SSA values: a reused spill slot may hold a scalar when
// the runtime stops at a later call, and Go would mis-adjust it as a pointer.
//
// The driver runs O3, including the inliner, before this pass. Maps are
// recorded on the merged frame, so an inlined call-bearing callee does not
// need its old offsets retargeted: its allocas and calls are the caller's.
// An inlined copy is no function of its own; goc-llc emits one stack check
// for the surviving caller. Nothing after this pass may inline — a later
// caller cannot retarget frame-word offsets or stackmap IDs already emitted.
// goc-reanchor then spills any alloca-derived address this pass did not
// already root, including a pointer argument that points at a caller's frame.
// O3 keeps that argument in a callee-saved register. The SysV morestack stub
// restores those registers verbatim — they also hold small integers, so the
// stub cannot mark them (0 < p < 4096 is "bad pointer in frame"). The reload
// from an adjusted slot is the copy the copier can see.
//
// O3 also drops !goc.color on instructions it rewrites. A pointer value is
// still a stack address when it is derived from a static alloca (or loaded
// from a slot seeded by one). Those values are roots even without a color tag.
// A heap pointer that merely lives in a stack slot is not a root. Go's
// adjustpointer rewrites a marked slot only when the value is inside the old
// stack, so a heap pointer in a marked slot would be left alone; it is still
// not marked, because a slot that also holds a small integer trips
// "bad pointer in frame". uptr is for an sptr stored where the copier cannot
// see it (the heap), not for a frame slot this map already scans.
//
// Spill slots share again (llc is not passed -no-stack-slot-sharing). Root
// allocas are not spill slots, and goc-reanchor drops their lifetime markers
// so StackColoring cannot merge them with a scalar. goc-pin-i64 stays out of
// the default pipeline: pinning every i64 half was 137 slots in
// JS_CallInternal. A short-lived spill of g used to share a colored slot with
// an i64 half (js_regexp_Symbol_replace). g is now one hoisted value, so its
// home is a callee-saved spill, which StackSlotColoring does not recolor.
// Stack-derived integers stay with goc-reanchor so growth can still adjust them.
#include <algorithm>
//
// The passes are split across files; Goc/Internal/StackMap.h has the shared helpers:
//   ../../Analysis/StackMap/SafepointAnalysis.cpp  safepoints, stack and
//                     frame addresses, uses across safepoints (read-only)
//   SMRoots.cpp       STACKMAP roots, TLS g hoist, frame-address remat
//   SMStackMap.cpp    goc-stackmap
//   SMReanchor.cpp    goc-reanchor
//   SMPinI64.cpp      goc-pin-i64
//   SMUptrInline.cpp  goc-inline-gate (inlined uptr helpers), goc-name-blocks

#include "Goc/Internal/StackMap.h"
#include "Goc/StackMap.h"

using namespace llvm;

bool goc::parseStackMapPass(StringRef Name, FunctionPassManager &FPM,
                            ArrayRef<PassBuilder::PipelineElement>) {
  if (Name == "goc-stackmap") {
    goc::sm::addGocStackMap(FPM);
    return true;
  }
  if (Name == "goc-reanchor") {
    goc::sm::addGocReanchor(FPM);
    return true;
  }
  if (Name == "goc-pin-i64") {
    goc::sm::addGocPinI64(FPM);
    return true;
  }
  if (Name == "goc-name-blocks") {
    goc::sm::addGocNameBlocks(FPM);
    return true;
  }
  if (Name == "goc-inline-gate") {
    goc::sm::addGocInlineGate(FPM);
    return true;
  }
  return false;
}
