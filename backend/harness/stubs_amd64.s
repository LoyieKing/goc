// Harness stubs only (helpers / counters).
// goc TEXT bodies (GocCheckedAdd, GocHoldLive, GocHoldArg, StoreGptrWB) come from
// binary goobj build/goobj/goc_funcs.o via toolexec pack — NOT reassembled from .s.
#include "textflag.h"
#include "funcdata.h"

DATA ·morestackHits+0(SB)/8, $0
GLOBL ·morestackHits(SB), NOPTR, $8
DATA ·forceOnceFlag+0(SB)/8, $0
GLOBL ·forceOnceFlag(SB), NOPTR, $8
DATA ·wbPathHits+0(SB)/8, $0
GLOBL ·wbPathHits(SB), NOPTR, $8

TEXT ·MorestackHits(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	·morestackHits(SB), AX
	MOVQ	AX, ret+0(FP)
	RET

TEXT ·WBPathHits(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	·wbPathHits(SB), AX
	MOVQ	AX, ret+0(FP)
	RET

TEXT ·GetSP(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	SP, ret+0(FP)
	RET

TEXT ·ForceMorestackOnce(SB), NOSPLIT, $0-16
entry_force:
	CMPQ	·forceOnceFlag(SB), $0
	JNE	after_force
	MOVQ	SP, ret+0(FP)
	MOVQ	$1, ·forceOnceFlag(SB)
	ADDQ	$1, ·morestackHits(SB)
	CALL	runtime·morestack_noctxt(SB)
	JMP	entry_force
after_force:
	MOVQ	SP, ret+8(FP)
	RET

TEXT ·HugeFrame(SB), $8192-16
	MOVQ	n+0(FP), AX
	MOVQ	AX, ret+8(FP)
	RET

TEXT ·HugeFrameVoid(SB), $8192-0
	RET

TEXT ·WBEnabled(SB), NOSPLIT, $0-1
	MOVL	runtime·writeBarrier(SB), AX
	CMPL	AX, $0
	SETNE	ret+0(FP)
	RET
