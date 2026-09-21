# Frontend helpers (legacy / refine)

| Dir | Origin | Role |
|-----|--------|------|
| `color-escape/` | P17 | IR color refine / escape checks (annotate-era) |
| `color-bridge/` | P18 | Color → WB / spill recipes |
| `vertical/` | P21 | Legacy color.ll → goobj **seed** demo (not product lower) |

Product Sema lives under `../clang/`. Product lower is `../backend/realbody/`
(P28, real ISel — **not** P21 seed MIR).

Public header: `../include/goc.h` (prefer over nested copies).
