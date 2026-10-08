/* Trampolines from the goc shim to glibc libm.
 *
 * elfpack turns the shim's `call goc_libc_sin` into a Go relocation to
 * main.goc_libc_sin. The Go stack is only 8-byte aligned. SysV requires
 * SP%16==0 at a call instruction, so glibc's entry sees SP%16==8 and its
 * movaps prologue is aligned. andq $-16 does that for either incoming
 * alignment. An extra subq $8 would enter glibc with SP%16==0 and fault.
 * xmm0 / xmm1 go in, xmm0 comes out. The result matches glibc, including
 * Math.sin(1) == "0.8414709848078965".
 *
 * qjs-build.sh assembles this into tests/qjs and tests/qjscli as a .syso.
 * The call runs on the goroutine stack. libm does not switch stacks.
 */
.section .text, "ax", @progbits

.macro glibc_jmp name, target
.globl main.goc_libc_\name
.type main.goc_libc_\name, @function
main.goc_libc_\name:
	pushq %rbx
	movq %rsp, %rbx
	andq $-16, %rsp
	call \target@PLT
	movq %rbx, %rsp
	popq %rbx
	ret
.size main.goc_libc_\name, .-main.goc_libc_\name
.endm

glibc_jmp pow, pow
glibc_jmp hypot, hypot
glibc_jmp fmod, fmod
glibc_jmp acos, acos
glibc_jmp acosh, acosh
glibc_jmp asin, asin
glibc_jmp asinh, asinh
glibc_jmp atan, atan
glibc_jmp atan2, atan2
glibc_jmp atanh, atanh
glibc_jmp cbrt, cbrt
glibc_jmp cos, cos
glibc_jmp cosh, cosh
glibc_jmp exp, exp
glibc_jmp expm1, expm1
glibc_jmp log, log
glibc_jmp log10, log10
glibc_jmp log1p, log1p
glibc_jmp log2, log2
glibc_jmp sin, sin
glibc_jmp sinh, sinh
glibc_jmp tan, tan
glibc_jmp tanh, tanh
