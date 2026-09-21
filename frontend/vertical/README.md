# P21 — color.ll → goobj vertical path

Proven slice: colored IR → bridge attrs → **generated** MIR seed (not
`pass/harness.mir`) → Spill/Maps/WB → Go-frame `printMIR` → `llc` → `elfpack`
→ linkable goobj.

```bash
./build.sh
# or
../bin/goc vertical
../bin/goc test --p21
```

See `docs/VERTICAL.md` and `../p21-archive/P21-REPORT.md`.
