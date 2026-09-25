# Benchmark

One comparison, same workstation, V8-v7 (`bench-v8.js`, `--stack-size 16384`).
Higher is faster. goc numbers are the mean of two sequential runs of
`build/qjs/qjscli` (SCORE 1198 in 37.95 s, SCORE 1203 in 39.73 s).
Goja, native QuickJS-ng, and Bellard QuickJS are the reference runs of the
same suite, not a new measurement.

| Suite | goc | Goja | native QuickJS-ng | Bellard QuickJS |
|-------|----:|-----:|------------------:|----------------:|
| Richards | 837.5 | 334 | 914 | 1170 |
| DeltaBlue | 846.5 | 400 | 948 | 1106 |
| Crypto | 804 | 166 | 994 | 1274 |
| RayTrace | 1776 | 348 | 1915 | 2576 |
| EarleyBoyer | 2262 | 711 | 2626 | 3132 |
| RegExp | 362 | 289 | 454 | 584 |
| Splay | 3449 | 1568 | 4081 | 5038 |
| NavierStokes | 1511 | 274 | 1733 | 2447 |
| **Overall** | **1200.5** | **402** | **1389** | **1768** |

goc wall time for those two runs: 37.95 s and 39.73 s.
Native QuickJS-ng reference wall time: 36.3 s. Goja: 87.7 s. Bellard: 30.6 s.

Microcall (`scripts/microcall-bench.sh`), calls/ms, higher is faster.
Ratio above 1 means goc is slower.

| | goc | native QuickJS-ng | ratio |
|--|----:|------------------:|------:|
| score | 19556 | 23356 | 1.19 |
| depth4 | 60 ms | 45 ms | 1.33 |

Reference commits: Goja `cfe4039cb6d77b297d8b637182f774fa4a54b7d5`,
native QuickJS-ng `6d46d07d04041b40f4f49eaa7fdebe44c314c699`.
Bellard QuickJS is the 2026-06-04 extras archive, a different codebase.
