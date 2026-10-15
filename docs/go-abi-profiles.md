# Go ABI profiles

Code goc generates has to agree with the Go runtime on a handful of
constants. They change rarely, but they are Go-version facts, so they live in
one table, `llvm/include/Goc/AbiProfiles.def`, with a Go copy in
`gocobj/abi.go` (a test keeps the two equal). A `.gocobj` names its profile in
`.goc.header`; the converter refuses a profile it does not know.

| | go1.24-amd64 | go1.24-arm64 | source |
|---|---|---|---|
| g register | R14 | R28 | `cmd/compile/internal/ssa/_gen/{AMD64,ARM64}Ops.go` |
| `g.stackguard0` offset | 16 | 16 | `runtime/runtime2.go` (after `stack{lo,hi}`) |
| StackSmall / StackBig | 128 / 4096 | 128 / 4096 | `internal/abi/stack.go` |
| morestack | `runtime.morestack_noctxt` | same | `cmd/internal/obj/x86/obj6.go`, `obj/arm64/obj7.go` |
| ABIInternal int args | RAX RBX RCX RDI RSI R8–R11 | R0–R15 | `cmd/compile/abi-internal.md` |
| ABIInternal float args | X0–X14 | F0–F15 | same |

Upgrading Go: check each row against the new sources. If nothing changed,
add a profile with the new name and the same values; otherwise add one with
the new values and teach the backend and converter about the difference.
