# backend

Product lower path and the machine passes it uses.

```text
tools/goc/     Clang IR → llc → elfpack
tools/goc-elfpack ELF bytes and relocations → goobj
llvm/         libGocPasses.so (opt plugin) and goc-llc (llc + GocFrameAddrFix)
tools/goc/        toolexec packer used by the QuickJS link
```

Build the passes with `scripts/build-passes.sh` (needs the LLVM 19.1.7
source and build trees; see `llvm/CMakeLists.txt`).

The QuickJS driver calls `tools/goc/realbody.sh`.

AVX, x87, and EH are rejected on the Go-callable path.
