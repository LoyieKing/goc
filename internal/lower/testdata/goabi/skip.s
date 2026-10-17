# Go 1.24 ABIInternal -> SysV entry thunks — generated
	.text
	.globl	fadd
	.type	fadd,@function
fadd:
	pushq	%rbp
	movq	%rsp, %rbp
	call	fadd.impl
	pxor	%xmm15, %xmm15
	movq	%rbp, %rsp
	popq	%rbp
	ret
	.size	fadd, .-fadd
