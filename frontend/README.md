# Frontend

| Dir | Role |
|-----|------|
| `color-escape/` | IR escape refine; inserts uptr encode/decode |
| `color-bridge/` | Color metadata to spill / write-barrier recipes |
| `vertical/` | Legacy color.ll → seed MIR demo. Not the product lower path. |

Product Sema is `clang/`. Product lower is `backend/realbody/`.
Public header: `include/goc.h`.
