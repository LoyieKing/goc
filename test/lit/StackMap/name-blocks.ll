; goc-name-blocks names every unnamed address-taken block. LLVM 19's .ll
; parser can merge a numbered and a named blockaddress target of one table,
; so the textual hand-off between tools must not carry numbered ones.
; RUN: %opt-goc -passes=goc-name-blocks -S %s -o - | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@table = global [2 x ptr] [ptr blockaddress(@dispatch, %1), ptr blockaddress(@dispatch, %2)]

define i32 @dispatch(i32 %i) {
entry:
  %slot = getelementptr [2 x ptr], ptr @table, i32 0, i32 %i
  %t = load ptr, ptr %slot
  indirectbr ptr %t, [label %1, label %2]
1:
  ret i32 1
2:
  ret i32 2
}

; CHECK: @table = global [2 x ptr] [ptr blockaddress(@dispatch, %goc.ba0), ptr blockaddress(@dispatch, %goc.ba1)]
; CHECK: goc.ba0:
; CHECK-NEXT: ret i32 1
; CHECK: goc.ba1:
; CHECK-NEXT: ret i32 2
