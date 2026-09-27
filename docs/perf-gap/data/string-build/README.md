# string_build1(200) under callgrind (2026-09-27)

Commands (box, valgrind 3.x):

    valgrind --tool=callgrind /workspace/perf-study/bellard/clang-O3-NDEBUG/qjs sb.js   # nat
    valgrind --tool=callgrind build/qjs-bellard/qjscli sb.js                            # goc

| | native clang-O3 | goc-bellard |
|---|---:|---:|
| Ir total | 63,070,000 | 196,580,581 |
| calls JS_ConcatString1 | 12,599 | 107,800 |
| calls malloc_usable_size (glibc / goc shim) | 116,569 | 306,968 |
| calls goc_memcpy | - | 308,617 |

nat-top.txt / goc-top.txt: callgrind_annotate --inclusive=no (top 40).
