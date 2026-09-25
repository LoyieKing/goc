# backend

Product lower path and the machine passes it uses.

```text
backend/realbody/     Clang IR → llc → elfpack
backend/goobj/elfpack ELF bytes and relocations → goobj
backend/pass/         stack maps, safepoint spill, morestack
backend/tools/        toolexec packer used by the QuickJS link
```

Build the passes (needs a local LLVM 19.1.7 source tree; see `pass/Makefile`):

```bash
export GOC_DOCS_ROOT=$HOME/goc-legacy   # or set LLVM_X86_SRC / LLVM_X86_BUILD
make -C backend/pass
```

The QuickJS driver calls `backend/realbody/goc_p28_realbody.sh`. It does not
use seed MIR templates.

AVX, x87, and EH are rejected on the Go-callable path.
