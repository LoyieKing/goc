# color-escape

IR pass: refine pointer colors and insert `uptr` encode/decode.

Contract: [docs/syntax-guide.md](../../../docs/syntax-guide.md).
IR notes: [docs/IR-COLOR.md](docs/IR-COLOR.md).

```bash
./build.sh
```

A raw `sptr` return is a compile error. There is no `dsptr`.
