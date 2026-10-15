; goc-llc on amd64: the Go stack check compares SP with g.stackguard0
; (g at %fs:-8, stackguard0 at +16) before the prologue, and the cold block
; saves the argument registers around runtime.morestack. The tables name
; the go1.24-amd64 profile.
; RUN: %goc-llc -O2 -relocation-model=pic -march=x86-64 -frame-pointer=all \
; RUN:   -disable-tail-calls -function-sections -goc-tables %t.tables.s %s -o - \
; RUN:   | FileCheck %s
; RUN: FileCheck --check-prefix=TABLES %s < %t.tables.s

target triple = "x86_64-unknown-linux-gnu"

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
; CHECK:       movq %fs:-8, %r11
; CHECK-NEXT:  cmpq 16(%r11), %rsp
; CHECK-NEXT:  jbe [[COLD:\.LBB[0-9_]+]]
; CHECK:       pushq %rbp
; CHECK:       callq fill@PLT
; CHECK:       retq
; CHECK:       pushq %rdi
; CHECK:       movq %fs:-8, %r14
; CHECK:       runtime.morestack

; TABLES-DAG: .section .goc.header
; TABLES-DAG: .section .goc.gcmaps
; TABLES-DAG: .asciz "go1.24-amd64"
