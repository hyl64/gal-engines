# Engine ① NScripter — OnscripterYuri @ macOS arm64

## 状态：✅ 构建成功 + 冒烟通过（2026-10-24，lead 直接完成）

## 来源
- 仓库：https://github.com/YuriSizuku/OnscripterYuri （master 分支 tarball，2026-10-24 拉取）
- 版本横幅：ONScripter Yuri 0.7.7 (Jh0.8.0, Ons20181218, NSC2.96)
- 官方支持矩阵里本就有 `mac x64, arm64 (local compile)`，本次为 M1 上的首次本机构建

## 构建
```bash
brew install cmake pkg-config lua sdl2 sdl2_mixer sdl2_image sdl2_ttf libvorbis freetype
cmake -B build -DCMAKE_BUILD_TYPE=Release   # CMake 自动走 config_darwin()
cmake --build build -j2
# 产物：build/onsyuri  (Mach-O 64-bit executable arm64)
```
- 唯一的坑：需要 `lua`（brew lua 5.5.1 直接兼容，无需旧版本）
- 网络注意：GitHub 直连超时，用本机 Clash（127.0.0.1:7890）+ codeload tarball

## 冒烟测试
- 剧本：test/00.txt（*define/game/*start/文本/end 最小闭环）
- 运行：`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/onsyuri --root test --font <ttf> --window`
- 结果：脚本解析→VM 执行→正常退出，exit 0，无 parse error
- 无害警告：缺可选资源（doffcur.bmp / cursor0/1.bmp / system.lua），引擎优雅跳过

## 常用参数（来自 README）
- `--root <游戏目录>` `--save-dir` `--font x.ttf`
- 编码：`--enc:sjis`（原生日文）/ `--enc:gbk` / `--enc:utf8`
- `--fullscreen2`（F10 切换拉伸全屏）`--width/--height` `--sharpness`

## macOS 适配补丁（2026-10-25）

### 字体回退（重要）
引擎硬性要求游戏目录里的 `default.ttf`，缺了就 `can't open font file` 并停止——
这对「字体打包在归档里」或「本来就没带字体」的游戏是致命的。已加 macOS 回退：

- 顺序：`ONS_FONT` 环境变量 → `Hiragino Sans GB.ttc` → `Supplemental/Arial Unicode.ttf`
  → `AppleSDGothicNeo.ttc` → `Hiragino Sans W3.ttc` → `/Library/Fonts/Arial Unicode.ttf`
- 命中时打印 `using system font: <路径>`，之后所有字体打开都用这个路径
- 位置：`src/onsyuri/ONScripter.cpp`（`FindSystemFont()` + initialize 里解析）

## 真实数据验证记录

| 数据集 | 来源 | 结果 |
|---|---|---|
| `testdata/lifegame` | OnscripterYuri 官方在线 demo（`onscript.nt2`，加密脚本） | ✅ 解析执行，进主循环 |
| NScripter 最终版 SDK `サンプル1/2-1/2-2/3` | 官方 nscr_2018.zip（[nscripter.com](https://www.nscripter.com/)） | ✅ 四个全部跑通，用系统字体，零脚本错误 |
| NScripter2 SDK 三套示例 | 官方 nscr2.zip | ⚠️ NScripter**2** 方言（Lua 驱动），ONScripter 不兼容——已记录 |

官方 SDK 的获取与解包（文件名是 CP932，需按 SJIS 解码）：
```bash
curl -O https://www.nscripter.com/nscr_2018.zip
python3 -c "
import zipfile,os
z=zipfile.ZipFile('nscr_2018.zip', metadata_encoding='cp932')
for i in z.infolist():
    if i.filename.endswith('/'): continue
    d=os.path.join('n18',i.filename); os.makedirs(os.path.dirname(d),exist_ok=True)
    open(d,'wb').write(z.read(i))"
```

## 待办
- [ ] 真实游戏数据验证（月姬/寒蝉原作等 NScripter 系游戏数据目录）——等用户提供
- [ ] 有真实数据后测：语音 OGG、视频 MPC、存档读写、全屏切换
- [ ] 打包成可双击的 .app（含 Info.plist + 图标）

## 影片后端（2026-10-25，task T1，lead 收尾修完）

### 结论：`mpegplay` / `avi` / `movie` 在 macOS 上不再是 disabled

之前 `playMPEG()` 的 SMPEG 主体和 `playAVI()` 的 WIN32/iOS/Android 分支之外
都落进 `#else`，Mac 上分别打印 `mpegplay command is disabled.` /
`avi command is disabled.`。根因是编译期：`config_darwin()` 不定义
`USE_SMPEG` / `USE_AVIFILE`，而 SMPEG 已被 Homebrew 判 deprecated、avifile
2005 年即停维护且只支持 x86-Linux。所以不是"没测"，是结构性缺失。

现在换成自研后端，路线（DESIGN-VIDEO.md 契约）：

```
playMPEG/playAVI (Mac 分支)
  → videoMapLookup(archive_path, path)      // _video/video_map.json 侧车
  → videoPlayBlocking()
      → avfPlayFrames()   AVFoundation + VideoToolbox 硬解   ← 主路
      → mpvPlayBlocking() libmpv，dlopen 加载，无编译期依赖  ← 兜底
  → 播放成功返回 1 → 调用方 endCommand()（NScripter 原作语义）
```

### 为什么要拆成三个 TU

AVFoundation 的 umbrella header 会经 ApplicationServices 拉进
`Quickdraw.h`，后者**无守卫地**定义 `struct FontInfo`；引擎的
`FontInfo.h` 定义 `class FontInfo`。两者同 TU 必然重定义报错，而把
Quickdraw 整体屏蔽会打断 HIServices（`RgnHandle`/`RGBColor` 缺失）。

所以按"能不能看见引擎头"切开：

| 文件 | 角色 | 依赖 |
|---|---|---|
| `video_player_mac.mm` | AVFoundation 解码，逐帧回调 | 只碰框架，**不 include ONScripter.h** |
| `video_player_bridge.cpp` | 帧写入 accumulation_surface、上纹理、present、SDL 事件泵 | 引擎 TU，**不 import 框架** |
| `video_player_mpv.cpp` | libmpv 兜底，dlopen | 只碰 mpv client API |
| `video_player_frame.h` | 两侧唯一接缝（`video_host` 回调结构） | 纯 C |

`ONScripter.h` 的 `private→public` 重标定现在只在 `video_player_bridge.cpp`
一处出现（此前在 .mm 里）；C++ 名字修饰不编码访问级别，链接不受影响。

### 另一个坑：CMake 的 enable_language 必须在文件作用域

`enable_language(OBJCXX)` 若写在 `config_darwin()` 函数体内，CMake 4.4.4
会报：

```
Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT
Missing variable is: CMAKE_OBJCXX_LINK_EXECUTABLE
```

配置阶段能过（"The OBJCXX compiler identification is AppleClang" 也认了），
但 generate 阶段失败。已移到文件顶部并在 Darwin 条件下启用。

### 验收（全部实测，复现命令在 `test/video/run_acceptance.sh`）

夹具：`bench/fixtures/`（mpeg1 640x480@25、wmv2 avi、mpeg4 avi、
h264 720x576 SAR16:15、故意损坏的 mpg）。

| 项 | 结果 |
|---|---|
| `mpegplay` MPEG-1 | ✅ AVFoundation，3.00s @25fps，dump 出 620 色的真实帧 |
| `avi` wmv2 | ✅ AVFoundation 拒绝 → mpv 兜底成功，275 色的彩条帧 |
| `mpegplay` MP4 直路径（无 map） | ✅ AVFoundation |
| `video_map.json` 命中 | ✅ 改播 `_video/*.mp4`，AVFoundation 硬解，120 色真实帧 |
| map 缺条目 | ✅ 回退原文件 → mpv 兜底 |
| `click_stop=1` + 600ms 注入点击 | ✅ 1.75s 提前结束（片长 3.00s） |
| `click_stop=0` + 同样点击 | ✅ 播满 3.66s，点击被吞掉 |
| 无视频的普通游戏（lifegame）回归 | ✅ CPU 1.0%，RSS 32.9MB，无新报错 |

### 已知限制

- **音频**：`AVAssetReaderAudioMixOutput` 整轨预解码 f32/44.1k/立体声 →
  独立 `SDL_OpenAudioDevice` 流（不动引擎 Mix_OpenAudio 状态）。但实测本机
  wmv2/mpeg1 夹具走的是 `audio=silent`——音频轨提取在部分容器上失败后按契约
  静默降级，不阻塞画面。这是"best effort"，不是 bug。
- **mpv 兜底是进程级回调**（`mpvSetFrameCallback`），因此要求播放串行——
  引擎本来就是单线程脚本 VM，成立。
- **loop + click_stop=0** 按契约永不终止，实现降级为单次播放（与 T4 一致）。
- 软解路径允许 1080p 掉帧（契约允许）；主路是 VideoToolbox 硬解。

## NEON（USE_SIMD_ARM_NEON）：实测后决定不启用（2026-10-25）

`config_darwin()` 一直没有定义 `USE_SIMD` / `USE_SIMD_ARM_NEON`，而
`config_windows_msvc()` 在 ARM64 分支上是定义了的，所以这看起来像漏配。
查过代码、也真的编出来跑过之后，结论是**保持关闭**：

**做了什么**

```bash
cmake -B build-neon -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-DUSE_SIMD -DUSE_SIMD_ARM_NEON"
cmake --build build-neon -j4      # 通过，二进制 692 KB
```

`simd/*.h` 里 36 处 NEON intrinsic（`vmovl_u8` / `vget_low_u16` /
`vld1_*` / `vqadd_u8` …）都在，看起来是完整的。

**为什么还是不开**

1. `simd/vec128.h:50-56`（`ivec128` 的 NEON 分支）是**空占位**：

   ```cpp
   #elif USE_SIMD_ARM_NEON
       static ivec128 zero() { return ivec128(); }
       operator uint8x4()  const { return uint8x4(); }   // 默认构造，未初始化
       operator uint8x8()  const { return uint8x8(); }
       ...
   ```

   `zero` 目前只被喂给 `widen(a, b)`，而 NEON 版 `widen` 用 `vmovl_u8(a)`
   忽略 `b`，所以**眼下**不炸；但那几个转换运算符一旦被调用就会交出未初始化
   向量。这是"上游没写完"，不是我们能安全踩上去的地面。
2. **没法验证像素正确性**。SIMD 影响的是 alpha blend
   （`AnimationInfo::blendPixel32` / `ONScripter_image.cpp:alphaBlendPixelCore32`），
   只有带透明立绘/特效的场景才会走到。想比对两边输出，得有个可靠的帧导出：

   - `ONS_VIDEO_DUMP` 只在影片播放结束时 dump，非影片场景用不上；
   - `savescreenshot 640,480,"shot.bmp"` 在这个构建里静默不产出文件
     （日志无报错，两个 build 都一样），所以连"有 bug 的基准"都拿不到；
   - `trans` 命令的 `mode,time,rule,no,param…` 语法在最小剧本里也没被接受。

   换句话说：**能编、能跑、不崩，但证不了画面对**。
3. 收益不明。galgame 的热点是解码、绘制与文字排版，blend 只在立绘淡入和
   特效上占一小块；M1 的标量路径本来就不慢。没有可测的收益证据，
   犯不上为它引入一个未完成的代码路径。

**如果要重新评估**，补两件事就够了：给非影片场景加一个可靠的
surface dump（例如 `ONS_SURFACE_DUMP=<file>` 在 repaint 后写盘），
然后找一部带透明立绘的游戏，两边 dump 同一帧做逐像素比对。

## 影片打断剧情：返回值语义写反了（2026-10-25 修，影响所有带插播影片的游戏）

**症状**：任何 `mpegplay` / `avi` / `movie` 播放完之后，脚本就终止——影片后面的
台词、选项、后续剧情全部丢失。对"OP 播完进标题"或"过场影片后继续对话"的游戏
是致命的：影片一放，游戏就结束。

**根因**：调用方写的是

```cpp
if (playMPEG( save_buf, click_flag )) endCommand();
```

这是一个**"失败才结束"**的写法。上游 `playMPEG` 以 `int ret = 0` 起步，在所有
正常路径（包括成功播放）返回 0，所以这个 `if` 几乎永远不成立，影片之后脚本照常
继续。

第一版 macOS 后端把语义搞反了：

```cpp
if ( videoPlayBlocking( to_play, click_flag, loop_flag ) ) ret = 1;  // ← 播成功反而置 1
```

播放成功 → `ret = 1` → 回到调用方 → `endCommand()` → 脚本终止。

**修复**：成功**不**改 `ret`；播不出来时打一条日志然后跳过（`ret` 仍是 0），
与 ONScripter 整体"宽容"的哲学一致——缺个片不该让游戏退出。

```cpp
if ( !videoPlayBlocking( to_play, click_flag, loop_flag ) )
    utils::printError( "mpegplay: could not play %s, skipped.\n", to_play );
// ret 保持 0 —— 影片后脚本继续（这是原作行为，不是容错）
```

`mpegplay` / `avi` / `movie` 三个命令统一处理。

**验收**（已固化进 `test/video/run_acceptance.sh` 第 4b 项，9/9 全绿）：

```
*start
bg #00FF00,10
waittimer 400
mpegplay "clip.mp4",0
bg #CC0000,10          ← 修复前这里永远不执行
waittimer 2000
end
```

判据是**颜色**而不是"进程没退出"：影片后画面必须是红色（#CC0000），
绿色（#00FF00）就说明脚本停在影片处。

端到端的对话场景也验证过：影片前台词 → 影片 → 影片后台词，两段日文都渲染出来
（dump 帧 1151 色、1533 个近白采样）。用的就是新增的 `ONS_SURFACE_DUMP`，
因为 `savescreenshot` 在这个构建里静默不产出文件。

**教训**：契约文档（DESIGN-VIDEO.md）当时把这条写成了"返回非 0 → endCommand
（影片后结束脚本，NScripter 原作行为）"——**契约本身写错了**，实现忠实执行了
错误的契约。已同步改正，并在文档里标注这是修正过的。

## 第二轮硬化：音频、movie stop/pos/async、边界、性能（2026-10-25）

### 音频：现在能证明"真的有声音"

之前日志里的 `audio=silent` 是**测试夹具没有音轨**，不是 bug。补了带音轨的夹具
后确认 `h264/aac` 与 `mpeg1/mp2` 都能解出 PCM。为了不让"有音轨"冒充"有声音"，
解码后打印能量：

```
video: audio decoded 2.00s, 176400 samples, peak=0.129 rms=0.0883
video: audio decoded 3.01s, 265442 samples, peak=0.126 rms=0.0881
```

静音轨会被标成 `(SILENT TRACK)`。整轨预解码耗时也单独计时，30 秒 720p 素材
只要 201ms，可以接受。

### movie stop / pos / async

三个命令原来都是 `not supported yet`，现在：

- **`movie ...,async`**：后台线程解码、主线程渲染（SDL 的 renderer 不能跨线程）。
  线程只往"最新帧槽"里写，`runEventLoop` 改成 `SDL_WaitEventTimeout(16ms)`，
  超时就取一帧呈现。脚本继续跑，实测 async 影片与 `bg` 命令交织正常。
- **`movie stop`**：停线程并 join，脚本继续。
- **`movie ...,pos,x,y,w,h`**：只把帧画进目标矩形，区域外保留引擎原有内容。

**踩到的坑**：第一版 async 在脚本 `end` 时触发 `Abort trap: 6`——可 join 的
`std::thread` 在退出时被析构会 `std::terminate`。用 `atexit` 显式 join 解决。

**第二个坑**（自己造的）：清理调试代码的正则把 `if (tw==w && th==h)` 和它的
memcpy 分支一起删了，于是 `else` 挂到了 pos 判断上——pos 生效时反而**不写像素**。
定位方法是把像素回读做成正式诊断（`ONS_VIDEO_PIXEL_LOG=1`）：

```
video: pos rect=0,0 200x150 inside=#fcf300 outside=#ff00ff
```

顺带发现 `SDL_SaveBMP` 对这个 surface 不可靠（dump 出来的永远是引擎重绘后的
背景），所以 pos 的验收用像素日志而不是 dump 文件。

### 边界：坏输入不崩，脚本继续

| 输入 | 结果 |
|---|---|
| 损坏的 mpg | rc=0，无崩溃，跳过并继续 |
| 0 字节文件 | 同上 |
| 200KB 随机字节 | 同上 |
| 不存在的文件 | 同上 |
| `video_map.json` 指向不存在的产物 | 回退原文件，跳过并继续 |

### 性能（实测数字）

| 素材 | 帧 | 结果 |
|---|---|---|
| 1080p60 3s | 180/180 | **0 dropped**，4.4s wall（含启动+音轨预解码） |
| 720p30 30s | 900/900 | **0 dropped**，30.4s（实时） |
| 静态页（lifegame） | — | CPU **1.09%**，RSS **18MB** |
| gal prep 转码 1080p 3s | — | **1.16s**（约 2.6× 实时） |

**试过又回退的优化**：给影片加"直接渲染到 renderer、跳过 accumulation_surface"
的快速路径。实测**没有帧率收益**（瓶颈是解码不是缩放，两条路都是 180/180），
却破坏了所有读 surface 的功能（`ONS_SURFACE_DUMP`、`savescreenshot`、存档缩略图
都拿到旧帧）。已回退并在代码里留注释说明。

### 验收：15 项，一条命令

```
$ ./test/video/run_acceptance.sh
passed 15, failed 0
```
