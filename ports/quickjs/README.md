# ports/quickjs

QuickJS compiled by goc and run on goroutine stacks: the port goc is
checked against (test262, the QuickJS test suite, V8 benchmark).

| Path | What |
|------|------|
| `build.sh` | `--flavor ng` (third_party/quickjs-ng, default) or `--flavor bellard`: the engine and the C host through `goc build`, the embed smoke, then `build/<flavor>/qjscli` |
| `patches/` | the gstack patches `build.sh` applies to the QuickJS tree: the stack limit follows the goroutine's `g.stack.hi` |
| `host/` | C compiled by goc: the freestanding libc shim, the CLI host (`qjs:std`, `qjs:os`, workers), the Bellard API adapter |
| `embed/` | Go program: the moving-stack smoke and growth sweep |
| `cli/` | Go program: the `qjscli` command |
| `tests/` | `test262.py`, `official.py` (QuickJS tests), `cli_tests.py` (the tests.conf selection) |

```bash
ports/quickjs/build.sh                   # build/qjs/qjscli
ports/quickjs/build.sh --flavor bellard  # build/qjs-bellard/qjscli
ports/quickjs/tests/cli_tests.py
```

Fetch the QuickJS trees as in [third_party/README.md](../../third_party/README.md).
