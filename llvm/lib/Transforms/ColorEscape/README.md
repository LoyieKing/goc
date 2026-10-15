# ColorEscape

IR pass `goc-color-escape` (in `libGocPasses.so`): refine pointer colors and
insert `uptr` encode/decode. The color analysis it builds on is in
`../../Analysis/GocColor`; `print<goc-color>` prints what it resolved.

Contract: [docs/syntax-guide.md](../../../../docs/syntax-guide.md).
IR notes: [docs/design/IR-COLOR.md](../../../../docs/design/IR-COLOR.md).

A raw `sptr` return is a compile error. There is no `dsptr`.
