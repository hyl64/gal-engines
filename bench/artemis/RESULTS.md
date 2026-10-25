# Artemis 容器层效率基准与瓶颈判定

真实数据、真实二进制、可复现。本文所有数字都来自 `bench/artemis/bench.sh` 的一次运行，
每项重复 ≥5 次，给 median / p95 / min / max 与「首次运行」四个数。没有单次数字。

被测对象：`artemis/build/` 里的 `pfstool/iettool/asttool/asbtool/agame/arun/pfsgame`
与它们链接的 `libartemis.a`。**源文件一行没改**（`artemis/` 下正在被三位队友并行开发），
基准只读运行；库内测部分由 `bench/artemis/src/*.cpp` 直接链接同一个 `libartemis.a` 完成。

---

## 0. 一句话结论

> 归档层没有可挑的效率问题：**打开 202 MB 的 pf8 只让 RSS 涨 0.6 MiB**（mmap 真的生效，
> 常驻 540 KiB = 全文件的 0.26%），列 1682 条目录在进程启动之外只花 **1.8 ms**，
> 单条目提取**没有**退化成整包解密（取 192 KB 脚本峰值 RSS 6.3 MB，不是 202 MB）。
>
> 唯一真正的瓶颈只有一个，而且它一个人把整套吞吐拖垮：
> **`pfs.cpp:155` 的 `ApplyKey` 曾逐字节 XOR**，实测 960 MiB/s，而这颗 CPU 用 20 字节
> 分组能做 19,999 MiB/s——20.8 倍差距；同一份 PNG，明文卷 0.036 ms，XOR 卷 1.50 ms。
> **该补丁已于 16:27 落地，实测：整卷解档 CPU 侧 6.8×（792→5,405 MiB/s），
> 单资产 17–19×（一张 2 MB CG 2.4 ms → 0.14 ms）**，见 §12.0.1。
> 容器层至此不再是 asset 加载的成本项。`Find()` 的未命中也在 R3 之后从 74.6 µs 降到
> **2.04 µs（37×）**，`Open()` 无退化。容器层三条建议（R1/R2/R3）全部落地并复测。
>
> **真实第一屏（真实素材、1024×698）已经能跑**：从 44 MB 归档直接跑，首帧 **41.9 ms**
> （min，7 次基线），峰值 RSS 52.9 MiB，四个画面与"先解包到目录再跑"**逐字节相同**；
> 归档路径**不比解包慢**，代价只是 RSS ~1 MiB。
> 全栈六层都进了基准：渲染 **5.3–6.0 ms/帧**（内部计时）、OGG 解码 **28 ms/10 s（357× 实时）**、
> 亮度变换 **0.658 ms/帧**、59 个真实 `.ast` 解析 **112 ms**。
> 唯一反复出现的"大数"是 **PNG 编码**（帧 215.8 ms、IPT 输出 476.8 ms）——
> 但它**只在落盘/截图/离线批处理时才付**，不属于运行时热路径，更不是渲染成本。

> **哪一版的数字**：§0.1 / §8.2 取**最新基线** `out/baseline3`
> （`libartemis.a` 17:44:00 `5d3d65712cd4`，load1≈38）；§3 ~ §7 的容器层表取
> V2（`libartemis.a` 16:01:59），因为 `pfs.cpp` 的容器逻辑在 V1→V3 之间除了我们自己的
> R1/R2/R3 三次改动外没有别人动过，§12 有逐项对照。**每张表的 caption 都写了它取哪一版**；
> 引用时请连口径一起引。

---

## 0.1 卖点数字（可直接进 README）

真实数据、真实二进制、≥5 次重复。**两列口径分开写，不许混**：

- **外（外部墙钟）** = `perf_counter` 包住 `fork+exec`，含 **~8 ms 进程启动**，以及工具顺带做的
  PNG 解码/编码与落盘；
- **内（工具自报计时）** = 工具自己打印的层内耗时（`fxtool` 的 `apply 0.658 ms`、
  `iettool` 的 `apply 38.0 ms / encode 543.2 ms`、`audiotool` 的 `decode 0.028 s`），
  排除了进程启动与 I/O，是"每帧/每层"真正该看的数字。

| # | 卖点 | 外（含进程启动） | 内（层内计时） | 条件 | 出处 |
|---|---|---|---|---|---|
| 1 | 打开一个 202 MB 归档 | — | **0.29 ms**（库内） | mmap + 索引 + 相位检测，暖态；常驻只涨 540 KiB（0.26%） | §3 |
| 2 | 列 1682 条目录 | **10.2 ms**（容器层只占 ~2 ms） | — | `pfstool list`，其余是 `fork+exec` | §3 |
| 3 | 取一张 2 MB 的 event CG | — | **0.129 ms**（库内） | `Content()`，XOR 卷，暖态 | §6.1 |
| 4 | 按名字找一条资产（未命中，1682 条） | — | **2.04 µs**（库内；R3 之前 74.6 µs） | `Find()` miss | §4.1 / §12.0.2 |
| 5 | 解档 202 MB 加密卷 | **266 ms**（写盘） | **39.8 ms**（CPU，库内） | R1+R3 之后；写盘版其余是磁盘 | §12.0.1 |
| 6 | 嗅探整卷 1682 条类型 | — | **2.38 ms / 24.5 MiB**（库内） | 有界 512 B 采样（R2） | §6.3 |
| 7 | 73 张真实 PNG 全解一遍 | — | **166.8 ms → 189 MiB/s**（库内） | 10.61 MiB / 2509 万像素；比 `sips` 快 2.2× | §7 |
| 8 | **真实第一屏：从归档直接跑** | **41.9 ms**（min） | — | 1024×698，真素材，4 页；峰值 RSS 52.9 MiB | §8.1 |
| 9 | 「不解包直接跑」vs「先解包再跑」 | **41.9 vs 48.8 ms** | — | 同一场景同一引擎；RSS 52.9 vs 51.7 MiB；画面逐字节相同 | §8.1 |
| 10 | 渲染每帧 | — | **5.3–6.0 ms** | 1024×698 | §8.1.3 |
| 11 | 一张帧 PNG 编码（只有落盘才付） | — | **215.8 ms**（库内） | 真实照片帧 1024×698 | §8.1.3 |
| 12 | 59 个真实 `.ast` 全语料解析 | **112.0 ms**（约 1.9 ms/文件） | — | `agame --ast-info` | §8.2 |
| 13 | OGG 解码（10 秒音频） | 57.4 ms | **28.0 ms** → 约 **357× 实时** | 3.4 MB / 3:02 真实 OGG | §8.2 |
| 14 | 混音路径（3 秒音频） | 64.7 ms | **24.0 ms** → 约 **125× 实时** | `Engine::Mix` 回调 | §8.2 |
| 15 | 亮度变换（特效，1024×698） | 81.7 ms | **0.658 ms/帧**（占 60 fps 一帧的 4%） | 真实照片帧 | §8.2 |
| 16a | **IPT 合成**（1668×998，1 tile） | — | **19.0 ms**（min） | 从归档解 tile，compose+blend+clip | §8.2.2 |
| 16b | **IPT 输出 PNG 编码**（同一次解码） | — | **476.8 ms**（min） | PNG write + 颜色统计；是合成的 **24×** | §8.2.2 |

第 11、15、16a、16b 行是同一件事的四个面：**这个引擎里"写 PNG"比"算像素"贵一个数量级**，
而"写 PNG"只在落盘/截图/离线批处理时才付，不在运行时热路径上。

## 0.2 为什么回归闸门只覆盖一部分指标（本次 21 项，视基线质量而定）

> **可直接引用的结论**：墙钟指标的 baseline 自身 spread 可达数百到上千%（`seq_pf8` 1750%），
> 因此对渲染/首帧这类指标设固定阈值只会产生假警报；闸门只覆盖 spread ≤25% 的 21 个
> CPU 密集型指标，其余靠 `--include-noisy` 在安静机器/CI 上启用。

这条不是设计偏好，是被自己的第一版打脸打出来的。第一版按"任一指标退化 >20% 就 exit 1"实现，
在**完全没有改动的构建上连报 11 项退化**，其中 `pfsbench_rand_pf8` 报 +73%。查下来是机器噪声：
用 baseline 自己那 7 次运行算 spread（max/min 之比）：

| 指标 | baseline 自身 spread | 能不能设 20% 闸 |
|---|---|---|
| `pfsbench_seq_pf8`（解档 202 MB） | **1750%** | 不能 |
| `pfsbench_rand_pf8`（1000 次随机读） | 554% | 不能 |
| `agame_real_archive`（真实首帧） | **1017%** | 不能 |
| `agame_astinfo_corpus`（59 个 .ast） | 341% | 不能 |
| `pfsbench_one_pf8_png`（取 2 MB CG） | 402% | 不能 |
| `pngbench_pf6_memory`（73 张 PNG 解码） | **14%** | 能 |
| `pfsbench_find_pf8`（Find 未命中） | **4%** | 能 |
| `capture_real_frame`（帧 PNG 编码） | **4%** | 能 |
| `iettool_ipt_compose`（IPT 合成） | 17% | 能 |

原因很清楚：墙上时钟把"等 CPU"和"等磁盘"都算了进去，而这台机器上别的进程正在抢这两样
（同期 `vm.loadavg` 13→57）。**只有真正由本进程 CPU 完成的工作，其耗时才是可复现的。**

所以 `gate.py` 的规则是：

1. 只对 `unit ∈ {ms, us, MiB/s, /s}` 的行设闸（字节数、条目数、峰值内存只记录不设闸）；
2. **baseline 自身 spread ≤25% 且 min ≥1 单位** 才设闸，否则打印成 `noisy(...)` 并跳过；
3. 退化必须同时满足"比基线差 >20%"**且**"超出 baseline 观察到的整个区间"——
   即比基线最慢的那一次还要慢，这样一条红旗必定不是机器抖动；
4. `memref_*` / `applykey_*` / `alloc_*` 是 CPU 天花板参照，只用来给倍数一个尺度，永不设闸
   （它们对内存带宽竞争极其敏感，负载 50 时能掉 30%，而同期真实路径只动 1.5%）；
5. `--include-noisy --spread 1000` 可以在安静机器/CI 上全开。

6. **先验环境，再判退化**：如果这次运行所在机器的负载明显高于 baseline 的条件
   （`load1 > max(基线×1.5, 基线+5)`，逐用例也检查一次），闸门**直接跳过并说明原因**，
   而不是硬报——一条"机器在忙"比十九条假退化有用。

**闸门的覆盖率取决于基线取在什么条件下**：`out/baseline3`（`libartemis.a` 17:44:00
`5d3d65712cd4`）是在 load1≈38 下取的，因此只有 **15 项**满足 spread ≤25%；
早一版 `out/baseline2`（load≈15）能设 **30 项**。所以：

> **基线必须在安静窗口重取**（`./bench/artemis/bench.sh --baseline`）。
> 同一个 gate 的覆盖率会随基线的质量变化——机器越闲，能设闸的指标越多。
> 本次新增的 IPT compose/encode 两项在 load 38 的基线里 spread 是 72% / 40%，
> **暂未设闸**，安静窗口重取后会自动纳入。

**验证过的四件事**：

| 场景 | 结果 |
|---|---|
| 把 `capture_real_frame` 人为 +30%、`pfsbench_find_pf8` +60% | **exit 1**，指名这两项（`PNG encode 207.111 -> 269.244 ms (+30%)`） |
| baseline 与自己比（未改动） | **exit 0**，30 项设闸、0 退化 |
| 同一份未改动构建，但机器 load 从 15 冲到 **140** | **exit 0 + SKIPPED**："机器比基线时忙 4.3 倍，不判" |
| 同一份未改动构建，load 约 2× 基线（30 vs 15） | exit 1，报 5 项 **+23%~+29%** —— **这是这台机器的分辨率上限** |

最后一行是诚实的代价，必须写清楚：**在 load 约 2 倍于基线时，未改动的代码也会落在
+23~29% 区间**，因为跨过去的那条线正好压在本机噪声上。所以：

- **CI / 独占机器**：`--max-load-factor 0 --load-margin 0`，20% 是硬闸，可用；
- **本地开发机**：闸门只在负载与 baseline 相当时有效，落在 20~30% 区间的红旗
  应该用同一负载再跑一次确认，而不是直接当成退化。

---

---

## 1. 机器与被测版本

| 项 | 值 |
|---|---|
| CPU | Apple M1（8 核，4P+4E） |
| 内存 | 8,589,934,592 B = 8 GiB |
| 系统 | macOS 27.0.1 (26A434)，kernel 27.0.0 arm64 |
| 页大小 | 16,384 B（16 KiB，M1 上就是这个值，mincore 计数按它换算） |
| 编译器 | Apple clang 21.0.0 (clang-2100.3.34.2) |
| libpng | 1.6.59（Homebrew） |
| Python | 3.14.0 + numpy 2.3.5 |
| 数据 | `root.pfs` 44,161,036 B / 125 条 / pf6 明文；`HENPRI.pfs.060` 211,602,406 B / 1682 条 / pf8 加密；`archive_index.txt` 2891 条元数据（索引 154,996 B，内容 222,878,531 B，只做离线分析，不解档） |

**测量期间的机器负载必须记下来**：`vm.loadavg` 在 8 核机器上是 **13 ~ 38**（iCloud/CloudKit/
fileproviderd 在扫盘，另有队友在并行构建）。所以本文同时给 median 与 min：
`min` 是受干扰最小的那一次，最接近 CPU 真实成本；`median` 是"一台正在忙的机器上你会看到的数"。

### 版本对照（三个构建，同一天下午，树一直在动）

| 版本 | `libartemis.a` | `pfstool` | 备注 |
|---|---|---|---|
| **V1** | 15:51:11 `a553dfa3076e` | 15:51:11 `b9fca80b8f11` | 首次全量测量（本轮运行中有一次重链接，pfstool 前后跨了两个构建） |
| **V2** | 16:01:59 `8c4edf6c44e9` | 16:01:37 `5dc664291fbc` | 稳定版；`iethook`/`iet.cpp` 加入后 lib 从 279 KB 涨到 303 KB |
| **V3** | 见 `bench/artemis/out/v3-*/env.txt` | 同左 | 隔 ~28 分钟复测 |

`artemis/src/pfs.cpp` 的 mtime 全程是 **13:29:42**，`sniff.cpp` 15:18、`pngio.cpp` 14:19 ——
也就是说这三次重链接改的是脚本层/图像描述符层，**容器层代码一个字节没动**。
下面的版本对照表正好验证了这一点：容器层数字在三个构建之间没有系统性漂移，
所有抖动都在负载噪声范围内（见 §8）。

`repo_head` = `ae64c5c9d9ad61b198352245851d6527250150b3`；
`artemis_head` = `5e0bf77771ba332ff4d9b11f5e6afe575790b14d`（`git status` 有 29 个改动/新增，
三人并行开发中）。每次运行的完整快照在 `bench/artemis/out/<tag>/env.txt`。

---

## 2. 怎么复现

```bash
cd /Users/huangyuliang/Documents/gal-engines

./bench/artemis/bench.sh                  # 全量，7 次/项，约 3 分钟
TAG=before RUNS=7 ./bench/artemis/bench.sh   # 给这次运行起名，便于前后对照
QUICK=1 ./bench/artemis/bench.sh          # 跳过两次 python 参考实现
```

产物（`bench/artemis/out/<tag>/`）：

| 文件 | 内容 |
|---|---|
| `env.txt` | 机器、系统、数据 mtime/size、**每个被测二进制 mtime+sha256**、`artemis_head` |
| `raw.tsv` | 每一次运行的原始记录（wall、peak RSS、footprint、load1、时刻），不聚合 |
| `results.csv` | 机器可读：case × metric × p50/p95/min/max/cold/RSS/load |
| `results.json` | 同上 + 完整 env，便于喂给别的脚本 |
| `report.txt` | 人类可读对齐表 |
| `memref.txt` / `alloc.txt` | CPU 天花板与分配器原始输出 |
| `pngs.list` | 参与图像基准的 73 个真实 PNG 路径 |

脚本本体：`bench.sh`（编排）、`measure.py`（跑 N 次、抓 `/usr/bin/time -l` 的
`maximum resident set size` 与 `peak memory footprint`、抓子进程自己报的 STAT/PCT）、
`firstframe.py`（首帧，轮询输出目录）、`summarize.py`（聚合 + 三份报告）、
`src/pfsbench.cpp`、`src/pngbench.cpp`（链接 `libartemis.a`，库内测）。

`bench.sh` 开头会**等构建安静下来**（连续 5 秒二进制 sha 不变）才开跑，跑完再校验一次；
如果期间有人重链接会明确打出 `!! a binary was rebuilt during this run`。
库内测的两个 harness 在链接那一刻就把 `libartemis.a` 固化了，所以 `pfsbench`/`pngbench`
的数字天然锁定在单一版本上——这也是为什么版本对照要用它们。

---

## 3. 归档打开与列目录：mmap 到底有没有生效

**判据不是"看起来用了 mmap"，是 RSS 与常驻页数。** `pfsbench open` 在 `Open()` 之后
立刻用 `mincore()` 数映射区里的常驻页，用 `task_info(TASK_VM_INFO).phys_footprint` 量进程占用。

命令：`pfsbench open <archive> 20`（20 次 `Open()`，报表里取 p50），7 次重复。

| 指标 | pf6 44 MB / 125 条 | pf8 202 MB / 1682 条 |
|---|---|---|
| `Open()` 暖态 p50 | **0.111 ms** | **0.374 ms** |
| `Open()` 暖态 p95 | 0.117 ms | 0.382 ms |
| `Open()` 冷态（进程首次） | 0.114 ms | 0.368 ms |
| 索引大小 | 5,728 B | 94,293 B |
| **Open 后常驻字节**（冷，首次） | 491,520 B | **540,672 B** |
| 映射大小 | 44,161,036 B | 211,602,406 B |
| **常驻占比** | 1.11% | **0.26%** |
| Open 后 `phys_footprint` | 1.69 MiB | 1.86 MiB |

### 3.1 换系统自己的话说：`vmmap`

工具跑完只要 10 ms，`vmmap <pid>` 采样不到，所以 `pfsbench hold` 专门开一个
"打开归档然后空转 5 秒"的进程给它看。原始输出在 `out/<tag>/vmmap.txt`，摘录：

```
mapped file  107278000-113c48000  [201.8M     0K     0K     0K] r--/rwx SM=COW  .../HENPRI.pfs.060
Physical footprint:  1792K
```

翻译：**201.8 MB 的虚拟地址空间，0 KB 常驻、0 KB 脏页，整个进程的物理占用 1792 KB。**
另一轮采样读到 `[201.8M 336K 0K 0K]`——常驻量在 0~336 KiB 之间浮动，取决于那几十个
采样页有没有已经被 fault 进来。两种读数说的是同一件事：**映射建立了，文件没有被读**。

（`vmmap` 的 resident 与 `mincore` 的口径不同：前者数"已经进到这个任务页表的页"，
后者数"backing object 里在内存中的页"（含页缓存）。所以我在表里给 mincore 的数字，
在正文里引 `vmmap` 的原话——两个都不是"读了 202 MB"。）

结论：

- **mmap 确实生效**。202 MB 的卷打开之后常驻页只有 540 KiB 量级
  （索引 94 KB + `DetectPhase` 采样的几十页），占全文件 **0.26%**；
  `vmmap` 给出的常驻是 0~336 KiB、脏页 0。
  `Archive::streaming()` 两个卷都报 `yes`。
- **打开耗时随索引大小走，不随归档大小走**：pf8 的索引是 pf6 的 16.5 倍（94 KB vs 5.7 KB），
  打开耗时只是 3.4 倍（0.374 vs 0.111 ms），两者都远低于一次 `fork+exec`（8.6 ms）。
- 值得注意的是 `Open()` 里 `DetectPhase` 要采 16 个条目的前 384 B × 3 种 KeyPhase，
  会在内容区**散点触碰 ~48 个页**。页缓存全冷时这就是几十次随机 I/O：
  本报告作者在新构建上第一次打开该文件（此前从未读过）时 `open_first_ms` = **5.55 ms**，
  与暖态的 0.374 ms 相差 15 倍。日常使用中这个成本只在进程启动时付一次，
  但**不要按每个资产 Open 一次**（见 §11 建议 R5）。

### 3.2 条目数更多时会怎样：2891 条的卷

本地两个真实卷只有 125 和 1682 条；网络版的 `archive_index.txt` 说明一个商业发行版可以
列 2891 条（`index_size=154,996`，`total_bytes=222,878,531`，平均 77 KB/条，
最大一条是 16,998,496 B 的 OTF 字体）。第三点不是外推出来的，是量出来的：
`index_scale.py` 用 `pfstool create` 造一个 2891 条的卷（索引规模与真实那个同量级），
再用 `pfsbench open` 测。

| 条目数 | 索引大小 | `Open()` 暖 p50 | 备注 |
|---|---|---|---|
| 125 | 5,728 B | 0.111 ms | pf6 真实卷 |
| 1682 | 94,293 B | 0.374 ms | pf8 真实卷（含 SHA1 + 三相位投票） |
| **2891** | ~160 KB | **0.12 ~ 0.38 ms** | 合成卷（明文，无 SHA1/voting），多次运行区间 |

**索引解析在 2891 条这个量级上仍然只要零点几毫秒**，比一次 `fork+exec`（8.3 ms）
小一个数量级。也就是说"列目录慢"永远不会是 Artemis 的问题——
要么是进程启动，要么是有人在列目录时顺手把内容解了（§6.3 那种）。

### 列目录（进程级，含进程启动）

`/usr/bin/time -l` 抓 peak RSS，`time.perf_counter()` 抓 wall，7 次：

| case | p50 | p95 | min | max | peak RSS |
|---|---|---|---|---|---|
| `pfstool`（无参数，纯启动地板） | 8.589 ms | 9.179 | **8.306** | 9.179 | 5.1 MiB |
| `pfstool list pf6`（125 条 / 44 MB） | 9.085 ms | 10.354 | 7.814 | 10.354 | 5.5 MiB |
| `pfstool list pf8`（1682 条 / 202 MB） | 10.363 ms | 19.754 | 8.492 | 19.754 | **5.8 MiB** |
| `pfstool info pf6` | 12.855 ms | 63.855 | 11.451 | 63.855 | 25.0 MiB |
| `pfstool info pf8` | 16.771 ms | 60.439 | 15.484 | 60.439 | 16.0 MiB |

- **列目录时 RSS 完全不随归档大小增长**：pf6 5.5 MiB、pf8 5.8 MiB、而纯启动就要 5.1 MiB。
  列一个 202 MB 的卷比列一个 44 MB 的卷只多 0.3 MiB。用户诉求里的"高效"这一层是达标的。
- **列目录本身的成本 ≈ 0**：`list pf6` 比启动地板只多 0.5 ms，`list pf8` 多 1.8 ms
  （其中还包含向 stdout 写 1682 行的格式化）。也就是说 `pfstool list` 的 10 ms 里
  **83% 是 fork+exec+dyld**，不是容器层。
- `info` 的 25 MiB 峰值 RSS 是另一回事：它要看前 6 个条目的"长相"，而实现是
  `Content(a,e)` **整条解密后再截 512 B**（`tools/pfstool.cpp:226`），
  pf6 第 0/1 条是 4.9 MB 的 TTF，于是为了嗅 512 B 付了 5 MB 的分配与解密。见建议 R2。

---

## 4. 随机访问：按需解密的关键指标

`pfsbench rand <archive> 1000`：随机抽 1000 个条目，固定种子 `20251025`，7 次重复。

| 指标 | pf8（1682 条，均值 103 KiB，XOR） | pf6（125 条，均值 309 KiB，明文） |
|---|---|---|
| 首次触碰条目首字节（mmap 缺页，冷） | **0.131 ms** p50 | — |
| 首次触碰（暖，页已在） | **0.001 ms** p50 | — |
| `Content()` 单条目 p50 | **0.046 ms** | **0.002 ms** |
| `Content()` 单条目 p95 | 0.048 ms | 0.002 ms |
| 1000 次总耗时 | 46 ms | 2 ms |
| 有效吞吐 | **868 MiB/s** | **17,403 MiB/s** |

- 缺页那一列要小心读：**第一次运行** p50 = 0.131 ms，之后每次运行都是 0.001 ms。
  文件已经在统一缓冲缓存里了，所以 0.131 ms 是"页不在 LRU 冷端"的上限估计，
  真正的 I/O 冷数字需要 `sudo purge`，本基准不做（不碰系统状态）。
  基准把这一项放在**流水线最前面**，保证它是第一个碰内容区的用例。
- **库里最细的读取粒度就是一整条**：`pfs.cpp` 只有 `Content()`（整条）和
  `ExtractTo()`（整条流式），没有"读前 N 字节"的公共接口。所以"随机读一个资产的第一个字节"
  在引擎语义上等于"解密整个资产"——均值 103 KiB × 868 MiB/s = 0.046 ms。
  对 galgame 这不是问题（它要的就是整张图），但它解释了 §5 那个 31 倍差距为什么直接决定手感。

### 4.1 名字查找：命中 0.3 µs 很便宜，**未命中 74.6 µs**

`Find()`（`src/pfs.cpp:514-521`）线性扫条目，并且对每个候选名调 `NormalizeName()`
——那会为每个条目新建一个 `std::string`。命中会在半路 return，所以看起来便宜；
未命中要把 1682 个名字全部规范化一遍。

`pfsbench find <archive> <member> 1000`，7 次重复：

| 归档 | 命中（p50） | **未命中（p50）** | 未命中 p95 | 倍数 |
|---|---|---|---|---|
| pf6（125 条） | 0.125 µs | **3.375 µs** | 3.5 µs | 27× |
| pf8（1682 条） | 0.291 µs | **74.625 µs** | 228 µs | 256× |

- 未命中成本**随条目数线性增长**（125 → 1682 条，3.4 µs → 74.6 µs，13.5 倍条目数
  ≈ 22 倍耗时）。
- galgame 的资产加载器会大量未命中（"这个角色有没有差分？""这个场景有没有 SE？"
  "有没有 fallback 图？"）。100 次未命中 = **7.5 ms**，这已经是一帧的预算。
- 命中路径没问题（0.3 µs）。要修的是"未命中"这一侧。

---

## 5. 顺序提取吞吐：XOR 是不是瓶颈？

两组测量，一组带盘一组不带盘，正好把 CPU 与 I/O 分开。

### 5.1 纯 CPU 侧（`ExtractTo` → `/dev/null`，无盘）

`pfsbench seq <archive> --out devnull`，7 次：

| case | p50 | p95 | min | max | 吞吐（p50） |
|---|---|---|---|---|---|
| pf6 44 MB 明文 | **5.346 ms** | 6.023 | 5.110 | 6.023 | **7,877 MiB/s** |
| pf8 202 MB XOR | **245.379 ms** | 249.450 | 240.491 | 249.450 | **822 MiB/s** |

**9.6 倍差距，而且这 245 ms 里没有一次磁盘写。** 按 pf6 的 7,877 MiB/s 折算，
202 MB 若不解密只需 25.6 ms —— 也就是说 **~220 ms（90%）是逐字节 XOR 循环吃掉的**。

### 5.2 带盘（`pfstool extract`，写到真实目录）

| case | p50 | p95 | min | max | peak RSS |
|---|---|---|---|---|---|
| `pfstool extract pf6`（44 MB） | 35.665 ms | 186.130 | 32.453 | 186.130 | 48.0 MiB |
| `pfstool extract pf8`（202 MB） | 508.599 ms | 1485.077 | 471.079 | 1485.077 | **207.8 MiB** |

- pf8 的 508 ms = 245 ms CPU + ~264 ms 写 202 MB（≈ 780 MiB/s 的落盘速度）。
  去掉 XOR 那一项，整卷解档会从 508 ms 掉到 ~290 ms。
- 整卷提取的峰值 RSS ≈ 归档大小（207.8 MiB / 202 MB）。这**不是泄漏**：
  mmap 读过的页算进 RSS，但它们是 file-backed clean page，可随时回收；
  单条目加载的峰值只有 6~10 MiB（§6）。工具做整卷解档时才会看到这个数。

### 5.3 CPU 天花板（同一颗 CPU、同样字节数、无 I/O、无分配）

`pfsbench memref 67108864`（64 MiB，每种做法跑 3 遍取最好）：

| 做法 | 吞吐 | 相对逐字节 |
|---|---|---|
| `memcpy` | **25,671 MiB/s** | 26.7× |
| **XOR 逐字节**（= 现在的 `ApplyKey`） | **960 MiB/s** | 1.0× |
| XOR 4 字节字 | 1,463 MiB/s | 1.5× |
| **XOR 20 字节分组（8+8+4）** | **19,999 MiB/s** | **20.8×** |

`ApplyKey` 的写法（`pfs.cpp:163-168`）：

```cpp
for(size_t i = 0; i < length; i++)
{
    data[i] ^= archive.key[phase];
    if(++phase == 20)
        phase = 0;
}
```

每个字节一次 load-modify-store，外加一次计数器自增与一次分支。20 字节的密钥周期
本来正适合 8+8+4 分组，改成那样就是 20.8 倍；4 字节字只有 1.5 倍说明"半吊子向量化"
没用，必须按 20 字节的周期来（4 字节会每 5 个词就撞一次周期边界，只能回到取模）。

---

## 6. 单条目提取：有没有退化成整包解密？

**没有。** 三重证据。

### 6.1 库内（暖态，只算库的成本）

`pfsbench one <archive> <member> N`，7 次：

| 条目 | 大小 | 归档 | p50 | 吞吐 | 峰值 RSS |
|---|---|---|---|---|---|
| `GenShinGothic-P-Medium.ttf` | 4,923 KiB | pf6 44 MB 明文 | **0.223 ms** | 19,048 MiB/s | 15.2 MiB |
| `backlog.iet`（脚本） | 17 KiB | pf6 44 MB 明文 | **0.001 ms** | 34,730 MiB/s | 5.5 MiB |
| `bgimage\utyu2.png` | 1,098 KiB | pf6 44 MB 明文 | **0.036 ms** | 29,554 MiB/s | — |
| `image\ev\ev_chi_01\ev_chi_01_01.png` | 1,478 KiB | pf8 202 MB XOR | **1.500 ms** | **941 MiB/s** | — |
| `image\cg\SYUURAKU.png` | 2,000 KiB | pf8 202 MB XOR | **2.143 ms** | 861 MiB/s | 9.7 MiB |
| `script\d01_c08_3.ast` | 192 KiB | pf8 202 MB XOR | **0.201 ms** | 885 MiB/s | 6.4 MiB |

- 峰值 RSS **6~10 MiB**。如果真去解密整个 202 MB，这个数不可能是 6.4 MiB。
- 时间也对得上"只读那一条"：192 KiB ÷ 885 MiB/s = 0.20 ms。
- **同一种文件、同一份代码，明文 29,554 MiB/s vs XOR 941 MiB/s = 31 倍**。
  这一行是本报告里最能说明"瓶在哪一层"的数字：不是 PNG 大，不是磁盘慢，是那条 XOR 循环。

### 6.2 进程级（含启动，`pfstool cat` → /dev/null，7 次）

| case | p50 | p95 | min | peak RSS |
|---|---|---|---|---|
| `cat pf6 GenShinGothic-P-Medium.ttf`（4.9 MB） | 9.536 ms | 23.959 | 7.390 | 10.6 MiB |
| `cat pf8 image\cg\SYUURAKU.png`（2.0 MB） | 20.422 ms | 34.186 | 14.576 | **8.0 MiB** |
| `cat pf8 script\d01_c08_3.ast`（192 KB） | 12.095 ms | 16.979 | 11.119 | **6.3 MiB** |

拿启动地板 8.3 ms（min）去减：从 202 MB 卷里取一个 192 KB 脚本，**净成本 ~2.8 ms**；
如果整包解密，光 XOR 就要 220 ms 以上、RSS 至少 202 MB。两者都不成立。

### 6.3 反例：现在的 `list --types` 才是整包解密

| case | p50 | p95 | min | peak RSS |
|---|---|---|---|---|
| `pfstool list --types pf8` | **244.830 ms** | 1124.976 | 243.441 | **223.8 MiB** |
| `pfsbench types pf8`（同样逻辑，库内） | 241.066 ms | 259.208 | 228.531 | 224.7 MiB |
| `pfsbench types pf8 --fix`（384 B 有界采样） | **2.234 ms** | 4.188 | 2.023 | **24.4 MiB** |

`list --types` 想要 512 B 来嗅探，实现却是 `artemis::Content(a, e)` 整条解密再截断
（`tools/pfstool.cpp:94`，`List()` 的 `--types` 分支；`Info()` 里同样的写法在 `:226`），
于是把 202 MB 全解了一遍，只为看每条的前 512 B。
同一个答案（`kind` 完全相同）用 384 B 有界解密只要 **2.23 ms、24.4 MiB**：
**约 110 倍时间差、9.2 倍内存差**。这是本报告里唯一"改一行代码、白捡两个数量级"的地方。

---

## 7. 图像解码：和系统基线比

73 张真实 PNG（从 `root.pfs` 现取，共 10.61 MiB / 25,092,211 像素）。
`pngbench` 链接 `libartemis.a`，走的就是引擎的 `src/pngio.cpp`（libpng → 8-bit straight RGBA）。

| case | p50 | p95 | min | peak RSS | 吞吐 |
|---|---|---|---|---|---|
| `pngio.cpp` 从文件读 | **167.921 ms** | 175.403 | 164.754 | 25.8 MiB | 189.6 MiB/s / 149.4 Mpx/s / 435 张/s |
| `pngio.cpp` 从内存读（归档路径） | 172.333 ms | 182.014 | 161.496 | 25.4 MiB | 184.7 MiB/s / 145.6 Mpx/s / 424 张/s |
| `sips -s format bmp`（解码+整幅 BMP 写回） | 387.616 ms | 480.998 | 363.233 | 17.8 MiB | — |
| `sips -s format bmp --resampleWidth 64`（解码为主，写回 64 px） | **370.910 ms** | 408.946 | 341.585 | 18.5 MiB | — |

- **从内存读和从文件读一样快**（172 vs 168 ms，差 2.6%）：解码成本 100% 在 inflate，
  不在 I/O。归档路径不会因为"多一层解压/拷贝"变慢。
- **比系统 `sips` 快 2.2 倍**（371 / 168），而且 `sips` 那个数字里还含 BMP 编码。
  这条链没有瓶颈，不必动。
- 平均单张 2.3 ms（10.61 MiB / 73 张）。一屏若 20 个图层，全部重解码 = 46 ms；
  这也说明**不要每帧重解码**，缓存解码结果是帧预算的关键（本条只是提醒，不属容器层）。

---

## 8. 首帧到出图

`agame artemis/tests/sample-scene.ast --frames <dir>`，`SDL_VIDEODRIVER=dummy`，
轮询输出目录抓第一个 BMP 落盘的时刻（`firstframe.py` 用 0.2 ms 粒度的忙等）。

| 指标 | p50 | p95 | min | cold(第 1 次) | peak RSS |
|---|---|---|---|---|---|
| exec → 第一帧落盘 | **46.499 ms** | 255.854 | **41.029 ms** | 255.854 | 32.3 MiB |
| 整场（12 帧） | 保留在 `raw.tsv` 的 wall 列 | | | | |

- **41~47 ms 出首帧**，其中含 8.3 ms 进程启动、CoreText 首次加载日文字体、
  SDL dummy 初始化。cold 那次 255.9 ms 是字体首次进缓存，之后都在 40 ms 量级。
  按 60 fps 的一帧 16.7 ms 算，首帧不是"每帧都要付"的成本，是一次性入场费。
- **这是 fixture 剧本，不是真实游戏数据**。真实数据的首帧在 §8.1 用 `.iet` 宏表路径测到了。
- **【2026-10-25 16:30 更正，此前结论作废】** 本报告早先写过"真实 `.ast` 在 AST 层解析失败"，
  **这个说法是错的**，作废。用 16:27 的二进制实测：

  | 检查 | 结果 |
  |---|---|
  | `asttool dump` 全部 59 个真实 `.ast`（`testdata/artemis-henpri-trial/extracted/script/`） | **ok=59, bad=0** |
  | `agame <真实 .ast> --frames` | 仍然报 `ast: unexpected token at byte 17742` |

  **真实原因是 agame 走的是另一条解析器**：`tools/agame.cpp` → `src/runtime.h` →
  `src/ast.h`/`src/ast.cpp`（mtime **03:02**，旧读取器），而 `tools/asttool.cpp` 已经切到
  `src/luaast.h`/`luaast.cpp`（mtime 15:51，新解析器）。所以"解析器不行"是误判，
  真实状态是"**agame 的 `.ast` 路径还没切到新解析器**"。报错原文来自 `src/ast.cpp:243`。
  这对性能基准没有影响（真实首帧走 `.iet` 路径，§8.1），但它是 agame 接真实 `.ast` 剧本的
  最后一道门。基准里仍然保留这项探测，脚本层切换后重跑会自动变绿。

---

## 8.1 真实第一屏：从归档跑 vs 从解包目录跑

上面 §8 是 fixture 剧本。**真实数据的第一屏**由队友 `screen` 打通（`first.iet` 188 条命令
解出 112 个宏 → `system/*.iet` 宏表 → `scenario/scene.iet` 1077 条命令 / 458 段文字 /
17 次宏展开，1024×698，真实银河背景 + 游戏自带 GenShin 字体，4 页推进）。基准测的就是
这条链路，命令形态以 `agame` 当前的 `--script/--archive/--data-dir/--max-pages` 为准。

### 8.1.1 两条数据源，一次对照

同一个场景、同一个引擎，只差数据来源；`firstframe.py` 轮询输出目录记下每一帧落盘的时刻。

| 指标 | `--archive root.pfs`（44 MB 归档） | `--data-dir extracted/`（解包目录） |
|---|---|---|
| **首帧**（exec → page001.png）p50 | **50.883 ms** | 55.233 ms |
| 首帧 min | **41.690 ms** | 51.839 ms |
| 首帧 p95 | 85.742 ms | 83.457 ms |
| 峰值 RSS | 59.9 MiB | **53.5 MiB** |
| 4 页总时长 p50（5 次区间） | 880 ms（821–1314） | 880 ms（约 800–1300） |
| 4 帧输出 | **与右侧逐字节相同**（`diff -r` 通过） | — |

**这是本报告里最有产品价值的一行**，而且结论稍微反直觉：

- **归档路径不比解包路径慢**：首帧 p50 快 4.3 ms，min 快 10.1 ms。
  解包路径要为每个资产做路径解析 + `open()/read()`（宏表、字体、背景、脚本加起来几十个文件），
  归档路径是一次 `mmap` + 按需解密 + 一遍索引。
- **代价是 RSS 高 6.4 MiB**（59.9 vs 53.5）：归档路径的 file-backed 映射页算进 RSS，
  解包路径只有 libc 的文件缓冲。6.4 MiB 换掉"必须先解包 44 MB 到磁盘"，这笔交易是划算的。
- **输出逐字节相同**：所以上面的时间差不是"两条路径做的事不一样"，是同一件事的两种读法。
  基准每次运行都会 `diff -r` 两个目录并写进 `META @env frame_parity`。

### 8.1.2 每一帧分别花了多久（外部观测，不改引擎）

`firstframe.py` 每 0.2 ms 扫一次输出目录，记下每个 `pageNNN.png` 出现的时刻。归档路径 5 次
运行的相邻帧间隔（ms）：

| 运行 | page1→2 | page2→3 | page3→4 |
|---|---|---|---|
| 1 | 35.5 | 66.8 | **489.5** |
| 2 | 35.1 | 68.0 | **517.7** |
| 3 | 34.4 | 62.6 | **448.0** |
| 4 | 33.7 | 64.0 | **466.6** |
| 5 | 56.1 | 135.9 | **681.3** |

page001/002 是 62 KB / 60 KB 的简单帧，page003/004 是 882 KB / 836 KB 的
**真实照片帧**——第 4 帧那一档 448–681 ms 就是"一张真图 + 一段字"的整帧成本。
（第 5 次整体偏高是 loadavg 尖峰，见 §1。）

### 8.1.3 把"捕获"从"渲染"里拆出来

渲染没法从外部直接测，但**捕获可以**：`--frames`（写 4 张 PNG）与 `--out`（只写 1 张）
跑的是同一套渲染，差的就是逐帧落盘。各 3 次：

| 运行 | `--frames`（4 张 PNG） | `--out`（1 张 PNG） | 差值 |
|---|---|---|---|
| 1 | 0.99 s | 0.35 s | **0.64 s** |
| 2 | 1.53 s | 0.92 s | **0.61 s** |
| 3 | 0.99 s | 0.32 s | **0.67 s** |

绝对值随负载抖（0.35→0.92 s），但**差值三次都稳定在 0.61~0.67 s**。
即：**一次 4 页跑里约 65% 的时间花在写那 4 张诊断 PNG 上**，渲染 + 开归档 + 宏表加载
合计只有 ~0.35 s。

再用 `pngbench --roundtrip` 直接量一张真实帧的编码成本：读入 `page004.png` 的像素，
用引擎自己的 `artemis::WritePng` 写回去（**输出与 agame 写的那个文件逐字节相同**，
所以设置一致、内容一致），重复 5 次取最好，3 次独立运行：

| 项 | 真实照片帧 1024×698 | 合成渐变 1024×698 | 倍数 |
|---|---|---|---|
| `WritePng`（PNG 编码） | **204.5 / 205.7 / 206.2 ms** | 22.2 ms | **9.2×** |
| `CountColors`（颜色统计） | **10.6 / 11.1 / 11.2 ms** | 13.9 ms | — |

**归因要说准（这一条 16:30 按 lead 的更正修订）**：

- `--frames` 是**用户显式要求的落盘**，不是引擎内部的"诊断代码污染主路径"。
  `agame` 内部真正的诊断统计是颜色普查（`CountColors`），**它只值 ~11 ms**。
- 所以正确的拆分是（渲染取自 `agame` 自己的内部计时，编码取自本节的独立实测）：

  | 每帧成本 | 数值 | 占比 | 什么时候才付 |
  |---|---|---|---|
  | 渲染（合成 + 文字） | **5.3–6.0 ms**（`agame` 自报） | ~5% | 每帧都付 |
  | PNG 编码（1024×698 真实照片帧） | **204.5–206.2 ms**（本报告实测） | ~94% | **只有逐帧落盘时才付** |
  | 颜色统计（诊断） | 10.6–11.2 ms | — | 内部诊断 |

- 结论要这样写：**逐帧 PNG 落盘不属于运行时热路径**（真实游戏跑起来根本不落盘，
  只有截图、存档缩略图、回归测试才需要），一旦要落盘，成本的大头是 PNG 编码而不是渲染，
  该降 zlib 级别或直接落 BMP。**不要把它当成"引擎渲染慢"**——渲染是 5~6 ms/帧。
- 内容依赖：同一尺寸的合成渐变编码只要 22 ms，真实照片帧要 205 ms（**9.2×**）。
  用占位图/渐变测出来的编码成本会低估一个数量级。

---

## 8.2 全栈：渲染 / 脚本 / 音频 / 特效 / 图像 / 存档

容器层之外的六层全部进了 `bench.sh`。**口径分两类，不许混**：

- **外 = 外部墙钟**：`time.perf_counter()` 包住 `fork+exec`，含约 8 ms 进程启动、
  以及工具顺带做的 PNG 解码/编码与落盘；
- **内 = 工具自报的计时**：`fxtool sfade` 的 `apply 0.658 ms`、`audiotool probe` 的
  `decode time 0.028 s`、`audiotool play` 的 `rendered ... in 0.024 s`——
  这是"每帧/每层"真正该看的数字，因为它把进程启动和 I/O 排除在外。
  `internal.py` 把这类输出抓成和其他测量一样的 CLI 记录，所以清单/闸门一视同仁。

基线（`out/baseline2`，`libartemis.a` 17:21:11 `6039d9326d89`，7 次除非注明）：

| 层 | 用例 | p50 | min | 口径 | 峰值 RSS |
|---|---|---|---|---|---|
| 脚本 | `asttool dump` 一个真实 `.ast` | 11.201 ms | 9.973 ms | 外 | 6.3 MiB |
| 脚本 | `agame --ast-info` 59 个 `.ast` | 121.917 ms | **112.011 ms** | 外 | 13.8 MiB |
| 渲染 | 真实第一屏，`--archive root.pfs` | 76.069 ms | **41.859 ms** | 外 | 52.9 MiB |
| 渲染 | 真实第一屏，`--data-dir extracted/` | 56.915 ms | 48.795 ms | 外 | 51.7 MiB |
| 渲染 | 帧 PNG 编码（真实照片帧） | 220.281 ms | 215.769 ms | 库内 | 11.3 MiB |
| 渲染 | 颜色统计（同一帧） | 12.904 ms | 11.459 ms | 库内 | 11.3 MiB |
| 音频 | OGG 解码 10 s 窗口 | 60.745 ms | 57.411 ms | 外 | 16.3 MiB |
| 音频 | **OGG 解码 10 s 窗口** | 31.0 ms | **28.0 ms** | **内** | — |
| 音频 | 混音 3 s | 71.821 ms | 64.659 ms | 外 | 16.7 MiB |
| 音频 | **混音 3 s** | 25.0 ms | **24.0 ms** | **内** | — |
| 音频 | 真实 `.sli` A-B 循环解析 | 17.275 ms | 15.487 ms | 外 | 9.1 MiB |
| 特效 | `sfade` 亮度：整工具 | 96.325 ms | 81.718 ms | 外 | 11.7 MiB |
| 特效 | **`sfade` 亮度：变换本身** | 1.232 ms | **0.658 ms** | **内** | — |
| 特效 | 补间 `tween`（0→1024 / 300 ms） | 19.133 ms | 13.338 ms | 外 | 5.7 MiB |
| 特效 | 震动 `quake`（5 次抖动 / 300 ms） | 12.310 ms | 10.901 ms | 外 | 5.7 MiB |
| 特效 | 逐字 `typewriter`（真实 `.ast`） | 17.077 ms | 14.416 ms | 外 | 7.5 MiB |
| 图像 | IPT 解码（整工具，真实 `.ipt` → 1668×998） | 711.240 ms | 515.458 ms | 外 | 21.9 MiB |
| 图像 | **IPT 合成本身** | 21.471 ms | **18.973 ms** | **内**（`iettool` 自报） | — |
| 图像 | **IPT 输出 PNG 编码** | 512.581 ms | **476.832 ms** | **内**（`iettool` 自报） | — |
| 存档 | 写/读一次 | **未就绪** | — | — | — |

### 8.2.1 特效：0.658 ms 和 96 ms 是同一个操作

`fxtool sfade` 整个进程 96 ms，但它自己报的变换耗时是 **0.658 ms**——其余 95 ms 是
进程启动（~8 ms）+ 解码那张 835 KB 的真实帧（~12 ms）+ **编码输出 PNG（~200 ms 量级，
见 §8.1.3）+ 写盘**。所以：

- 作为**每帧特效成本**，亮度变换是 **0.658 ms**，占 60 fps 一帧预算（16.7 ms）的 **4%**；
- 作为**离线批处理**，真正的成本在 PNG 编解码，不在特效本身。

这条口径如果不分开，就会得出"特效很慢"的错误结论。

### 8.2.2 IPT：合成 38 ms，编码 543 ms——上一轮"拆不出来"，现在工具自报了

上一轮我写的是"`iettool` 不自报计时，所以合成本身拆不出来，只能给边界"。lead 随后给
`tools/iettool.cpp` / `src/ipt.h` 的 `ComposeReport` 加了两个计数字段（纯加法，136 条 selftest 全绿），
现在 `iettool decode` 自己会说：

```
$ iettool decode --archive HENPRI.pfs.060 image\ev\ev_chi_01\ev_chi_01_02.ipt out.png
out.png: diff, 1668 x 998, 1 tile(s) drawn, 98747 colour(s), opaque (Bgr32)
  apply 38.002 ms  (compose + blend + clip)
  encode 543.158 ms  (PNG write + colour count)
```

于是那个"515 ms"的真实构成是：

| 组成 | 耗时 | 口径 |
|---|---|---|
| 进程启动 | ~8 ms | 外（`pfstool` 空跑测得） |
| 归档打开 + 索引 | ~0.3 ms | 库内 |
| 源 tile（12,318 B）读 + 解密 | < 0.1 ms | 库内（12 KB ÷ 5,405 MiB/s） |
| 源 tile PNG 解码 | ~2–4 ms | 库内（12 KB 压缩数据） |
| **合成 + blend + clip** | **19.0 ms**（min）／21.5 ms（p50），7 次 | **内**（`iettool` 自报） |
| **输出 PNG 编码 + 颜色统计** | **476.8 ms**（min）／512.6 ms（p50），7 次 | **内**（`iettool` 自报） |

**合成只占 ~20 ms，编码 ~500 ms——约 24 倍。**（lead 另测的 3000×1796 / 538 万像素样本是
15.8 vs 165 ms，同样约 10 倍；两个样本都指向同一结论。） 这条和 §8.1.3（帧 PNG 编码 215.8 ms）、
§8.2.1（`sfade` 96 ms 里变换只 0.658 ms）是同一件事的三个面：
**这个引擎里"把像素写成 PNG"比"算像素"贵一个数量级。**

基准已经把它拆成两条独立测量（`iettool_decode_compose` / `iettool_decode_encode`，
走 `internal.py` 的同一套 CLI 记录），闸门会自动跟踪这两项——它们是 CPU 密集且 spread 很小的，
属于本机能设 20% 闸的那一类。

### 8.2.3 音频层的自检状态变了两次

`audiotool selftest` 在 17:22 是 **64 checks / 5 failed**（loop seam、crossfade 残留、
音轨组静音、单声道上混），17:25:49 队友重链接后 **77 checks / 0 failed**。
基准把每层的自检结论写进 `env.txt`（`META @env audiotool_selftest`），
所以"这一层当时是好的还是坏的"永远可查——**这是版本元数据的一部分，不是性能数字**。

### 8.2.4 存档层：未就绪

`artemis/src/save/` 目前只有 `json.{h,cpp}` / `state`，`savetool` 还不存在。
基准的处理是：preflight 里探测 `src/save/` 与 `build/savetool`，**缺失就记
`META @env save_layer not ready` 并跳过**，不编数字。

**交接**：`savetool` 落盘后需补测（写一次、读一次、缩略图生成三条 case）。
加的地方是 `bench.sh` 的 preflight 之后、`pfstool` 用例之前，照 `audiotool_*` 的写法抄三行即可；
`labels.tsv` 加三行；重跑 `./bench/artemis/bench.sh --baseline` 让闸门开始跟踪。
**在此之前 `save_layer` 一项保持"未就绪"，不得填数字。**

---

## 9. 对照：Python 参考实现与分配器

### 9.1 `testdata/artemis-tools/pfs_extract.py`（numpy 向量化 XOR）

| case | C++ p50 | Python p50 | C++ 快多少 |
|---|---|---|---|
| pf6 44 MB 明文解档 | 35.665 ms | 123.105 ms | **3.5×** |
| pf8 202 MB XOR 解档 | 508.599 ms | 635.986 ms | **1.25×** |

**这张表是整份报告里最刺眼的一行**：numpy 的 XOR 是 SIMD 的（~10 GB/s 量级），
C++ 的 XOR 是逐字节的（0.96 GB/s），于是"用 C++ 重写"在加密卷上几乎没赚到东西——
只有 1.25 倍，其中省下的其实是 Python 的 I/O 开销，不是算法。
明文卷上 3.5 倍才是这套代码应有的水平。`ApplyKey` 修好之后，pf8 这一行应该拉到 3~5 倍。

### 9.2 `ExtractTo` 的每条目 256 KiB 缓冲

`pfs.cpp:383-384` 对**每个条目**都 `std::vector<uint8_t> buffer(CHUNK)`（256 KiB，
且 vector 会值初始化 → 一次 memset）。1682 条就是 1682 次分配 + 430 MB 的 memset：

| 做法 | 总耗时（1682 次） | 单次 |
|---|---|---|
| 每次新建 256 KiB vector（现在的写法） | **4.602 ms** | 2.74 µs |
| 一个 buffer 复用 | **0.003 ms** | — |

只占 245 ms 的 1.9%，不值得单独发版，但和 R1 一起改是顺手的事。

---

## 10. 瓶颈判定

按"对用户体验的实际影响"排序，全部有数字支撑：

| # | 瓶颈 | 位置 | 证据 | 影响面 |
|---|---|---|---|---|
| ~~1~~ | ~~`ApplyKey` 逐字节 XOR~~ **已修（R1，16:27 落地）** | `src/pfs.cpp:155-169` | 修前 943 MiB/s → 修后实测整卷解档 **6.8×**、单资产 **17–19×**（§12.0.1） | 已关闭 |
| **2** | 嗅探用整条 `Content()`（R2） | `tools/pfstool.cpp:94` / `:226` | `list --types` 244.8 ms / 223.8 MiB vs 有界采样 2.23 ms / 24.4 MiB | 工具与任何"按类型索引资产"的批处理 |
| 3 | `Open()` 里 `DetectPhase` 的散点采样（R5） | `src/pfs.cpp:193-294` | 页缓存全冷时首次 open 5.55 ms、48 个散页；暖态 0.374 ms | 只在"每资产 Open 一次"的用法里痛 |
| ~~4~~ | ~~`Find()` 未命中线性扫~~ **已修（R3，17:21 落地）** | `src/pfs.cpp:514-521` | 未命中 74.6 µs → **2.04 µs（37×）**，`Open()` 无退化 | 已关闭 |
| 5 | `ExtractTo` 每条目 256 KiB 缓冲（R4） | `src/pfs.cpp:383-384` | 4.6 ms / 430 MB memset（占 1.9%） | 整卷解档 |
| **R** | 逐帧 PNG 落盘（诊断用）在热路径上 | `tools/agame.cpp:871-877`（`--frames` 每页 `WritePng`） | 4 页跑 0.99 s，其中 0.61~0.67 s 是写 PNG；一张真实帧编码 204~206 ms vs 合成渐变 22 ms | **真实运行时最大的单项**，不是容器层问题 |
| — | 归档打开/列目录/RSS | — | 540 KiB 常驻 / 0.26%；list 超启动 1.8 ms | **不是瓶颈，已达标的样张** |
| — | 单条目提取 | — | 192 KiB 条目 0.20 ms、RSS 6.4 MiB | **不是瓶颈**（无整包解密） |
| — | PNG 解码 | `src/pngio.cpp` | 189.6 MiB/s，比 sips 快 2.2× | **不是瓶颈** |
| **S1** | 帧 PNG **编码**（只有落盘/截图才付） | `tools/agame.cpp:871-877`、`fxtool sfade` 的输出端 | 真实照片帧 215.8 ms vs 合成渐变 23 ms | 离线批处理、截图；**不是运行时热路径** |
| S2 | 特效、音频、脚本三层的层内成本都很小 | `src/fx/`、`src/audio/`、`luaast` | 亮度 0.658 ms/帧、OGG 解码 28 ms/10 s（357× 实时）、混音 24 ms/3 s、59 个 `.ast` 112 ms | **都不是瓶颈** |

---

## 11. 建议（按 ROI 排序，含代码位置）

| 编号 | 内容 | 状态 | 收益（实测） |
|---|---|---|---|
| **R1** | `ApplyKey` 20 字节分组 | **已落地**（16:27） | 整卷解档 CPU 6.8×，单资产 17–19× |
| **R2** | 嗅探改有界采样 | **已落地** | 16× 时间 + 9.2× 内存 |
| **R3** | `Find()` 预规范化名字 | **已落地**（17:21，提交 `1e3ae02`） | 未命中 74.6 → 2.04 µs（**37×**），`Open()` 零退化 |
| **R4** | `ExtractTo` 复用缓冲 | 待定（顺手） | 整卷 1.9% |
| **R5** | `Open()` 相位检测 | 只在"每资产 Open"时痛 | 5.55 ms 冷 / 0.37 ms 暖 |

> 我在 `bench/artemis/` 里有写权限，`artemis/` 下**没有**——以下均为建议，
> 具体由 lead 分派给对应队友；R1/R2 的收益我已用 `pfsbench` 在同版本库上量化过（见 §12）。

### R1（最高 ROI，~20 行）`ApplyKey` 改成 20 字节分组 XOR

`artemis/src/pfs.cpp:155-169`。保持签名与语义不变，只换循环体：

```cpp
static void ApplyKey(const Archive& archive, const Entry& entry, uint8_t* data,
    size_t length, size_t relative)
{
    if(!archive.encrypted() || archive.key.size() != 20)
        return;
    size_t phase = archive.phase == KeyPhase::ArchiveOffset
        ? (size_t)((entry.offset + relative) % 20)
        : (relative % 20);

    // 先把 phase 对齐到 0，后面就是整齐的 20 字节块
    size_t head = (20 - phase) % 20;
    if(head > length) head = length;
    for(size_t i = 0; i < head; i++)
        data[i] ^= archive.key[phase + i];
    size_t at = head;
    phase = 0;

    // 20 字节密钥周期的天然分组：8 + 8 + 4
    uint64_t k0, k1;
    uint32_t k2;
    memcpy(&k0, archive.key, 8);
    memcpy(&k1, archive.key + 8, 8);
    memcpy(&k2, archive.key + 16, 4);
    for(; at + 20 <= length; at += 20)
    {
        uint64_t a0, a1;
        uint32_t a2;
        memcpy(&a0, data + at, 8);
        memcpy(&a1, data + at + 8, 8);
        memcpy(&a2, data + at + 16, 4);
        a0 ^= k0; a1 ^= k1; a2 ^= k2;
        memcpy(data + at, &a0, 8);
        memcpy(data + at + 8, &a1, 8);
        memcpy(data + at + 16, &a2, 4);
    }
    for(size_t i = 0; at + i < length; i++)
        data[i + at] ^= archive.key[i];
}
```

**这份补丁已经在 `bench/artemis/src/applykey-check.cpp` 里跑过验证和计时**
（把两个循环原样拷出来对拍，不改引擎）：

| 项 | 结果 |
|---|---|
| 字节等价性 | **368,734 个用例，0 处不一致**：2 种相位规则 × 9 个条目偏移 × 长度 0…4096 × 5 种 `relative`（含 `ExtractTo` 的 262144 分块边界），外加 4 个真实条目大小 |
| 现写法 | **901.5 MiB/s** |
| 20 字节分组 | **23,832.8 MiB/s** |
| 加速比 | **26.4×**（多次测量区间 20×~27×，随机器负载浮动） |

预期落地（`pfsbench memref` 用同样字节数实测）：**0.9 → 20 GiB/s 量级，20 倍以上**。
落到真实路径：
- 202 MB pf8 纯 CPU 解档 245 ms → **~30 ms**（§5.1 的 25.6 ms 理论值 + 一点尾巴）
- 2.0 MB PNG 的 `Content()` 2.143 ms → **~0.15 ms**
- pf8 带盘整卷解档 508 ms → **~290 ms**（剩下的全是写盘）
- 与 Python 的差距从 1.25× 拉回 3~5×

注意三点：
1. `Content()` 现在是"先 memcpy 再原地 XOR"（`pfs.cpp:372-374`），两遍内存流量。
   R1 之后可以考虑 fusing 成"copy+XOR 一遍过"，但那要新 API，建议**先只改 R1 拿 20 倍**。
2. `ExtractTo` 的 `relative` 是跨 chunk 累加的（`pfs.cpp:390`），R1 里的对齐逻辑要按
   `relative` 算，不能假设每次 `relative % 20 == 0`——上面的实现已经按此处理。
3. 改完请拿 `pfstool cat` 出来的 3 个真实条目和 `bench/artemis/out/<tag>/pngs.list` 里的
   73 张 PNG 做**字节级比对**（`pfstool` 自己有 `selftest`），别只测速度。

### R2（最高性价比，~6 行）嗅探改用有界采样

`artemis/tools/pfstool.cpp`：`List()` 的 `--types` 分支与 `Info()` 的 sample 循环，
现在都是

```cpp
std::vector<uint8_t> sample = artemis::Content(a, e);   // 整条解密
if(sample.size() > 512) sample.resize(512);             // 然后扔掉 99.9%
```

改成只解前 ≤384 B。`pfs.cpp` 里已有现成的正确写法 `DecryptSample`（现在是 file-local static），
建议把它**导出到 `pfs.h`**（例如 `std::vector<uint8_t> SampleContent(const Archive&, const Entry&, size_t n)`），
让工具层与相位检测共用一份实现，避免第二份"看起来一样其实差一点"的解密代码。

**补丁已写好、已验等价，可直接应用**：`bench/artemis/patches/R2-bounded-sniff.patch`
（`cd artemis && git apply ../bench/artemis/patches/R2-bounded-sniff.patch`，
`git apply --check` 已通过）。改动三处、共 66 行 diff：

- `src/pfs.h` / `src/pfs.cpp`：新增 `std::vector<uint8_t> Sample(const Archive&, const Entry&, size_t limit)`
  —— 与 `Content()` 完全相同的 `memcpy` + `ApplyKey(..., relative = 0)`，只是只取前 `limit` 字节，
  所以它**必然是 `Content()` 的前缀**，不是另一份"看起来一样"的解密代码。
- `tools/pfstool.cpp`：`List()` 的 `--types` 分支与 `Info()` 的 sample 循环改用 `Sample(a, e, 512)`。

**等价性验证**（`bench/artemis/src/sniff-equivalence.cpp`，链接**打过补丁的 `pfs.cpp`**）：

```
members          1807        (root.pfs 125 + HENPRI.pfs.060 1682 的全部真实成员)
byte_mismatches     0        (Sample(512) 与 Content()+resize(512) 逐字节相同)
kind_mismatches     0        (SniffBytes 判定完全一致)
kind census  png 624 / ogg 773 / jpeg 19 / ttf 2 / text 19
```

收益（同版本库、同数据，7 次交替运行取 min/median）：

| | 现状 `Content`+512 | `Sample(512)` | 倍数 |
|---|---|---|---|
| 嗅探全部 1682 条（R1 之后） | min 40.7 / med 56.5 ms | **min 2.5 / med 3.6 ms** | **16×** |
| 峰值 RSS | 224.1 MiB | **24.4 MiB** | 9.2× |
| （R1 之前的对照） | 244.8 ms | 2.23 ms | 约 110× |

注意：**R1 落地后 R2 的倍数从 110× 降到 16×**，因为 `Content` 那条路本身被 R1 加速了
4 倍；但 R2 依然值 16 倍时间 + 9.2 倍内存，而且它修的是"为了看 512 B 把 202 MB 全解一遍"
这个语义错误，与 R1 是否落地无关。

### R4（顺手）`ExtractTo` 复用缓冲区

`pfs.cpp:383-384`：把 `std::vector<uint8_t> buffer(CHUNK)` 移到循环外，
或按 `min(CHUNK, entry.size)` 分配。实测 4.6 ms → 0.003 ms（整卷 1.9%）。
注意 `ExtractTo` 是每条目调用一次的公共 API，若想真正做到"一次分配"，
可以把缓冲提到 `Archive` 里或由调用方传入——但那会改签名，建议**只做循环外那一步**。

### R5（只在特定用法下需要）`Open()` 的相位检测

`src/pfs.cpp:193-294` 的 `DetectPhase` 每次 `Open()` 都用内容投票，会在 202 MB 卷的
内容区散点触碰 ~48 页（冷 ~5.5 ms）。单次启动无所谓；但如果加载器会"每读一个资产就
`Open()` 一次"，这就是每资产 5 ms 的固定税。两个不改语义的做法：
- 已经支持的 `Open(path, KeyPhase)` 显式传相位（走 `want != Auto` 早退分支，不做投票）；
  长生命周期加载器可以在第一次探测后把结果记下来，后续显式传入。
- 或者给 `Archive` 加一个"跳过检测"的构造开关，让 `ListAll()`（`pfs.cpp:591`）这种
  只列目录的路径不做投票——它现在其实也用不到 `phase`。

### R3（低成本）`Find()` 先把名字规范化好，别再每个条目分配一次

`src/pfs.cpp:514-521`：每次 `Find()` 都对每个候选名 `NormalizeName(e.name)`，
每次分配一个 `std::string`。两种改法，都只动容器层内部：

1. **最省事**：`Parse()`（`pfs.cpp:296`）建条目时顺手把规范化名字存进 `Entry`
   （或另开一个 `std::vector<std::string> normalized`），`Find()` 直接比，不再分配。
2. **最彻底**：`Open()` 时建一张 `std::unordered_map<std::string, size_t>`，
   命中与未命中都变成 O(1)。代价是一次性建表（1682 条 ≈ 0.2 ms），
   换来未命中从 **74.6 µs → ~0.1 µs**。

**两种改法的代价（选一个即可）**：

| 方案 | 改法 | 内存代价 | 启动代价 | 未命中收益 |
|---|---|---|---|---|
| **A（建议先做）** | `Parse()` 时把 `NormalizeName(e.name)` 存进 `Entry`（或并行的 `std::vector<std::string>`），`Find()` 只比不再分配 | 1682 × ~40 B ≈ **67 KB** | 解析时多做一遍小写转换，**< 0.1 ms** | 71.5 µs → 约 **5–8 µs**（仍 O(n)，但不再有 1682 次分配） |
| **B（彻底）** | `Open()` 后**懒建** `std::unordered_map<std::string, size_t>`（第一次 `Find()` 时建），之后 O(1) | 1682 × ~120 B ≈ **200 KB** | 一次性建表约 **0.2–0.4 ms**（1682 次短字符串插入 + 分配） | 71.5 µs → **约 0.1 µs** |

- 两个方案的**内存代价都远小于一次资产解码**（一张 2 MB PNG 就是 8 MB RGBA），可以放心选。
- 启动代价注意：pf8 当前 `Open()` 暖态 **0.40 ms**，方案 B 会把它翻倍。所以建议**懒建**
  （第一次 `Find()` 才建），或者先做 A。
- `FindSuffix()`（`pfs.cpp:523`）也有同样的每条目 `NormalizeName` 分配，它是"按扩展名批量
  找资产"的入口，方案 A 同样受益。
- 未命中 71.5 µs 这个数字是 pf8（1682 条）；pf6（125 条）是 3.4 µs，可见**成本随条目数线性
  增长**，条目越多越值得修。

### 不建议动的

- **RSS 随整卷解档涨到 ≈ 归档大小**：这是 mmap 的正常表现（file-backed clean page），
  单条目路径的峰值 6~10 MiB 已经证明没有额外占用。改掉 mmap 反而会让"打开 202 MB 只要 540 KiB"
  这个优秀特性消失。
- **PNG 解码链**：189.6 MiB/s、比 sips 快 2.2 倍、内存与文件同速。没有优化空间值得占用队友时间。

---

## 12. 版本 → 数字对照

同一天下午，`artemis/` 一直在被三位队友改。三次测量相隔约 2 分钟与 28 分钟，
`libartemis.a` 换了三个；`src/pfs.cpp`（容器层）全程 mtime 13:29:42 未变。

| 指标（p50） | V1 `15:51:11` | V2 `16:01:59` | V3 `16:17:20` | 三版极差 |
|---|---|---|---|---|
| `Open()` pf6 | 0.123 ms | 0.111 ms | 0.117 ms | 10% |
| `Open()` pf8 | 0.341 ms | 0.374 ms | 0.403 ms | 18% |
| 顺序解档 pf6（CPU） | 6.257 ms | 5.346 ms | 6.121 ms | 17% |
| 顺序解档 pf8（CPU） | 254.799 ms | 245.379 ms | 250.943 ms | 4% |
| 随机 `Content()` pf8 | 0.050 ms | 0.046 ms | 0.048 ms | 9% |
| `Content()` 2.0 MB PNG（pf8） | 2.114 ms | 2.143 ms | 2.352 ms | 11% |
| `Content()` 4.9 MB TTF（pf6） | 0.225 ms | 0.223 ms | 0.221 ms | 2% |
| 嗅探全部 pf8（现状） | 233.478 ms | 241.066 ms | 244.400 ms | 5% |
| 嗅探全部 pf8（有界） | 1.990 ms | 2.234 ms | 2.110 ms | 12% |
| PNG 解码 73 张（内存） | 165.666 ms | 172.333 ms | 166.045 ms | 4% |
| Python 解档 pf8 | 624.894 ms | 635.986 ms | 635.694 ms | 2% |
| Python 解档 pf6 | 112.334 ms | 123.105 ms | 139.455 ms | 24% |
| `pfstool list pf8`（含启动） | 8.579 ms | 10.363 ms | 14.545 ms | 70% |
| `pfstool extract pf8`（含写盘） | 521.846 ms | 508.599 ms | 616.638 ms | 21% |
| `pfstool extract pf6`（含写盘） | 49.880 ms | 35.665 ms | 41.536 ms | 39% |
| fixture 首帧 | 41.639 ms | 46.499 ms | 41.257 ms | 13% |
| 真实首帧（归档） | — | — | 50.883 ms | 仅 V3 有此用例 |
| 真实首帧（解包目录） | — | — | 55.233 ms | 仅 V3 有此用例 |

> 上表全部是 **R1 之前**的数字（`pfs.cpp` 16:27:46 才被改）。R1 之后的对照在 §12.0.1，
> 那里 `seq_pf8` 从 245~255 ms 变成 **37~50 ms**，`one` 从 2.1~2.4 ms 变成 **0.12~0.14 ms**。

**读法**：库内测（`pfsbench_*`、`pngbench_*`）三版之间的极差都在 **≤18%**，而同期
`vm.loadavg` 在 **13→38**（V3 期间还有一次到 64）之间跳，这已经大于被测代码可能造成的变化。
**容器层的 11 个指标里没有一个出现单向漂移**：`pfs.cpp` 自 13:29:42 起没被改过，
三位队友这三次重链接加的是 `iet.cpp` / `ipt.cpp` / `render/`，都没进容器的热路径。

进程级用例（含 `fork+exec` 与磁盘）波动明显更大，逐个解释：

- `pfstool list pf8` **+20.8%**：同一次运行里进程启动地板从 7.234 ms 涨到 8.589 ms（+18.7%），
  因为 `libartemis.a` 从 279,520 B 涨到 302,888 B（新加的 `iet.cpp`/`ipt.cpp` 进了库），
  pfstool 也跟着从 125,016 B 涨到 141,656 B，dyld 要多映射一圈。**容器层本身没变**：
  `list pf8 − 启动地板` = 1.35 ms（V1）vs 1.77 ms（V2）。
- `pfstool extract pf6` **−28.5%**（V1→V2）：被磁盘写与页缓存状态支配，两次运行的 p95 分别是
  87 ms 与 186 ms，p50 差异是缓存命中的运气，不是代码。
- `pfstool list pf8` **V3 又涨到 14.5 ms**：同一个原因——`pfstool` 二进制的体积从
  125,016 B（V1）→ 141,656 B（V2/V3）一路变大，进程启动地板同步抬高；
  V3 那次 loadavg 也最高。`list pf8 − 启动地板` 在 V1/V2 分别是 1.35 / 1.77 ms，
  **容器层要花的钱一直是 1~2 ms**。

### 12.0 一句话：容器层已经稳定，别再盯它

三次测量跨了三个构建、30 分钟、loadavg 13~64，容器层的每一条数字都落在 ±18% 里，
且没有方向性。R1 已落地（§12.0.1：整卷解档 6.8×、单资产 17–19×）之后，
**容器层已经没有值得再动的热路径**；剩下的收益在 R2（嗅探 16×，补丁已验等价）
与 §8.1.3（逐帧 PNG 落盘不属于运行时热路径）。

### 12.0.1 R1（`ApplyKey` 20 字节分组）落地后的前后对照

lead 在 `pfs.cpp` 应用了 R1（16:27:46 修改），并在 16:27:56 完成重链接。
对照方法：**同一份 `pfsbench` 源码，分别链接 R1 前（`out/v3-1622/pfsbench`，lib
`cbaed6832429`）与 R1 后（`/tmp/pfs-new`，lib `615a544345d2`）的 `libartemis.a`**，
7 次交替运行取 min 与 median（交替是为了让两次测量看到同样的页缓存状态）：

| 指标 | R1 前 | R1 后 | 加速 |
|---|---|---|---|
| **整卷解档 pf8（202 MB，纯 CPU，无盘）** | min 254.7 / med 342.6 ms | **min 37.3 / med 50.2 ms** | **6.8×** |
| 同上，折算吞吐 | 792 MiB/s | **5,405 MiB/s** | 6.8× |
| **`Content()` 一张 2.0 MB PNG** | min 2.09 / med 2.64 ms | **min 0.12 / med 0.14 ms** | **19×** |
| **`Content()` 一段 192 KB 脚本** | min 0.20 / med 0.21 ms | **min 0.011 / med 0.012 ms** | **17×** |
| 随机 1000 次 `Content()`（pf8） | min 116.1 / med 118.6 ms | **min 13.6 / med 14.6 ms** | **8.1×** |
| 嗅探全部 1682 条（现状 `Content` 路径） | min 40.7 / med 56.5 ms | 同左（这就是 R2 要改的） | — |
| `pfstool extract` 202 MB 到盘（墙钟） | min 471 ms（V2）/ 509 ms（V2 p50） | **min 370 ms / real 0.35 s，user 0.03–0.04 s** | 1.3×（已变 I/O 受限） |

**怎么读**：

- 整卷解档的 **CPU 侧**从 792 MiB/s 到 5,405 MiB/s，**6.8 倍**，与 `applykey-check` 预测的
  20 倍之间的差距就是剩下的 memcpy + 写 `/dev/null`（现在这些才是大头）。
- **单资产加载（引擎真正在乎的路径）快了 17~19 倍**：一张 2 MB 的 event CG 从 2.6 ms
  降到 0.14 ms，一段 192 KB 脚本从 0.21 ms 降到 0.012 ms。此时已经接近 memcpy 速度
  （2.0 MiB / 0.14 ms ≈ 14.6 GiB/s），容器层不再是资产加载的成本项。
- **写盘整卷解档只快 1.3 倍**，因为它现在 100% 等磁盘：user CPU 从 ~0.25 s 降到
  **0.03–0.04 s**，剩下 0.35 s 墙钟全是写 202 MB。要继续压这一项得改 I/O 策略
  （`ExtractTo` 的缓冲、并行写），不是改 XOR。
- 明文卷（pf6）不受影响，符合预期。

**V4 全量复跑（16:33，`out/v4-r1`，`libartemis.a` 16:27:56 `615a544345d2`）的旁证**：
那一轮赶上 loadavg 尖峰，p50 明显被抬高（`agame_real_archive` 的 p50 从 50.9 涨到 225.9 ms），
所以看 **min** 更可信：`seq_pf8` **44.9 ms**、`one_pf8_png` **0.125 ms**、
`one_pf8_ast` **0.012 ms**、`rand` 1000 次 **6 µs/次**、嗅探全卷 `Content` 路径 **44.3 ms**
vs 有界 **2.18 ms**。方向与 §12.0.1 的主表一致，绝对值受负载影响。
**负载尖峰期的数字不要单独引用**——这也是本报告一律同时给 min 与 p50 的原因。

**等价性**：R1 的补丁在 `bench/artemis/src/applykey-check.cpp` 里被验证过
**368,734 个用例 0 处字节不一致**（覆盖 2 种相位规则 × 9 个条目偏移 × 长度 0…4096 × 5 种
`relative`，含 `ExtractTo` 的 262144 分块边界）。lead 另外用 `pfstool/asttool/asbtool/iettool`
的 `selftest` 与 30 项独立验收复核过。

### 12.0.2 R3（`Find()` 预存规范化名）落地后的前后对照

lead 于 17:21 应用了 `patches/R3-find-precomputed-name.patch`（提交 `1e3ae02`）。
复测用**仓库里现在的 `pfs.cpp`**（R3 已在其中）对 **R3 之前**的 `pfs.cpp`，
同一份 `pfsbench` 源码，7 次交替运行：

| 指标（pf8，1682 条） | R3 前 | R3 后（当前仓库） | 加速 |
|---|---|---|---|
| **`Find()` 未命中** | 74.6 µs | **2.00–2.63 µs** | **约 37×** |
| `Find()` 命中 | 0.291 µs | **0.041 µs** | 7.1× |
| `Open()` 暖态（代价侧） | 0.179–0.303 ms | **0.178–0.218 ms** | **无可测退化** |
| 峰值 RSS | 1.89 MiB | 1.89 MiB | 持平 |
| pf6（125 条）未命中 | 3.42 µs | **0.25 µs** | 14× |

等价性：`src/find-equivalence.cpp` 对 **3,058 条查询（2,305 命中 + 750 未命中）**
逐行 diff 为空，含构造的重复名归档（`a\b\dup.png` / `A\B\DUP.PNG` / `a/b/dup.png`
三条规范化后相同，first-match 仍返回 index 0，`FindSuffix` 仍返回 3 条）。

**这个补丁的第一版测量是错的，值得记一笔**：第一次测得 `Open()` 从 0.20 ms 涨到 1.9 ms（9.5×），
差一点当成"R3 的代价"报上去。实际是**构建错**——`pfsbench.cpp` 里写的是相对 include
`"../../../artemis/src/pfs.h"`，而补丁版的 `pfs.cpp` 通过 `-I` 找到了另一个 `pfs.h`，
同一个二进制里存在两个 `struct Entry` 定义（ODR violation），`entries` 数组被写坏成
14757395258967643984 条，最后 SIGBUS。修法是把 bench 源码放进一棵镜像目录树，
让两边的 `pfs.h` 解析到同一个文件。
**教训：等价性/性能对照只有在一份头文件定义下才算数。**

### 12.1 为什么容器层数字能跨版本直接比

- `pfsbench`/`pngbench` 在 `bench.sh` 开头**链接当下的 `libartemis.a` 并固化**，
  所以它们的每个数字都锁定在 `env.txt` 里记的那一个 sha 上，天然不会被运行中的
  重链接污染。`bench.sh` 还会在跑完后复核一次二进制 sha，变了就打印
  `!! a binary was rebuilt during this run`（V1 那一轮就打印了，所以 V1 的 `pfstool_*`
  行需按"跨两个构建"读；库内行不受影响）。
- 容器层代码 `pfs.cpp` 三个版本完全一致，因此"版本 → 数字"在这里的意义是
  **证明容器层已经稳定**：三位队友的改动（Lua AST、IPT/IET 解码、ASB）没有碰到热路径，
  也没有把容器层拖慢。

### 12.2 V3（隔 20 分钟复测，16:22 运行）

| 项 | 值 |
|---|---|
| tag | `out/v3-1622` |
| 运行时刻 | 2026-10-25T16:22:26+0800 |
| `libartemis.a` | 16:17:20 `cbaed6832429`（302 KB，新加 `render/`） |
| `pfstool` | 16:18:58 `7075b9f317a6` |
| `agame` | 16:21:06 `db9670203ce3`（381 KB，链 `artemis_render`） |
| `artemis_head` | `5e0bf77771ba332ff4d9b11f5e6afe575790b14b` |
| 与 V2 的差别 | 容器层零漂移（见 §12.0）；新增真实首屏、`Find()`、`ApplyKey` 补丁验证、vmmap、2891 条索引五组用例 |

复测不靠手抄：`bench.sh` 每次运行都产出 `out/<tag>/{env.txt,results.csv,results.json,raw.tsv}`，
上面三列就是三份 `results.csv` 的同名行。第四次运行时换个 `TAG=` 即可再添一列。

---

## 13. 局限与还没做的

1. **首帧已经用真实素材测了（§8.1），但走的是 `.iet` 宏表路径**；真实 `.ast` 脚本仍然在
   AST 层解析失败（`ast: unexpected token at byte 16649`，§8 有原文）。`.ast` 打通后
   重跑 `bench.sh` 会自动补上 `real_ast_first_frame` 一列，无需改脚本。
   另外 §8.1.3 的渲染/捕获拆分是**外部**测的（A/B 两种输出方式 + 独立复算编码成本），
   引擎内部的 render 计时器能给到更细的逐页拆分，两边应该对得上，值得对一次。
2. **没有 I/O 冷态数字**。`sudo purge` 会清系统缓存，本基准刻意不碰系统状态；
   所以随机访问那一项给的是"页在缓存里但未驻留"的数字，真正的冷盘延迟会更高。
   要测的话单独跑 `sudo purge && ./bench/artemis/bench.sh`，结论表里的"冷"列才有意义。
3. **机器负载不是干净的**（load 13~38）。所有表都同时给 min 与 p50，
   比值稳定说明结论不受影响；要绝对干净的数字，请在空闲机器上跑同一条命令。
4. **没有测多线程/并发加载**。引擎最终会并行解码多个资产，那时 `ApplyKey` 的
   逐字节循环还会额外吃满并行的收益（内存带宽不是瓶颈才能真正并行）；
   R1 修好之后再测并发才有意义。
5. ~~这些文件被 git 忽略~~ **已修**：lead 把根 `.gitignore` 的 `artemis/` 改成 `/artemis/`，
   `bench/artemis/` 现在可以入库。
6. **存档层未就绪**：`src/save/` 只有 `json.{h,cpp}`，没有 `savetool`。基准记
   `save_layer not ready` 并跳过，等它出现后加两行 case 即可。
7. **IPT 合成的层内成本拆不出来**：`iettool` 不自报计时，只能给"合成本身远小于输出 PNG 编码"
   的边界（§8.2.2）。要精确拆分需要 `iettool` 加一行自报计时。
8. **`agame --ast-info` 的 0.54 s → 0.112 s 无法归因**：同一用例在本机 load 14 时是
   104–122 ms、load 57 时是 150–340 ms，而早期那次 0.54 s 的机器状态没有记录。
   **不写"因为修了 CLI 路由所以快了 4 倍"这种没有证据的因果**；要归因需要在一台安静机器上
   对两个版本各跑 7 次。
9. **`asttool` / `asbtool` 只测了一条粗粒度用例**（`asttool dump` 单文件、`agame --ast-info`
   整语料）。它们正在被队友大改（mtime 一直在动），
   测出来的数字十分钟后就作废。等它们的接口稳定后加 case 即可，`bench.sh` 的
   case 表是数据驱动的，加一行就行。
