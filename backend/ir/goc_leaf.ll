; goc P3: plain SysV leaf — prologue / stackmap live in the Go-asm wrapper from goc_lower.
define i64 @goc_leaf(i64 %a, i64 %b) {
entry:
  %sum = add i64 %a, %b
  ret i64 %sum
}
