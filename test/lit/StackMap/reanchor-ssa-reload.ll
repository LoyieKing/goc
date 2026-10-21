; goc-reanchor keeps a pointer argument in its SSA value on paths without a
; safepoint and reloads its anchor only after a call the argument is live
; across. A loop whose only call is on a cold path reloads there, and the
; hot path reads the PHI, not the stack slot.
; RUN: %opt-goc -passes=goc-reanchor -S %s -o - | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

declare void @slow(i32)

define i64 @sum(ptr %p, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %next ]
  %acc = phi i64 [ 0, %entry ], [ %acc1, %next ]
  %idx = zext i32 %i to i64
  %q = getelementptr inbounds i64, ptr %p, i64 %idx
  %v = load i64, ptr %q, align 8
  %acc1 = add i64 %acc, %v
  %cold = icmp eq i64 %v, 0
  br i1 %cold, label %call, label %next
call:
  call void @slow(i32 %i)
  br label %next
next:
  %i1 = add i32 %i, 1
  %done = icmp eq i32 %i1, %n
  br i1 %done, label %exit, label %loop
exit:
  ret i64 %acc1
}

; CHECK-LABEL: define i64 @sum(
; CHECK:       store volatile ptr %p, ptr %goc.arganchor
; CHECK:       loop:
; CHECK-NEXT:  [[P:%.*]] = phi ptr [ %p, %entry ], [ [[PN:%.*]], %next ]
; CHECK-NOT:   load volatile
; CHECK:       getelementptr inbounds i64, ptr [[P]]
; CHECK:       call:
; CHECK:       call void @slow
; CHECK-NEXT:  [[R:%goc.reanchor.*]] = load volatile ptr, ptr %goc.arganchor
; CHECK:       next:
; CHECK-NEXT:  [[PN]] = phi ptr [ [[R]], %call ], [ [[P]], %loop ]

; An argument whose uses all precede the first call needs no anchor, even
; when the use is outside the entry block.
define i64 @early(ptr %p, i1 %c) {
entry:
  br i1 %c, label %a, label %b
a:
  %v = load i64, ptr %p, align 8
  call void @slow(i32 0)
  ret i64 %v
b:
  ret i64 0
}

; CHECK-LABEL: define i64 @early(
; CHECK-NOT:   goc.arganchor
; CHECK:       ret i64 0
