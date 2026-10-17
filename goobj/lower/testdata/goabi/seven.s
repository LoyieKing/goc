# Go 1.24 ABIInternal -> SysV entry thunks — generated
	.text
	.globl	sum7
	.type	sum7,@function
sum7:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$16, %rsp
	movl	%r9d, 0(%rsp)
	movq	%rcx, %rdx
	movq	%rdi, %rcx
	movq	%rax, %rdi
	movq	%r8, %r9
	movq	%rsi, %r8
	movq	%rbx, %rsi
	call	sum7.impl
	pxor	%xmm15, %xmm15
	movq	%rbp, %rsp
	popq	%rbp
	ret
	.size	sum7, .-sum7
