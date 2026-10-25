# Engine ④ Ethornell / BGI — OpenBGI @ macOS arm64

## 状态：✅ 补全完成（2026-10-25 第二轮，lead + 3 名队友）

| 项 | 结果 | 证据 |
|---|---|---|
| 构建 | ✅ `make PLATFORM=darwin -j8` → `bin/bgi`（Mach-O arm64） | 本文「验证」节 |
| 单元测试 | ✅ **21/21 全绿**（原 16 + 新增 5） | `bin/test_*` |
| 真实商业游戏 | ✅ 《遥か碧の花嫁に 体験版》engine 1.669：开机 → 标题 → **点进正文剧情** | `screenshots/haruka-*.png` |
| 验收脚本 | ✅ `../verify.sh`（build + 单测 + 真实游戏 30s + 深度点击进正文） | `openbgi/verify-out/` |
| 影片 | ✅ 外部影片(90 F0)/档案内影片(92 F0)/视频纹理(91 F0..F7, 92 F2..F5) | 真 584MB `movie_op.mpg` 解码 178 帧/6s |
| CBG alpha | ✅ CBG v2 mode 2（Huffman alpha 平面）实现，反汇编逐条对照 | 1.669 exe 反汇编证据链 |
| 3D | ✅ `90 5D` 三轴旋转 + 透视单应渲染 | 60 组参数与 90 5C 逐像素 0 diff |
| macOS 效率 | ✅ arm64 NEON blit + 直通上屏；满载 CPU **−39.4%**，静置 2% | 交错 A/B 四组 |

## 上游与我们的炉灶
- 上游：https://github.com/Cytlan/openbgi （`upstream-base` tag = 2026-10-21 快照；本轮开工时上游最新提交为 2026-10-20，无新可合）
- 本目录 `openbgi-src/` 是一个 git 仓库，第一个提交是上游原样快照，之后全部是本项目的移植与补全
- 许可证 GPL-2.0-only；新文件都带同款 SPDX 头，符合 CONVENTIONS.md
- `tools/` 是上游命令行工具的**独立旧快照**（另一套 Makefile/源码树），不参与引擎构建；本次补全**没有**同步过去

## 这是什么引擎
Ethornell「Buriko General Interpreter」。覆盖 Lump of Sugar 系（Tayutama / 学王 / 世界与世界的正中央 / Magical Charming! 等）。

## 移植架构
上游 OS 抽象层 `inc/bgi/os.h` 有 800 行接口，已有 win32 / posix(X11+ALSA) / wasm 三套后端；本项目新增第四套 **`src/os/darwin/`（SDL2）**：

| 文件 | 说明 |
|---|---|
| `window.c` | SDL2 窗口/渲染/事件泵/光标/显示模式/呈现（`Dwin_Present`） |
| `audio.c` | SDL2 拉取式音频回调 + MCI/CD/PlaySound 失败型 stub |
| `stubs.c` | 仅剩启动器/安装器对话框等少数低频面（见「已知缺口」） |
| `dchild.c` | 子窗口（`B0 10..1C`，最多 8 个），独立 SDL 窗口 + 32 位表面 |
| `dedit.c` | 文本输入框（`B0 20..29`）：叠在画面上、透明背景、IME/UTF-8↔Shift-JIS |
| `ddialogs.c` | 命名/资料/选择对话框 + 文件/文件夹对话框（`B0 84/85/87/8C/8F`、`B0 8B`、`81 38`、`81 3A`） |
| `duikit.c` + `duifield.c` + `dui_internal.h` | 自绘对话框工具箱与单行文本编辑控件（照 POSIX 后端的软件 UI 移植到 SDL） |
| `ddbgwin.c` | 调试器窗口（F12） |
| `font.c` files.c machine.c native.c paths.c pipeproc.c process.c registry.c sjis.c thread.c time.c | 从 posix 复制，改 macOS 差异点 |

**渲染模型**：引擎自己算像素，OS 层只做搬运。blit 合成到「画布尺寸」staging surface，`OS_WindowReleaseDc`/每条 blit 末尾即 present 点；`SDL_RenderSetLogicalSize` 负责等比 letterbox，鼠标坐标反向映射回画布坐标。

## 构建
```bash
brew install sdl2 freetype
make PLATFORM=darwin -j8          # → bin/bgi (Mach-O arm64)
make PLATFORM=darwin tools        # → bin/bpasm bgiscn bgidump bgipack
make PLATFORM=darwin test         # 20 个测试
# 并行/隔离构建（本轮新增，互不踩 obj 与单实例锁）：
make PLATFORM=darwin OBJDIR=obj-x TARGET=bin/bgi-x BINDIR=bin-x -j8
```
Makefile 新增 `OBJDIR ?= obj/$(PLATFORM)`、`BINDIR ?= bin` 覆盖点（多个构建可同时存在）。

## 本轮（2026-10-25 第二轮）补全内容

### A. darwin UI 层（原来是 stubs.c 全失败）—— task-1 交付
- **子窗口**（`B0 10..1C`）：8 个独立 SDL 窗口，引擎 back buffer → `child_paint` 呈现，`child_destroyed` 回收；键事件转发主窗口
- **文本输入框**（`B0 20..29`）：EDIT 语义（ES_CENTER/ES_NOHIDESEL 行为），叠在画面上透明绘制，光标/选全/长度限制/只读数字/颜色；UTF-8 SDL_TEXTINPUT ↔ CP932
- **对话框**：`OS_DialogInput1`（0x71）、`Input2`（0x72）、`Input2Ex`（0x7E 并排）、**`OS_DialogSelect`（B0 8C 列表选择，本轮新增 OS 契约）**、`Profile`（0x77 姓名/生日，含半角校验与月天数联动）
- **文件/文件夹对话框**：`OS_FileDialog`、`OS_FileDialogN`、`OS_BrowseFolder`（自绘列表 + 过滤/默认扩展名/初始目录）
- **调试器窗口**：`OS_DebugWindow*` 真窗口呈现
- 无人值守开关：`BGI_AUTO_DIALOG=1`（自动选第一项/确认）、`BGI_AUTO_FILE=<path>`（文件对话框直接返回该路径）
- **修复**：无主窗口/无事件源时所有对话框必须立即返回「取消」（否则 `test_vm` 的 `81 38` 会永久 hang —— 这是本轮抓到的一个真实回归，已修并回归测试）

### B. 影片 —— task-2 交付
- **90 F0 外部影片**：darwin 上 `OS_MovieOpen` 恒 FAILED → ffmpeg 回落（`MovieFx`）真解码；长度/停止/音量/状态机全通
- **92 F0 档案内影片**（本轮从空壳变真实现）：`(arc,name)` 按 loader 语义解析（`gBaseDir`→`gAltDir`→原名；`data02xxx.arc` 是 `ipl._bp` 注册的 FileSetList 伪档案，必须拼成 `C:\data02xxx.arc` 才能命中）→ `ArcMgr_ReadRange` 4MB 分块流式解到 `$TMPDIR/openbgi-movies/<arc>__<name>__<size>.mpg`（`.part`+rename 原子落盘，同名同大小复用）→ `Movie_PlayNative`
- **视频纹理**（`92 F2/F4/F5` + `91 F0..F7`，新文件 `src/gfx/vidtex.c`）：把档案里的 BF_Movie 挂到 bitmap 槽，按 fps 墙钟逐帧解码（复用 `BmSeq`/`BfMovie_DecodeFrame`），槽 generation 变化自动 drop；错误码 1/2/3/4 真实返回
- **修正**：`91 F1` / `92 F5` 的原注释把压栈顺序写反，真实脚本是 `(&out, bmp)`（`moviemngr._bp`）；`92 F4 control=0` 返回 1 正好满足 `bsctrl2` 的等待循环

### C. CBG alpha —— task-3 交付
- **CBG v2 mode 2（Huffman 编码的 alpha 平面）从「未实现→不透明」变为真解码**。格式由 1.669.3 的 `haruyome_trial.exe` 逐条反汇编确定：
  - `offsets[rows]` 处：`u32 mode=2 | u32 count | 256 varint 权重 | MSB-first 位流`
  - 解出 `count` 字节 = `(rows*cols+7)/8` 的 8×8 block 存在位图（光栅序、字节内 LSB first）+ 每个置位 block 的 `bw*bh` 个 alpha 字节（块内按行）
  - 未置位块原版不写（给动画帧留上一帧平面）；本实现在 CBG2 单帧路径先填 0xff（不透明）再覆盖
- 反汇编证据链：`0x4b68a0`(入口) → `0x4b31a0/0x4b31d0`(type 0x10001 帧解码) → `0x4b3870`(job 分发: mode1→`0x4b6290`, mode2→`0x4b6520`+`0x4b5f00`)、`0x4b56d0`(符号读取 MSB-first)、`0x4b5bb0`(BuildTree，与现有 C 实现同一 quirk)；**mode 1 的 `0x4b6290` 与现有 `AlphaLz` 逐行一致**（解读口径的强证据）
- **`Cbg2_DecodeSized(src,size,dst)`**：size 已知时 header/offset/band/alpha 每一处读取都夹在文件长度内（顺带修掉 band `b.len` 退化成 `0x7fffffff` 的潜在越界）；`loadfile.c` 的两个解码点已把已知长度传进去
- 真实数据回归：44 个 arc、2201 张 32bpp CBG、47.2 亿像素，alpha 精确 0x00 占 81.49%、0xff 占 17.19%、中间值 1.32%（1752 张混合图）→ 不是全 0/全 255 的错误产物

### D. 3D 精灵 —— task-4 交付
- **`90 5D`（1.588+）三轴旋转/透视从「忽略旋转」变为真渲染**。模型（16.16°；图像 y 向下、+z 远离观察者）：
  ```
  rotX: y1 = v·cosX,            z1 = v·sinX
  rotY: x2 = u·cosY + z1·sinY,  z2 = -u·sinY + z1·cosY
  透视: k = projDist / (projDist + fz + z2)   （projDist=0 时 k=1）
  平面: A = angle + rotZ（Z 旋转与透视分母可交换 → 就是 90 5C 的平面旋转）
  ```
- 新文件 `src/gfx/blit_persp.c`（四角投影 → 3×3 单应求逆 → 逐像素逆映射，近邻/4bit 双线性，采样与边界规则同 `Blit_Xform`）；`sprite.c` 接 3D 状态与绘制，`gfxcall.c` 包装，`ops_gfx0.c` 传入三轴角
- **rotX=rotY=0 严格走 90 5C 原路径**（逐像素 0 diff），行为零变化
- 视觉证据：`screenshots/sprite3d_composite.bmp`、`sprite3d_perspective.bmp`、`sprite3d_rotation.bmp`

### E. macOS 效率（本轮合计 **−39.4% CPU**，满载）
- 基线（title 等待态）：**321.7 pass/s，3.11 ms/帧，0.256 ms CPU/pass**；静置 CPU 约 **2%**；实际游戏正文约 20% 单核（持续动画 + BGM）
- **NEON blit（task-4）**：`Blend32_Plain`、`Tint32_Core`、`AlphaCopy32_To1` 三条 32 位内核加 NEON 路径（只在 `__ARM_NEON` 编译，标量是定义）；微基准 **blend 3.33×、tint 3.62×**
- **直通上屏（lead，present 快路径）**：整帧 blit 直接从源上传纹理，不再先拷进 staging 再拷进纹理（每帧省一次 1920×1080 = 8.3MB 拷贝）；`BGI_DUMP_STAGE`/`BGI_SCREENSHOT` 会自动退回旧路径保证诊断正确
- 交错 A/B（`BGI_ALWAYS_PRESENT=1` 满载，12s 窗 × 4 轮，中位数，功耗以 CPU-s/s 计）：
  | 配置 | CPU-s/s | 相对基线 |
  |---|---|---|
  | 优化前（标量 blit + 旧上屏） | 0.3021 | — |
  | 只 NEON blit | 0.2778 | −8.0% |
  | 只直通上屏 | 0.2187 | −27.6% |
  | **两者都开（默认）** | **0.1830** | **−39.4%** |
- 逐字节正确性：`bin/test_blit_neon` 用 `Blit_SetNeon(1/0)` 对三条内核 × 14 种宽度（含 1..17、31..33、100）× 多档 level/颜色/alpha 逐字节比对，并对 AlphaCopy/Blend 用测试内独立标量定义复核 → **0 diff**；`BGI_NO_NEON=1` 可运行时退回标量（A/B 用）
- 真实窗口（cocoa/Metal，非 dummy）实测：正文场景 **4.5% 单核**（BGM + 动画 + 60fps 上屏）；headless dummy 的 20% 是 SDL 软件缩放/模拟 vsync 的开销，不代表真机
- 剩余热点：`memmove`（引擎合成 + 纹理上传）、`Blit_AlphaCopy`（已 NEON 化）、`Blit_Blend`；SDL 软件缩放只在 headless/dummy 出现
- 注意：真实窗口验证会真的弹窗（反复跑会看起来像闪窗口）；自动化验证一律 `SDL_VIDEODRIVER=dummy`

### F. opcode 缺口现状（「补全」的诚实口径）
- **1.669 体验版的全部 111 个程序用到的 opcode 都有 handler**：`bgidump` 全量反编译，0 个 "undefined/not implemented"（真机跑到正文也验证了执行链）
- 本轮补掉的实际占位：`90 5D`（3D）、`B0 8C`（列表选择对话框，新增 `OS_DialogSelect` 契约 + 4 个后端接线）、`92 F0`（档案内影片）、`91 F0..F7`/`92 F2..F5`（视频纹理）
- 仍缺（`--list-missing` 可查，且**已知数据里没有调用者**）：
  - `81 B0`/`81 B7`：字符串距离（Magical Charming! 的 `recoveryrip._bp` 用；无该作数据、无签名证据，硬猜会破坏栈约定，故保持「not implemented」明确报错）
  - `81 6C`：渲染输出尺寸，只存值；高分辨率档（4K/3200×2400）目前按显示模式尺寸渲染（画质保持 1080p 级，显示正确）
  - `D0`(evaluation)、`E0`、`7F` 家族与部分 `81/90/91/92` 子项：profile 定义了但未实现，**未在任何已知游戏数据中被调用**
  - Windows 专属能力（Direct3D 像素着色器、WinInet、触摸等）按「不可用但可回答」处理
- 判据：`bin/bgi --list-missing --engine=<profile>`；未实现 opcode 会以 `not implemented` 明确报错并指出 opcode，而不是静默错行

## 验证（可复现）

### 1) 一条命令验收
```bash
cd ~/Documents/gal-engines/openbgi
./verify.sh --deep
```
输出 PASS 行：build / **21 个单测** / 真实游戏 30s 存活 / engine profile 1.669 / stage dump 非空白 / 自动点击注入生效 / **深度点击后离开标题、画出场景**（干净树实测 0 失败）。
全部步骤用 `SDL_VIDEODRIVER=dummy`，不会弹窗。

### 2) 真实游戏证据
```bash
cd ../testdata/bgi-trial
SDL_VIDEODRIVER=dummy BGI_CONFIG_DIR=/tmp/bgiconf \
BGI_DUMP_STAGE=/tmp/stage.bmp \
BGI_AUTO_CLICK="500,1027,16000;960,540,23000;960,540,27000;960,540,31000;960,540,35000" \
../openbgi-src/bin/bgi
```
- 标题画面：`screenshots/haruka-title.png`
- 版权声明：`screenshots/haruka-ingame-copyright.png`
- **正文剧情**（对话框 + 底栏 AUTO/SKIP/LOG/SAVE/LOAD/QSAVE/QLOAD/SYSTEM）：`screenshots/haruka-ingame-story.png`

### 3) 影片（真实 584MB 条目）
```bash
# [BGI_DEBUG_MOVIE=1] 实际日志
[movie] archive C:\data02xxx.arc:movie_op.mpg unpacked ... (584458244 bytes in 675 ms)
[moviefx] start ... 1920x1080 fps 30.000 length 123360 ms
[moviefx] frame 1 / 60 / 120 shown ...
[probe] played 6004 ms, playing=1, frames shown=178, still running=1
# 二次运行走缓存：extract+start 127 ms
```
长度 123360 ms == ffprobe `duration 123.36s`；r_frame_rate=60 但 avg_frame_rate=30（按 avg 播放，不再白解一倍帧）。

### 4) CBG alpha
```bash
bin/test_cbg2_alpha   # mode2 编码器往返（灰阶/全透明/全不透明/棋盘存在位）+ 7 种畸形 + 64 字节 guard
bin/test_codec        # v1 回归
```

### 5) 3D 与性能
```bash
bin/test_sprite_3d    # 60 组 rot=0 与 5C 逐像素 0 diff；透视尺寸公式 ≤2px；退化安全
SPRITE3D_BENCH=1 bin/test_sprite_3d   # blend 3.33× / tint 3.62×
bin/test_blit_neon    # NEON 三条内核 vs 标量 + 独立参考：14 宽度 × 多参数 逐字节 0 diff
```

## 诊断开关总表（全部零开销，默认关）

| 开关 | 作用 |
|---|---|
| `BGI_DEBUG_BOOT=1` | 启动链（Engine_Init / VmMain 进出） |
| `BGI_DEBUG_PROGRESS=1` | 每秒打印线程/IP/操作码/帧数（=帧率探针） |
| `BGI_TRACE=1` / `BGI_TRACE_LAST=N` | 指令级追踪 / 环形缓冲 |
| `BGI_LENIENT_OPCODES=1` | 未实现 opcode 跳过而非中止（诊断，非正确模拟） |
| `BGI_DEBUG_CPUID=1` | arm64 上仿真 CPUID 的每个 leaf |
| `BGI_SCREENSHOT=<path>` / `BGI_SCREENSHOT_MS` | 首帧/定时截图（BMP） |
| `BGI_DUMP_STAGE=<path>` | 每秒把引擎绘制表面直接 dump（绕开 present，无头可用） |
| `BGI_DUMP_RENDERED=<path>` / `BGI_DUMP_RENDERED_MS` | 读 SDL 渲染器输出（验证窗口真的显示） |
| `BGI_ALWAYS_PRESENT=1` | 每 pass 强制全屏合成+present（性能满载） |
| `BGI_AUTO_KEY_MS=<ms>` | 启动后按一次 Enter（老诊断） |
| **`BGI_AUTO_CLICK="x,y,ms;..."`** | 画面坐标自动点击（本轮新增，引擎侧、虚拟指针，headless 可命中） |
| **`BGI_AUTO_KEY="vk,ms;..."`** | 自动按键（本轮新增） |
| **`BGI_AUTO_DIALOG=1`** / **`BGI_AUTO_FILE=<path>`** | 对话框无人值守：自动确认/返回路径（本轮新增，darwin） |
| **`BGI_CONFIG_DIR=<dir>`** | 覆盖配置目录（本轮新增；同时让两个实例不抢单实例锁） |
| `BGI_MSGBOX_STDERR=1` | 消息框打到 stderr + 返回默认按钮（测试必需） |
| `BGI_DEBUG_MOVIE=1` | 影片解包/ffmpeg/帧数日志（本轮新增） |
| **`BGI_FAST_PRESENT=0`** | 关闭整帧直通上屏（A/B 用；默认开） |
| **`BGI_NO_NEON=1`** | 关闭 32 位 blit 的 NEON 路径，退回标量（A/B/定位用；默认关） |
| `BGI_PANEL_DEBUG=1` | 面板事件 + SpritePanel 按钮真实矩形（定位点击坐标用） |
| `BGI_DEBUG_THREADS=1` / `BGI_DEBUG_OS=1` / `BGI_SND_DEBUG=1` / `BGI_INPUT_DEBUG=1` | 线程/OS/音频/输入 |

## 已知缺口（v2 darwin 后端，按影响排序）
1. **启动器/安装器对话框**：`OS_LauncherDialog`、`OS_InstallerDialog` 仍 stub → 无 `games.json` 的「启动器模式」与安装程序 UI 不可用（直接进游戏目录运行不受影响）
2. **CD-DA / MCI**：失败型 → 老游戏 CD 音轨无音乐（BW 容器内的 voice/BGM 不受影响；本作无 CD 音轨）
3. **`81 6C` 高分辨率渲染**：只存值；4K 档按显示模式尺寸渲染（显示正确，画质为 1080p 级）
4. **`81 B0/B7` 与 D0/E0/7F 等未实现 opcode**：见「opcode 缺口现状」，无已知数据调用者
5. **影片**：依赖外部 ffmpeg/ffprobe（macOS 无原生播放器）；92 F0 首次要整条拷贝（584MB/675ms）；缓存无容量上限/清理策略；video texture 只支持 BF_Movie type 0
6. **CBG v2 mode 2**：无公开真实样本，往返测试用的是按反汇编格式自写的编码器；未置位 block 的默认值（0xff）是静止图语义下的选择，拿到真样本可一行切换
7. **3D**：近平面不裁剪（越界按空投影返回 8）；rotX/rotY 没有 tween 增量 opcode；深度沿用 90 5C 的中心 fz 排序；3D+mask 路径未单独测试
8. **音频**：SDL 拉取式回调工作正常，但未做延迟/设备热插拔实测
9. **调试器窗口**：能开能用，但没有做与引擎同帧的逐帧调试体验验证

## 本轮踩坑记录（值得记住）
- **单实例锁是引擎级的**（`~/Library/Application Support/OpenBGI/Buriko_*.lock`），多实例/并行测试会静默 `exit 0` 且无输出 → 用 `BGI_CONFIG_DIR` 隔离
- `pkill`（SIGTERM）杀不掉 BGI（它有信号处理）→ 测试前一律 `pkill -9`
- macOS arm64 没有 CPUID，而 BGI 启动必须查 CPU → `src/os/darwin/machine.c` 仿真一台基线 x86-64（leaf 0 厂商 `GenuineIntel` 的 EBX/EDX/ECX 顺序踩过坑）
- 游戏表里的**产品名** `HarukaAonoHanayomeniTV` 必须写全（引擎用 product 优先于 match），否则 `ipl._bp` 主动 `sys.quit()`
- **present 每帧多一次全帧拷贝**：整帧 blit 先 memcpy 进 staging、再 SDL_UpdateTexture 拷进纹理；改成整帧直接上传（诊断开关存在时自动退回）后满载 CPU −27.6%
- 呈现只走一次的问题：Windows 上「拿 DC/释放 DC」成对，而 `Present_Blit` 路径只有一侧 → 在 `OS_BlitToWindow`/`OS_StretchToWindow` 末尾直接 `Dwin_Present()`，「一次 blit 进窗口就是一帧」
- SDL 2.32 移除了 `SDL_BUTTON_WHEELUP/WHEELDOWN`（滚轮走 `SDL_MOUSEWHEEL` 事件），homebrew 新版会直接编不过
- 对话框在**没有主窗口**时必须退化成「取消」，否则单测（`81 38`）永久 hang —— 本轮真实回归
- 高分屏下鼠标映射要双向：`GetMouseClientPos` 的逆是「buffer→screen + client origin」，截图/自动点击都用这条链

## 下一步
1. 若拿到其它 BGI 作品（尤其 1.573 Magical Charming! / 1.636 千の刃濤 体验版）：跑 `--list-missing` + 真机，补 `81 B0/B7` 等真正被调用的 opcode
2. darwin present 路径少一次全帧拷贝（热点 `memmove` 44%），预计再省 10-20% CPU/帧
3. `81 6C` 若确有必要，做 2× 超采样渲染（text/2D 高分辨率重绘），需评估复杂度
4. 影片缓存加容量上限 + 启动清理；考虑 ffmpeg 硬解（VideoToolbox）
