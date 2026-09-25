# Runtime helpers

| Path | Role |
|------|------|
| `uptr/` | MSB encode/decode and the amd64 TLS `stack.hi` read |
| `pass/` | `GocUptrLower` sources |
| `harness/` | Go smoke for the TLS read |
| `goc_uptr.h`, `UPTR-MSB.md`, `TLS-HI.md` | API |

Not a libc and not the QuickJS runtime.
