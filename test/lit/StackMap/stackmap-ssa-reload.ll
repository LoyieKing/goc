; goc-stackmap saves an sptr value to its root once and reads the SSA value
; until a call it is live across; only after that call does it reload the
; root (the copier adjusts the slot, not the register). A value returned by
; a call (here a stack address) is saved after that call, and the call
; itself is not followed by a reload of the not-yet-written root.
; RUN: %opt-goc -passes=goc-stackmap -S %s -o - | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

declare void @g()
declare ptr @pick(ptr)

define i64 @f(i1 %c) {
entry:
  %buf = alloca [4 x i64], align 16
  %p = call ptr @pick(ptr %buf)
  %a = load i64, ptr %p, align 8
  call void @g()
  %b = load i64, ptr %p, align 8
  %s = add i64 %a, %b
  ret i64 %s
}

; CHECK-LABEL: define i64 @f(
; CHECK:       %p = call ptr @pick(ptr %buf)
; CHECK-NEXT:  store volatile ptr %p, ptr %goc.spill.root
; CHECK-NEXT:  %a = load i64, ptr %p
; CHECK:       call void @g()
; CHECK-NEXT:  [[R:%goc.spill.reload.*]] = load volatile ptr, ptr %goc.spill.root
; CHECK-NEXT:  %b = load i64, ptr [[R]]
; CHECK-NOT:   load volatile ptr, ptr %goc.spill.root
; CHECK:       ret i64
