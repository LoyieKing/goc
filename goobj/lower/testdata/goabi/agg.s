# Go 1.24 ABIInternal -> SysV entry thunks — generated
	.text
	.globl	pair
	.type	pair,@function
pair:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	%rax, %rdi
	movq	%rbx, %rsi
	movq	%rcx, %rdx
	call	pair.impl
	movq	%rdx, %rbx
	pxor	%xmm15, %xmm15
	movq	%rbp, %rsp
	popq	%rbp
	ret
	.size	pair, .-pair
