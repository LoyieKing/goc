#include "textflag.h"

// ABI0 → SysV trampolines into freestanding C in goc_uptr_amd64.syso.
// Frame sizes leave room for CALL (same pattern as P1 GocLeaf).

// func GetSP() uintptr
TEXT ·GetSP(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	SP, ret+0(FP)
	RET

// func GocStackHiTLS() uintptr — C goc_stack_hi → goc_runtime_stack_hi (FS:-8)
TEXT ·GocStackHiTLS(SB), NOSPLIT, $8-8
	CALL	goc_stack_hi(SB)
	MOVQ	AX, ret+0(FP)
	RET

// func GocStackLoTLS() uintptr
TEXT ·GocStackLoTLS(SB), NOSPLIT, $8-8
	CALL	goc_stack_lo(SB)
	MOVQ	AX, ret+0(FP)
	RET

// func GocRuntimeGetg() uintptr — direct TLS hook
TEXT ·GocRuntimeGetg(SB), NOSPLIT, $8-8
	CALL	goc_runtime_getg(SB)
	MOVQ	AX, ret+0(FP)
	RET

// Pure Go TLS cross-check (same convention as P1 morestack)
// func GocStackHiGo() uintptr
TEXT ·GocStackHiGo(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	TLS, CX
	MOVQ	0(CX)(TLS*1), AX
	MOVQ	8(AX), AX
	MOVQ	AX, ret+0(FP)
	RET

// func GocUptrFromSptr(p uintptr) uintptr — default API (live TLS hi)
TEXT ·GocUptrFromSptr(SB), NOSPLIT, $16-16
	MOVQ	p+0(FP), DI
	CALL	goc_uptr_from_sptr(SB)
	MOVQ	AX, ret+8(FP)
	RET

// func GocUptrAsSptr(u uintptr) uintptr — default API (live TLS hi)
TEXT ·GocUptrAsSptr(SB), NOSPLIT, $16-16
	MOVQ	u+0(FP), DI
	CALL	goc_uptr_as_sptr(SB)
	MOVQ	AX, ret+8(FP)
	RET

// func GocUptrAsSptrHi(u, hi uintptr) uintptr — explicit hi (stack-move proof)
TEXT ·GocUptrAsSptrHi(SB), NOSPLIT, $24-24
	MOVQ	u+0(FP), DI
	MOVQ	hi+8(FP), SI
	CALL	goc_uptr_as_sptr_hi(SB)
	MOVQ	AX, ret+16(FP)
	RET

// func GocUptrFromSptrHi(p, hi uintptr) uintptr
TEXT ·GocUptrFromSptrHi(SB), NOSPLIT, $24-24
	MOVQ	p+0(FP), DI
	MOVQ	hi+8(FP), SI
	CALL	goc_uptr_from_sptr_hi(SB)
	MOVQ	AX, ret+16(FP)
	RET
