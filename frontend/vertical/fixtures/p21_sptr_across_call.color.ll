; P21 fixture: sptr live across CALL → maps → goobj FUNCDATA (color-driven)
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @external_safepoint()

define void @p21_sptr_across_call(ptr %p) {
  %slot = alloca ptr, align 8, !goc.color !7, !goc.prov !8
  store ptr %p, ptr %slot, align 8
  %v = load ptr, ptr %slot, align 8, !goc.color !7, !goc.prov !8
  call void @external_safepoint()
  store ptr %v, ptr %slot, align 8
  ret void
}

!llvm.module.flags = !{!5}
!5 = !{i32 2, !"goc.color.schema", !"annotate+!goc.color;v0.2.1-P17"}
!7 = !{!"sptr"}
!8 = !{!"stack"}
