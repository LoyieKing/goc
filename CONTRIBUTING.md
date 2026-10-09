# Contributing to goc

Thanks for your interest. `goc` is experimental — small, well-documented
patches are preferred over large refactors.

## Before you start

1. The language contract is [docs/syntax-guide.md](docs/syntax-guide.md)
   (Chinese; authoritative). Do not reintroduce `dsptr`.
2. Prefer honesty: if a path still uses seed MIR or fixtures, say so in the PR.

## Development setup

See [docs/build-from-source.md](docs/build-from-source.md).

Build dependencies: `clang-19` / `clang++-19`, `cmake`, `ninja`,
Go 1.24+, `python3`, `rg` (ripgrep).

```bash
./scripts/build-clang.sh
./scripts/build-passes.sh
./cmd/goc test --p28
```

## Pull requests

- One concern per PR when practical
- Add or extend golden tests under `tests/`
- Do not commit `_deps/`, `build/`, `*.o`, clang binaries, or a full
  `quickjs-ng` tree
- Do not hardcode absolute machine paths; use `$GOC_ROOT`. The driver finds clang itself

## Code style

- C/C++: match surrounding Clang / LLVM style in `clang/` and `backend/pass`
- Go: standard `gofmt`
- Shell: `set -euo pipefail`, quote paths

## License

By contributing, you agree that your contributions are licensed under the MIT
License (Copyright (c) 2026 Loyie King).
