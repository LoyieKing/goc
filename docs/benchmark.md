# 跑分

同一台机器，五个引擎，2026-09-27 重测。每节先看图，表是全部数字。原始输出在 [`docs/benchmark/data/raw/`](benchmark/data/raw/)，汇总在 [`docs/benchmark/data/all.json`](benchmark/data/all.json)。

浏览器打开 [`docs/benchmark/report.html`](benchmark/report.html) 时图会直接嵌在页里（用的是同名的 SVG）。图里的标题、坐标轴和图例都用英文，正文仍是中文。每张性能图都包含全部五个引擎；Goja 的数字比其他几家大几倍的图用对数轴。

goc 现在能编译两份 QuickJS：QuickJS-ng（下称 goc-ng，原来的“goc”列）和 Fabrice Bellard 的原版 QuickJS（goc-bellard，本次新增，移植细节见“Bellard QuickJS 移植”一节）。每个 goc 构建都和用同一份源码编译的原生构建对照：goc-ng 对 native ng，goc-bellard 对 native Bellard。

| 引擎 | 二进制 |
|------|--------|
| goc-ng | `build/qjs/qjscli --stack-size 16384`，commit `208df3b` 用 `scripts/qjs-cli-build.sh` 构建，默认 O3 + `-DNDEBUG` |
| goc-bellard | `build/qjs-bellard/qjscli --stack-size 16384`，同一 commit 用 `QJS_FLAVOR=bellard scripts/qjs-cli-build.sh` 构建，源码是 Bellard QuickJS 2026-06-04，O3 + `-DNDEBUG` |
| native ng | QuickJS-ng 0.17.0，`qjs -C --stack-size 16384`（`-C` 是经典脚本） |
| native Bellard | QuickJS 2026-06-04，上游 Makefile 默认构建（gcc 14 `-O2`），`qjs --stack-size 16M` |
| Goja | `cfe4039cb6d77b297d8b637182f774fa4a54b7d5`，用 [`scripts/gojacli`](../scripts/gojacli/main.go) 运行 |

native ng 的构建：同一份 quickjs-ng 源码，CMake `Release`，编译器 `clang-19`，即 `-O2 -DNDEBUG -std=gnu11 -funsigned-char`（外加上游 CMakeLists 的 `-fvisibility=hidden` 和警告开关，宏 `-D_GNU_SOURCE -DQUICKJS_NG_BUILD`）。goc 侧 `scripts/qjs-build.sh` 也用 `-DNDEBUG`，O 级见 `GOC_OPT_LEVEL`；它不加 `-funsigned-char`（shim 与 cli host 共用这组宏，改 char 符号会改变它们的语义）。native Bellard 用的是上游 Makefile 的默认参数：gcc `-O2 -g -funsigned-char -fwrapv`，没有 `-DNDEBUG`。goc-bellard 的前端是 goc 自带的 Clang，所以 goc-bellard 对 native Bellard 的比值里同时含有“goc 本身的开销”和“clang 与 gcc 的差别”。为了把两者分开，另测了一个用 clang-19 编译的 native Bellard 作参考，见“参考：clang 编译的 native Bellard”一节。

native ng 不加 `-C` 时会把这些文件当成模块，松散赋值直接 ReferenceError，所以下面的 ng 数字都是 `-C`。

## 怎么测的

整套流程在 `scripts/bench-all.sh` 里，可以直接重跑。`scripts/bench-summarize.py` 把原始输出汇总成 `all.json`，`scripts/bench-charts.py` 画图，`scripts/bench-tables.py` 生成本页的表，`scripts/bench-report-html.py` 生成 `report.html`。内存部分由 `scripts/bench-mem.sh` 单独测量，结果同样汇总进 `all.json`。

- 计时的套件都用 `taskset -c 3` 固定在同一个核上。每一轮五个引擎各跑一次，下一轮换一个起始引擎，这样五家交替运行、处在同一时段。
- V8-v7：每个引擎 5 轮，总分和每个子项都取中位数。
- SunSpider：每个引擎 3 轮，每项取中位数。
- microbench：每个引擎 3 轮，每项取中位数。
- 微调用（`scripts/microcall-bench.sh`）：五个引擎，5 轮，取中位数。
- 内存：峰值 RSS 每个引擎 3 轮取中位数，多实例每个点 3 次取中位数，细节见“内存占用”一节。
- test262 抽样和 QuickJS 官方测试只看对错，不计时，8 个进程并行跑。
- 几何平均只算五家都跑出结果的项，各列覆盖的是同一批测试。

所有计时套件（V8-v7、SunSpider、microbench、微调用）都在同一个时段一次跑完：北京时间 2026-09-27 14:02–14:51，接着 14:51–15:46 测内存（`env.txt`、`env-mem.txt` 里记的是机器时钟 KST，比北京时间快 1 小时）。clang 版 Bellard 的参考测量在这之后单独跑（15:47–16:13，`env.txt` 在 `raw/bellard-clang/` 里）。这台机器由多个任务共用，同一时段内的波动约 ±5%，每轮的数字都列在各节里。

本次在同一天早些时候先跑过一次，结果作废：那次 goc-bellard 的 qjscli 没有 `performance` 全局（ng 在引擎里提供，Bellard 的 qjs 在 quickjs-libc 里提供，goc 的 CLI 宿主没有用 quickjs-libc），`microbench.js` 因此退回到以 `Date.now()` 为基础的毫秒时钟，goc-bellard 的 microbench 数字被量化成 20、25、40 ns 这样的整数，还偏大。commit `208df3b` 给 Bellard 构建的 qjscli 补上了 `performance.now()`，本页的数字全部来自补上之后重跑的这一次。

## 总览

![相对 native ng 的速度](benchmark/charts/overview-speed.png)

![正确性](benchmark/charts/overview-correct.png)

正确性上，两个 goc 构建都和各自的原生构建完全一致：goc-ng 与 native ng、goc-bellard 与 native Bellard，在 test262 抽样上失败的文件相同，在官方测试上逐函数相同（见下面的“正确性对照”表）。

速度上，goc-ng 的 V8 总分是 native ng 的 0.93 倍，SunSpider 和 microbench 的几何平均分别慢 5.5% 和 6.2%，微调用是 0.93 倍，和上一版报告基本一样。goc-bellard 相对 native Bellard 差得更多：V8 是 0.80 倍，SunSpider 慢 46%，microbench 慢 40%，微调用只有 0.68 倍。这里面相当一部分是编译器的差别而不是 goc 的开销：同一份 Bellard 源码改用 clang-19 `-O2` 编译，V8 总分就从 gcc 版的 1544 掉到 1327（0.86 倍），goc-bellard 相对 clang 版是 0.94 倍，和 goc-ng/ng 的 0.93 接近；SunSpider 和 microbench 上 goc-bellard 相对 clang 版慢 14%，比 goc-ng/ng 的 5% 到 6% 大一些（详见“参考：clang 编译的 native Bellard”）。

五家横着比：native Bellard 最快，比 native ng 快三到六成。goc-bellard 在 V8、SunSpider、microbench 上都略快于 native ng（V8 1.04 倍，SunSpider 快 2%，microbench 快 13%），只有微调用慢一成。Goja 比 goc-ng 慢 3.5 到 6 倍。

内存上，两个 goc 构建都比对应的原生构建多一个约 6 到 9 MiB 的固定开销：空载时是 2.8 到 3.0 倍，大负载（整套 V8）时只有 1.06 倍。一个进程里每多一个存活的 runtime，goc-ng 要 225 KiB（native ng 211 KiB），goc-bellard 要 199 KiB（native Bellard 190 KiB），多出的部分主要是 goroutine 栈。Goja 空闲 runtime 最省（102 KiB），但跑负载时峰值最高，整套 V8 达到 native ng 的 7.8 倍。

| 套件 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard | 怎么读 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| V8-v7 总分（5 轮中位数） | 1119 | 1247 | 1203 | 1558 | 247 | 0.930 | 0.800 | 分数比，越高越快 |
| SunSpider 几何平均 ms（25 项） | 16.68 | 15.50 | 15.81 | 10.64 | 97.27 | 1.055 | 1.457 | 时间比，越低越快 |
| microbench 几何平均 ns（72 项） | 55.5 | 45.4 | 52.3 | 32.4 | 195.6 | 1.062 | 1.404 | 时间比，越低越快 |
| microcall score（calls/ms） | 21596 | 20781 | 23337 | 30784 | 5106 | 0.925 | 0.675 | 分数比，越高越快 |
| test262 通过 | 1502/1526 | 1501/1526 | 1502/1526 | 1501/1526 | 1453/1526 | | | 抽样，不是全量 |
| QuickJS 官方测试 | 69/77 | 73/77 | 69/77 | 73/77 | 58/77 | | | 按函数计 |

表中 goc-ng/ng 一列是 goc-ng 除以 native ng，goc-bellard/Bellard 一列是 goc-bellard 除以 native Bellard。内存那几行放在“内存占用”一节的表里（按负载分列）；这里摘几个数：空载峰值 RSS goc-ng 8.5 / goc-bellard 8.5 / native ng 3.0 / native Bellard 2.8 / Goja 5.9 MiB；整套 V8 峰值 164.6 / 155.9 / 155.5 / 147.0 / 1218.9 MiB；每多一个 runtime 225 / 199 / 211 / 190 / 102 KiB。

### 正确性对照

| goc 构建 | 对照的原生构建 | test262 通过（goc / 原生） | test262 结果不同的文件 | 官方测试通过（goc / 原生） | 官方测试结果不同的函数 |
|---|---|---:|---:|---:|---:|
| goc-ng | native ng | 1502 / 1502 | 0 | 69 / 69 | 0 |
| goc-bellard | native Bellard | 1501 / 1501 | 0 | 73 / 73 | 0 |

goc-ng 的 CLI 测试（`scripts/qjs-cli-tests.sh`）同样没有变化：115 项通过，1 项 unsupported（`bug1468.js`，和改动前一样）。这套测试是针对 ng 的宿主行为写的，没有在 goc-bellard 上跑。goc-bellard 另外通过了 `tests/qjs` 的冒烟测试和 300 个 goroutine 的栈增长扫描。

上一版报告里 goc（即现在的 goc-ng）的数字是 V8 1128、SunSpider 16.27 ms、microbench 53.2 ns、微调用 22240。这次是 1119、16.68、55.5、21596，同时 native ng 也从 1204、15.28、50.3、23800 变成 1203、15.81、52.3、23337，比值都在上一版的 ±1.5% 以内，属于时段间的波动。

## 参考：clang 编译的 native Bellard

goc-bellard 对 native Bellard 的差距（V8 0.80 倍）明显比 goc-ng 对 native ng（0.93 倍）大。两组对照有一个不对称的地方：native ng 是 clang-19 编译的，native Bellard 按上游 Makefile 用的是 gcc，而 goc 的前端和后端都是 LLVM。为了看清这部分，用同一份 Bellard 2026-06-04 源码、同一个上游 Makefile，只换成 `make CONFIG_CLANG=y CC=clang-19`（`-O2`，其余参数不变，也没有 `-DNDEBUG`），得到 `/workspace/perf-study/bellard/clang-O2/qjs`。然后在主测试之后另跑一个短时段，按同样的方法（`taskset -c 3`、轮换顺序、V8 5 轮、SunSpider 3 轮、microbench 3 轮，取中位数）把 gcc 版、clang 版和 goc-bellard 三家放在一起测。这一时段的原始数据在 `data/raw/bellard-clang/`，数字只在这一节里用，不和上面主时段的表混用。

| 指标 | native Bellard（gcc -O2） | native Bellard（clang-19 -O2） | clang/gcc | goc-bellard | goc-bellard / clang 版 | 怎么读 |
|---|---:|---:|---:|---:|---:|---|
| V8-v7 总分（5 轮中位数） | 1544 | 1327 | 0.859 | 1248 | 0.940 | 分数比，越高越快 |
| SunSpider 几何平均 ms（25 项） | 10.65 | 13.70 | 1.285 | 15.56 | 1.136 | 时间比，越低越快 |
| microbench 几何平均 ns（72 项） | 32.7 | 40.4 | 1.236 | 46.1 | 1.141 | 时间比，越低越快 |

同一份源码，clang 版比 gcc 版慢了一截：V8 总分低 14%，SunSpider 和 microbench 的几何平均慢 29% 和 24%。所以 goc-bellard 对 gcc 版的差距可以大致拆成两个相乘的因子：以 V8 为例，0.86（gcc 换成 clang）× 0.94（goc 相对同为 clang 的原生构建）≈ 0.80；SunSpider 是 1.29 × 1.14 ≈ 1.46，microbench 是 1.24 × 1.14 ≈ 1.40，和主时段的比值（0.80、1.46、1.40）对得上。扣掉编译器这一块后，goc-bellard 相对 clang 版在 V8 上是 0.94 倍，和 goc-ng 相对 native ng 的 0.93 倍同一水平；在 SunSpider 和 microbench 上慢 14%，比 goc-ng 相对 native ng 的 5.5% 和 6.2% 仍大一些。剩下这几个百分点从哪里来、gcc 为什么在 Bellard 的解释器上比 clang 快这么多，后来专门做了一次逐层拆解，见 [perf-gap.md](perf-gap.md)。简单说：gcc 给每个操作码复制了一份分发跳转，clang 和 goc 都共用一个分发块；换成同一个编译器（clang -O3）比，goc-bellard 和 goc-ng 的差距是一样的（V8 慢 5.5% 和 6.1%）。



## 性能优化前后（2026-09-26）

这一节是 2026-09-26 的优化记录，只比 goc-ng（当时叫 goc）和 native ng。它比较的是 goc 自身几种构建的前后差别，Bellard 和 Goja 不在这组构建里，那一时段也没有测它们，所以这张表没有这两列。所有构建在同一时段交替运行，并用 `taskset -c 3` 固定在同一个核上。第 3 行就是现在 goc-ng 的默认构建（此后 goc-ng 的构建参数没有再变）。不同时段的绝对分数不要直接比，看同一张表里的比值。goc-bellard 用的是同一套 goc 参数，没有单独做这组前后对比。

native ng 用 clang-19 `-O2 -DNDEBUG` 编译（完整参数见本页开头）。各列含义如下：

- V8：`bench-v8.js` 总分，越高越快，取 5 轮中位数，括号里是最小到最大。
- 固定工作量：V8-v7 各子项按固定迭代次数运行，统计总耗时（ms），越低越快。每项取 7 轮中的最小值后求和。
- 微调用：`scripts/microcall-bench.sh`，calls/ms，越高越快，取 5 轮中位数。
- SunSpider：几何平均（ms），越低越快，取 3 轮中位数。
- 指令数：callgrind 统计的执行指令总数。

括号外的比值都是 goc-ng 除以 native ng。

| 构建 | V8 中位（最小–最大） | 固定工作量 | 微调用 | SunSpider | 指令数 |
|---|---:|---:|---:|---:|---:|
| native ng | 1203（1169–1221） | 3791 ms | 22954 | 16.46 ms | 17.13G |
| 0 优化前 | 1088（1053–1090） | 4153 / 1.095 | 18791 / 0.82 | 19.27 / 1.17 | 20.95G / 1.22 |
| 1 帧地址修正 + 栈搬移正确性修复 | 1058（1000–1089） | 4152 / 1.095 | 19672 / 0.86 | 18.75 / 1.14 | 21.27G / 1.24 |
| 2 `-DNDEBUG` | 1116（1092–1134） | 3911 / 1.032 | 21554 / 0.94 | 17.30 / 1.05 | 20.40G / 1.19 |
| 3 shim 快速路径（当前默认） | 1119（1095–1150） | 3879 / 1.023 | 21505 / 0.94 | 17.30 / 1.05 | 20.47G / 1.20 |
| 4 改用 O2（未采用） | 1128（1118–1135） | 3905 / 1.030 | 21103 / 0.92 | 17.56 / 1.07 | 20.47G / 1.20 |

在这一轮里，当前默认构建（第 3 行）和 native ng 相比，V8 总分约为 0.93 倍，固定工作量约慢 2.3%，SunSpider 约慢 5%，执行的指令约多 19.5%。

各行说明：

- 第 1 行是帧地址处理的新方案。它用 `goc-llc` 里的后端 pass `GocFrameAddrFix` 取代原来的内联汇编做法。这一行还包含几项栈搬移正确性修复：静态函数的 stackmap 重定位、根的活跃性分析、关闭 tail-merge，以及此前检测逻辑有误、从未真正打上的 `qjs-gstack.patch`。最后这项修复让 5 个与栈深度相关的 CLI 测试由失败变为通过。这些修复有代价，所以这一行比优化前略慢。和“同样修好正确性、但沿用旧汇编方案”的构建相比，新方案的 V8 是 1095 对 1081，固定工作量是 4070 对 4161 ms，指令数是 21.27G 对 22.65G（这组数据来自另一时段）。旧方案仍可用 `GOC_FRAMEADDR_MODE=asm` 打开。
- 第 3 行是 libc shim 的改动：小尺寸 `memcpy`/`memset` 走快速路径，`malloc` 按尺寸分档缓存。第 2 行到第 3 行的差别在噪声范围内，但分配密集的测试（`alloc.js`）快了约 11%。
- 第 4 行的 O2 相对 O3 没有可测收益，已撤回，默认仍是 O3（`GOC_OPT_LEVEL`）。

每一行都通过了同一套正确性检查，结果和优化前逐行一致：test262 抽样 1502/1526，QuickJS 官方测试 69/77，两者都和 native ng 零差异。另外，300 个 goroutine 的栈增长扫描也全部通过。

## V8-v7

![V8-v7 各子项分数](benchmark/charts/v8.png)

`bench-v8.js`（QuickJS 树里 `tests/bench-v8` 的合并版，前面加了一行 `console` 兜底）。每个子项取 5 轮中位数，所以子项之间不一定能算回总分。

| 子项 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard |
|---|---:|---:|---:|---:|---:|---:|---:|
| Richards | 768 | 815 | 796 | 1029 | 265 | 0.96 | 0.79 |
| DeltaBlue | 734 | 830 | 789 | 947 | 276 | 0.93 | 0.88 |
| Crypto | 880 | 931 | 850 | 1377 | 118 | 1.04 | 0.68 |
| RayTrace | 1478 | 1654 | 1787 | 1943 | 245 | 0.83 | 0.85 |
| EarleyBoyer | 2077 | 2311 | 2270 | 2525 | 427 | 0.91 | 0.92 |
| RegExp | 360 | 442 | 400 | 547 | 196 | 0.90 | 0.81 |
| Splay | 2825 | 3240 | 3162 | 3484 | 438 | 0.89 | 0.93 |
| NavierStokes | 1622 | 1759 | 1593 | 2817 | 185 | 1.02 | 0.62 |
| **总分** | **1119** | **1247** | **1203** | **1558** | **247** | **0.930** | **0.800** |

goc-ng 在 Crypto 和 NavierStokes 上与 native ng 持平或略快，差距主要在 RayTrace、Splay、EarleyBoyer 和 RegExp，都在一成左右，和上一版一样。goc-bellard 对 native Bellard 差得最多的是 NavierStokes（0.62）和 Crypto（0.68），这两项都是紧凑的数值循环，最能体现编译器生成的解释器代码的差别；差得最少的是 Splay（0.93）和 EarleyBoyer（0.92），这两项的时间更多花在分配和 GC 上。

各轮总分：

| 引擎 | 各轮总分 | 中位数 | 最小–最大 | 单轮墙钟中位数 |
|---|---|---:|---:|---:|
| goc-ng | 1110 / 1119 / 1126 / 1125 / 1072 | 1119 | 1072–1126 | 38.0 s |
| goc-bellard | 1241 / 1258 / 1247 / 1248 / 1247 | 1247 | 1241–1258 | 37.0 s |
| native ng | 1208 / 1195 / 1189 / 1204 / 1203 | 1203 | 1189–1208 | 37.6 s |
| native Bellard | 1563 / 1558 / 1569 / 1555 / 1538 | 1558 | 1538–1569 | 32.4 s |
| Goja | 235 / 244 / 266 / 261 / 247 | 247 | 235–266 | 124.6 s |

### 微调用

![微调用各用例耗时](benchmark/charts/microcall.png)

`scripts/microcall-bench.sh`，五个引擎都跑（goc-bellard 通过 `GOC_BELLARD_QJS` 加入，Bellard 用 `--stack-size 16M`，Goja 用 `gojacli`）。每个用例的 ms 取 5 轮中位数，越低越快；score 是调用类用例的 calls/ms 几何平均，越高越快，括号里是最小到最大。`arith` 和 `propget` 是对照组，不计入 score。

| 用例 | goc-ng ms | goc-bellard ms | native ng ms | native Bellard ms | Goja ms | goc-ng/ng | goc-bellard/Bellard |
|---|---:|---:|---:|---:|---:|---:|---:|
| arith | 74 | 94 | 77 | 52 | 565 | 0.96 | 1.81 |
| propget | 77 | 88 | 79 | 48 | 426 | 0.97 | 1.83 |
| empty | 67 | 71 | 61 | 46 | 317 | 1.10 | 1.54 |
| id | 71 | 82 | 69 | 48 | 324 | 1.03 | 1.71 |
| six | 62 | 71 | 62 | 47 | 359 | 1.00 | 1.51 |
| eight | 70 | 82 | 71 | 55 | 421 | 0.99 | 1.49 |
| method | 71 | 71 | 69 | 55 | 273 | 1.03 | 1.29 |
| depth4 | 53 | 47 | 45 | 31 | 155 | 1.18 | 1.52 |
| closure | 37 | 46 | 38 | 27 | 178 | 0.97 | 1.70 |
| mutual | 61 | 63 | 53 | 45 | 261 | 1.15 | 1.40 |
| sched | 170 | 133 | 136 | 105 | 441 | 1.25 | 1.27 |
| **score（calls/ms，越高越快）** | **21596**（21305–21906） | **20781**（18317–20995） | **23337**（22742–23599） | **30784**（30436–31562） | **5106**（4983–5163） | **0.93** | **0.68** |

goc-ng 的调用速度是 native ng 的 0.93 倍。差距最大的仍是深调用链（`depth4`）、互相递归（`mutual`）和调度（`sched`），都在一到二成半。goc-bellard 对 native Bellard 是 0.68 倍，但它在对照组 `arith` 和 `propget` 上也慢 1.8 倍，所以这里主要是解释器整体变慢，而不是调用路径特别慢（调用用例的比值 1.3 到 1.7，反而比对照组小）。goc-bellard 的 score 第 3 轮是 18317，明显低于其他四轮（20343–20995），中位数不受影响。Bellard 的 score 是 native ng 的 1.32 倍，Goja 是 0.22 倍；Goja 在对照组 `arith` 上也慢 7 倍多，所以它的差距不只在调用上。

## test262

![test262 有失败的目录](benchmark/charts/test262.png)

tc39/test262 `7ab7faf` 的 `test/language`，由 `scripts/test262-sample.py` 抽样。每项一个新进程，`assert.js` + `sta.js`，直接 `eval`。跳过 `import` / `export` / `module-code`，以及 `module` / `async` / `raw` / `CanBlock`，还有 Atomics、SharedArrayBuffer、agent。每目录均匀抽取最多 80 个正例和 40 个反例。合格池 13898 正例 + 4252 反例，实跑 1526。不是官方全量 harness，也没有每项新 realm。

goc-ng、native ng、native Bellard、Goja 的结果和上一版完全相同。goc-bellard 与 native Bellard 通过数相同（1501），失败的文件也逐个相同。

| 目录 | 合格正例 | 合格反例 | 实跑 | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| arguments-object | 200 | 1 | 81 | 81/81 | 81/81 | 81/81 | 81/81 | 81/81 |
| asi | 67 | 35 | 102 | 102/102 | 102/102 | 102/102 | 102/102 | 102/102 |
| block-scope | 43 | 102 | 83 | 83/83 | 83/83 | 83/83 | 83/83 | 83/83 |
| comments | 21 | 8 | 29 | 29/29 | 29/29 | 29/29 | 29/29 | 28/29 |
| computed-property-names | 48 | 0 | 48 | 48/48 | 48/48 | 48/48 | 48/48 | 48/48 |
| destructuring | 19 | 0 | 19 | 18/19 | 19/19 | 18/19 | 19/19 | 16/19 |
| directive-prologue | 51 | 6 | 57 | 57/57 | 57/57 | 57/57 | 57/57 | 57/57 |
| eval-code | 292 | 3 | 83 | 81/83 | 81/83 | 81/83 | 81/83 | 67/83 |
| expressions | 6903 | 2015 | 120 | 117/120 | 117/120 | 117/120 | 117/120 | 112/120 |
| function-code | 217 | 0 | 80 | 80/80 | 80/80 | 80/80 | 80/80 | 80/80 |
| future-reserved-words | 29 | 26 | 55 | 55/55 | 55/55 | 55/55 | 55/55 | 55/55 |
| global-code | 27 | 15 | 42 | 30/42 | 29/42 | 30/42 | 29/42 | 29/42 |
| identifier-resolution | 12 | 2 | 14 | 13/14 | 13/14 | 13/14 | 13/14 | 14/14 |
| identifiers | 152 | 116 | 120 | 120/120 | 120/120 | 120/120 | 120/120 | 107/120 |
| keywords | 0 | 25 | 25 | 25/25 | 25/25 | 25/25 | 25/25 | 25/25 |
| line-terminators | 17 | 24 | 41 | 41/41 | 41/41 | 41/41 | 41/41 | 41/41 |
| literals | 215 | 321 | 120 | 120/120 | 120/120 | 120/120 | 120/120 | 111/120 |
| punctuators | 1 | 10 | 11 | 11/11 | 11/11 | 11/11 | 11/11 | 11/11 |
| reserved-words | 14 | 12 | 26 | 26/26 | 26/26 | 26/26 | 26/26 | 26/26 |
| rest-parameters | 10 | 1 | 11 | 11/11 | 11/11 | 11/11 | 11/11 | 11/11 |
| source-text | 1 | 0 | 1 | 1/1 | 1/1 | 1/1 | 1/1 | 1/1 |
| statementList | 80 | 0 | 80 | 80/80 | 80/80 | 80/80 | 80/80 | 80/80 |
| statements | 5316 | 1513 | 120 | 119/120 | 118/120 | 119/120 | 118/120 | 114/120 |
| types | 102 | 11 | 91 | 87/91 | 87/91 | 87/91 | 87/91 | 87/91 |
| white-space | 61 | 6 | 67 | 67/67 | 67/67 | 67/67 | 67/67 | 67/67 |

goc-ng 与 native ng 的失败路径相同，goc-bellard 与 native Bellard 的失败路径相同。共同缺口是旧式全局 `var` 属性、少量该抛未抛的早期错误、3 个 `$262.createRealm`、2 个模块片段、1 个 decorator。Bellard 的两个构建比 ng 多过一个 destructuring 求值顺序测试，另失败 `decl-lex-restricted-global` 和 `using/static-init-await-binding-valid`。Goja 多出来的主要是 async generator、class fields、regexp `v` flag。

失败清单：

| 引擎 | 路径 | 结果 |
|---|---|---|
| goc-ng | destructuring/binding/keyed-destructuring-property-reference-target-evaluation-order-with-bindings.js | FAIL undefined: Test262Error: Actual [binding::source, binding::sourceKey, sourceKey, get source, binding::defaultValue, binding::varTarget] |
| goc-ng | eval-code/indirect/lex-env-heritage.js | FAIL ReferenceError: ReferenceError: x is not defined |
| goc-ng | eval-code/indirect/var-env-global-lex-non-strict.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «undefined») to be false |
| goc-ng | expressions/class/private-setter-brand-check-multiple-evaluations-of-class-realm-function-ctor.js | FAIL Error: Error: $262.createRealm |
| goc-ng | expressions/dynamic-import/assignment-expression/module-code-other_FIXTURE.js | FAIL SyntaxError: SyntaxError: unsupported keyword: export |
| goc-ng | expressions/dynamic-import/syntax/valid/nested-else-import-defer-script-code-valid.js | FAIL SyntaxError: SyntaxError: meta expected |
| goc-ng | global-code/S10.4.1_A1_T1.js | FAIL undefined: Test262Error: #2: variable x has property attribute DontDelete |
| goc-ng | global-code/decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-ng | global-code/decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-ng | global-code/script-decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-ng | global-code/script-decl-lex-deletion.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| goc-ng | global-code/script-decl-lex-lex.js | FAIL undefined: Test262Error: `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-ng | global-code/script-decl-lex-restricted-global.js | FAIL undefined: Test262Error: Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-ng | global-code/script-decl-lex-var-declared-via-eval.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «1») to be true |
| goc-ng | global-code/script-decl-lex-var.js | FAIL undefined: Test262Error: variable Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-ng | global-code/script-decl-lex.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| goc-ng | global-code/script-decl-var-collision.js | FAIL undefined: Test262Error: `var` on `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-ng | global-code/script-decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-ng | identifier-resolution/assign-to-global-undefined.js | FAIL expected ReferenceError |
| goc-ng | statements/class/decorator/syntax/valid/decorator-parenthesized-expr-identifier-reference.js | FAIL SyntaxError: SyntaxError: unexpected token in expression: '@' |
| goc-ng | types/reference/S8.7.1_A2.js | FAIL undefined: Test262Error: #1: y = 1; (delete y) === false. Actual: true |
| goc-ng | types/reference/S8.7_A5_T1.js | FAIL undefined: Test262Error: #3: obj = new Object(); var __ref = obj; delete __ref === false. Actual: true |
| goc-ng | types/reference/get-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| goc-ng | types/reference/put-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| goc-bellard | eval-code/indirect/lex-env-heritage.js | FAIL ReferenceError: ReferenceError: 'x' is not defined |
| goc-bellard | eval-code/indirect/var-env-global-lex-non-strict.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «undefined») to be false |
| goc-bellard | expressions/class/private-setter-brand-check-multiple-evaluations-of-class-realm-function-ctor.js | FAIL Error: Error: $262.createRealm |
| goc-bellard | expressions/dynamic-import/assignment-expression/module-code-other_FIXTURE.js | FAIL SyntaxError: SyntaxError: unsupported keyword: export |
| goc-bellard | expressions/dynamic-import/syntax/valid/nested-else-import-defer-script-code-valid.js | FAIL SyntaxError: SyntaxError: meta expected |
| goc-bellard | global-code/S10.4.1_A1_T1.js | FAIL undefined: Test262Error: #2: variable x has property attribute DontDelete |
| goc-bellard | global-code/decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-bellard | global-code/decl-lex-restricted-global.js | FAIL expected SyntaxError |
| goc-bellard | global-code/decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-bellard | global-code/script-decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-bellard | global-code/script-decl-lex-deletion.js | FAIL ReferenceError: ReferenceError: 'test262let' is not defined |
| goc-bellard | global-code/script-decl-lex-lex.js | FAIL undefined: Test262Error: `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-bellard | global-code/script-decl-lex-restricted-global.js | FAIL undefined: Test262Error: Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-bellard | global-code/script-decl-lex-var-declared-via-eval.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «1») to be true |
| goc-bellard | global-code/script-decl-lex-var.js | FAIL undefined: Test262Error: variable Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-bellard | global-code/script-decl-lex.js | FAIL ReferenceError: ReferenceError: 'test262let' is not defined |
| goc-bellard | global-code/script-decl-var-collision.js | FAIL undefined: Test262Error: `var` on `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| goc-bellard | global-code/script-decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| goc-bellard | identifier-resolution/assign-to-global-undefined.js | FAIL expected ReferenceError |
| goc-bellard | statements/class/decorator/syntax/valid/decorator-parenthesized-expr-identifier-reference.js | FAIL SyntaxError: SyntaxError: unexpected token in expression: '@' |
| goc-bellard | statements/using/static-init-await-binding-valid.js | FAIL SyntaxError: SyntaxError: expecting ';' |
| goc-bellard | types/reference/S8.7.1_A2.js | FAIL undefined: Test262Error: #1: y = 1; (delete y) === false. Actual: true |
| goc-bellard | types/reference/S8.7_A5_T1.js | FAIL undefined: Test262Error: #3: obj = new Object(); var __ref = obj; delete __ref === false. Actual: true |
| goc-bellard | types/reference/get-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| goc-bellard | types/reference/put-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| native ng | destructuring/binding/keyed-destructuring-property-reference-target-evaluation-order-with-bindings.js | FAIL undefined: Test262Error: Actual [binding::source, binding::sourceKey, sourceKey, get source, binding::defaultValue, binding::varTarget] |
| native ng | eval-code/indirect/lex-env-heritage.js | FAIL ReferenceError: ReferenceError: x is not defined |
| native ng | eval-code/indirect/var-env-global-lex-non-strict.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «undefined») to be false |
| native ng | expressions/class/private-setter-brand-check-multiple-evaluations-of-class-realm-function-ctor.js | FAIL Error: Error: $262.createRealm |
| native ng | expressions/dynamic-import/assignment-expression/module-code-other_FIXTURE.js | FAIL SyntaxError: SyntaxError: unsupported keyword: export |
| native ng | expressions/dynamic-import/syntax/valid/nested-else-import-defer-script-code-valid.js | FAIL SyntaxError: SyntaxError: meta expected |
| native ng | global-code/S10.4.1_A1_T1.js | FAIL undefined: Test262Error: #2: variable x has property attribute DontDelete |
| native ng | global-code/decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native ng | global-code/decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native ng | global-code/script-decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native ng | global-code/script-decl-lex-deletion.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| native ng | global-code/script-decl-lex-lex.js | FAIL undefined: Test262Error: `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| native ng | global-code/script-decl-lex-restricted-global.js | FAIL undefined: Test262Error: Expected a SyntaxError to be thrown but no exception was thrown at all |
| native ng | global-code/script-decl-lex-var-declared-via-eval.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «1») to be true |
| native ng | global-code/script-decl-lex-var.js | FAIL undefined: Test262Error: variable Expected a SyntaxError to be thrown but no exception was thrown at all |
| native ng | global-code/script-decl-lex.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| native ng | global-code/script-decl-var-collision.js | FAIL undefined: Test262Error: `var` on `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| native ng | global-code/script-decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native ng | identifier-resolution/assign-to-global-undefined.js | FAIL expected ReferenceError |
| native ng | statements/class/decorator/syntax/valid/decorator-parenthesized-expr-identifier-reference.js | FAIL SyntaxError: SyntaxError: unexpected token in expression: '@' |
| native ng | types/reference/S8.7.1_A2.js | FAIL undefined: Test262Error: #1: y = 1; (delete y) === false. Actual: true |
| native ng | types/reference/S8.7_A5_T1.js | FAIL undefined: Test262Error: #3: obj = new Object(); var __ref = obj; delete __ref === false. Actual: true |
| native ng | types/reference/get-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| native ng | types/reference/put-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| native Bellard | eval-code/indirect/lex-env-heritage.js | FAIL ReferenceError: ReferenceError: 'x' is not defined |
| native Bellard | eval-code/indirect/var-env-global-lex-non-strict.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «undefined») to be false |
| native Bellard | expressions/class/private-setter-brand-check-multiple-evaluations-of-class-realm-function-ctor.js | FAIL Error: Error: $262.createRealm |
| native Bellard | expressions/dynamic-import/assignment-expression/module-code-other_FIXTURE.js | FAIL SyntaxError: SyntaxError: unsupported keyword: export |
| native Bellard | expressions/dynamic-import/syntax/valid/nested-else-import-defer-script-code-valid.js | FAIL SyntaxError: SyntaxError: meta expected |
| native Bellard | global-code/S10.4.1_A1_T1.js | FAIL undefined: Test262Error: #2: variable x has property attribute DontDelete |
| native Bellard | global-code/decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native Bellard | global-code/decl-lex-restricted-global.js | FAIL expected SyntaxError |
| native Bellard | global-code/decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native Bellard | global-code/script-decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native Bellard | global-code/script-decl-lex-deletion.js | FAIL ReferenceError: ReferenceError: 'test262let' is not defined |
| native Bellard | global-code/script-decl-lex-lex.js | FAIL undefined: Test262Error: `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| native Bellard | global-code/script-decl-lex-restricted-global.js | FAIL undefined: Test262Error: Expected a SyntaxError to be thrown but no exception was thrown at all |
| native Bellard | global-code/script-decl-lex-var-declared-via-eval.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «1») to be true |
| native Bellard | global-code/script-decl-lex-var.js | FAIL undefined: Test262Error: variable Expected a SyntaxError to be thrown but no exception was thrown at all |
| native Bellard | global-code/script-decl-lex.js | FAIL ReferenceError: ReferenceError: 'test262let' is not defined |
| native Bellard | global-code/script-decl-var-collision.js | FAIL undefined: Test262Error: `var` on `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| native Bellard | global-code/script-decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| native Bellard | identifier-resolution/assign-to-global-undefined.js | FAIL expected ReferenceError |
| native Bellard | statements/class/decorator/syntax/valid/decorator-parenthesized-expr-identifier-reference.js | FAIL SyntaxError: SyntaxError: unexpected token in expression: '@' |
| native Bellard | statements/using/static-init-await-binding-valid.js | FAIL SyntaxError: SyntaxError: expecting ';' |
| native Bellard | types/reference/S8.7.1_A2.js | FAIL undefined: Test262Error: #1: y = 1; (delete y) === false. Actual: true |
| native Bellard | types/reference/S8.7_A5_T1.js | FAIL undefined: Test262Error: #3: obj = new Object(); var __ref = obj; delete __ref === false. Actual: true |
| native Bellard | types/reference/get-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| native Bellard | types/reference/put-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| Goja | comments/hashbang/function-constructor.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:16:33 |
| Goja | destructuring/binding/syntax/destructuring-array-parameters-function-arguments-length.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:34:19 |
| Goja | destructuring/binding/syntax/destructuring-object-parameters-function-arguments-length.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:34:19 |
| Goja | destructuring/binding/typedarray-backed-by-resizable-buffer.js | FAIL TypeError: TypeError: Object has no member 'resize' |
| Goja | eval-code/direct/async-gen-func-decl-a-preceding-parameter-is-named-arguments-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:1 |
| Goja | eval-code/direct/async-gen-func-decl-fn-body-cntns-arguments-lex-bind-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:1 |
| Goja | eval-code/direct/async-gen-func-decl-no-pre-existing-arguments-bindings-are-present-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:1 |
| Goja | eval-code/direct/async-gen-func-expr-a-preceding-parameter-is-named-arguments-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-func-expr-fn-body-cntns-arguments-func-decl-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-func-expr-fn-body-cntns-arguments-var-bind-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-meth-a-following-parameter-is-named-arguments-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 12:18 Unexpected token * (and 9 more errors) |
| Goja | eval-code/direct/async-gen-meth-fn-body-cntns-arguments-func-decl-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 12:18 Unexpected token * (and 12 more errors) |
| Goja | eval-code/direct/async-gen-meth-fn-body-cntns-arguments-var-bind-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 12:18 Unexpected token * (and 16 more errors) |
| Goja | eval-code/direct/async-gen-named-func-expr-a-following-parameter-is-named-arguments-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-named-func-expr-fn-body-cntns-arguments-func-decl-declare-arguments-and-assign.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-named-func-expr-fn-body-cntns-arguments-lex-bind-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/async-gen-named-func-expr-no-pre-existing-arguments-bindings-are-present-declare-arguments.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:12:9 |
| Goja | eval-code/direct/var-env-global-lex-non-strict.js | FAIL expected SyntaxError |
| Goja | eval-code/indirect/lex-env-heritage.js | FAIL ReferenceError: ReferenceError: x is not defined |
| Goja | eval-code/indirect/var-env-global-lex-non-strict.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «undefined») to be false |
| Goja | expressions/async-generator/dstr/dflt-ary-ptrn-elision-step-err.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:43:9 |
| Goja | expressions/async-generator/named-dflt-params-ref-self.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:35:5 |
| Goja | expressions/class/dstr/async-gen-meth-obj-ptrn-prop-id-init-unresolvable.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:62:3 |
| Goja | expressions/class/private-setter-brand-check-multiple-evaluations-of-class-realm-function-ctor.js | FAIL Error: Error: $262.createRealm |
| Goja | expressions/dynamic-import/assignment-expression/module-code-other_FIXTURE.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 6:1 Unexpected reserved word (and 2 more errors) |
| Goja | expressions/dynamic-import/syntax/valid/nested-async-arrow-function-await-nested-imports.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 26:9 Unexpected reserved word (and 2 more errors) |
| Goja | expressions/dynamic-import/syntax/valid/nested-else-import-defer-script-code-valid.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 28:3 Unexpected reserved word (and 1 more errors) |
| Goja | expressions/object/dstr/async-gen-meth-obj-init-undefined.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 32:10 Unexpected token * (and 4 more errors) |
| Goja | global-code/S10.4.1_A1_T1.js | FAIL undefined: Test262Error: #2: variable x has property attribute DontDelete |
| Goja | global-code/decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| Goja | global-code/decl-lex-restricted-global.js | FAIL expected SyntaxError |
| Goja | global-code/decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| Goja | global-code/script-decl-func.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| Goja | global-code/script-decl-lex-deletion.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| Goja | global-code/script-decl-lex-lex.js | FAIL undefined: Test262Error: `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| Goja | global-code/script-decl-lex-restricted-global.js | FAIL undefined: Test262Error: Expected a SyntaxError to be thrown but no exception was thrown at all |
| Goja | global-code/script-decl-lex-var-declared-via-eval.js | FAIL undefined: Test262Error: Expected SameValue(«undefined», «1») to be true |
| Goja | global-code/script-decl-lex-var.js | FAIL undefined: Test262Error: variable Expected a SyntaxError to be thrown but no exception was thrown at all |
| Goja | global-code/script-decl-lex.js | FAIL ReferenceError: ReferenceError: test262let is not defined |
| Goja | global-code/script-decl-var-collision.js | FAIL undefined: Test262Error: `var` on `let` binding Expected a SyntaxError to be thrown but no exception was thrown at all |
| Goja | global-code/script-decl-var.js | FAIL undefined: Test262Error: brandNew descriptor should not be configurable |
| Goja | identifiers/part-unicode-15.1.0-class-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 19:22 Unexpected token ILLEGAL (and 1 more errors) |
| Goja | identifiers/part-unicode-15.1.0-class.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:11 Unexpected token ILLEGAL (and 4 more errors) |
| Goja | identifiers/part-unicode-15.1.0.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 14:12 Unexpected token ILLEGAL (and 3 more errors) |
| Goja | identifiers/part-unicode-16.0.0-class.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:5 Unexpected token ILLEGAL (and 133 more errors) |
| Goja | identifiers/part-unicode-16.0.0.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 14:6 Unexpected token ILLEGAL (and 132 more errors) |
| Goja | identifiers/part-unicode-17.0.0-class.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:5 Unexpected token ILLEGAL (and 54 more errors) |
| Goja | identifiers/part-unicode-17.0.0.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 14:6 Unexpected token ILLEGAL (and 53 more errors) |
| Goja | identifiers/start-unicode-15.1.0-class.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:4 Unexpected token ILLEGAL (and 1865 more errors) |
| Goja | identifiers/start-unicode-15.1.0-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:5 Unexpected token ILLEGAL (and 1242 more errors) |
| Goja | identifiers/start-unicode-16.0.0-class-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 19:9 Unexpected token ILLEGAL (and 9 more errors) |
| Goja | identifiers/start-unicode-16.0.0-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:5 Unexpected token ILLEGAL (and 8602 more errors) |
| Goja | identifiers/start-unicode-17.0.0-class-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 19:9 Unexpected token ILLEGAL (and 9 more errors) |
| Goja | identifiers/start-unicode-17.0.0-escaped.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 16:5 Unexpected token ILLEGAL (and 9292 more errors) |
| Goja | literals/regexp/S7.8.5_A1.4_T2.js | FAIL undefined: Test262Error: Code unit: d800 Expected SameValue(«"\\\\\\ud800"», «"\\\ud800"») to be true |
| Goja | literals/regexp/S7.8.5_A2.1_T2.js | FAIL undefined: Test262Error: Code unit: d800 Expected SameValue(«"nnnn\\ud800"», «"nnnn\ud800"») to be true |
| Goja | literals/regexp/invalid-range-lookbehind.js | FAIL expected SyntaxError got String: Test262: This statement should not be evaluated. |
| Goja | literals/regexp/u-case-mapping.js | FAIL undefined: Test262Error: Case mapping is not applied in the absence of the `u` flag Expected SameValue(«true», «false») to be true |
| Goja | literals/regexp/u-invalid-range-lookahead.js | FAIL expected SyntaxError got String: Test262: This statement should not be evaluated. |
| Goja | literals/string/S7.8.4_A4.3_T2.js | FAIL expected SyntaxError got String: Test262: This statement should not be evaluated. |
| Goja | literals/string/legacy-non-octal-escape-sequence-2-strict-explicit-pragma.js | FAIL expected SyntaxError got String: Test262: This statement should not be evaluated. |
| Goja | literals/string/legacy-non-octal-escape-sequence-9-strict-explicit-pragma.js | FAIL expected SyntaxError got String: Test262: This statement should not be evaluated. |
| Goja | literals/string/legacy-octal-escape-sequence.js | FAIL undefined: Test262Error: \400 Expected SameValue(«"Ā"», «" 0"») to be true |
| Goja | statements/class/decorator/syntax/valid/decorator-parenthesized-expr-identifier-reference.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 54:1 Unexpected token ILLEGAL (and 7 more errors) |
| Goja | statements/class/dstr/async-gen-meth-dflt-ary-ptrn-elem-id-iter-val-err.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:74:3 |
| Goja | statements/class/dstr/async-gen-meth-static-dflt-obj-ptrn-prop-id-init-throws.js | FAIL SyntaxError: SyntaxError: SyntaxError: Async generators are not supported yet at <eval>:57:10 |
| Goja | statements/for-await-of/let-block-with-newline.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 17:7 Unexpected token await (and 10 more errors) |
| Goja | statements/let/global-closure-set-before-initialization.js | FAIL undefined: Test262Error: Expected a ReferenceError to be thrown but no exception was thrown at all |
| Goja | statements/using/static-init-await-binding-valid.js | FAIL SyntaxError: SyntaxError: SyntaxError: <eval>: Line 17:20 Unexpected token await (and 5 more errors) |
| Goja | types/reference/S8.7.1_A2.js | FAIL undefined: Test262Error: #1: y = 1; (delete y) === false. Actual: true |
| Goja | types/reference/S8.7_A5_T1.js | FAIL undefined: Test262Error: #3: obj = new Object(); var __ref = obj; delete __ref === false. Actual: true |
| Goja | types/reference/get-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |
| Goja | types/reference/put-value-prop-base-primitive-realm.js | FAIL Error: Error: $262.createRealm |

## QuickJS 官方测试

![QuickJS 官方测试各文件通过数](benchmark/charts/qjs-tests.png)

Bellard 2026-06-04 的 `tests/test_language.js`、`test_closure.js`、`test_loop.js`、`test_bigint.js`、`test_builtin.js`，由 `scripts/qjs-official-tests.py` 拆成每个函数一个进程。`std` / `os` 五家都没有，所以相关函数一起失败。goc-ng 与 native ng 逐函数一致，goc-bellard 与 native Bellard 逐函数一致（通过/失败相同；失败信息前面多了 qjscli 的 `qjscli:runtime: <t>:` 前缀）。

| 文件 | n | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---:|---:|---:|---:|---:|---:|
| test_language.js | 27 | 26 | 27 | 26 | 27 | 23 |
| test_closure.js | 7 | 7 | 7 | 7 | 7 | 6 |
| test_loop.js | 18 | 17 | 18 | 17 | 18 | 17 |
| test_bigint.js | 4 | 4 | 4 | 4 | 4 | 3 |
| test_builtin.js | 21 | 15 | 17 | 15 | 17 | 9 |

逐函数。`pass` 以外的格子是失败信息。

| 文件 | 函数 | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---|---|---|---|---|---|
| test_language | test_op1 | pass | pass | pass | pass | pass |
| test_language | test_cvt | pass | pass | pass | pass | pass |
| test_language | test_eq | pass | pass | pass | pass | pass |
| test_language | test_inc_dec | pass | pass | pass | pass | pass |
| test_language | test_op2 | pass | pass | pass | pass | pass |
| test_language | test_constructor | pass | pass | pass | pass | FAIL Error: assertion failed: got \|Value is not a constructor\|, expected \|G i |
| test_language | test_delete | pass | pass | pass | pass | pass |
| test_language | test_prototype | pass | pass | pass | pass | pass |
| test_language | test_arguments | pass | pass | pass | pass | pass |
| test_language | test_class | pass | pass | pass | pass | FAIL SyntaxError: SyntaxError: <t>: Line 91:15 Unexpected token = (and 3 more er |
| test_language | test_template | pass | pass | pass | pass | pass |
| test_language | test_template_skip | pass | pass | pass | pass | pass |
| test_language | test_object_literal | pass | pass | pass | pass | pass |
| test_language | test_regexp_skip | pass | pass | pass | pass | pass |
| test_language | test_labels | pass | pass | pass | pass | pass |
| test_language | test_labels2 | pass | pass | pass | pass | pass |
| test_language | test_destructuring | pass | pass | pass | pass | pass |
| test_language | test_spread | pass | pass | pass | pass | pass |
| test_language | test_function_length | pass | pass | pass | pass | pass |
| test_language | test_argument_scope | pass | pass | pass | pass | FAIL Error: assertion failed: got \|undefined\|, expected \|12\| at Error (nativ |
| test_language | test_function_expr_name | pass | pass | pass | pass | pass |
| test_language | test_parse_semicolon | pass | pass | pass | pass | pass |
| test_language | test_optional_chaining | pass | pass | pass | pass | FAIL Error: assertion failed: got \|{"b":{"c":2}}\|, expected \|{"b":{}}\| (opti |
| test_language | test_parse_arrow_function | pass | pass | pass | pass | pass |
| test_language | test_unicode_ident | FAIL qjscli:runtime: <t>: Error: assertion failed: got \|number\|, expected \|un | pass | FAIL Error: assertion failed: got \|number\|, expected \|undefined\| | pass | pass |
| test_language | test_global_var_opt | pass | pass | pass | pass | pass |
| test_language | test_number_literals | pass | pass | pass | pass | pass |
| test_closure | test_closure1 | pass | pass | pass | pass | pass |
| test_closure | test_closure2 | pass | pass | pass | pass | pass |
| test_closure | test_closure3 | pass | pass | pass | pass | pass |
| test_closure | test_arrow_function | pass | pass | pass | pass | FAIL Error: assertion failed: got \|4\|, expected \|2\| at Error (native) |
| test_closure | test_with | pass | pass | pass | pass | pass |
| test_closure | test_eval_closure | pass | pass | pass | pass | pass |
| test_closure | test_eval_const | pass | pass | pass | pass | pass |
| test_loop | test_while | pass | pass | pass | pass | pass |
| test_loop | test_while_break | pass | pass | pass | pass | pass |
| test_loop | test_do_while | pass | pass | pass | pass | pass |
| test_loop | test_for | pass | pass | pass | pass | pass |
| test_loop | test_for_break | pass | pass | pass | pass | pass |
| test_loop | test_switch1 | pass | pass | pass | pass | pass |
| test_loop | test_switch2 | pass | pass | pass | pass | pass |
| test_loop | test_for_in | pass | pass | pass | pass | FAIL SyntaxError: SyntaxError: <t>: Line 90:17 for-in loop variable declaration  |
| test_loop | test_for_in2 | pass | pass | pass | pass | pass |
| test_loop | test_for_in_proxy | FAIL qjscli:runtime: <t>: Error: assertion failed: got \|false\|, expected \|tru | pass | FAIL Error: assertion failed: got \|false\|, expected \|true\| | pass | pass |
| test_loop | test_try_catch1 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch2 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch3 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch4 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch5 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch6 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch7 | pass | pass | pass | pass | pass |
| test_loop | test_try_catch8 | pass | pass | pass | pass | pass |
| test_bigint | test_bigint1 | pass | pass | pass | pass | pass |
| test_bigint | test_bigint2 | pass | pass | pass | pass | pass |
| test_bigint | test_bigint3 | pass | pass | pass | pass | FAIL Error: assertion failed: got \|-1\|, expected \|18446744073709552000\| at E |
| test_bigint | test_pi | pass | pass | pass | pass | pass |
| test_builtin | test | pass | pass | pass | pass | pass |
| test_builtin | test_function | pass | pass | pass | pass | pass |
| test_builtin | test_enum | pass | pass | pass | pass | pass |
| test_builtin | test_array | pass | pass | pass | pass | pass |
| test_builtin | test_string | pass | pass | pass | pass | FAIL panic: unexpected unicode length while parsing '\u{10ffff}' |
| test_builtin | test_math | pass | pass | pass | pass | FAIL TypeError: Object has no member 'sumPrecise' at test_math (<t>:68:27(137)) |
| test_builtin | test_number | pass | pass | pass | pass | pass |
| test_builtin | test_eval | pass | pass | pass | pass | FAIL TypeError: Cannot read property 'length' of undefined at <eval>:1:11(2) |
| test_builtin | test_typed_array | pass | pass | pass | pass | FAIL ReferenceError: Float16Array is not defined at test_typed_array (<t>:139:13 |
| test_builtin | test_json | FAIL qjscli:runtime: <t>: Error: unexpected line or column number. error=Bad esc | pass | FAIL Error: unexpected line or column number. error=Bad escaped character in JSO | pass | FAIL Error: unexpected line or column number. error=invalid character 'x' in str |
| test_builtin | test_date | pass | pass | pass | pass | FAIL Error: assertion failed: got number:\|29256\|, expected number:\|29312\| (o |
| test_builtin | test_regexp | pass | pass | pass | pass | FAIL SyntaxError: SyntaxError: Invalid flags supplied to RegExp constructor 'gvi |
| test_builtin | test_symbol | pass | pass | pass | pass | pass |
| test_builtin | test_map | pass | pass | pass | pass | pass |
| test_builtin | test_weak_map | FAIL qjscli:runtime: <t>: ReferenceError: std is not defined | FAIL qjscli:runtime: <t>: ReferenceError: 'std' is not defined | FAIL ReferenceError: std is not defined | FAIL ReferenceError: 'std' is not defined | FAIL TypeError: Value is not an object: x1 at set (native) |
| test_builtin | test_weak_map_cycles | FAIL qjscli:runtime: <t>: ReferenceError: std is not defined | FAIL qjscli:runtime: <t>: ReferenceError: 'std' is not defined | FAIL ReferenceError: std is not defined | FAIL ReferenceError: 'std' is not defined | FAIL ReferenceError: std is not defined at test_weak_map_cycles (<t>:24:5(15)) |
| test_builtin | test_weak_ref | FAIL qjscli:runtime: <t>: ReferenceError: std is not defined | FAIL qjscli:runtime: <t>: ReferenceError: 'std' is not defined | FAIL ReferenceError: std is not defined | FAIL ReferenceError: 'std' is not defined | FAIL ReferenceError: WeakRef is not defined at test_weak_ref (<t>:61:18(18)) |
| test_builtin | test_finalization_registry | FAIL qjscli:runtime: <t>: ReferenceError: os is not defined | FAIL qjscli:runtime: <t>: ReferenceError: 'os' is not defined | FAIL ReferenceError: os is not defined | FAIL ReferenceError: 'os' is not defined | FAIL ReferenceError: FinalizationRegistry is not defined at test_finalization_re |
| test_builtin | test_generator | pass | pass | pass | pass | pass |
| test_builtin | test_rope | pass | pass | pass | pass | pass |
| test_builtin | test_line_column_numbers | FAIL qjscli:runtime: <t>: Error: unexpected line or column number. error=hello.g | pass | FAIL Error: unexpected line or column number. error=hello.got \|    at <eval> (< | pass | FAIL Error: unexpected line or column number. error=SyntaxError: <eval>: Line 2: |

## SunSpider 1.0.2

![SunSpider 各项相对 native ng 的耗时](benchmark/charts/sunspider.png)

![SunSpider 五引擎绝对耗时，对数轴](benchmark/charts/sunspider-log.png)

第一张图是每项耗时除以 native ng 的耗时（对数轴，虚线是 native ng），第二张是五家的绝对耗时（对数轴）。两张图都包含 Goja。

WebKit `sunspider-1.0.2`，由 `scripts/sunspider-wrap.py` 包装：每个文件包进一个函数，`document.write` 打了桩。先连续调用 150 ms 定下次数 n，再跑 5 批、每批 n 次，取最快一批的平均单次耗时，用 `Date.now` 计时。ms/次，越低越快，每项取 3 轮中位数。

Goja 的 `3d-cube` 仍然失败（向量和与期望值差在最后几位），所以五家的几何平均是 25 项，不含 Goja 的四个 QuickJS 构建的几何平均是 26 项。两个 goc 构建在全部 26 项上都通过。

| 测试 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard |
|---|---:|---:|---:|---:|---:|---:|---:|
| 3d-cube | 27.83 | 26.17 | 26.83 | 18.12 | 失败 | 1.04 | 1.44 |
| 3d-morph | 32.20 | 27.00 | 21.57 | 13.08 | 143.50 | 1.49 | 2.06 |
| 3d-raytrace | 2.50 | 2.38 | 2.38 | 1.27 | 62.00 | 1.05 | 1.87 |
| access-binary-trees | 13.64 | 13.17 | 12.92 | 10.36 | 59.33 | 1.06 | 1.27 |
| access-fannkuch | 73.50 | 45.00 | 66.33 | 30.40 | 160.00 | 1.11 | 1.48 |
| access-nbody | 18.50 | 20.50 | 16.67 | 12.58 | 154.00 | 1.11 | 1.63 |
| access-nsieve | 34.80 | 23.29 | 35.80 | 17.00 | 107.00 | 0.97 | 1.37 |
| bitops-3bit-bits-in-byte | 10.67 | 12.75 | 11.64 | 7.79 | 79.00 | 0.92 | 1.64 |
| bitops-bits-in-byte | 24.67 | 27.80 | 25.33 | 20.12 | 99.00 | 0.97 | 1.38 |
| bitops-bitwise-and | 12.42 | 14.91 | 15.10 | 7.63 | 138.00 | 0.82 | 1.95 |
| bitops-nsieve-bits | 20.38 | 21.86 | 21.29 | 13.45 | 218.00 | 0.96 | 1.62 |
| controlflow-recursive | 7.70 | 8.67 | 7.43 | 5.44 | 23.67 | 1.04 | 1.59 |
| crypto-aes | 23.57 | 16.89 | 22.33 | 12.38 | 92.50 | 1.06 | 1.36 |
| crypto-md5 | 7.50 | 8.50 | 8.11 | 5.59 | 71.33 | 0.93 | 1.52 |
| crypto-sha1 | 7.37 | 8.28 | 8.00 | 5.21 | 68.67 | 0.92 | 1.59 |
| date-format-tofte | 6.80 | 6.60 | 5.80 | 4.17 | 37.00 | 1.17 | 1.58 |
| date-format-xparb | 2.38 | 1.78 | 3.00 | 2.38 | 10.75 | 0.79 | 0.75 |
| math-cordic | 27.33 | 32.60 | 27.50 | 22.50 | 149.00 | 0.99 | 1.45 |
| math-partial-sums | 20.38 | 19.88 | 13.36 | 9.44 | 119.50 | 1.52 | 2.11 |
| math-spectral-norm | 9.50 | 10.43 | 9.50 | 7.20 | 61.00 | 1.00 | 1.45 |
| regexp-dna | 48.00 | 49.00 | 43.00 | 36.50 | 185.00 | 1.12 | 1.34 |
| string-base64 | 14.90 | 14.09 | 14.09 | 11.83 | 155.00 | 1.06 | 1.19 |
| string-fasta | 54.67 | 26.00 | 50.00 | 22.43 | 143.00 | 1.09 | 1.16 |
| string-tagcloud | 34.20 | 27.67 | 29.80 | 21.43 | 259.00 | 1.15 | 1.29 |
| string-unpack-code | 52.00 | 43.50 | 48.00 | 39.00 | 75.00 | 1.08 | 1.12 |
| string-validate-input | 17.33 | 15.50 | 13.33 | 10.86 | 414.00 | 1.30 | 1.43 |
| **几何平均 25 项（五家都通过）** | **16.68** | **15.50** | **15.81** | **10.64** | **97.27** | **1.05** | **1.46** |
| **几何平均 26 项（四个 QuickJS 构建都通过，不含 Goja）** | **17.01** | **15.81** | **16.13** | **10.86** | — | **1.05** | **1.46** |

goc-ng 在 bitops、crypto-md5/sha1 和 date-format-xparb 上比 native ng 快。慢得最多的是 `math-partial-sums`、`3d-morph`、`string-validate-input` 和 `date-format-tofte`。前两项和最后一项大量调用 `Math.*` 或 `Date`，goc 的 libm 和时间函数要经过桥接调用 glibc，差距可能来自这里；这一点没有用 profile 确认。goc-bellard 对 native Bellard 最慢的也是 `math-partial-sums`（2.11）和 `3d-morph`（2.06），外加 `bitops-bitwise-and`（1.95）和 `3d-raytrace`（1.87）；只有 `date-format-xparb` 比 native Bellard 快。

各轮的几何平均（同一批 25 项）：

| 引擎 | 各轮几何平均（25 项） |
|---|---|
| goc-ng | 16.73 / 17.23 / 16.68 |
| goc-bellard | 15.25 / 15.62 / 16.08 |
| native ng | 15.65 / 15.80 / 15.94 |
| native Bellard | 10.68 / 10.71 / 10.61 |
| Goja | 98.54 / 96.59 / 97.24 |

## microbench

![microbench 分组](benchmark/charts/micro-groups.png)

![microbench 各项相对 native ng 的耗时](benchmark/charts/micro-ratio.png)

第二张图是每项耗时除以 native ng 的耗时，goc-ng、goc-bellard、native Bellard、Goja 四家并排（对数轴，虚线是 native ng），按 goc-ng 的比值从慢到快排。

Bellard 树的 `tests/microbench.js`，前面加了一行 `console` 兜底（goc 的 qjscli 没有 `console` 全局）。TIME 列，ns/op，越低越快，每项取 3 轮中位数。没有参考文件，所以 SCORE 列是空的。

- 五家都用 `performance.now` 计时。goc-bellard 的 `performance.now` 由 qjscli 宿主提供（commit `208df3b`），Goja 的由 `scripts/gojacli` 提供。
- Goja 把 `Date.prototype.toGMTString` 指到 `toUTCString` 才能跑完（`gojacli --micro`），否则会停在 `date_parse`。

|  | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard |
|---|---:|---:|---:|---:|---:|---:|---:|
| TIME 总和（中位数） | 14452 | 5935 | 15168 | 4509 | 39428 | 0.95 | 1.32 |
| 几何平均 72 项 | 55.5 | 45.4 | 52.3 | 32.4 | 195.6 | 1.06 | 1.40 |

TIME 总和被 `map_set_int` 和 `map_set_bigint` 这两项主导：native ng 和 goc-ng 在这两项上都要 3 到 4 µs，两个 Bellard 构建只要 0.1 µs 左右。所以总和不宜直接比，几何平均更能反映整体。

分组是按测试名前缀划的，每项只属于一组，规则在 `scripts/bench-summarize.py` 的 `MICRO_GROUPS` 里。组内是几何平均。

| 组 | 项数 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| loop | 4 | 19.0 | 20.8 | 18.7 | 14.8 | 49.7 | 1.01 | 1.40 |
| prop | 6 | 32.6 | 29.1 | 30.6 | 20.5 | 116.4 | 1.07 | 1.42 |
| var | 8 | 33.1 | 30.1 | 32.5 | 22.5 | 255.0 | 1.02 | 1.34 |
| call | 3 | 27.3 | 27.2 | 24.4 | 17.8 | 78.4 | 1.12 | 1.53 |
| array | 16 | 26.3 | 22.6 | 25.7 | 15.7 | 102.1 | 1.02 | 1.44 |
| string | 11 | 53.8 | 49.8 | 49.1 | 32.7 | 357.5 | 1.10 | 1.52 |
| numconv | 7 | 105.7 | 101.1 | 96.5 | 86.0 | 223.4 | 1.10 | 1.18 |
| arith | 3 | 25.5 | 26.5 | 25.5 | 16.7 | 130.7 | 1.00 | 1.59 |
| bigint | 3 | 61.8 | 54.9 | 60.5 | 35.9 | 232.2 | 1.02 | 1.53 |
| map | 6 | 522.2 | 139.4 | 536.0 | 115.0 | 488.1 | 0.97 | 1.21 |
| regexp | 3 | 462.7 | 279.5 | 432.3 | 236.2 | 1405.1 | 1.07 | 1.18 |
| date | 2 | 365.9 | 346.8 | 192.7 | 170.4 | 384.9 | 1.90 | 2.04 |

goc-ng 除了 `date` 组（1.90）以外，各组都在 native ng 的 0.97 到 1.12 倍之间。goc-bellard 对 native Bellard 各组都在 1.18 到 1.59 倍之间，`date` 组是 2.04 倍；各组都慢、而且慢得差不多，说明主要原因是解释器主循环整体的代码质量，而不是某条特定路径（和上面 clang 参考的结论一致）。

最慢的几项（按 goc-ng / native ng 排），同时列出 goc-bellard/Bellard、Bellard/ng 和 Goja/ng：

| 测试 | goc-ng/ng | goc-bellard/Bellard | Bellard/ng | Goja/ng |
|---|---:|---:|---:|---:|
| date_now | 2.30 | 2.70 | 0.82 | 2.56 |
| date_parse | 1.57 | 1.53 | 0.96 | 1.56 |
| string_build_large1 | 1.19 | 1.42 | 0.75 | 174.79 |
| regexp_replace | 1.19 | 1.38 | 0.37 | 0.80 |
| int_toString | 1.18 | 1.33 | 0.84 | 3.67 |
| string_build1 | 1.18 | 2.16 | 0.46 | 4.01 |
| global_func_call | 1.18 | 1.57 | 0.73 | 3.85 |
| string_build1x | 1.17 | 2.10 | 0.46 | 3.87 |

按 goc-bellard / native Bellard 排的最慢几项：

| 测试 | goc-ng/ng | goc-bellard/Bellard | Bellard/ng | Goja/ng |
|---|---:|---:|---:|---:|
| date_now | 2.30 | 2.70 | 0.82 | 2.56 |
| string_build1 | 1.18 | 2.16 | 0.46 | 4.01 |
| prop_write | 1.01 | 2.11 | 0.43 | 2.32 |
| string_build2c | 0.95 | 2.10 | 0.43 | 3.66 |
| string_build1x | 1.17 | 2.10 | 0.46 | 3.87 |
| global_read | 0.94 | 1.89 | 0.54 | 5.13 |
| empty_loop | 0.99 | 1.84 | 0.62 | 2.87 |
| array_for | 0.97 | 1.78 | 0.64 | 4.32 |

`date_now` 和 `date_parse` 是两个 goc 构建共同的短板，这两项都要经过桥接取时间或时区。goc-bellard 最慢的其余几项里，`string_build1/1x/2c` 有一个具体原因：goc shim 的 `malloc_usable_size` 返回申请的尺寸而不是块的容量，Bellard 的 `s += "x"` 超过 512 字节后就无法原地追加，每次都整串重建（callgrind 显示重建次数是原生的 8.6 倍，见 [perf-gap.md](perf-gap.md) 5.5 节）。其余几项（`prop_write`、`global_read`、`empty_loop`、`array_for`）都是很短的字节码循环，每次操作只有几纳秒，解释器分发和取操作数的开销占主导。

全部 TIME。

| 测试 | 组 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| arguments_read | var | 160.63 | 121.29 | 142.59 | 101.92 | 1092.73 | 1.13 | 1.19 |
| arguments_strict_read | var | 130.96 | 105.75 | 117.26 | 79.44 | 1069.47 | 1.12 | 1.33 |
| array_for | array | 18.65 | 21.91 | 19.17 | 12.32 | 82.83 | 0.97 | 1.78 |
| array_for_in | array | 60.32 | 58.28 | 57.21 | 40.65 | 334.01 | 1.05 | 1.43 |
| array_for_of | array | 21.96 | 19.84 | 21.17 | 19.30 | 405.35 | 1.04 | 1.03 |
| array_hole_length_decr | array | 69.57 | 61.61 | 65.04 | 49.58 | 163.66 | 1.07 | 1.24 |
| array_length_decr | array | 49.93 | 51.85 | 46.20 | 35.78 | 167.81 | 1.08 | 1.45 |
| array_length_read | array | 11.25 | 12.25 | 11.70 | 7.17 | 53.93 | 0.96 | 1.71 |
| array_pop | array | 72.73 | 73.86 | 66.08 | 59.98 | 215.10 | 1.10 | 1.23 |
| array_prop_create | array | 32.51 | 20.62 | 33.71 | 13.71 | 155.31 | 0.96 | 1.50 |
| array_push | array | 35.49 | 39.17 | 34.21 | 27.43 | 245.06 | 1.04 | 1.43 |
| array_read | array | 11.12 | 10.19 | 11.50 | 6.40 | 47.52 | 0.97 | 1.59 |
| array_slice | array | 18.11 | 22.33 | 16.06 | 13.12 | 82.09 | 1.13 | 1.70 |
| array_update | array | 12.79 | 12.02 | 14.27 | 8.61 | 67.07 | 0.90 | 1.40 |
| array_write | array | 28.63 | 8.49 | 28.84 | 7.11 | 28.43 | 0.99 | 1.19 |
| bigint256_arith | bigint | 102.05 | 97.46 | 103.81 | 73.62 | 243.70 | 0.98 | 1.32 |
| bigint32_arith | bigint | 41.55 | 37.49 | 39.77 | 21.80 | 232.89 | 1.04 | 1.72 |
| bigint64_arith | bigint | 55.69 | 45.36 | 53.70 | 28.81 | 220.56 | 1.04 | 1.57 |
| date_now | date | 158.32 | 151.75 | 68.89 | 56.19 | 176.45 | 2.30 | 2.70 |
| date_parse | date | 845.55 | 792.75 | 538.78 | 516.86 | 839.71 | 1.57 | 1.53 |
| empty_do_loop | loop | 18.88 | 20.90 | 18.74 | 14.57 | 51.90 | 1.01 | 1.43 |
| empty_down_loop | loop | 18.42 | 20.43 | 18.23 | 15.57 | 45.75 | 1.01 | 1.31 |
| empty_down_loop2 | loop | 22.98 | 23.50 | 22.01 | 21.06 | 54.59 | 1.04 | 1.12 |
| empty_loop | loop | 16.16 | 18.64 | 16.37 | 10.14 | 47.01 | 0.99 | 1.84 |
| float_arith | arith | 26.22 | 27.76 | 26.05 | 16.73 | 172.19 | 1.01 | 1.66 |
| float_toExponential | numconv | 118.29 | 110.51 | 106.50 | 93.77 | 271.43 | 1.11 | 1.18 |
| float_toFixed | numconv | 104.22 | 93.20 | 90.67 | 82.10 | 544.93 | 1.15 | 1.14 |
| float_toPrecision | numconv | 120.09 | 110.93 | 108.87 | 95.23 | 287.66 | 1.10 | 1.16 |
| float_toString | numconv | 232.07 | 234.86 | 225.31 | 218.67 | 209.83 | 1.03 | 1.07 |
| float_to_string | numconv | 221.56 | 227.19 | 213.86 | 213.77 | 197.47 | 1.04 | 1.06 |
| func_call | call | 24.63 | 26.39 | 21.92 | 16.30 | 73.91 | 1.12 | 1.62 |
| func_closure_call | call | 29.83 | 28.25 | 28.01 | 20.14 | 71.83 | 1.06 | 1.40 |
| global_destruct | var | 41.56 | 34.57 | 39.77 | 31.08 | 408.54 | 1.05 | 1.11 |
| global_destruct_strict | var | 42.45 | 35.99 | 39.46 | 31.10 | 425.54 | 1.08 | 1.16 |
| global_func_call | call | 27.66 | 26.97 | 23.54 | 17.21 | 90.63 | 1.18 | 1.57 |
| global_read | var | 9.75 | 10.68 | 10.41 | 5.64 | 53.43 | 0.94 | 1.89 |
| global_write | var | 11.75 | 12.44 | 12.99 | 7.79 | 75.39 | 0.90 | 1.60 |
| global_write_strict | var | 11.73 | 12.20 | 12.39 | 8.28 | 97.15 | 0.95 | 1.47 |
| int_arith | arith | 18.27 | 20.53 | 19.73 | 12.16 | 95.40 | 0.93 | 1.69 |
| int_toString | numconv | 50.51 | 47.42 | 42.83 | 35.78 | 157.08 | 1.18 | 1.33 |
| int_to_string | numconv | 38.44 | 37.35 | 35.91 | 28.38 | 100.36 | 1.07 | 1.32 |
| local_destruct | var | 28.73 | 26.05 | 28.61 | 22.67 | 225.13 | 1.00 | 1.15 |
| map_delete | map | 195.51 | 198.65 | 174.15 | 165.68 | 595.95 | 1.12 | 1.20 |
| map_set_bigint | map | 3304.93 | 136.81 | 4403.93 | 112.28 | 602.56 | 0.75 | 1.22 |
| map_set_int | map | 3701.10 | 111.54 | 4261.20 | 83.54 | 332.19 | 0.87 | 1.34 |
| map_set_string | map | 196.36 | 195.57 | 176.04 | 171.93 | 549.26 | 1.12 | 1.14 |
| math_min | arith | 34.72 | 32.59 | 32.26 | 22.75 | 135.84 | 1.08 | 1.43 |
| prop_clone | prop | 53.00 | 45.74 | 46.31 | 44.67 | 270.23 | 1.14 | 1.02 |
| prop_create | prop | 61.15 | 47.00 | 58.09 | 39.01 | 138.61 | 1.05 | 1.20 |
| prop_delete | prop | 88.32 | 81.33 | 77.63 | 71.02 | 388.29 | 1.14 | 1.15 |
| prop_read | prop | 11.87 | 12.86 | 12.32 | 7.44 | 54.49 | 0.96 | 1.73 |
| prop_update | prop | 19.00 | 16.09 | 17.42 | 10.20 | 73.81 | 1.09 | 1.58 |
| prop_write | prop | 18.56 | 16.67 | 18.32 | 7.90 | 42.48 | 1.01 | 2.11 |
| regexp_ascii | regexp | 215.94 | 156.73 | 209.60 | 142.95 | 1150.64 | 1.03 | 1.10 |
| regexp_replace | regexp | 2007.31 | 859.93 | 1688.00 | 623.51 | 1350.34 | 1.19 | 1.38 |
| regexp_utf16 | regexp | 228.49 | 161.98 | 228.35 | 147.91 | 1785.44 | 1.00 | 1.10 |
| sort_bench | array | 22.58 | 20.28 | 19.43 | 14.64 | 62.03 | 1.16 | 1.39 |
| string_build1 | string | 50.44 | 42.10 | 42.85 | 19.51 | 171.98 | 1.18 | 2.16 |
| string_build1x | string | 50.29 | 41.72 | 43.11 | 19.84 | 167.01 | 1.17 | 2.10 |
| string_build2 | string | 54.90 | 46.34 | 48.19 | 34.70 | 170.11 | 1.14 | 1.34 |
| string_build2c | string | 56.42 | 53.72 | 59.50 | 25.54 | 217.54 | 0.95 | 2.10 |
| string_build3 | string | 49.08 | 42.34 | 44.16 | 32.84 | 166.62 | 1.11 | 1.29 |
| string_build4 | string | 52.28 | 49.53 | 45.85 | 36.68 | 181.18 | 1.14 | 1.35 |
| string_build_large1 | string | 67.57 | 60.34 | 56.78 | 42.44 | 9924.79 | 1.19 | 1.42 |
| string_build_large2 | string | 61.84 | 52.52 | 55.30 | 41.45 | 9649.34 | 1.12 | 1.27 |
| string_length | string | 14.09 | 15.57 | 13.85 | 9.03 | 112.90 | 1.02 | 1.72 |
| string_to_float | string | 106.33 | 112.72 | 102.83 | 95.15 | 232.79 | 1.03 | 1.18 |
| string_to_int | string | 86.80 | 92.08 | 83.21 | 72.75 | 151.18 | 1.04 | 1.27 |
| typed_array_read | array | 12.99 | 13.42 | 12.47 | 8.40 | 49.42 | 1.04 | 1.60 |
| typed_array_write | array | 30.16 | 12.22 | 31.56 | 7.63 | 44.66 | 0.96 | 1.60 |
| weak_map_delete | map | 281.43 | 170.91 | 271.17 | 147.51 | 802.32 | 1.04 | 1.16 |
| weak_map_set | map | 153.52 | 72.40 | 152.01 | 58.79 | 257.33 | 1.01 | 1.23 |

## 内存占用

![各负载的峰值 RSS](benchmark/charts/mem-peak.png)

![多实例：总 RSS 和每个 runtime 的内存](benchmark/charts/mem-scaling.png)

这一节在 2026-09-27 北京时间 14:51–15:46 测量，由 `scripts/bench-mem.sh` 一次跑完，紧接在计时套件之后。单位 MiB 和 KiB 都按 1024 进位，越低越好。

测量分两部分：

- 峰值 RSS：每个负载都在一个新进程里运行，用 `/usr/bin/time -v` 读 `Maximum resident set size`。命令行和计时套件完全相同（goc-ng 和 goc-bellard 用 `qjscli --stack-size 16384`，native ng 加 `-C`），也用 `taskset -c 3` 固定在同一个核上。每个引擎跑 3 轮，轮与轮之间轮换引擎顺序，表里是中位数。
- 多实例：一个进程里建 N 个 JS runtime，每个都跑完一小段脚本（[`tests/qjscli/instance.js`](../tests/qjscli/instance.js)）后保持存活，然后读进程的 `VmRSS`。N 取 0、1、10、100、1000，每个点跑 3 次取中位数。这部分没有绑核，理由见本节末尾的注意事项。

### 空载与各负载的峰值

空脚本是一个空文件，它的峰值就是引擎进程本身的空载占用。V8-v7 的 8 个子项各跑一个进程：文件还是 `bench-v8.js`，只是在 `Run()` 前把 `BenchmarkSuite.suites` 过滤成一个，所以其他子项的代码仍会被解析，但不会运行。SunSpider 每个文件本来就是一个进程，这里取 26 个文件里的最大值，每个文件的数字列在后面。

| 负载 | goc-ng | goc-bellard | native ng | native Bellard | Goja | goc-ng/ng | goc-bellard/Bellard | Bellard/ng | Goja/ng |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 空脚本（空载） | 8.5 | 8.5 | 3.0 | 2.8 | 5.9 | 2.80 | 2.99 | 0.94 | 1.95 |
| V8 Richards | 12.4 | 12.2 | 6.8 | 6.6 | 22.3 | 1.83 | 1.84 | 0.98 | 3.29 |
| V8 DeltaBlue | 12.7 | 12.6 | 6.9 | 6.9 | 22.4 | 1.83 | 1.83 | 0.99 | 3.24 |
| V8 Crypto | 12.1 | 11.6 | 5.8 | 5.7 | 25.6 | 2.09 | 2.04 | 0.98 | 4.41 |
| V8 RayTrace | 12.1 | 11.5 | 5.8 | 5.6 | 25.3 | 2.08 | 2.05 | 0.96 | 4.33 |
| V8 EarleyBoyer | 24.2 | 24.1 | 18.7 | 18.4 | 117.8 | 1.29 | 1.31 | 0.98 | 6.30 |
| V8 RegExp | 14.5 | 14.5 | 8.5 | 8.3 | 38.1 | 1.69 | 1.74 | 0.97 | 4.45 |
| V8 Splay | 164.2 | 158.0 | 154.7 | 146.5 | 1198.6 | 1.06 | 1.08 | 0.95 | 7.75 |
| V8 NavierStokes | 14.4 | 13.8 | 8.1 | 7.8 | 30.5 | 1.78 | 1.78 | 0.96 | 3.77 |
| V8-v7 整套 | 164.6 | 155.9 | 155.5 | 147.0 | 1218.9 | 1.06 | 1.06 | 0.95 | 7.84 |
| SunSpider（26 个文件中的最大值） | 14.8 | 15.8 | 8.1 | 7.5 | 21.8 | 1.83 | 2.12 | 0.92 | 2.69 |
| microbench | 11.1 | 10.9 | 4.5 | 4.4 | 381.7 | 2.44 | 2.49 | 0.96 | 83.94 |
| alloc.js | 8.7 | 8.4 | 3.1 | 2.8 | 17.8 | 2.83 | 2.96 | 0.93 | 5.80 |
| mapset.js | 18.3 | 16.2 | 12.6 | 10.3 | 35.7 | 1.45 | 1.57 | 0.82 | 2.83 |

Goja 的 SunSpider `3d-cube` 失败（与计时部分相同），它的 SunSpider 最大值只统计另外 25 个文件。

两个 goc 构建比各自的原生构建多出的部分基本是一个常数，而不是一个比例。空载时 goc-ng 和 goc-bellard 都是 8.5 MiB，native ng 是 3.0 MiB、native Bellard 是 2.8 MiB，差 5.5 到 5.7 MiB；小负载上的差距在 6 MiB 左右，所以比值接近 2 倍。到了 Splay 和整套 V8 这种一百多 MiB 的负载，差距仍然只有 9 MiB 左右，两组的比值都降到 1.06 到 1.08。这说明 QuickJS 对象本身在 goc 的 shim 堆里并没有明显变大，多出来的是进程级的固定开销。这部分固定开销的来源有几项推测，都没有逐项验证：Go 运行时本身；qjscli 二进制比 native qjs 大（goc-ng 4.7 MB、goc-bellard 4.6 MB，native ng 1.3 MB；native Bellard 带调试信息的文件是 5.3 MB，但代码段只有 1.0 MB，goc 的两个 qjscli 代码段约 3.4 到 3.6 MB），载入的代码页更多；qjscli 启动时 `forceGrow(40)` 把主 goroutine 的栈预先撑到 1 MiB 以上。一个旁证是，下面的多实例探针（同一套 goc 目标文件，但不调用 `forceGrow`，也不装 CLI 的宿主对象）在 N=0 时只有 4.8 到 4.9 MiB。

native Bellard 比 native ng 略低，多数负载低 1% 到 8%，`mapset.js` 低 18%。goc-bellard 在多数负载上也比 goc-ng 略低（整套 V8 低 5%，`mapset.js` 低 11%），SunSpider 最大值是例外（15.8 对 14.8 MiB）。

Goja 的峰值高得多：Splay 约 1.2 GiB，是 native ng 的 7.7 倍；整套 V8 也是 1.2 GiB，达 7.8 倍；microbench 是 382 MiB，达 84 倍。可能的原因是 Go 的垃圾回收在默认 `GOGC=100` 下允许堆长到上次存活量的两倍左右，再加上 Goja 的对象和字符串表示比 QuickJS 占空间，microbench 里 `string_build_large*` 这类构造大字符串的测试尤其明显（Goja 在这两项上也慢 170 倍左右）。这些都是推测，没有做堆剖析。

SunSpider 各文件的峰值（MiB）：

| 测试 | goc-ng | goc-bellard | native ng | native Bellard | Goja |
|---|---:|---:|---:|---:|---:|
| 3d-cube | 9.3 | 8.9 | 3.6 | 3.3 | 失败 |
| 3d-morph | 11.3 | 10.9 | 4.1 | 3.8 | 14.9 |
| 3d-raytrace | 9.4 | 9.1 | 3.5 | 3.5 | 16.9 |
| access-binary-trees | 9.0 | 8.7 | 3.3 | 3.0 | 12.6 |
| access-fannkuch | 8.7 | 8.6 | 3.2 | 2.8 | 7.8 |
| access-nbody | 8.9 | 8.7 | 3.3 | 3.1 | 12.6 |
| access-nsieve | 13.8 | 13.6 | 6.4 | 6.1 | 18.1 |
| bitops-3bit-bits-in-byte | 8.7 | 8.6 | 3.1 | 2.9 | 12.3 |
| bitops-bits-in-byte | 8.7 | 8.6 | 3.1 | 2.8 | 12.3 |
| bitops-bitwise-and | 8.7 | 8.6 | 3.1 | 2.9 | 15.3 |
| bitops-nsieve-bits | 8.9 | 8.8 | 3.2 | 3.0 | 13.0 |
| controlflow-recursive | 8.8 | 8.7 | 3.4 | 3.1 | 7.8 |
| crypto-aes | 9.1 | 8.8 | 3.5 | 3.4 | 13.5 |
| crypto-md5 | 9.1 | 8.9 | 3.6 | 3.2 | 13.4 |
| crypto-sha1 | 9.0 | 8.8 | 3.4 | 3.1 | 13.1 |
| date-format-tofte | 9.0 | 8.7 | 3.4 | 3.2 | 13.5 |
| date-format-xparb | 9.1 | 8.8 | 3.4 | 3.1 | 13.1 |
| math-cordic | 8.8 | 8.5 | 3.2 | 2.9 | 12.3 |
| math-partial-sums | 9.0 | 8.8 | 3.4 | 3.1 | 14.4 |
| math-spectral-norm | 8.7 | 8.6 | 3.2 | 2.8 | 12.6 |
| regexp-dna | 11.4 | 12.0 | 5.1 | 5.8 | 14.0 |
| string-base64 | 9.0 | 8.7 | 3.3 | 3.0 | 14.8 |
| string-fasta | 8.9 | 8.6 | 3.1 | 3.0 | 12.9 |
| string-tagcloud | 14.8 | 15.8 | 6.9 | 6.6 | 21.5 |
| string-unpack-code | 14.5 | 13.8 | 8.1 | 7.5 | 16.3 |
| string-validate-input | 9.7 | 10.5 | 3.9 | 3.6 | 14.6 |

### 多实例

这是 goc 使用场景里最关心的一项：一个 Go 进程里同时存活很多个 JS runtime 时，每多一个要多少内存。

- goc-ng：`build/qjs/qjsmem`，它就是 qjscli 这个包加上构建标签 `qjsmem` 编出来的（代码在 [`tests/qjscli/mem_instances.go`](../tests/qjscli/mem_instances.go)），和 qjscli 走同一条 `scripts/qjs-cli-build.sh` 流程、链接同一批 goc 目标文件。`qjsmem --mem-instances N` 启动 N 个 goroutine，每个 goroutine 各自 `JS_NewRuntime` + `JS_NewContext`，求值脚本后阻塞在一个 channel 上。默认的 qjscli 二进制里没有这段代码。
- goc-bellard：`build/qjs-bellard/qjsmem`，同一份 Go 代码，用 `QJS_FLAVOR=bellard QJSCLI_TAGS=qjsmem scripts/qjs-cli-build.sh` 构建，链接的是 goc 编译的 Bellard 目标文件，输出里的引擎名是 `goc-bellard`。
- native ng 和 native Bellard：[`tests/qjsmem/threads.c`](../tests/qjsmem/threads.c)，分别链接 ng 的 `libqjs.a`（和 native qjs 同一次 CMake Release 构建）和 Bellard 上游 Makefile 编出的 `.obj/*.o`。每个 runtime 一个 pthread，线程栈设为 1 MiB（`QJSMEM_STACK_KB`），`JS_SetMaxStackSize` 设为线程栈的一半。线程栈只是预留，RSS 只算实际碰过的页。
- Goja：[`scripts/gojamem`](../scripts/gojamem/main.go)，每个 runtime 一个 goroutine，各持有一个 `goja.Runtime`。
- 五家都是一个接一个地建 runtime：上一个求值完、进入阻塞之后才建下一个。goc 这样做是必须的，因为 goc 的 libc shim 堆没有锁，注释里写明同一时刻只能有一个 goroutine 在 QuickJS 里；另外几家为了可比也用同样的顺序。
- native ng 另跑了一组 `MALLOC_ARENA_MAX=1`，用来看 glibc 每线程 malloc arena 的影响。

进程 RSS（MiB）和每个 runtime 的边际内存：

| 引擎 | N=0 | N=1 | N=10 | N=100 | N=1000 | 每个 runtime（N=1→1000 斜率） | 最小二乘斜率 |
|---|---:|---:|---:|---:|---:|---:|---:|
| goc-ng（goroutine） | 4.8 | 6.5 | 8.5 | 27.8 | 225.9 | **225 KiB** | 225 KiB |
| goc-bellard（goroutine） | 4.9 | 6.4 | 8.1 | 25.1 | 201.0 | **199 KiB** | 200 KiB |
| native ng（pthread） | 2.1 | 3.1 | 5.0 | 23.8 | 208.9 | **211 KiB** | 211 KiB |
| native ng，`MALLOC_ARENA_MAX=1` | 2.0 | 3.1 | 4.9 | 23.4 | 208.3 | **210 KiB** | 210 KiB |
| native Bellard（pthread） | 2.0 | 3.0 | 4.7 | 21.7 | 188.1 | **190 KiB** | 190 KiB |
| Goja（goroutine） | 5.8 | 8.0 | 10.1 | 18.5 | 107.5 | **102 KiB** | 101 KiB |

斜率一列是 (RSS(1000) − RSS(1)) / 999；最小二乘斜率用 N=1、10、100、1000 四个点拟合，两者一致，说明在这个范围内是线性增长。斜率比：goc-ng/ng 1.07，goc-bellard/Bellard 1.05。

按 (RSS(N) − RSS(0)) / N 计算的每个 runtime 平均占用（KiB）。N 小的时候，第一个 runtime 要摊掉代码页、原子表等一次性开销，所以数字偏大：

| 引擎 | N=1 | N=10 | N=100 | N=1000 |
|---|---:|---:|---:|---:|
| goc-ng（goroutine） | 1736 | 373 | 235 | 226 |
| goc-bellard（goroutine） | 1544 | 331 | 207 | 201 |
| native ng（pthread） | 1036 | 298 | 222 | 212 |
| native ng，`MALLOC_ARENA_MAX=1` | 1076 | 296 | 219 | 211 |
| native Bellard（pthread） | 952 | 273 | 202 | 190 |
| Goja（goroutine） | 2276 | 439 | 131 | 104 |

每多一个 runtime，goc-ng 要 225 KiB，是 native ng（211 KiB）的 1.07 倍；goc-bellard 要 199 KiB，是 native Bellard（190 KiB）的 1.05 倍。Goja 只要 102 KiB，是五家里最少的；可能是因为 Goja 的内建对象是按需创建的，一段不碰多少内建对象的小脚本用不到它们，这一点没有验证。注意这只是“空闲 runtime 的常驻成本”，Goja 在真正运行负载时峰值反而最高（见上一小节）。

`MALLOC_ARENA_MAX=1` 对 native ng 几乎没有影响（210 对 211 KiB）。推测是因为每个线程分到的 arena 只碰到了自己真正用过的页，所以在这个规模下 arena 的额外开销可以忽略；这一点没有单独验证。

主动回收之后再读一次 RSS：goc 和 Goja 调 `runtime.GC()` 加 `debug.FreeOSMemory()`，C 探针调 `malloc_trim(0)`：

| 引擎 | 回收方式 | N=0 | N=1 | N=10 | N=100 | N=1000 | 斜率 |
|---|---|---:|---:|---:|---:|---:|---:|
| goc-ng（goroutine） | `runtime.GC()` + `debug.FreeOSMemory()` | 5.1 | 6.8 | 8.6 | 27.0 | 214.3 | 213 KiB |
| goc-bellard（goroutine） | `runtime.GC()` + `debug.FreeOSMemory()` | 5.2 | 6.7 | 8.3 | 24.4 | 189.5 | 187 KiB |
| native ng（pthread） | `malloc_trim(0)` | 2.1 | 3.1 | 5.0 | 23.8 | 208.9 | 211 KiB |
| native ng，`MALLOC_ARENA_MAX=1` | `malloc_trim(0)` | 2.0 | 3.1 | 4.9 | 23.4 | 208.3 | 210 KiB |
| native Bellard（pthread） | `malloc_trim(0)` | 2.0 | 3.0 | 4.7 | 21.7 | 188.1 | 190 KiB |
| Goja（goroutine） | `runtime.GC()` + `debug.FreeOSMemory()` | 6.2 | 8.3 | 9.4 | 17.0 | 91.1 | 85 KiB |

goc-ng 的斜率从 225 降到 213 KiB，goc-bellard 从 199 降到 187 KiB，下降的部分来自 goroutine 栈被 GC 收缩（见下表）。回收后 goc-ng 与 native ng 基本持平，goc-bellard 略低于 native Bellard。Goja 从 102 降到 85 KiB，是 Go 堆里的垃圾被回收了。

#### goc 的内存去了哪里

goc 探针在所有 runtime 都进入阻塞后读 `runtime.MemStats`。“平均每个 goroutine 栈”是 (StackInuse(N) − StackInuse(0)) / N。goc-ng：

| N | RSS MiB | HeapSys MiB | HeapInuse MiB | StackSys MiB | StackInuse MiB | 平均每个 goroutine 栈 KiB | GC 后每个 goroutine 栈 KiB |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4.8 | 3.7 | 0.4 | 0.31 | 0.31 | — | — |
| 1 | 6.5 | 3.7 | 0.4 | 0.34 | 0.34 | 32.0 | 32.0 |
| 10 | 8.5 | 3.4 | 0.5 | 0.62 | 0.62 | 32.0 | 12.8 |
| 100 | 27.8 | 4.6 | 0.6 | 3.44 | 3.44 | 32.0 | 9.6 |
| 1000 | 225.9 | 4.4 | 1.5 | 31.56 | 31.56 | 32.0 | 8.2 |

goc-bellard：

| N | RSS MiB | HeapSys MiB | HeapInuse MiB | StackSys MiB | StackInuse MiB | 平均每个 goroutine 栈 KiB | GC 后每个 goroutine 栈 KiB |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4.9 | 3.7 | 0.4 | 0.31 | 0.31 | — | — |
| 1 | 6.4 | 3.7 | 0.5 | 0.34 | 0.34 | 32.0 | -32.0 |
| 10 | 8.1 | 3.4 | 0.5 | 0.62 | 0.62 | 32.0 | 9.6 |
| 100 | 25.1 | 4.6 | 0.6 | 3.44 | 3.44 | 32.0 | 9.3 |
| 1000 | 201.0 | 4.4 | 1.5 | 31.56 | 31.56 | 32.0 | 8.1 |

N=1000 时，goc-ng 进程 RSS 是 225.9 MiB，其中 goroutine 栈 31.6 MiB，Go 堆 4.4 MiB。剩下约 190 MiB 不归 Go 运行时管（再扣掉 Go 运行时其他零碎的元数据还会少一点），它们是 QuickJS 的 runtime、context 和对象，放在 goc 的 libc shim 堆里（64 MiB 的静态 arena，用满后改用 mmap 分配的 slab）。折合每个 runtime 约 190 KiB，和 native ng 每个 runtime 的 211 KiB（其中含 glibc malloc 的开销和线程栈碰过的页）处在同一量级。goc-bellard 的 goroutine 栈和 Go 堆与 goc-ng 完全相同（31.6 MiB 和 4.4 MiB），剩下约 165 MiB，折合每个 runtime 约 165 KiB，比 goc-ng 少的 25 KiB 左右就是 Bellard 引擎本身比 ng 省的部分（native 两家之间差 21 KiB）。

每个 goroutine 的栈在求值后是 32 KiB。goroutine 的初始栈很小（Go 的最小栈是 2 KiB，Go 1.19 起初始大小还会按平均栈用量自适应），QuickJS 的解析器和解释器的 C 帧直接跑在 goroutine 栈上，栈就按倍数长到了 32 KiB；求值结束后栈不会立即缩回去，要等下一次 GC 扫描时才收缩。GC 之后平均降到 8 KiB 左右（N=1 和 N=10 的 GC 后数字受栈缓存复用影响，不可靠，goc-bellard 在 N=1 时甚至算出负数）。所以 goc 比原生多出的那 10 到 14 KiB，主要就是这 32 KiB 的 goroutine 栈，减去原生线程栈实际碰过的页。这个解释和上面的数字对得上，但没有逐页核对。

Goja 的同一组数字作对照。它的内存主要在 Go 堆上（N=1000 时 HeapInuse 80.3 MiB），每个 goroutine 的栈只有 6 KiB 左右：

| N | RSS MiB | HeapSys MiB | HeapInuse MiB | StackSys MiB | StackInuse MiB | 平均每个 goroutine 栈 KiB | GC 后每个 goroutine 栈 KiB |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 5.8 | 3.8 | 0.9 | 0.25 | 0.25 | — | — |
| 1 | 8.0 | 3.7 | 1.2 | 0.31 | 0.31 | 64.0 | -96.0 |
| 10 | 10.1 | 7.6 | 3.0 | 0.44 | 0.44 | 19.2 | 19.2 |
| 100 | 18.5 | 10.9 | 8.9 | 1.12 | 1.12 | 9.0 | 5.8 |
| 1000 | 107.5 | 93.4 | 80.3 | 6.59 | 6.59 | 6.5 | 4.3 |

### 注意事项

- RSS 包括进程映射的可执行文件和共享库里被碰过的页。goc 和 Goja 的二进制比 native qjs 大，这部分算在它们头上。
- goc 和 Goja 的 RSS 里含 Go 运行时本身，以及 GC 留出的余量。这里 `GOGC` 和 `GOMEMLIMIT` 都保持默认（`GOGC=100`，没有内存上限），没有测别的取值。调低 `GOGC` 通常能压低 Goja 的峰值，但会多花 GC 时间。
- 峰值 RSS 那部分绑了核，所以 goc 和 Goja 进程里的 `GOMAXPROCS` 是 1。多实例那部分没有绑核（8 个 vCPU，`GOMAXPROCS=8`），因为它测的是常驻内存而不是速度，而且 glibc 的 arena 上限和 Go 的每个 P 的缓存都跟 CPU 数有关，绑到一个核上反而不像真实部署。
- C 探针的 native 数字里含 glibc malloc 的 arena 开销和每个线程的栈、TLS。线程栈预留 1 MiB，但只有碰过的页算进 RSS。
- 多实例探针里的 runtime 是依次建立的，没有测多个 runtime 同时运行时的内存，也没有测每个 runtime 跑较大负载后的占用。
- goc 的 shim 堆目前没有锁，所以本节的 goc 探针只能依次在各自的 goroutine 上建 runtime 和求值。要让多个 goroutine 真正并行地运行各自的 runtime，shim 堆需要改成线程安全的（或者加锁），这可能会改变上面的数字。
- 本节原始数据：`data/raw/mem-rss-*.txt`、`data/raw/mem-inst-r*.txt` 和 `data/raw/env-mem.txt`，重跑用 `scripts/bench-mem.sh`。

## Bellard QuickJS 移植

goc-bellard 是用同一套 goc 流水线编译 Fabrice Bellard 的 QuickJS 2026-06-04（和 native Bellard 同一个发布包 `quickjs-2026-06-04.tar.xz`）。构建方式和 goc-ng 平行：`QJS_FLAVOR=bellard scripts/qjs-cli-build.sh`，源码树在 `third_party/quickjs-bellard`（不入库，取法见 `third_party/README.md`），输出在 `build/qjs-bellard/`，CLI 是 `build/qjs-bellard/qjscli`。不设 `QJS_FLAVOR` 时一切和原来一样，仍然构建 goc-ng 到 `build/qjs/`。两者共用同一个 libc shim、uptr 运行时、同一组 goc 环境变量（`GOC_DEFAULT_PTR_COLOR=cptr`、morestack、stackmap、`GOC_CRESERVE=8192` 等）、同样的 O3 + `-DNDEBUG`，也共用 Go 侧的 `tests/qjscli`。

改了什么：

- 源码补丁 `scripts/qjs-gstack-bellard.patch`，由 `scripts/qjs-build.sh` 自动打上，全部在 `#ifdef GOC_QJS_GSTACK` 里，所以原生构建不受影响：
  - `js_check_stack_overflow`：和 ng 的补丁相同，按当前 `g.stack.hi` 量栈深，因为 goroutine 栈会被整体搬走。
  - `libregexp.c` 的 `lre_exec_backtrack`：和 ng 的补丁逐块相同（Bellard 和 ng 这部分代码几乎一样），回溯栈里存的指针按 uptr 解码，循环计数器不再塞进 `capture[]`。
  - `JS_AtomGetStrRT`：纯 ASCII 原子的快速路径原本直接返回原子在堆上的字节，否则返回调用者栈上的 `buf`。同一个返回值有时是栈指针、有时是堆指针，goc 的指针着色检查不接受这种混合返回（返回值槽里的指针不是从某个栈上的形参借来的）。补丁改成把 ASCII 名字也拷进 `buf` 再返回。代价是超过 63 字节的名字在用到它的错误信息里会被截断，Bellard 对非 ASCII 名字本来就这样截断。这个函数只用于错误信息、调试输出和内建构造函数的短名字。
- 编译参数：Bellard 多一个 `cutils.c`；定义 `CONFIG_VERSION`；**不加** `-DJS_NAN_BOXING=0`。ng 用 `#if JS_NAN_BOXING`，Bellard 用 `#ifdef JS_NAN_BOXING`，定义成 0 反而会打开 NaN boxing，JSValue 变成 8 字节，和 Go 侧的 16 字节结构对不上（第一次构建就因此在启动时崩溃）。x86-64 上 Bellard 默认就是 16 字节的 `struct JSValue`。
- `tests/qjs/_qjs_bellard_api.c`（只在 Bellard 构建里）：Go 侧用 `go:linkname` 调的 `JS_FreeValue` 在 Bellard 里是头文件里的 `static inline`，这里导出成真正的函数（连同 `JS_FreeValueRT`、`JS_DupValue`）；另外模拟 ng 的 `JS_AddRuntimeFinalizer`：`quickjs.c` 编译时把自己的 `JS_FreeRuntime` 改名，这个文件提供公开的 `JS_FreeRuntime`，先按 ng 的顺序（后注册先调用）跑终结器，再释放 runtime。和 ng 的区别是终结器在 runtime 最后一次 GC 之前而不是之后运行；CLI 注册的终结器只释放自己的 C 记账结构，不受影响。
- `tests/qjs/_qjs_bellard_compat.h`：CLI 宿主（`tests/qjs/_qjs_cli_*.c`）是照 ng 的 API 写的，这个头文件把用到的 ng API 映射到 Bellard 的公开 API 上，例如 `JS_NewClassID(rt, &id)` → `JS_NewClassID(&id)`、`JS_IsBigInt(v)` → `JS_IsBigInt(ctx, v)`、`JS_SetOpaque` 的返回值、`JS_ThrowPlainError`、`JS_NewArrayFrom`、`JS_GetLength`、`JS_NewUint8Array*`、`JS_IsArrayBuffer`、SharedArrayBuffer 表（`JSSABTab`）。宿主源文件本身只改了 include 和几处 `#ifdef`，ng 构建看到的预处理结果不变。
- libc shim：Bellard 的 BigInt 用 `unsigned __int128` 做除法，LLVM 会生成 `__udivti3` 调用，goobj 里没有 libgcc，所以 shim 里加了一个 `__udivti3`（除数不超过 64 位时一条 `divq`，否则移位相减），只在 Bellard 构建里编译。用一批随机生成的大整数做乘除、取模和 `toString`，输出和 native Bellard 逐字节一致。除此之外 Bellard 不需要新的 libc 函数。
- `performance.now()`：ng 在引擎里定义 `globalThis.performance`，Bellard 是在 quickjs-libc 的 `js_std_add_helpers` 里加的，goc 的 CLI 宿主不用 quickjs-libc，所以 Bellard 构建的宿主（`tests/qjs/_qjs_cli_host.c`，`#ifdef GOC_QJS_BELLARD`）自己补了一个，语义和 Bellard 相同（CLOCK_MONOTONIC，毫秒，浮点）。
- 冒烟测试 `tests/qjs`：Bellard 没有 `JS_GetVersion` 和 `JS_SetPromiseHook`，探针里给了一个返回版本串的 `JS_GetVersion`，并以 `QJS_PROMISE=0` 跳过 promise hook 那一项；300 次栈增长扫描照常全部通过。

限制：

- CLI 宿主缺少 ng 独有的几项：`import ... with { type: "bytes" }` 得到的 Uint8Array 不是只读的（Bellard 没有 immutable ArrayBuffer）；`qjs:bjson` 没有 `WRITE_OBJ_STRIP_DEBUG` / `WRITE_OBJ_STRIP_SOURCE`；`qjs.getStringKind` 恒为 -1。ng 的 CLI 测试集（115 项）是针对 ng 的，没有在 goc-bellard 上跑。
- 同样的 `--stack-size 16384`，goc-bellard 能递归的 JS 深度比 native Bellard 浅：简单递归函数 goc-bellard 约 9200 层，native Bellard（`--stack-size 16M`，`ulimit -s unlimited`）约 25000 层；goc-ng 约 10700 层，native ng 约 15400 层。goc 编译出的 C 帧比原生大：`JS_CallInternal` 的栈帧 goc-bellard 1768 字节、gcc 版 native Bellard 520 字节，goc-ng 1512 字节、native ng 952 字节，主要来自 goc 要求的 `-no-stack-slot-sharing`（每个溢出槽只装一个值）；gcc 编出的帧本来就特别小，所以 Bellard 的差距更大（见 [perf-gap.md](perf-gap.md) 第 4 节）。这只影响栈溢出的阈值，test262 和官方测试不受影响。另外，qjscli 不加 `--stack-size` 时两个 goc 构建连 1000 层递归都会报栈溢出，所以本页所有 goc 命令都带 `--stack-size 16384`。
- goc 的 shim 堆没有锁，goc-bellard 和 goc-ng 一样，同一时刻只能有一个 goroutine 在 QuickJS 里。

## 原始文件

| 文件 | 内容 |
|------|------|
| [data/all.json](benchmark/data/all.json) | 汇总后的全部数字（`scripts/bench-summarize.py` 生成），`bellard_clang_ref` 是 clang 版 Bellard 参考时段的汇总 |
| [data/raw/env.txt](benchmark/data/raw/env.txt) | 计时套件时段的时间（机器时钟 KST）、五个二进制和 sha256、轮数 |
| [data/raw/env-mem.txt](benchmark/data/raw/env-mem.txt) | 内存测量时段，含探针二进制的 sha256 |
| [data/raw/v8-\<引擎\>-r\<轮\>.txt](benchmark/data/raw/) | V8-v7 每轮原文，末行是墙钟；引擎是 `goc-ng`、`goc-bellard`、`ng`、`bellard`、`goja` |
| [data/raw/ss-\<引擎\>-r\<轮\>.txt](benchmark/data/raw/) | SunSpider 每轮每项的 ms/次 和 n |
| [data/raw/micro-\<引擎\>-r\<轮\>.txt](benchmark/data/raw/) | microbench 每轮原文 |
| [data/raw/microcall-r\<轮\>.txt](benchmark/data/raw/) | 微调用每轮原文，五个引擎（`=== goc ===` 是 goc-ng，`=== native ===` 是 native ng） |
| [data/raw/mem-rss-\<引擎\>-r\<轮\>.txt](benchmark/data/raw/) | 峰值 RSS，每行是 `负载 最大RSS(KiB) 墙钟(s) 退出码` |
| [data/raw/mem-inst-r\<轮\>.txt](benchmark/data/raw/) | 多实例，每行一个 `MEMINST` JSON（引擎、N、RSS，goc 和 Goja 另有 `runtime.MemStats`） |
| [data/raw/test262-results.json](benchmark/data/raw/test262-results.json) | test262 抽样逐文件结果 |
| [data/raw/qjs-tests-results.json](benchmark/data/raw/qjs-tests-results.json) | 官方测试逐函数结果 |
| [data/raw/bellard-clang/](benchmark/data/raw/bellard-clang/) | clang 版 Bellard 参考时段：`env.txt`，以及 `bellard-clang`、`bellard`、`goc-bellard` 三家的 V8 / SunSpider / microbench 原文 |

图都在 [`docs/benchmark/charts/`](benchmark/charts/)，每张图有同名的 `.png` 和 `.svg`，本页嵌 PNG，`report.html` 嵌 SVG。图由 `scripts/bench-charts.py` 从 `all.json` 生成，图中文字是英文，五个引擎（goc-ng、goc-bellard、native ng、native Bellard、Goja）在每张性能图里都有。
