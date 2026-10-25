# C3 稳定性缺陷 —— 技术交接

> 目标项 C3：真实商业游戏《トライアンソロジー》体验版能否连续跑 10 分钟不崩溃。
> **当前状态：失败。约 25 秒必崩。根因未定位，但已排除 7 条路径，并锁定问题类别为
> use-after-free。**

## 症状

| 构建 / 配置 | 存活时间 |
|---|---|
| Release（`-O3`） | 24–26 秒 |
| Release `-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks` | 25 秒 |
| Release + `MallocNanoZone=0` | 30 秒 |
| Release + `--no-video` | 26 秒 |
| Release + `ONS_NO_OGGLOAD=1` | 26 秒 |
| Release + 两者同时 | 26 秒 |
| Release + `MallocScribble=1` | 26 秒（稳定复现） |
| **ASan（RelWithDebInfo + `-fsanitize=address`）** | **120 秒、260 秒均存活** |

崩溃点**随运行游走**（这是关键特征）：

```
VideoToolbox  vtDecompressionDuctEmitDecodedFrame
CoreMedia     FigXPCMessageCopyCFObject / memoryOriginServer
libxpc        _xpc_alloc_typed
SDL3          SDL_GetSurfaceProperties -> SDL_CreateProperties -> SDL_calloc
libvorbis     ov_pcm_seek -> _make_vds_shared_init
IOSurface     IOSurfaceClientSetValue -> IOCFSerialize -> CFDataInit
```

即：崩溃点永远在**分配内存的那一刻**，且每次都不是同一个模块。

## 已排除的路径（都做过对照实验）

| # | 假设 | 实验 | 结论 |
|---|---|---|---|
| 1 | 影片解码销毁竞态 | 加宽 reader 拆解等待 50ms → 300ms | 不是根因（崩溃点只是移走） |
| 2 | 影片路径 | `--no-video` | 排除（反而更早崩，因为更快进入别处） |
| 3 | OGG/vorbis 路径 | `ONS_NO_OGGLOAD=1` 完全跳过 | **排除**（崩溃照旧） |
| 4 | SDL_mixer RWops 所有权 | `freesrc=0` → `1` | 是真实泄漏，但不是崩溃原因 |
| 5 | OGG 被当作 chunk 加载 | 嗅探 `OggS` 改走 music 路径 | 是真实 UAF，但不是这个崩溃 |
| 6 | 编译器优化 / 严格别名 UB | `-O2 -fno-strict-aliasing` | 排除 |
| 7 | macOS nano 分配器布局 | `MallocNanoZone=0` | 排除 |

## 关键推理：为什么是 use-after-free 而不是写越界

ASan 与系统 malloc 的差异有两处：红区、以及**隔离区（quarantine）**。
第 7 项排除了分配器布局，于是关键差异只剩隔离区：

- 系统 malloc：`free` 后内存**立刻可被复用**
- ASan：释放的内存进入隔离区，**一段时间不还给任何人**

若某段代码在我们 `free` 之后仍持有并使用那块内存：

- 系统 malloc 下：内存已被别人占用 → 数据被踩坏 → 崩溃（在下次 malloc 时爆）
- ASan 下：内容仍是旧值、没人动过 → 读到的值"合理" → 不崩

**这解释了三件事**：崩溃点游走（受害者随机）、ASan 零越界报告（因为它不是越界）、
图层绘制约六成成功率（损坏不总踩到关键数据）。

## 已做的其它排除

- 空文件名 `*** can't find file [] ***` 在日志里出现过，但**也是间歇性的**
  （加了 `ONS_TRACE_EMPTYNAME=1` 探针后，下一次运行没出现）。不能作为固定入口。
- `music_buffer` 不释放（`ONS_NO_FREE_MUSIC=1`）后仍崩 → 排除这块缓冲区。
- 文字路径的 `TTF_RenderGlyph_Shaded` surface 释放顺序**是对的**（先 blit 后 free）。

## 下一步（按性价比排序）

### 1. 用 ASan 的 quarantine 做二分（最便宜）

ASan 下不崩、系统下崩。那就**缩小 ASan 的隔离区**，让它接近系统行为：

```
ASAN_OPTIONS=quarantine_size_mb=0
```

如果这样 ASan 版也崩了 → 确认是 UAF；然后逐步加大 quarantine，观察崩溃何时消失，
可以估算"被 UAF 的那块内存有多大、活多久"。

### 2. 追踪所有 free 的调用栈，对比崩溃时间点

用一个 `free` 钩子（`MallocLogFile` 或 LD_PRELOAD）记录最近 N 次 free 的调用栈，
崩溃时 dump 出来。UAF 的那次 free 会出现在窗口内。

### 3. 自建带 ASan 的依赖链（最贵但最确定）

freetype / harfbuzz / SDL_ttf / sdl2-compat 全部用 `-fsanitize=address` 编一遍。
那时越界/UAF 的**写入方**也被插桩，ASan 会直接指出行号。

工作量：编 4 个 C/C++ 库 + 重链引擎。预计 2–4 轮。

### 4. 临时规避（如果能接受）

把可疑缓冲区改成**永不释放**（进程级缓存）。已经试过 `music_buffer` 不行，
下一个候选是文字缓存 surface 和 `accumulation_surface` 的像素指针。

## 不要重复的弯路

- 不要再按"崩溃点在哪"去推断根因——崩溃点是受害者，不是凶手
- 不要再用单次运行下结论——图层绘制约六成成功，单次结果没有判别力
- 不要相信"修了某个真 bug 所以崩溃应该好了"——已经修了 3 个真 bug，崩溃依旧

## 复现命令

```bash
cd ~/Documents/gal-engines/testdata/ns-07th-trt/game/trt_trial
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ONS_AUTOCLICK_MS=1500 ONS_AUTOCLICK_X=305 ONS_AUTOCLICK_Y=424 \
  ../../../../OnscripterYuri/build/onsyuri --root . --enc:sjis --window --no-video
# 约 25 秒后 Trace/BPT trap: 5
```

## 相关工具（本项目已有）

| 工具 | 用途 |
|---|---|
| `ONS_CRASH_HANDLER=0` | 关闭崩溃回溯打印 |
| `ONS_TRACE_LAYER_FRAME=1` | 图层取帧/绘制计数 |
| `ONS_TRACE_EMPTYNAME=1` | 空文件名调用栈 |
| `ONS_TRACE_CMDS=1` | 逐条脚本命令追踪 |
| `ONS_TRACE_EFFECT=1` | 效果计数/时长 |
| `ONS_SURFACE_DUMP=<file>` | 每 500ms 落一帧（原子写） |
| `ONS_AUTOCLICK_MS/X/Y` | 定时点击，可指定坐标 |
| `ONS_NO_FREE_MUSIC=1` | 不释放 BGM 缓冲区（二分实验用） |
| `ONS_NO_OGGLOAD=1` | 跳过 Lua 侧 OGG 加载 |
| `test/video/long_run_check.sh` | 长跑 + RSS 采样 |
| `test/video/loop_check.sh` | 循环播放验证 |
| `test/video/layer_ops_check.sh` | 图层定位/暂停/重开 |

---

## 追记（第 46 轮）：隔离区不是关键，红区理论成立

### 实验

```
ASAN_OPTIONS=quarantine_size_mb=0:handle_abort=0:handle_segv=0
→ 存活 150 秒
```

关掉 ASan 的释放隔离区后**依然不崩**。

### 于是理论定稿

ASan 与系统 malloc 的差异里，**起作用的是红区（redzone），不是隔离区**：

- 每次分配前后各加一段保护区，**小幅度越界写不会碰到相邻分配**
- 系统 malloc 紧挨着放，越界几个字节就写坏了邻居

**结论：某个预编译库有轻微越界写（几个字节），被 ASan 的红区吸收，
在系统分配器下则破坏相邻堆块，崩溃点是随机的受害者。**

### 这个理论为什么之前被推翻过

第 42 轮我说过"ASan 也崩，所以红区理论不成立"。
但那次 3 分钟的崩溃**是 ASan 自己的信号处理器 abort 的**；
用 `handle_abort=0:handle_segv=0` 让 ASan 放手后，它活了 260 秒。

**教训：ASan 的默认行为会掩盖它本来没检测到的东西。**

### 现在的定论（可以作为交接结论）

| 项 | 结论 |
|---|---|
| 类别 | 轻微堆越界写（overrun），不是 UAF |
| 位置 | 某个预编译库（freetype / harfbuzz / SDL_ttf / sdl2-compat 之一） |
| 为何 ASan 沉默 | 写入方未插桩，ASan 只在被插桩代码写红区时才报 |
| 为何崩溃点游走 | 越界只破坏相邻块，受害者每次都不同 |

### 可行的规避方向（不需要编依赖链）

既然越界只有几个字节，那就**给缓冲区加尾部 padding**。候选：
1. 引擎里交给 SDL_ttf/SDL_blit 的目标 surface，按 32 字节对齐 + 额外 64 字节尾部余量分配
2. `accumulation_surface` 同上

如果越界来自预编译库写我们分配的 surface，加 padding 就能把破坏挡在缓冲区内部，
崩溃消失（数据可能仍有个别字节错，但不会踩坏别人）。

这条比自建 ASan 依赖链便宜得多，且可以立刻验证。

---

## 追记二（第 47 轮）：padding 规避也无效

### 实验

给 `AnimationInfo::allocSurface` 加了一条 padding 路径：pitch 对齐 + 额外 4 行 +
尾部余量，用 `SDL_CreateRGBSurfaceFrom` 建在我们自己分配的大缓冲区上。

```
ONS_SURFACE_PAD=64  →  30 秒崩
```

**无效。** 所以越界不发生在 `allocSurface` 分配的 surface 上（至少不只是）。

### 至此排除清单（8 条）

| # | 假设 | 结果 |
|---|---|---|
| 1 | 影片解码销毁竞态 | 不是根因 |
| 2 | 影片路径 | 排除 |
| 3 | OGG 路径 | 排除 |
| 4 | RWops 所有权 | 真泄漏，非崩溃原因 |
| 5 | OGG 当 chunk | 真 UAF，非此崩溃 |
| 6 | 编译器优化 / 严格别名 | 排除 |
| 7 | nano 分配器 / 隔离区 | 排除 |
| 8 | surface 尾部越界 | **排除** |

### 定论

- 类别：**轻量堆越界写**（ASan 红区吸收、系统分配器下破坏邻居）
- 位置：某个预编译库，且**不是**写 `allocSurface` 的 surface
- 剩余候选：文字缓存 surface（`AnimationInfo` 内部另建的）、
  `TTF_RenderGlyph_Shaded` 返回的 surface、`SDL_Texture` 的后备像素
- **无法靠改 OnscripterYuri 的分配代码规避**——已试过两条

### 结论：必须走方案 3

自建带 ASan 的 freetype / harfbuzz / SDL_ttf / sdl2-compat。
这是唯一能把"未插桩的写入方"也纳入检测的办法。

**17 轮追踪的最终成果：问题类别的确定 + 8 条路径的排除 + 一个明确的下一步。**
这比"再猜一个假设"有价值得多。
