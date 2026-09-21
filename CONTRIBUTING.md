# Contributing to goc

Thanks for your interest. `goc` is experimental — small, well-documented
patches are preferred over large refactors.

## Before you start

1. Read [docs/status.md](docs/status.md) and [docs/roadmap.md](docs/roadmap.md).
2. The language contract is [docs/syntax-guide.md](docs/syntax-guide.md)
   (Chinese; authoritative). Do not reintroduce `dsptr`.
3. Prefer honesty: if a path still uses seed MIR or fixtures, say so in the PR.

## Development setup

See [README.md](README.md) Quick start and [clang/README.md](clang/README.md).

Build dependencies: `clang-19` / `clang++-19`, `llc-19`, `cmake`, `ninja`,
Go 1.22+, `python3`, `rg` (ripgrep).

```bash
export GOC_ROOT="$(pwd)"
export GOC_CLANG=/path/to/patched/clang   # after applying in-tree patches
./scripts/apply-patches.sh               # against your LLVM 19.1.7 checkout
./cmd/goc test --p28                     # requires patched clang
```

## Pull requests

- One concern per PR when practical
- Update `docs/status.md` if you change what works / what does not
- Add or extend golden tests under `tests/`
- Do not commit `_deps/`, `build/`, `*.o`, clang binaries, or a full
  `quickjs-ng` tree
- Do not hardcode absolute machine paths; use `$GOC_ROOT` / `$GOC_CLANG`

## Code style

- C/C++: match surrounding Clang / LLVM style in `clang/` and `backend/pass`
- Go: standard `gofmt`
- Shell: `set -euo pipefail`, quote paths

## License

By contributing, you agree that your contributions are licensed under the MIT
License (Copyright (c) 2026 Loyie King).
