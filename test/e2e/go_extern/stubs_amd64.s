#include "textflag.h"

DATA ·goExternMarker+0(SB)/8, $0x60e7e4
GLOBL ·goExternMarker(SB), NOPTR, $8

TEXT ·GoExternMarker(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	·goExternMarker(SB), AX
	MOVQ	AX, ret+0(FP)
	RET
