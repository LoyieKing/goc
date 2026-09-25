// Stubs only: the goabi golden's C bodies come from the packed goobj TEXT
// (Go-ABI thunks + <name>.impl SysV bodies) via toolexec_pack_goobj.sh.
#include "textflag.h"

DATA ·goabiMarker+0(SB)/8, $0x670ab1
GLOBL ·goabiMarker(SB), NOPTR, $8

TEXT ·GoabiMarker(SB), NOSPLIT|NOFRAME, $0-8
	MOVQ	·goabiMarker(SB), AX
	MOVQ	AX, ret+0(FP)
	RET
