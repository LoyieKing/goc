# backend

Product lower path and the machine passes it uses.

```text
backend/realbody/     Clang IR → llc → elfpack
backend/goobj/elfpack ELF bytes and relocations → goobj
backend/pass/         GocStackMap.so (opt plugin) and goc-llc (llc + GocFrameAddrFix)
backend/tools/        toolexec packer used by the QuickJS link
```

Build the passes with `scripts/build-passes.sh` (needs the LLVM 19.1.7
source and build trees; see `pass/Makefile`).

The QuickJS driver calls `backend/realbody/goc_p28_realbody.sh`.

AVX, x87, and EH are rejected on the Go-callable path.
