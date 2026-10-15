; Returning a frame address is an error, not an implicit uptr promotion.
; RUN: not %goc-color-escape %s -o /dev/null -default-ptr-color=cptr 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

define ptr @bad_ret() {
entry:
  %local = alloca i32, align 4
  store i32 3, ptr %local, align 4
  ret ptr %local
}

; CHECK: error: sptr escape: returning a stack pointer is forbidden
; CHECK: in function 'bad_ret'
; CHECK: summary: 1 error(s)
