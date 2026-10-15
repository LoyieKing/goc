; goc-stackmap records a stack map (ID tagged "GOC") before each call that
; can grow the stack, and recomputes a frame address used after a call from
; its alloca instead of keeping the pre-call value.
; RUN: %opt-goc -passes=goc-stackmap -S %s -o - | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

declare void @fill(ptr)

define i32 @use_after() {
entry:
  %buf = alloca [4 x i32], align 16
  call void @fill(ptr %buf)
  %p1 = getelementptr inbounds i8, ptr %buf, i64 4
  call void @fill(ptr %p1)
  %a = load i32, ptr %buf, align 16
  %b = load i32, ptr %p1, align 4
  %s = add nsw i32 %b, %a
  ret i32 %s
}

; 0x474f430000000001 = "GOC" << 40 | 1
; CHECK-LABEL: define i32 @use_after()
; CHECK:       call void @fill(ptr %buf)
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap(i64 5138399367132086273, i32 0)
; CHECK-NEXT:  call void @fill(ptr %p1)
; CHECK:       [[FA:%goc.fa[0-9]*]] = getelementptr inbounds i8, ptr %buf, i64 4
; CHECK:       load i32, ptr [[FA]]
