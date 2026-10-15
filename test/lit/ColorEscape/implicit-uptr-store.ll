; A stack address stored into a global cptr slot is encoded as uptr first;
; the frame itself keeps the plain pointer.
; RUN: %goc-color-escape %s -o %t.ll -default-ptr-color=cptr 2>%t.err
; RUN: FileCheck %s < %t.ll
; RUN: FileCheck --check-prefix=SUMMARY %s < %t.err

target triple = "x86_64-unknown-linux-gnu"

@g_slot = global ptr null, align 8

declare void @keep(ptr)

define void @store_local() {
entry:
  %local = alloca i32, align 4
  store i32 7, ptr %local, align 4
  store ptr %local, ptr @g_slot, align 8
  call void @keep(ptr %local)
  ret void
}

; CHECK-LABEL: define void @store_local()
; CHECK:       %local = alloca i32{{.*}}!goc.color
; CHECK:       [[ENC:%[a-z.]+]] = call ptr @goc_uptr_from_ptr(ptr %local){{.*}}!goc.uptr_encoded
; CHECK-NEXT:  store ptr [[ENC]], ptr @g_slot
; CHECK-NEXT:  call void @keep(ptr %local)

; SUMMARY: summary: 0 error(s), {{.*}}implicit uptr stores=1
