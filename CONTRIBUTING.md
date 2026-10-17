# Contributing to goc

Thanks for your interest. goc is experimental; small, well-documented patches are preferred over large refactors.

## Before you start

1. The language contract is [docs/language.md](docs/language.md) (Chinese; authoritative). Do not reintroduce `dsptr`.
2. The code map is [ARCHITECTURE.md](ARCHITECTURE.md); tests and how to extend goc are in [docs/contributing.md](docs/contributing.md).

## Development setup

See [docs/build-from-source.md](docs/build-from-source.md). In short:

```bash
./scripts/build-clang.sh
./scripts/build-passes.sh
go build -o bin/goc ./cmd/goc
./scripts/ci-test.sh
```

## Pull requests

- One concern per PR when practical.
- Add or extend tests under `test/` (`lit` for Sema and passes, `e2e` for Go programs, `gocobj` for the format).
- A change to the compile path also needs the QuickJS check in [docs/contributing.md](docs/contributing.md) §2.
- Do not commit `build/`, `bin/`, `*.o`, `*.goobj`, clang binaries, or a QuickJS tree.
- Do not hardcode absolute machine paths. goc finds its tools itself and reads no `GOC_*` environment variable.

## Code style

- C/C++: match the surrounding Clang / LLVM style in `clang/` and `llvm/`.
- Go: `gofmt`.
- Shell: `set -euo pipefail`, quote paths.

## License

By contributing, you agree that your contributions are licensed under the MIT License (Copyright (c) 2026 Loyie King).
