# P18 — color annotations → stackmap / write-barrier

Wires P17 `!goc.color` / `!goc.prov` into the existing P5 Spill / EmitMaps / ExpandStoreGptr path.

## One command

```bash
./p18-color-bridge/build.sh
# or: ./bin/goc-p18
```

Keeps P17 goldens green and adds P18 bridge + machine proofs.

## Layout

| Path | Role |
|------|------|
| `pass/GocColorBridge.cpp` | IR: color MD → fn attrs + recipe |
| `pass/goc_p18_driver.cpp` | Machine: attrs → P5 Spill/Maps/WB |
| `fixtures/*.color.ll` | Minimal color IR goldens |
| `tests/*.c` | C → P17 → bridge vertical |
| `docs/DATAFLOW.md` | Exact dataflow |

## P5 hooks (additive)

- Spill **R5** color-driven (`goc-color-driven` / `goc-arg-ptr-colors` / `goc-color-cptr-only`)
- ExpandStoreGptr accepts `goc-color-wb`; refuses `goc-color-cptr-only`
- EmitPointerMaps: color-aware Args bits; per-fn maps dir when color-driven

## P20 统一入口

```bash
./bin/goc bridge    # 同 ./bin/goc-p18 → 本树 build.sh
./bin/goc test      # P17 + P18 + P19
```
详见 [../p20-archive/P20-REPORT.md](../p20-archive/P20-REPORT.md)。
