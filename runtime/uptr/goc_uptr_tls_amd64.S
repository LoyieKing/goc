/* goc_uptr_tls_amd64.S — P22: read owner g->stack.hi via Go linux/amd64 TLS.
 *
 * Convention (same as P1 morestack / P5 FS:-8):
 *   movq %fs:-8, %rax   // g
 *   movq 8(%rax), %rax  // g.stack.hi  (stack.lo @ 0, stack.hi @ 8)
 *
 * Link this .o into a Go binary (via .syso) so FS TLS is Go's g.
 * Do NOT link into freestanding host unit tests — those use goc_test_set_stack_hi.
 */
	.text
	.globl	goc_runtime_getg
	.type	goc_runtime_getg, @function
goc_runtime_getg:
	movq	%fs:-8, %rax
	ret
	.size	goc_runtime_getg, .-goc_runtime_getg

	.globl	goc_runtime_stack_hi
	.type	goc_runtime_stack_hi, @function
goc_runtime_stack_hi:
	movq	%fs:-8, %rax
	movq	8(%rax), %rax
	ret
	.size	goc_runtime_stack_hi, .-goc_runtime_stack_hi

	.globl	goc_runtime_stack_lo
	.type	goc_runtime_stack_lo, @function
goc_runtime_stack_lo:
	movq	%fs:-8, %rax
	movq	0(%rax), %rax
	ret
	.size	goc_runtime_stack_lo, .-goc_runtime_stack_lo
