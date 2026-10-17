
declare i32 @leaf.impl(i32, i32, i32, i32, i32, i32, i32)
define i32 @sum7.impl(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g) {
  %r = call i32 @leaf.impl(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g)
  ret i32 %r
}
