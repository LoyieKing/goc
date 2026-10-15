; goc-llc on arm64: the stack check reads g.stackguard0 from x28, every
; calling frame keeps its LR copy at 0(SP) (where Go's unwinder looks), and
; a frame address kept in a callee-saved register across a call is moved by
; however far the stack was copied: old SP is saved at 8(SP) before the call,
; and v in [old SP, old g.stack.hi) gets the delta added afterwards.
; RUN: %goc-llc -O2 -relocation-model=static -mtriple=aarch64-unknown-linux-gnu \
; RUN:   -mattr=+reserve-x28 -frame-pointer=all -disable-tail-calls \
; RUN:   -function-sections -goc-profile=go1.24-arm64 -goc-tables %t.tables.s %s -o - \
; RUN:   | FileCheck %s
; RUN: FileCheck --check-prefix=TABLES %s < %t.tables.s

target triple = "aarch64-unknown-linux-gnu"

declare void @fill(ptr)

define i32 @use_after() {
entry:
  %buf = alloca [4 x i32], align 16
  call void @fill(ptr %buf)
  %p1 = getelementptr inbounds i8, ptr %buf, i64 4
  call void @fill(ptr %p1)
  %a = load i32, ptr %buf, align 16
  ret i32 %a
}

; CHECK-LABEL: use_after:
; CHECK:       ldr x16, [x28, #16]
; CHECK-NEXT:  cmp sp, x16
; CHECK-NEXT:  b.ls
; CHECK:       stp x29, x30, [sp, #[[FP:[0-9]+]]]
; CHECK:       str x30, [sp]
; CHECK:       mov [[OLD:x[0-9]+]], sp
; CHECK-NEXT:  str [[OLD]], [sp, #8]
; CHECK-NEXT:  .Lgoc_call0:
; CHECK-NEXT:  bl fill
; CHECK-NEXT:  .Lgoc_ret0:
; CHECK-NEXT:  ldr [[OSP:x[0-9]+]], [sp, #8]
; CHECK-NEXT:  mov [[D:x[0-9]+]], sp
; CHECK-NEXT:  sub [[D]], [[D]], [[OSP]]
; CHECK-NEXT:  ldr [[HI:x[0-9]+]], [x28, #8]
; CHECK-NEXT:  sub [[HI]], [[HI]], [[D]]
; CHECK:       cmp [[R:x(19|2[0-7])]], [[OSP]]
; CHECK-NEXT:  ccmp [[R]], [[HI]], #2, hs
; CHECK-NEXT:  csel [[R]], {{x[0-9]+}}, [[R]], lo
; CHECK:       bl fill

; TABLES: .asciz "go1.24-arm64"
