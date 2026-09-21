# Status — what works / what does not

**As of:** 2026-09-22 (Asia/Shanghai) · Phase **P28**  
**Honesty first:** this is a research compiler, not a production toolchain.

## Works

| Area | Evidence |
|------|----------|
| In-tree Clang Sema pointer colors | `tests/sema/01_ok_*.c` with patched clang, no `-fplugin` |
| `sptr` store to heap/global = hard error | `tests/sema/02_err_sptr_to_heap.c` → `goc: sptr escape` |
| Real-body goobj (not P21 seed MIR) | `tests/realbody/03_ok_realbody_goobj.c` → magic `0x28C0DE42` in TEXT; meta `encoding=clang-real-isel` |
| Out-of-tree plugin fallback | `clang/plugin` + `./cmd/goc test --p27` |
| goobj / MIR research backend | `backend/` (P5–P16 path; harness goldens historically L/S/S2/S3/A/F/W) |
| `uptr` MSB + TLS stack.hi helpers | `runtime/` |
| Language contract (no `dsptr`) | [syntax-guide.md](syntax-guide.md) |

## Does not work (yet)

| Gap | Notes / target |
|-----|----------------|
| Full QuickJS-ng via `goc` | P23–P26 explored slices; full colored QJS = **P29+** |
| Arbitrary Clang IR → complete Spill/Maps/StackCheck/WB | P28 realbody uses llc+elfpack; full MF pipeline on arbitrary IR = **P29** |
| Complete Go amd64 **ABIInternal** | Partial research only |
| AVX / x87 / EH on Go-callable path | Permanently unsupported (P16 contract) |
| Multi-TU / LTO-grade lowering | Not started |
| Stable releases / installers | `main` only |

## How to verify locally

```bash
export GOC_ROOT="$(pwd)"
export GOC_CLANG=/path/to/patched/clang
./cmd/goc test --p28
```

Plugin-only (no patched clang):

```bash
./cmd/goc test --p27
```

## Phase summaries

Condensed reports: [phases/](phases/). Prefer those over archaeology of the
private research tree.
