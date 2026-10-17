// Go 1.24 arm64 ABIInternal -> AAPCS64 entry thunks — generated
	.text
	.globl	sum9
	.type	sum9, %function
	.balign	4
sum9:
	sub	sp, sp, #32
	stp	x29, x30, [sp, #16]
	add	x29, sp, #16
	str	x30, [sp]
	sub	sp, sp, #32
	str	x30, [sp]
	str	x8, [sp, #16]
	bl	sum9.impl
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	ret
	.size	sum9, .-sum9
	.globl	box
	.type	box, %function
	.balign	4
box:
	sub	sp, sp, #32
	stp	x29, x30, [sp, #16]
	add	x29, sp, #16
	str	x30, [sp]
	bl	box.impl
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	ret
	.size	box, .-box
