; ModuleID = 'pass/fixtures/clang_samples/eh_try.cpp'
source_filename = "pass/fixtures/clang_samples/eh_try.cpp"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

@_ZTIi = external constant ptr

; Function Attrs: mustprogress noinline optnone uwtable
define dso_local noundef i32 @_Z10goc_eh_tryi(i32 noundef %0) #0 personality ptr @__gxx_personality_v0 {
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca ptr, align 8
  %5 = alloca i32, align 4
  %6 = alloca i32, align 4
  store i32 %0, ptr %3, align 4
  %7 = load i32, ptr %3, align 4
  %8 = icmp ne i32 %7, 0
  br i1 %8, label %9, label %24

9:                                                ; preds = %1
  %10 = call ptr @__cxa_allocate_exception(i64 4) #2
  %11 = load i32, ptr %3, align 4
  store i32 %11, ptr %10, align 16
  invoke void @__cxa_throw(ptr %10, ptr @_ZTIi, ptr null) #3
          to label %33 unwind label %12

12:                                               ; preds = %9
  %13 = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  %14 = extractvalue { ptr, i32 } %13, 0
  store ptr %14, ptr %4, align 8
  %15 = extractvalue { ptr, i32 } %13, 1
  store i32 %15, ptr %5, align 4
  br label %16

16:                                               ; preds = %12
  %17 = load i32, ptr %5, align 4
  %18 = call i32 @llvm.eh.typeid.for.p0(ptr @_ZTIi) #2
  %19 = icmp eq i32 %17, %18
  br i1 %19, label %20, label %28

20:                                               ; preds = %16
  %21 = load ptr, ptr %4, align 8
  %22 = call ptr @__cxa_begin_catch(ptr %21) #2
  %23 = load i32, ptr %22, align 4
  store i32 %23, ptr %6, align 4
  store i32 -1, ptr %2, align 4
  call void @__cxa_end_catch() #2
  br label %26

24:                                               ; preds = %1
  br label %25

25:                                               ; preds = %24
  store i32 0, ptr %2, align 4
  br label %26

26:                                               ; preds = %25, %20
  %27 = load i32, ptr %2, align 4
  ret i32 %27

28:                                               ; preds = %16
  %29 = load ptr, ptr %4, align 8
  %30 = load i32, ptr %5, align 4
  %31 = insertvalue { ptr, i32 } poison, ptr %29, 0
  %32 = insertvalue { ptr, i32 } %31, i32 %30, 1
  resume { ptr, i32 } %32

33:                                               ; preds = %9
  unreachable
}

declare ptr @__cxa_allocate_exception(i64)

declare void @__cxa_throw(ptr, ptr, ptr)

declare i32 @__gxx_personality_v0(...)

; Function Attrs: nounwind memory(none)
declare i32 @llvm.eh.typeid.for.p0(ptr) #1

declare ptr @__cxa_begin_catch(ptr)

declare void @__cxa_end_catch()

attributes #0 = { mustprogress noinline optnone uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nounwind memory(none) }
attributes #2 = { nounwind }
attributes #3 = { noreturn }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 2}
!5 = !{!"Debian clang version 19.1.7 (3+b1)"}
