# third_party

Nothing here is in git except this file. Everything below is fetched or built locally and is gitignored.

| Path | What | How to get it |
|---|---|---|
| `llvm-19.1.7-clang-build/` | The patched Clang 19.1.7 and its LLVM tools | `scripts/build-clang.sh` downloads, patches and builds it ([docs/build-from-source.md](../docs/build-from-source.md)) |
| `quickjs-ng/` | QuickJS-ng (optional) | `git clone https://github.com/quickjs-ng/quickjs.git third_party/quickjs-ng` |
| `quickjs-bellard/` | Bellard's QuickJS (optional) | unpack the pristine `quickjs-2026-06-04.tar.xz` from <https://bellard.org/quickjs/> and rename the directory |

`ports/quickjs/build.sh` applies its own patches to the QuickJS trees; do not commit them. See [ports/quickjs/README.md](../ports/quickjs/README.md).
