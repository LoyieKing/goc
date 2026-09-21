# Runtime helpers

| Path | Role |
|------|------|
| `uptr/` | MSB encode/decode + amd64 TLS `stack.hi` assembly (P19/P22) |
| `pass/` | `GocUptrLower` LLVM pass sources |
| `harness/` | Go harness for TLS smoke (rebuild `.syso` locally; not shipped) |
| `goc_uptr.h` / `UPTR-MSB.md` / `TLS-HI.md` | API + notes |

These are building blocks for colored C, not a complete libc or QJS runtime.
