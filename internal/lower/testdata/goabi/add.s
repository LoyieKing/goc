# Go 1.24 ABIInternal -> SysV entry thunks — generated
	.text
	.globl	add
	.type	add,@function
add:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	%rax, %rdi
	movq	%rbx, %rsi
	call	add.impl
	pxor	%xmm15, %xmm15
	movq	%rbp, %rsp
	popq	%rbp
	ret
	.size	add, .-add
