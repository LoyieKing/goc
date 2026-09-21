; P18 fixture: gptr store must drive WB (via bridge → goc-store-gptr)
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.GoBox = type { ptr }
@g_go = global %struct.GoBox zeroinitializer, align 8

define void @p18_gptr_store(ptr %slot, ptr %newv) {
  %a = alloca ptr, align 8, !goc.color !7, !goc.prov !8
  store ptr %newv, ptr %a, align 8
  %v = load ptr, ptr %a, align 8, !goc.color !7, !goc.prov !9
  store ptr %v, ptr %slot, align 8
  ret void
}

!llvm.module.flags = !{!5}
!5 = !{i32 2, !"goc.color.schema", !"annotate+!goc.color;v0.2.1-P17"}
!7 = !{!"gptr"}
!8 = !{!"stack"}
!9 = !{!"goheap"}
