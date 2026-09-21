# P17 frontend — pointer colors + escape analysis

**Date:** 2026-09-21 (Asia/Shanghai)  
**Contract:** [`../goc-syntax-guide.md`](../goc-syntax-guide.md) v0.2 / v0.2.1  
**Archive:** [`../p17-archive/P17-REPORT.md`](../p17-archive/P17-REPORT.md)  
**IR scheme:** [`docs/IR-COLOR.md`](./docs/IR-COLOR.md)

## Layout

```text
p17-frontend/
  include/goc.h          # cptr/sptr/uptr/auto_ptr/gptr + JSValue + builtins
  include/goc_stubs.c    # optional link stubs (not needed for IR pass)
  pass/GocColorEscape.cpp
  pass/Makefile
  tests/                 # golden PASS/FAIL
  docs/IR-COLOR.md
  build.sh               # one-command build + tests
  build/goc-color-escape
../bin/goc-fe            # clang -emit-llvm → color-escape
```

## How to run

```bash
cd $GOC_ROOT/p17-frontend
./build.sh
# → 7 PASS lines; exit 0

../bin/goc-fe --emit-ir /tmp/x.color.ll tests/02_ok_uptr_encode_heap.c
```

## Colors (source → IR)

| Source | IR evidence |
|--------|-------------|
| `cptr`/`sptr`/`uptr`/`auto_ptr`/`gptr` macros | `annotate("goc.color.*")` → `llvm.var.annotation` / `llvm.ptr.annotation` |
| bare `T *` | auto; provenance refine |
| `goc_uptr_from_sptr` | result `!goc.color !{!"uptr"}` + `!goc.uptr_encoded` |
| pass output | `!goc.color` / `!goc.prov` on pointer insts; module flag `goc.color.schema` |

**Hard rule:** `sptr` escape → compile error; **no** auto-promote to `uptr`; **no** `dsptr`.

## Out of scope (P17)

Full QJS port · Clang fork · O2/GC polish · replacing P16 encode path · auto wiring color IR → stackmap/WB (P18).

## P20 统一入口

```bash
./bin/goc fe [--emit-ir out.ll] file.c   # 同 goc-fe
./bin/goc test                           # 含本树 goldens
```
`bin/goc-fe` 现为 `goc fe` 别名。详见 [../p20-archive/P20-REPORT.md](../p20-archive/P20-REPORT.md)。
