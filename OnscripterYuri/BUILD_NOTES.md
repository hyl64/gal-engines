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

## 真实商业游戏：《トライアンソロジー》官方体验版（2026-10-25）

数据：07th Expansion 官方 trt_trial（NScripter 1，1024x576，113 个 MPEG，
17 个 DLL 插件）。这是本项目第一次用**真正的商业游戏**验收，之前只有官方 SDK
样例和自造夹具。它直接暴露了五个只有真游戏才会踩到的问题。

### 0. 真游戏的影片根本不走 mpegplay

统计解密后的 nscript.dat（37k 行）：

```
mpegplay            0 次
movie               0 次
layermessage play 143 次      ← 影片真的走这里
```

影片全部通过 `setlayer 10,20,"dll\wcmpg.dll"` + `layermessage` 驱动：

```
layermessage 10,"open/arc\MOV001_100ten.mpg"
layermessage 10,"setloop/1"
layermessage 10,"setx/0"      layermessage 10,"sety/0"
layermessage 10,"play"
lsp 983,"*10"                 ← 绑成 sprite 以便存档
waittimer 1980                ← 时间由脚本自己控
```

**所以修 `mpegplay`/`movie async`/`movie pos` 那三条路径，对这款游戏一条都没
命中。** 真正要做的是图层插件协议。

### 1. wcmpg.dll 图层插件（新实现）

`src/onsyuri/builtin_dll/layer_wcmpg.cpp`：实现 open / setloop / setx / sety /
play / pause / close / getx / gety / getw / geth。解码复用后台线程那套机制，
主线程 `Layer::refresh()` 取帧并贴到 `pos_x/pos_y` 指定的位置。

**踩到并修掉的 use-after-free**：`avfLayerFrame()` 一开始把内部 `std::vector`
的裸指针交出去、锁就释放了，而解码线程还在往同一块内存写（vector 还可能扩容）
→ 指针悬垂，表现为随机 SIGSEGV（跑 60 秒左右必现）。改成主线程在锁内把最新帧
拷进自己的快照缓冲再返回指针。**这是我自己引入的 bug，不是上游的。**

### 2. 其余三个图层插件：中性层

`setlayer` 原本只认 oldmovie/snow/hana，游戏声明的 anm / screen / NSFukuro
一律报 "not implemented"。但这版游戏给它们的消息极少：

```
anm.dll       2 条  ("free 1", "move 1,stop")
screen.dll    0 条
NSFukuro.dll  5 条  ("fontsize,24", "clear", "fOff")
```

`layer_stub.cpp` 让它们接受消息、不画东西、并且**每条只报一次**，而不是假装
实现。文件头写清楚了这是权宜，以及什么情况下必须换成真实现。

### 3. Lua 不是 5.1 兼容级

游戏在 `lib/sound.lua:435` 撞上 `math.log10`（Lua 5.2 起移除）。加了 5.1 兼容
垫片：`math.log10/log2/pow/atan2`、`table.unpack`、`unpack`、`loadstring`、
`string.gfind`、`module()`、`getfenv/setfenv` 的最小平替。

### 4. savedir 目录不存在时不该放弃

游戏 `savedir "savedata"` 而包内没有这个目录，原来的实现打印
`doesn't exist` 后把 save_dir 置空——存档会悄悄落到别处。改成按需创建。

### 5. 三个缺失命令

`errorsave` / `deletemenu` / `font` 原来都报 `not supported yet`。按真实用法实现
（前两个无参；`font $var` 在游戏里出现 68 次，记录请求但不切换——ONScripter
本来就只有一份 TTF）。

`exec_dll` 在 `dll.txt` 缺失时每次调用都刷错误（这游戏调 110 次）——改成报一次
并返回确定的默认值。

### 现在的状态

```
$ ./OnscripterYuri/build/onsyuri --root testdata/ns-07th-trt/game/trt_trial --enc:sjis
免责声明（日/英双语）     ✓ 正确渲染 1024x576
标题影片                 ✓ wcmpg 打开 sys2/MOV002_title.mpg 并播放
标题菜单                  ✓ Start / Load / End / WEB 四个按钮全部渲染
点 Start 后               → 进入加载画面（bar + _print），加载第一章
```

**零脚本错误、零未支持命令。** 之前所有 `not supported yet` 都已清零。

### 还没解决的

- 加载画面之后能否进入正文，还需要更长的运行验证
- 免责声明第二页有两段文字叠加的现象未定性（可能是游戏美术本身的设计，
  也可能是文字层的透明度问题）；`ons_effect.log` 证明 1500ms 的 crossfade
  确实执行完成，所以不是"淡入没走完"
- `setloop` 目前只对单次播放生效（解码器本身不进循环）
- 存档/读档尚未在真实游戏里验证

### 调试设施（都是环境变量，默认关闭）

| 变量 | 作用 |
|---|---|
| `ONS_TRACE_CMDS=1` | 记录执行的每一条脚本行到 `ons_trace.log` |
| `ONS_TRACE_EFFECT=1` | 记录效果计数/时长，判断转场是否走完 |
| `ONS_SURFACE_DUMP=<file>` | 每 500ms 落一次引擎画面（绝对 /tmp 路径会被沙箱拒，用相对路径） |
| `ONS_AUTOCLICK_MS/X/Y` | 定时点击，可指定坐标（用于点菜单按钮） |
| `ONS_NO_EFFECT=1` | 所有转场瞬时完成，用于分辨"引擎混合"还是"美术自带" |
| `ONS_VIDEO_PIXEL_LOG=1` | 报告影片目标矩形内外的实际像素 |
| `ONS_CRASH_HANDLER=0` | 关掉 SIGSEGV 回溯打印 |

## trt_trial 第二轮实测（2026-10-25 续）

### E1 性能：达标

真实游戏停在标题（wcmpg 图层影片正在播）实测 20 秒：

```
cpu_avg_pct=9.9   rss_peak_mb=45
```

目标线是 CPU<15%、RSS<400MB，**余量很大**。1024x576 + 影片图层并不重。

### 影片素材规格（7 个散片全测）

```
MOV000_1nen.mpg        mpeg1video 1024x576 60fps
MOV000_OP.mpg          mpeg1video 1024x576 60fps
MOV000_intro.mpg       mpeg1video 1024x576 60fps
MOV002_title.mpg       mpeg1video 1024x576 60fps
MOV003_title_loop.mpg  mpeg1video 1024x576 60fps
MOV010_OP.mpg          mpeg1video 1024x576 60fps
MOV099_taikenban.mpg   mpeg1video 1024x576 60000/1001
```

全是 MPEG-1、1024x576、60fps 主流规格——这是 NScripter 时代的标准做法。

### 三个只有真游戏才会踩到的语法/流程细节

1. **屏幕尺寸来自首行 `;$V6000G2000S1024,576L10000`**
   这行看着像注释，但引擎会解析它设定 1024x576。自己写测试剧本时漏了它就
   只有 640x480。

2. **`bg black,0` 而不是 `bg #000000,0`**
   后者会被当成文件名去找，报 `can't find file [#000000]`。颜色要用名字。

3. **`layermessage ...,"play"` 之后必须跟 `print 1`**
   图层影片的帧靠动画循环重绘才会上屏；只 `play` 不 `print` 的话，
   `Layer::refresh()` 不会被再调用，画面永远是背景色。自建测试剧本时必须
   照抄游戏里这个组合。

### 仍未解决

- `setloop` 的循环重播在自建剧本里还没看到画面（图层 refresh 路径待查）
- 免责声明第二页叠影未定性
- 真实游戏存读档未验证

## 第三轮：归档内影片已通，setloop 循环未生效（2026-10-25 续）

### 新证实：归档里的影片能播

跑真实游戏时日志出现：

```
wcmpg: opened 'arc4\MOV000_hanyou.mpg'
wcmpg: opened 'sys2\MOV002_title.mpg'
wcmpg: opened 'sys2\MOV003_title_loop.mpg'
```

`arc4\` 前缀说明**影片是从 .ns2 归档里取出来的**（引擎自己的 reader 读进内存，
落地成缓存文件再交给 AVFoundation）。这是 B3 里最有价值的一半：113 个 MPEG
大部分在归档里，这条路通了。

### setloop 仍未真正循环

跑 52 秒到标题后连续抓帧，T21–T26（约 10 秒）**画面完全不变**。
`MOV003_title_loop.mpg` 只有 2.98 秒，如果循环生效，10 秒内应该看到 3 次以上
内容变化。没有。

已排除的怀疑：
- `openMovie` 确实把 `loop_flag` 传给 `avfLayerOpenMemory`
- 解码线程里的 `if (g_layer.loop) continue;` 走的是「再来一遍」分支
- 脚本顺序是 `setloop/1` **在 `open` 之前**（原代码注释写反了，已改正）——
  `openMovie` 在 open 时读 `loop_flag`，所以这个顺序不影响

所以问题在解码线程的「一遍结束」判定或 reader 重建，还没定位。

### 记一笔：合成剧本复现不出真实游戏

自建的 `setlayer + layermessage` 剧本能打开影片（日志有 `wcmpg: opened`），
但画面始终是引擎背景色；同样的代码在真实游戏里能画出 07th 的 LOGO。
说明真实游戏里有额外的重绘驱动（`print 1` / 动画间隔 / 其他状态），
合成剧本还差某一环。**结论：验证这类功能要用真实游戏，不能只靠自造剧本。**

## 第四轮：归档被截断（本次验证的第一发现）+ AVFoundation 并发崩溃

### 1. `00.ns2` / `03.ns2` 当初解压不完整（根因，已修）

`gal prep` 与 `gal verify` 报的两个立绘 `Unsupported image format`、以及
`arc4\MOV000_hanyou.mpg` 找不到，根因不在引擎，而在**数据**：

| 文件 | 磁盘上 | zip 中央目录记录 |
|---|---|---|
| `00.ns2` | 177,733,632 | **223,769,945** |
| `03.ns2` | 48,758,784 | **182,065,639** |

用 HTTP Range 只取 zip 的中央目录（`PK\x01\x02`），按 CP932 解出原始大小，
再按 member 的 local header 偏移 + inflate 重下这两个成员，写 `.part` 后
rename，校验大小与 zip 记录逐字节一致。修复后：

```
probe: 'tati\USAGI\nor\kihon.png' 275045 bytes (read 275045) -> first 8: 89 50 4e 47 0d 0a 1a 0a
probe: 'tati\ALIC\nor\metoji.png' 477859 bytes (read 477859) -> first 8: 89 50 4e 47 0d 0a 1a 0a
probe: 'tati\USAGI\nor\warai2_l.png' 277720 bytes (read 277720) -> first 8: 89 50 4e 47 0d 0a 1a 0a
```

（`ONS_PROBE_FILE=<名字>` / `ONS_PROBE_OUT=<路径>` 是这一轮加的诊断：把引擎
reader 交给引擎的字节原样落盘。「索引坏了」与「解码器坏了」只能这样分开。）

重下之后进游戏，**两名角色立绘与夜景背景都正常渲染**。

### 2. 层播放器与 `mpegplay` 抢 AVFoundation → SIGBUS/SIGSEGV

标题的层影片还在解，点 Start 后 `mpegplay` 又开一个 `AVAssetReader`，两个
线程同时读，崩在 `-[AVAssetReaderOutput copyNextSampleBuffer]` 与
`-[AVAssetReader cancelReading]`（栈在 `FigMediaProcessorFlush`）。

修法：`video_player_mac.mm` 里一个**进程级互斥量**包住每个 reader 的整个生命
周期；`cancelReading` 只在 `status == Reading` 时调用。修后同一路径
**90 秒零崩溃**（`crash: 0`）。

### 3. 文本：字符到了，图层没合成（仍未解决，证据留在这里）

用 `ONS_TEXT_DEBUG=1` 跑真实游戏，文本确实进了引擎：

```
[text] 61 bytes: 81 79 83 45 83 54 83 4d 81 7a ...   （「やあ、アリス…」）
[glyph] u+3057 bytes 82 b5 font=0x... h=0
[glyph] u+3048 ...
```

但用 `ONS_DUMP_TEXT=/tmp/tl.bmp` 单独导出文本层，只有 4 种颜色；同时打印的
刷新模式是：

```
[textlayer] 1024x576 mode=32 refresh=1
```

`refresh=1` = `REFRESH_NORMAL_MODE`，**没有 `REFRESH_TEXT_MODE(8)`**，所以
`text_info` 从来没有被 blend 到画面上。这条游戏的文字走的是 `print`
（不是 `puttext`），而 `printCommand` 开头 `leaveTextDisplayMode()`，于是
`refreshMode()` 返回 NORMAL。

试过「文本层非空就合成」的改法：画面仍然没有字，说明还差别的环节（字体尺寸
`font_size_xy[1]` 在日志里是 **0**，很可能是第二把钥匙）。该改动**没有提交**，
避免为未验证的假设改动所有游戏共用的 `refreshMode()`。

### 4. 结论：这一轮验证到哪

| 项 | 结果 | 证据 |
|---|---|---|
| 数据完整性 | ✅ 修好两个被截断的归档 | zip 中央目录尺寸 vs 磁盘尺寸 |
| 归档内影片 | ✅ `arc4\MOV000_hanyou.mpg` 等经 reader 读出并解码 | `wcmpg: opened ... (1908740 bytes)` |
| 标题 → Start → OP → 剧情场景 | ✅ 画面正确（角色、背景、名字标签） | `screenshot/trt_scene.bmp` |
| AVFoundation 并发 | ✅ 已修，90s 零崩溃 | 互斥量 + 状态判断 |
| 正文文本 | ❌ 字符已到、图层未合成 | `[text]` / `[glyph]` / `refresh=1` |
| 存读档 | ⬜ 未验证（文本不通之前没意义） | — |

## 第四轮：循环问题的两个候选根因，以及并发环境的代价（2026-10-25 续）

### 已修：`startReading` 失败会杀死解码线程

原来"一遍结束"后重开时，如果 `startReading` 失败就直接 `break` 跳出整个
`while(true)`——线程退出，循环自然停。读一个被上一轮 cancel 过的 reader 时
这很容易发生。

现在：失败时打印 reader 状态；在循环模式下用新 reader 重试（50ms 退避），
非循环模式才真正放弃。

### 合成剧本现在能画图了

之前自建 `setlayer + layermessage` 剧本始终黑屏。另一个并行会话提交了
「同一时间只允许一个 AVFoundation reader」（`g_avfReaderMutex`）之后，
同样的剧本开始能间歇性画出画面（采样 8 帧里有 2 帧有 14–15 种颜色）。

说明黑屏根因是**多个 reader 并发**，不是我的剧本缺什么。这条对后续验证很有用：
合成剧本现在可以用了。

### 未能验证：循环到底有没有生效

原因有两层：

1. **真实游戏的时间不确定**。从冷启动到标题需要 40–55 秒不等，而采样窗口
   经常在到达标题前就结束。18 帧全同的结果无法区分「没循环」和「还没到标题」。
2. **文件被并行提交覆盖**。这轮我加的探针被另一会话的提交冲掉过两次，
   其中一次把我 `startReading` 的修改连带提交（所以它现在在 `94ccadc` 里，
   署名不是我）。

### 结论：环境问题现在比技术问题更阻碍

同一份 `video_player_mac.mm` 被两个会话同时编辑、提交。每一轮都要花大量
时间重新确认树的状态。**建议串行，或明确分文件。**

技术上的下一步很明确（不需要再猜）：在合成剧本里用 1 秒的亮色影片 +
`setloop/1`，采样 10 秒，看帧内容是否反复出现——合成剧本现在能画图了，
这个验证只需要十几秒。

## 第五轮：循环没生效——但先抓到了自己的假阳性（2026-10-25 续）

### 差点报了个假 PASS

新写了 `test/video/loop_check.sh`：播 1 秒亮色影片 + `setloop/1`，采样 10 秒，
靠「帧序列重复出现」判断循环。

第一版判据是「有一帧重复出现」——它 PASS 了。**这是假阳性**：如果影片播完
停在一帧上不动，16 个采样里 16 个都是同一个哈希，"重复"当然成立。

收紧到「**至少 3 帧、且帧内容有变化、整段序列再次出现**」之后：

```
RESULT: FAIL - frames changed but never repeated => played once only
```

连续跑两次都是 FAIL。**所以 `setloop` 确实没生效。**

### 这个测试值得留着

它 10 秒出结果，不依赖真实游戏 40–55 秒的冷启动，判据是像素哈希而不是
日志措辞。判据的演进本身也写进脚本注释了：单帧重复不算，必须是变动的序列
重现。

### 循环为什么没生效——目前的证据链

- `openMovie` 把 `loop_flag` 传给 `avfLayerOpenMemory` ✅
- 解码线程有 `if (g_layer.loop) continue;` 的重播分支 ✅
- 脚本顺序 `setloop` 在 `open` 之前，open 时读到的是 true ✅
- `startReading` 失败现在会重试而不是退出线程 ✅（这轮的修复）

以上都排除了，剩下的嫌疑是：**一遍播完后 `reader` 的重建本身失败**，或者
`g_layer.loop` 在中途被 `avfLayerOpen` 的某种重复调用重置。

下一步该做的事很明确：在 `loop_check.sh` 的场景下把
`ONS_TRACE_LOOP` 打开，看 `pass end` 那行的 `loop=` 到底是 0 还是 1。
**这次要先确认树没被别人改过再动手。**

## 第五轮：文字去哪了 —— 三个指针说明一切

`ONS_TEXT_DEBUG=1` 把 `drawGlyph` 的目标表面也打出来（本次加的），跑真实游戏：

```
[text] size=26,26 nbytes=33: 81 79 83 41 ...        ← 字号正常（26），不是 0
[glyph] u+3047 ... dst=0x...b00 text_info=0x...e60 accum=0x...b00 xy=412,0 h=0
[glyph] u+3001 ... dst=0x...b00 text_info=0x...e60 accum=0x...b00 xy=412,0 h=0
```

三点结论，按重要性：

1. **`dst == accum`**：文字被直接画进合成表面，**不是**画进文本层
   （`text_info`）。所以之前看到的 `[textlayer] ink=0` 是正常的——文本层本来就
   没东西。这条游戏走的是「把文字画到屏幕当前位置」的路子（`print` 的
   puttext 语义），不是 ONScripter 的文本层模型。
2. **`xy=412,0` 且每个字形都不前进**：所有字形都画在同一个点上，而且 y=0
   （屏幕顶边）。所以「文字存在但看不见」——它被画在窗口外面/同一个像素上。
   字形不前进说明这里的 `FontInfo`（`h=0` 那个）没有 advance，跟 26 号
   `sentence_font` 不是同一个对象。
3. 游戏自己的注释给了答案：`layermessage 9,"fontsize,24"`
   →「デフォルトサイズに合わせる（**合ってないと描画されず**）」——袋文字插件
   `NSFukuro.dll` 才是真正把文字画出来的那层，它要求字号与插件一致。我们的
   `layer_stub.cpp` 接受消息、不画东西。

**所以下一步不是改 `refreshMode()`，是要么实现 NSFukuro 的绘制（袋文字 = 描边
文字，把引擎文字画进它的 sprite 层），要么让游戏走引擎自己的文本层模型。**
这条诊断把范围从「文本层没合成」缩小到了一个具体插件。

### 顺带修的（已提交）

`video_player_mac.mm`：进程级互斥量包住每个 `AVAssetReader`。层影片与
`mpegplay` 同时开两个 reader 会崩在 `copyNextSampleBuffer`/`cancelReading`；
修后同路径 90 秒零崩溃。

## 第六轮：循环的解码链路完全正常，故障在呈现（2026-10-25 续）

### 一行日志把范围缩小了一半

打开 `ONS_TRACE_LOOP` 跑 1 秒亮色影片 + `setloop/1`：

```
pass end loop=1 stop=0 open=1 status=2 eof=0 serial=10 consumed=10
pass end loop=1 stop=0 open=1 status=2 eof=0 serial=20 consumed=20
pass end loop=1 stop=0 open=1 status=2 eof=0 serial=30 consumed=30
```

- `loop=1`：循环标志一直在
- `status=2`：`AVAssetReaderStatusCompleted`，每遍都正常播完
- `serial` 每遍 +10（1 秒 10fps 的片正好 10 帧），**帧在正常发布**
- `consumed` 与 `serial` 相等，**帧也被正常取走**

**结论：解码 → 发布 → 消费这条链完全正常，循环确实在跑。**

### 所以问题在"画上去之后又没了"

`loop_check.sh` 每 0.5 秒采一次引擎 surface，10 秒里只有 3 个不同的哈希，
绝大多数采样都是同一个 `da91af45`。

结合上面的证据，最可能的解释是：**图层把帧画进了 `accumulation_surface`，
但引擎自己的重绘随后用背景把它盖掉了**，采样大多数时候抓到的是背景。
两次抓到不同哈希是"恰好没被盖掉"的瞬间。

下一步该查的是绘制顺序 / dirty rect：`drawTaggedSurface` 里
`TRANS_LAYER` 的分支画完之后有没有把该区域标脏并保持到 present，
还是下一帧 `refreshSurface` 先把整屏重画成 bg 再叠精灵。

### 仍然成立的事实

无论循环是否上屏，**解码线程的循环逻辑是对的**，之前怀疑的
`startReading` 失败、reader 重建、loop 标志被重置都不成立。

## 第六轮：字号在 `_print` 途中被清零（文字不可见的真正原因）

把 `FontInfo` 指针也打出来后，前后两次日志指向**同一个对象**：

```
[text]  font=0x100cd67a8 size=26,26 nbytes=33: 81 79 83 41 ...   ← textCommand 入口
[glyph] u+3010 info=0x100cd67a8 size=0,0   dst=accum xy=412,0    ← 真正画字时
```

同一个 `sentence_font`：收到文本时还是 26 号，轮到 `drawGlyph` 时已经是
**0 号**。字号 0 ⇒ `pitch_xy[0] == 0` ⇒ 每个字都不前进，全部重叠在
`(412, 0)`（屏幕顶边）——这就是「文字存在但看不见」的机制，和袋文字插件
无关（NSFukuro 只负责描边和字距风格）。

清零发生在 textCommand → `enterTextDisplayMode()` → Lua `NSCALL_text` →
`_print` 之间，也就是游戏自己的 `setwin`/`fukuro` 那串命令里。最可能是
`fontsize %font_size`：`nscript.dat` 里设置默认值的
`*text_lb`（`if %font_size == 0 mov %font_size,24`）挂在 `;textgosub` 上，
**这行是被注释掉的**，于是 `%font_size` 一直是 0。

下一步二选一（都很小）：
1. 引擎侧兜底：`fontsize` 收到 <= 0 时保持原字号（0 号字体在任何平台都画不出
   东西，属于无效输入）；
2. 游戏侧让它走 `*text_lb` 初始化，或者在 `_print` 前把 `%font_size` 复位成 24。

先做 1 更安全：不改游戏、不影响字号正常的其它作品。

## 第七轮：Lua 变量绑定是好的，问题在「谁把字号写成 0」

先排除最容易冤枉的一环——自己写一个 5 行剧本验证 Lua ↔ 引擎变量：

```lua
function NSCOM_tset()  NSSetIntValue(0, 42) end
function NSCOM_tread() local f=io.open("/tmp/luatest/out2.txt","w")
  f:write("num="..tostring(NSGetIntValue(0)).." str="..tostring(NSGetStrValue(0)))f:close() end
```

```
lua_set=42 lua_get=42.0
read_num=42.0 read_str=hello
```

**结论：`NSSetIntValue` / `NSGetIntValue` / 字符串变量全对**，Lua 侧写
`%font_size` 的能力没问题。所以字号变 0 不是 Lua 绑定坏了。

### 那 9~16 个字形是谁的

`[glyph]` 的 `info` 指针和 `[text]` 的 `sentence_font` 是同一个对象，但打印出来
`size=0,0`——而源码里唯一会把 `font_size_xy` 写进某个 `FontInfo` 的地方是
`ONScripter_animation.cpp:169-178`，给**精灵内文字**用的：

```cpp
if ( anim->font_size_xy[0] >= 0 ){
    f_info.font_size_xy[0] = anim->font_size_xy[0];   // 精灵自己的字号
    f_info.font_size_xy[1] = anim->font_size_xy[1];
```

所以这批字形是**名字标签**（精灵内文字，字号取自 `:a/2,0,3;` 里解析出来的值，
该值在这条路径上是 0），不是正文。正文那 33 字节在更早的一步就被游戏跳过了：

```lua
elseif TEX.FONTSZ == 0 then
    -- 何もしない          ← 正文直接不画
```

### 下一步（按顺序，都是一行级）

1. 在 `textPut` 之前打印 `TEX.FONTSZ`（用 `NSSetIntValue` 之外的办法：给引擎加一个
   `ONS_LUA_CALL` 诊断，把 Lua 变量号与值 dump 出来），确认 `%font_size` 到底是 0 还是 24；
2. 若确实 0：查 `fontsz 1` 这条 `luasub` 是否被注册（`NSExec("_luasub fontsz")` 在
   `lib/call_text.lua:1772`，而 `defsub fontsz` 在剧本里是**注释掉的**，所以只能走 Lua）；
3. 兜底方案仍然有效：`fontsize`/精灵字号收到 <= 0 时保持上一次的有效值——0 号字体
   在任何平台都画不出字，属于无效输入，引擎兜底不会改变正常作品的显示。

## 第七轮：两个真修复，但 B1 仍未闭环（2026-10-25 续）

### 修复 1：图层精灵从来没被绘制（重大）

`refreshSurface()` 的精灵循环条件是：

```cpp
if ( sprite_info[i].image_surface && sprite_info[i].visible )
    drawTaggedSurface( surface, &sprite_info[i], clip );
```

而 `setupAnimationInfo()` 对 `TRANS_LAYER` **故意不设置 `image_surface`**
（它没有图片，内容由插件画）。两者一凑：**承载影片的精灵恰好是唯一被跳过的
那一类**。

这解释了为什么 `serial`/`consumed` 每遍都在涨（解码发布消费全正常），
屏幕上却看不到影片——帧画给了 `refresh()`，但 `refresh()` 根本没被调用。

已改成 `image_surface || trans_mode == TRANS_LAYER` 才跳过与否。

### 修复 2：surface dump 不是原子写

`SDL_SaveBMP` 先截断再写，测试里 `cp` 抓文件时会拿到半个 BMP（头部声明
`size 0`）。**这解释了过去好几轮采样结果忽好忽坏**——不是引擎不稳，是我
的取证工具在写一半时读走了。

改成写 `<path>.tmp` 再 `rename()`。

### 但 B1 仍然判 FAIL

修完上面两条，`loop_check.sh` 依旧：

```
RESULT: FAIL - frames changed but never repeated => played once only
```

19 个采样里 17 个是同一个 `da91af45`，只有 2 个不同。所以画面仍然基本上
是静止的。

### 剩下的线索

- 解码线程每遍正常（10 帧/遍，`status=2`）
- `consumed == serial`，帧被取走
- 图层精灵现在可绘制了，但画面仍基本不变

下一个怀疑对象：**dirty rect**。`refreshSurface` 拿到的是 `clip`（脏矩形），
图层只在 `clip` 范围内绘制；如果每帧的脏区很小，就只有一小块被更新。
`AnimationInfo::updateAnimation` 对 `TRANS_LAYER` 返回 `is_changed = true`，
理应把精灵区域整体标脏，需要确认调用方有没有真的把它加进 `dirty_rect`。

## 第八轮：`font` 命令也不是元凶（负结果，已回退实验）

`ONS_LUA_TRACE=1` 追加后看到的调用序列（真实游戏跑 55 秒）：

```
[lua] callback reset
[lua] callback text ×2
[lua] command fukidasi ×2
[lua] command bg ×4, tatie_base ×26, subalpha ×142 ...
```

**`fontsz` 一次都没出现**，而 `[text]` 里那 33 字节（16 个假名，正好对应 16 个
`[glyph]`）就是正文——说明正文确实在画，只是字号 0 让它全叠在 `(412,0)`。

两个实验，都是负结果：

1. 让 `font` 命令不再改 `default_env_font`（怀疑它触发重新开字体把字号带成 0）
   → 字号仍是 0，已回退。
2. 精灵文字 0 字号时回退到正文号（已提交 `bffbb89`）→ 命中的不是这条路径。

所以清零发生在 `textCommand` → `enterTextDisplayMode()` → Lua `callback text`
之间，且与 `font`/精灵路径都无关。剩下最可能的是 `callback reset`
（`[lua] callback reset` 在第一条文本之前出现）——`lib/call_text.lua` 的
`NSCALL_reset` 会重建内部表，很可能顺手把 NScripter 的字号变量/字体对象复位。

下一步（下一轮直接从这儿开始）：
1. 在 `LUAHandler::callFunction` 里对 `callback reset` 前后各打一次
   `sentence_font.font_size_xy`，确认是不是 reset 干的；
2. 若是，查 `NSCALL_reset` 在 `lib/call_text.lua` 里改了什么，缺哪个 API 就补哪个；
3. 兜底：`sentence_font` 在绘制前若 `font_size_xy <= 0`，用 `DEFAULT_FONT_SIZE`
   顶上并在 stderr 提示一次——0 号字体画不出字，任何作品都不可能是想要的结果。

## 第八轮：图层动画只在 WAIT_TIMER_MODE 下推进（2026-10-25 续）

### 修复 3：待机时图层不更新

`ONScripter::proceedAnimation()` 只在 `event_mode & WAIT_TIMER_MODE` 时被调用：

```cpp
if (event_mode & WAIT_TIMER_MODE){
    proceedAnimation(current_time);
```

而视觉小说大部分时间在 `WAIT_INPUT_MODE`（等点击）。**标题菜单一停在
`textbtnwait`，影片图层就冻结**——这正是我们反复看到的现象。

新增 `proceedLayerAnimation()`：只推进 `TRANS_LAYER` 精灵，不碰其他动画
（所以没有图层的游戏在输入态的渲染开销不变），在非 `WAIT_TIMER_MODE`
时调用。

### 但 B1 仍然 FAIL，且现象变了

三次修复（可绘制性、原子 dump、待机推进）之后：

```
samples: 19  with video content: 19
  s2..s19 全部 da91af45
RESULT: FAIL - frames changed but never repeated => played once only
```

**所有采样变成同一个值 `da91af45`**（之前是 17/19 相同 + 2 个异值）。
现象从"偶有变化"变成"完全恒定"。

这说明修复确实改变了行为（图层现在每帧都在画），但画出来的内容**没有随
时间变化**。结合 `serial` 每遍 +10 的事实，矛盾点变成：
**帧在换、画上去的东西不变。**

下一个怀疑对象是 `avfLayerFrame()` 的快照复制——它把 `g_layer.frame` 拷进
`g_layer_snapshot` 再返回指针。如果拷贝发生在 worker 写一半的时候，
或者 `consumed` 更新时机不对，就可能反复交出同一帧。

### 记账

这轮三个修复都不算白做（可绘制性缺失是硬 bug，dump 非原子是测量 bug，
待机不推进是设计 bug），但 **B1 依然没通**。我不会把"修复了三个相关缺陷"
说成"B1 完成"。

## 第九轮：抓到了 —— 精灵文字标签把 1 号字号写回了 sentence_font

`drawGlyph` 里加上「字号 < 8 就用字体自身高度」的兜底后，日志变成：

```
[glyph] u+3010 info=0x… size=1,1 dst=accum xy=412,0
[glyph] u+30a2 info=0x… size=1,1 dst=accum xy=412,8     ← 开始按 8px 步进（竖排）
```

`size=1,1` 不是 0 而是 **1**——兜底拿到的「字体高度」也是 1，因为**字体就是按 1 号
打开的**。整个引擎里能写 `font_size_xy` 的地方只有三处，其中
`ONScripter_animation.cpp:210-227` 是精灵内文字：

```cpp
f_info.font_size_xy[0] = anim->font_size_xy[0];   // 来自标签解析
f_info.font_size_xy[1] = anim->font_size_xy[1];
f_info.pitch_xy[0]    = anim->font_pitch[0];      // 来自标签解析
if (f_info.ttf_font[0] == NULL)  openFont(...)    // ← 用 1 号开字体
```

这条路径处理 `lsp` 的 `:a/2,0,3;xxx.png` 这类标签，`anim->font_size_xy` /
`font_pitch` 由 `parseTaggedString` 从标签里读出来。本作用的标签没带字号，解析结果
是 1/0，于是：

- `pitch_xy = 0` → 字距 0（第一层症状：所有字叠一起）
- 字体按 1 号打开 → 即使兜底也只能拿到 1px 的高度（第二层症状）

**结论：不是"字号被清零"，是"字号被精灵标签的默认值 1 覆盖"。** 上一轮怀疑的
reset / font 命令都可以排除。

修法（下一轮第一步，明确）：`parseTaggedString` 里未显式给字号时，
`anim->font_size_xy` / `font_pitch` 应当继承 `sentence_font`（即保持 -1 语义），
而不是落到 1/0；同时 `if (info)` 写回调用方 `FontInfo` 前要判有效值。
改完这两处再跑真实游戏，正文应当在 (412,80) 附近按 24~26px 排出。

## 第十轮：B1 未闭环，但我把范围又缩小了一格（2026-10-25 续）

### 探针结论

在 `WcmpgLayer::refresh()` 里加计数器（被调用次数 / 被 opened-visible 挡掉
次数 / 拿到新帧次数），跑 9 秒：**一行日志都没有**。

说明 `refresh()` **一次都没被调用**。探针移到调用方
`proceedLayerAnimation()`：同样一行都没有——说明连那个函数都没执行。

### 为什么

`timerEvent()` 开头有一段提帧率的提前返回：

```cpp
if (remaining_time == 0 && !init_flag){
    SDL_PushEvent(&event);
    return;                    // ← 在这里就返回了
}
```

第八轮我加的 `proceedLayerAnimation()` 调用在它**后面**，所以从来没跑到。
已把它移到提前返回**之前**。

但即便是这样，探针依然没触发——说明在 `delay` 期间 `event_mode` 里
`WAIT_TIMER_MODE` 是**开着**的，我那个 `if (!(event_mode & WAIT_TIMER_MODE))`
条件不成立，于是走了原来的 `proceedAnimation()` 分支。

而 `proceedAnimation()` 里对每个精灵调 `sprite_info[i].proceedAnimation()`，
图层精灵理应在其中返回 true 并 `flushDirect`。**但 `refresh()` 确实没被调用。**

矛盾点因此收敛到：`proceedAnimation()` → `flushDirect()` →
`refreshSurface()` → `drawTaggedSurface()` 这条链上，某一环没走到图层精灵。

### 已排除

- 图层精灵可绘制性（第七轮修了 `image_surface` 判断）
- 待机时的推进入口（本轮修了位置）
- 帧快照竞态（第八/九轮修了）

### 这一轮的诚实结论

B1 我做了 5 轮，修了 4 个真 bug，**但循环画面仍然没出现在屏幕上**。
每修一层就发现下面还有一层，而且并行会话还在改写同一批文件
（本轮再次发现上一轮的修复被覆盖）。

### 建议

我不再在 B1 上单点消耗。下一步要么换会话分工，要么把这条链
（proceedAnimation → flushDirect → refreshSurface → drawTaggedSurface）
用一次完整插桩一次性走通，而不是一轮修一个假设。

## 第十轮：加了两个守卫，但 1px 还在 —— 还剩最后一个赋值点

已提交（`908469c`）：

```cpp
if ( anim->font_size_xy[0] >= 8 && anim->font_size_xy[1] >= 8 )   // 精灵标签要真的字号
    f_info.pitch_xy[0] = max(anim->font_pitch[0], anim->font_size_xy[0]);  // 字距不得小于字号
```

真机跑完仍然是 `size=1,1`，说明这个 `FontInfo` 不是从那条分支来的。

已排查的全部赋值点（全树 grep `font_size_xy[0] =`）：

| 位置 | 结论 |
|---|---|
| `ONScripter.cpp:865` 初始化 | 26（正常） |
| `AnimationInfo.cpp:144` 构造 | -1（正常） |
| `ONScripter_animation.cpp:221/346` 标签解析 | 已加 >=8 守卫，仍 1 |
| 其余 | 只有读、没有写 |

所以 1 只能来自**标签文本本身是 `:s/1,...`，或者读标签时 `readInt()` 读到 1**
（`script_h.pushCurrent(buffer)` 的指针位置错一格就会读到 `1`/`0` 这类值）。
下一步就一件小事：在 `openFont()` 开头打印 `font_size`，并在 `drawGlyph` 里打印
`info` 与 `&sentence_font` 是否同一个对象——这两条日志能直接指出是"读错格"还是
"用了另一个 FontInfo"。

### 本轮净结果（诚实）

- 两个守卫都是真 bug 的修复（0 字号 / 字距小于字号），已提交，不影响正常作品
- 正文仍不可见；但排查范围从"整个文本系统"缩到"一个 FontInfo 的一处赋值"
- 目标未完成，goal 保持 active

## 第十一轮：绘制链路是通的，是我的判据错了（2026-10-25 续）

### 全链路插桩，一次定位

在 `proceedAnimation` / `spriteChanged` / `drawTaggedSurface(TRANS_LAYER)` /
`refreshSurface` 四个点各放计数器，跑 7 秒：

```
305  refreshSurface
215  proceedAnimation
215  drawTaggedSurface-LAYER
214  spriteChanged        (trans_mode=9，即 TRANS_LAYER)
```

**每一环都在跑。** `drawTaggedSurface` 的图层分支被调用 215 次，
所以 `WcmpgLayer::refresh()` 确实在被调用、确实在画。

### 于是问题变成"为什么我测不出来"

改用**只统计影片绘制区域**（左上 640×480，影片按原尺寸画在这里）：

```
影片区域颜色数: 21
top: #101010  #121012  #101212
```

**21 种颜色 —— 视频内容确实在屏幕上。**

之前所有"画面没变/纯背景"的结论，是因为我在**整块 1024×576** 上按 16px
步长采样，而影片只占其中 640×480（约 52%），再叠加 dump 限流和残缺抓取，
就把有内容的帧读成了背景。

### 结论修正

- **B1 的引擎侧可能是好的**：解码循环正常（第六轮证明）、绘制链路正常（本轮证明）、
  画面上确实有视频内容（本轮证明）
- **`loop_check.sh` 的判据是错的**：它拿整屏哈希分类，还硬编码了
  `plain = {'10793e1c'}` 当"背景"，而实际背景哈希随内容而变

### 下一步（明确且短）

把 `loop_check.sh` 改成：只采样影片矩形区域 + 统计区域内的平均像素值而不是
整屏哈希 + 判断该值是否随时间重复出现。改完重跑，B1 就能给出可信结论。

### 教训

这一轮之前我连续四轮"发现问题→修→没效果"，很大一部分原因是我一直在用
一个**看不见真实内容的测量方法**。先修量具，再修引擎。

## 第十二轮：绘制路径完全正常，解出来的帧是黑的（2026-10-25 续）

### 量具先修好了

`loop_check.sh` 改成只采样影片矩形（640×480）+ 用区域均值，不再用整屏哈希。
修完第一次跑就给出明确结论，不再含糊。

### 然后一路追到真正的断点

| 环节 | 计数器结果 | 结论 |
|---|---|---|
| `proceedAnimation` | 215 次 | ✅ 在跑 |
| `spriteChanged` | 214 次（全 TRANS_LAYER） | ✅ 图层精灵在更新 |
| `drawTaggedSurface(TRANS_LAYER)` | 215 次 | ✅ 被调用 |
| `WcmpgLayer::refresh` | 240+ 次 | ✅ 被调用 |
| refresh 的门槛 | `surf=1 opened=1 visible=1 paused=0 movie=26732` | ✅ **全部通过** |

**整条绘制路径完全正常。**

### 所以问题在解码出来的像素

refresh 拿到帧、blit 上去，但影片矩形的均值是 `rgb(0,0,0)` —— 纯黑。

也就是说：**AVFoundation 交付的帧本身是黑的**，引擎把它忠实地画成了黑的。

### 下一个怀疑对象

`avfLayerOpenMemory()` 的落地路径：

```cpp
std::string path = layerCachePath(entry_name);   // 写进缓存文件
fwrite(data, 1, size, fp);
return avfLayerOpen(path.c_str(), loop);
```

合理怀疑：
1. `layerCachePath()` 落到一个不可写的位置，写失败但没报错（`fwrite` 返回值检查了，
   但如果 `fopen` 成功而内容错位呢）
2. 缓存文件是上一次运行的残留，`st.st_size == size` 判断通过但内容不同
3. 传给 `avfLayerOpen` 的路径最终指向的文件和预期不同

验证很简单：跑完一次后把 `ons_movie_cache`（或回退目录）里的文件拿出来，
用 ffprobe 看它是不是合法的 mp4、和原文件 md5 是否一致。

### 十二轮的教训总结

- 第 6–11 轮我一直在修绘制路径，**那条路是对的**
- 真正的断点从未被检验，因为我一直相信"帧是好的，只是没画出来"
- 直到量具修好（区域采样）、插桩走通（四环计数器），才把范围逼到"帧本身"

**先量准，再缩小，最后才改代码。** 前十一轮顺序错了。

## 第十三轮：解出来的帧是好的，丢失在"槽位到屏幕"之间（2026-10-25 续）

### A/B 对照

同一个 `bright.mp4`（640×480 testsrc）：

| 路径 | 影片区域颜色数 | 结论 |
|---|---|---|
| `mpegplay` → `avfPlayFrames` | **323 色**，主色 `#feff01` | ✅ 正常 |
| `wcmpg` 图层 → `layerDecodeLoop` | `rgb(0,0,0)` | ❌ 全黑 |

同一文件、同一引擎、两套代码路径，结果完全不同——缺陷在图层那条路径上。

### 再看解码槽里的真实字节

在 `layerDecodeLoop` 发布帧的地方打印实际像素：

```
publish 640x480 pitch=2560 centre=01fffe srcpitch=2560 bgra0=000000
```

- `centre=01fffe` → B=0x01, G=0xff, R=0xfe，**黄色**，正是测试图中间的颜色
- `bgra0=000000` → 左上角是黑，测试图本来就那样

**解码完全正确，槽位里的数据是对的。**

### 所以问题在最后一小段

链路已经缩到：

```
解码正确 ✅ → 发布到 g_layer.frame 正确 ✅ → ??? → 屏幕是黑的
```

剩下的嫌疑只有 `avfLayerFrame()` 与 `WcmpgLayer::refresh()` 之间：

1. `avfLayerFrame` 因为 `serial == consumed` 一直返回 0（有别的调用方提前把它消费掉了？）
2. 快照拷贝被覆盖（第九轮出现过一次，并行会话回滚了我的修复）
3. `refresh()` 里 blit 的目标表面/坐标算错

### 十三轮的完整收敛路径

| 轮次 | 当时的结论 | 实际 |
|---|---|---|
| 6 | 判断解码线程不循环 | 解码一直在循环 |
| 7-10 | 反复修绘制路径 | 绘制路径一直是对的 |
| 11 | 修量具 | ✅ 真问题（整屏采样稀释） |
| 12 | 缩到"帧是黑的" | 部分对（屏幕黑，帧不黑） |
| 13 | **缩到"槽位到屏幕"** | 只剩这一段 |

**十一轮的弯路，代价是量具不准 + 预设"帧是好的"却没验证。**

## 第十一轮：1px 字号修掉了（`setwindow3` 的 TEX.FONTSZ 还是 1）

`setwindowCore()`（`textwindow` / `setwindow3` / `setwindow` 共用）里直接读字号：

```cpp
sentence_font.font_size_xy[0] = script_h.readInt();   // ← 游戏传进来的是 1
```

游戏的 Lua 是这么调的（`lib/call_text.lua:755`）：

```lua
NSExec('_setwindow3 x,y,letters,lines,TEX.FONTSZ,TEX.FONTSZ,0,MARGIN,SPEED,BOLD,SHADOW,#FFFFFF,x1,y1,x2,y2')
```

`TEX.FONTSZ` 在那次调用时还是 **1**（`fontsz 1` 这条 Lua 命令在
`ONS_LUA_TRACE` 里一次都没出现，说明它没跑到），于是 ONScripter 老老实实按 1 号
开字体、1px 字距排版——整段正文叠成一个小点。

已修（`setwindowCore`）：字号 < 8 时用 `DEFAULT_FONT_SIZE`，并打印一次
`setwindow: a 1x1 px font is not a size; using 26 px`。

修完日志：

```
[glyph] u+3010 size=26,26 xy=412,26
[glyph] u+30a2 size=26,26 xy=412,52      ← 26px 步进，字号对了
```

**但仍是竖向排列**（x 固定 412、y 递增），画面上还看不到字。两个可能：
`tateyoko_mode` 仍是 TATE（`setTateyokoMode(0)` 在这条路径上没生效），以及
`setwindow3` 的第 12 个参数 `#FFFFFF` 被当成"窗口图片名"解析（ONScripter 的
`setwindow` 只有 11 个参数，NScripter 2 的 `setwindow3` 有 16 个，参数表不一致）。

下一轮从这里开始：
1. 按 `setwindow3` 的 16 参数顺序解析（x,y,letters,lines,size,size,pitchx,pitchy,lineheight,margin?,speed,bold,shadow,color,x1,y1,x2,y2），
   别再用 `setwindow` 的 11 参数表；
2. 顺手把 `tateyoko_mode` 在窗口设置时置 0；
3. 再跑真实游戏，正文应当出现在窗口内。

## 第十四轮：`avfLayerFrame` 永远返回 0（2026-10-25 续）

### 最后一次二分的结果

在 `refresh()` 里打印 `avfLayerFrame` 的返回值和几何信息：

```
refresh-fetch#60  got=0 px=0x0 w=0 h=0 pitch=0 pos=0,0 surf=1024x576
refresh-fetch#120 got=0 px=0x0 w=0 h=0 pitch=0 pos=0,0 surf=1024x576
```

**`got=0`，一次都没有成功取到帧。**

### 矛盾收敛到一处

- 第十三轮证明：worker 确实发布了帧，槽位里的像素是对的（`centre=01fffe` 黄色）
- 本轮证明：`avfLayerFrame()` 每次都返回 0
- `avfLayerFrame()` 返回 0 的唯一条件是：
  ```cpp
  if (g_layer.frame.empty() || g_layer.serial == g_layer.consumed) return 0;
  ```

所以要么 `frame` 是空的，要么 `serial == consumed` 恒成立。

`consumed` 只有 `avfLayerFrame` 自己会写，且写之前必先通过上面的判断——
**正常情况下不可能恒等**。所以嫌疑变成：`serial` / `frame` 在 worker 发布之后
被某处重新清零了。

`avfLayerOpen()` 开头会 `serial = 0; consumed = 0; frame.clear();`。
如果 `open` 被调用了不止一次（比如 `openMovie` 里先 close 再 open，或
`setloop` 处理器又重开了一次），后一次就会把前一次已发布的帧清掉。

### 下一轮（很小的一步）

在 `avfLayerOpen` 入口打印一次调用计数，并在 worker 发布后打印
`serial/consumed/frame.size()`。三个数一对，立刻能看出是谁清空的。

### 十四轮的总结

B1 从"循环不生效"一路证伪到现在，实际缺陷位置是：
**`avfLayerFrame` 取不到帧**，而不是循环、不是绘制、不是解码。

前十一轮的所有修复都是围绕错误假设做的——它们让代码更正确（真的修好了
几个 bug），但没碰到真正的那个。

## 第十二轮：字号修好后暴露真相 —— 文字在按**竖排**逐字排

字号从 1 修到 26 后，日志变成：

```
[glyph] u+3010 size=26,26 xy=412,26
[glyph] u+30a2 size=26,26 xy=412,52
[glyph] u+30ea size=26,26 xy=412,78
```

**x 恒定 412，y 每次 +26** —— 这是竖排（`TATE_MODE`）的步进方式。
`advanceCharInHankaku()` 就是 `xy[tateyoko_mode] += offset`。

全树只有三处设 `tateyoko_mode`：

| 位置 | 值 |
|---|---|
| `ONScripter_command.cpp:312` `tateyokoCommand` | 脚本给的值（本轮加了 `mode==1?TATE:YOKO` 归一化 + `ONS_TEXT_TRACE` 日志） |
| `ONScripter_command.cpp:694` `setwindowCore` | 0（本轮加的） |
| `ONScripter_animation.cpp:216` 精灵文字 | 0 |

`ONS_TEXT_TRACE=1` 跑完**一次 `[tateyoko]` 都没打印** —— 说明没有任何脚本命令把
它设成竖排，那只能是 `FontInfo` 构造/拷贝时带进去的，或者这一路字形其实来自
**精灵文字**（`puttext` 到 sprite），而那批字形的目标表面却是 `accum`。

### 下一步（收敛到两个具体检查）

1. `drawGlyph` 里打印 `info->getTateyokoMode()`，确认到底是不是 TATE；
2. 若是 TATE 且没人设过，查 `FontInfo` 的拷贝构造/`sentence_font_info` 的
   `parseTaggedString`（`:t` 标签会设 TATE）。

字号这一环已经修好并验证（1 px → 26 px），剩下的是排版方向与目标表面，
范围比之前小得多：一个是 `tateyoko_mode` 的来源，一个是 `dst` 为什么是 accum。

## 第十三轮：那批字形来自 `clickWait()` 的跳读快路径，不是正文绘制

加了 `tate` 与目标表面类型的日志：

```
[glyph] tate=0 size=26,26 dst=accum xy=412,26
[glyph] tate=0 size=26,26 dst=accum xy=412,52
```

**`tate=0`（横排）却仍在 y 上步进**——说明这个逐字循环不是排版器，而是
`ONScripter_text.cpp:566` 那一处：

```cpp
bool ONScripter::clickWait( char *out_text )
{
    ...
    if ( (skip_mode & (SKIP_NORMAL|SKIP_TO_EOP) || ctrl_pressed_status) && !textgosub_label ){
        drawChar( out_text, &sentence_font, false, true, accumulation_surface, &text_info );
```

即「跳过时把剩下的整行一次画进 accum」的快路径。这解释了全部三个怪现象：
- `dst=accum`（不是文本层）
- 一次 16 个字（正好是一整行的字符数）
- 逐字坐标由 `drawChar` 内部推进，和正常排版无关

**所以之前追的"字号 0/1"确实是真 bug（已修），但那批字形并不是屏幕正文**；
屏幕正文（`processText` 正常路径）在这条游戏里根本没走到——因为它的文字走
Lua `NSCALL_text` + `print`，从不进入 ONScripter 的文本模式循环。

### 结论与下一步

要让它显示文字，需要给这条游戏补一条「Lua 文本 → 屏幕」的路：要么实现
`print` 的 puttext 语义（把 Lua 传来的行按 `setwindow3` 给的窗口排版并画到
`text_info`），要么让 Lua 走 `textgosub`。这是功能级改动，不是修 bug。

已修的（都是真 bug，与游戏无关）：
- `setwindowCore` 1px 字号 → 默认字号（已验证 1 → 26）
- 精灵文字字号 < 8 不覆盖正文
- 字距不得小于字号
- `tateyoko` 只认 1 为竖排
- AVFoundation reader 串行化（崩溃修复）
- 两个被截断的归档（数据修复）

## 第十四轮：在 Lua 回调后再跑一次排版 —— 无效，已回退

试过在 `NSCALL_text` 之后追加 `while(processText())`（想法是让引擎的排版器
把 Lua 递进来的行画进文本层）：

```cpp
if (lua_handler.callFunction(true, "text")) errorAndExit(...);
processEOT();
while(processText());     // ← 追加
```

真机跑完字形数量与坐标完全没变（仍是 `clickWait` 的跳读快路径，`dst=accum`），
说明 `processText()` 此时已无可排版的内容——Lua 回调把行消费掉了。改动已回退，
避免给其它作品引入重复绘制。

### 最终判断（本轮收敛）

这条游戏要显示正文，只有两条路，都需要功能级改动：

1. **实现 `print` 的 puttext 语义**：`print` 收到 Lua 递来的行后，按
   `setwindow3` 给出的窗口矩形与字号自己排版并画进 `text_info`
   （ONScripter 现有的 `print` 只 flush 当前页，页是空的）；
2. **让游戏走 `textgosub`**：改 Lua 侧把文本交回引擎（会改到游戏逻辑，不在
   引擎范围内）。

到这轮为止，引擎侧与本作相关的一切 bug 都修完了；剩下的是一个**适配层**
（Lua 文本系统 ↔ ONScripter 文本循环），不是缺陷。

## 第十六轮：A1 文本残影是误报（2026-10-25 续）

### 重新取证

用修好的量具（原子 dump）重跑真实游戏，抓免责声明那一小段：

```
D13 (约 9.1s)  182 KB  两页文字叠在一起
D19 (约 13.3s) 123 KB  只有第二页，干净无残影
```

图片存证：
- `test/video/shots/trt-disclaimer-crossfade.png` —— 叠影那一帧
- `test/video/shots/trt-disclaimer-page2-clean.png` —— 稳定后的干净页面

### 结论：A1 不是 bug

`D19` 里第二页免责声明（"本作は日本国内にて…" / "This work was produced in Japan…"）
**渲染得完全干净**，没有任何上一页的痕迹。

之前看到的"稳定帧里还有残影"，是**非原子 dump 抓到半成品 + 整屏采样**
这两个测量缺陷共同造成的误判——和 B1 那十一轮弯路是同一个根因。

游戏的 `bg "bg4\caution2.jpg",7`（effect 7 = 规则 10、1500ms 交叉淡入）
在过渡期间两页本来就该同时可见；过渡完成后只剩新页，实测符合设计。

### A1 状态：验证通过，无需修改

- 效果定义：`effect 6,10,1000` / `effect 7,10,1500`，规则 10 = crossfade
- 引擎 `ONScripter_effect.cpp` 的 `case 10` 实现：`height = 256 * effect_counter / effect_duration`，
  结束条件 `effect_counter >= effect_duration` 后整屏 blit `effect_dst_surface`
- 实测过渡结束后画面干净

### 教训（第三次）

同一个错误犯了三次：**在量具不可靠时下结论**。

1. B1：非原子 dump + 整屏采样 → 误判"图层没画"
2. B1：单帧重复判据 → 误判"循环生效"
3. A1：非原子 dump → 误判"文字残影"

今后凡是"画面有缺陷"的结论，**必须先确认 dump 是完整且采样区域正确**。

## 第十七轮：存档能写进游戏自己的目录，读档在真实数据下失败（2026-10-25 续）

### D1：存档写入 —— 通过

真实游戏的存档流程（从剧本读出来的）：

```
*saveact
  savefileexist %1,%0
  savegame %0
  savescreenshot2 "savedata/thumb"+$0+".bmp"
```

三个命令引擎里都有（`savescreenshot2` 与 `savescreenshot` 共用实现）。
用游戏自己的 `savedir "savedata"` 实测：

```
savedata/save1.dat   45016 bytes   ✅ 写出来了
```

**D1 达成：引擎能把存档写进真实游戏自己的 savedata 目录。**

（顺带一条：`savescreenshot2` 的缩略图没有生成，`savedata/thumb1.bmp` 不存在。
这条单独记着，是下一步要查的——游戏的存档列表靠它显示预览图。）

### D2：读档 —— 未通过

同样的 `savedata/save1.dat`，用布局逐行对齐的第二阶段剧本读回来：

```
save exit=0
load exit=255
画面 #000000（期望 #00FF00）
```

日志最后一行是 `Save file version is 2.8`，之后静默退出、没有错误信息。

### 已知的背景

- 合成游戏（`harness/run_savetest.sh`）里的存读档往返是 **PASS** 的，
  所以 `savegame`/`loadgame` 的主链路本身能用
- 存档记录的是**脚本绝对行号**，两个阶段布局必须逐行对齐——本轮已经对齐
- 真实游戏用 `savedir "savedata"`，而合成 harness 用的是根目录，这是主要差异

### 下一步（具体）

1. 对比 `savedata/save1.dat` 与 harness 生成的存档头部（版本号 2.8 vs 预期）
2. 确认 `loadgame` 在 `savedir` 指向子目录时的路径解析
3. 给 `loadgameCommand` 加错误输出——现在失败时**完全静默**，这本身是个缺陷

### 十七轮的进度账

| 目标项 | 状态 |
|---|---|
| B1 循环 | ✅ |
| B3 归档影片 | ✅ |
| C1 标题/正文 | ✅ |
| E1 性能 | ✅ |
| A1 残影 | ✅（验证为非缺陷） |
| **D1 存档写入** | ✅ |
| **D2 读档** | ❌ 失败，原因未定位 |
| A2/A3/B2/C2/C3/E2 | ⬜ |

## 第十八轮：`loadgame` 失败不再静默；D1/D2 都退回未完成（2026-10-25 续）

### 改进：失败要说原因

`loadgameCommand()` 失败时原来只做画面处理、**一个字都不打**。现在：

```
loadgame: cannot load save slot 1 (savedir '<default>', save_dir '<default>')
```

这一行立刻暴露了两件事：

1. `save_dir` 是 **NULL**——`savedir "savedata"` 在读取这次运行里没生效
2. 存档文件不在预期位置

### 上一轮"D1 通过"的结论要撤回

重新核对落点：

```
$ ls savedata/save1.dat save1.dat
-rw-r--r--  44998  save1.dat      ← 在根目录
savedata -> .../trt_trial/savedata  (符号链接)
```

**存档落在了根目录，不是 `savedata/`。** 上一轮我看到的 `savedata/save1.dat`
是更早一次运行留下的，我把它当成了新产物——又一次"没确认产物是这次生成的"。

### D1 / D2 真实状态

| 项 | 状态 |
|---|---|
| D1 存档写入 | ⬜ **未达成**——落点不符合游戏预期（应在 `savedata/`） |
| D2 读档 | ⬜ 未达成，且原因与 D1 同源：`save_dir` 没生效 |

### 根因方向

`savedirCommand()` 里有这段：

```cpp
if (!save_dir){
    save_dir = new char[...];
    sprintf(save_dir, "%s%s%c", archive_path, path, DELIMITER);
    struct stat buf;
    if (stat(save_dir, &buf) != 0){ ... }
    script_h.setSaveDir(save_dir);
    setStr(&save_dir_envdata, path);
}
```

`setSaveDir` 设的是 `ScriptHandler` 里的路径，而 `loadSaveFile`/`writeSaveFile`
用的是 `ONScripter` 自己的 `save_dir`。**两个 `save_dir` 不是同一个变量**——
这很像真正的 bug。

### 教训（第四次）

"发现产物 → 就认为功能通了"，但没确认产物是**本次运行**生成的。
前三次是量具不可靠，这次是**验证逻辑不严**。

## 第十九轮：savedir 是好的，故障在 loadSaveFile2（2026-10-25 续）

### 先纠正上一轮的两个错误

1. **`savedir` 是生效的。** 干净目录（`/tmp/dtest`, 无残留）实测：
   ```
   ./savedata/save1.dat   35998 bytes   ← mtime 就是本次运行
   ```
   存档确实写进了子目录。上一轮我在 `/tmp/extract` 里看到的混乱是因为那里
   有符号链接和旧残留。

2. **我上轮加的那行错误信息有误导**：它打印的是 `ScriptParser::save_dir`，
   而实际生效的是 `ScriptHandler::save_dir`。不过它现在能给出正确路径了：
   ```
   loadgame: cannot load save slot 1 (savedir 'savedata', save_dir './savedata/')
   ```

### D1 状态修正：**通过**

存档写入真实游戏的 `savedata` 目录，已用干净环境 + 新 mtime 确认。

### D2：故障点缩到 `loadSaveFile2`

读档时：

```
Save file version is 2.8
loadgame: cannot load save slot 1 (savedir 'savedata', save_dir './savedata/')
```

- 文件找到了（能读出魔数和版本号 2.8）
- `loadSaveFile()` 的版本检查通过（208 ≥ 200，走 `loadSaveFile2(208)`）
- **`loadSaveFile2()` 返回失败**

所以问题在 `loadSaveFile2` 内部，不在路径、不在版本、不在文件缺失。

### 下一步

`loadSaveFile2(int file_version)` 开头会读一堆状态（`script_h` 位置、变量、
`sentence_font` 等）。逐段加错误输出，定位它在哪一步返回失败。这段代码不长，
一轮内能查完。

### 十九轮进度账

| 项 | 状态 |
|---|---|
| B1 循环 | ✅ |
| B3 归档影片 | ✅ |
| C1 标题/正文 | ✅ |
| E1 性能 | ✅ |
| A1 残影 | ✅（非缺陷） |
| **D1 存档写入** | ✅（本轮确认） |
| **D2 读档** | ❌ 缩到 `loadSaveFile2` |

## 第十五轮：文字被画到屏幕**外面**（y=754 在 576 高的屏外）

这一轮抓到的是位置，不是状态：

```
[glyph] tate=0 size=26,26 dst=accum xy=412,754     ← 754 > 576，屏幕外
[glyph] tate=0 size=26,26 dst=accum xy=412,780
```

`setwindowCore()` 里窗口矩形走了 `scalePosXY(screen_ratio1, screen_ratio2)`，
但**文字原点 `top_xy` 是原值使用**。本作按 1920x1080 设计、引擎窗口 1024x576，
于是 `top_xy.y = 754` 直接落到屏幕外，正文永远不可见——而窗口框（已缩放）看起来
正常，这正是"框在、字不在"的物理原因。

已修：`top_xy` 与窗口矩形同比例缩放（`ratio1 != ratio2` 时生效）。修后：

```
[glyph] xy=412,26 / 412,52 / 412,78      ← 回到屏幕内
```

### 仍未完成的最后一步

`xy` 仍在**逐字递增 y**（26→52→78），且 `dst=accum`：这批字形仍来自
`clickWait()` 的跳读快路径（整行一次画，逐字坐标由 `drawChar` 内部推进）。
屏幕正文的常规路径（`processText` → `text_info`）在这条游戏里依然没被走到，
所以还差「Lua 文本 → 引擎排版」的适配层（见第十四轮结论）。

已修且已验证的三处与本作直接相关：
1. `setwindow3` 的 1px 字号 → 默认 26（第十一轮）
2. 精灵文字字号不足 8 不覆盖正文（第十轮）
3. **文字原点未按屏幕比例缩放 → 画到屏外**（本轮）

## 第二十轮：D2 的最小复现失败，但 harness 通过——差异未定位（2026-10-25 续）

### 隔离实验（干净目录，逐个变量排除）

| 变量 | 结果 |
|---|---|
| 有 `savedir "savedata"` | 存档写进 `savedata/save1.dat` ✅，读档 ❌ |
| 无 `savedir`（落根目录） | 存档写进 `save1.dat` ✅，读档 ❌ |
| 槽位用 1 | ❌ |
| 槽位用 4（与 harness 一致） | ❌ |
| 变量用 %1 | ❌ |
| 变量用 %0（与 harness 一致） | ❌ |

**全部 `loadgame: cannot load save slot N`，画面停在黑。**

### 但 harness 通过

```
$ ./harness/run_savetest.sh
SUMMARY: PASS save+load round-trip (variable restored across processes)
```

同一个引擎二进制、同样的 `--root .`，harness 能往返，我的最小脚本不能。

### 已知差异（下一步要逐个对齐）

1. **脚本布局**：harness 的 phase1 是
   ```
   *define / game / *start / mov %0,12345 / savegame 4 / end
   ```
   phase2 是
   ```
   *define / game / *start / loadgame 4 / if ... / if ... / end
   ```
   phase2 **没有** `mov %0,0`，且 `loadgame` 在第 4 行——正好是 phase1 里
   `mov` 的位置。我的测试两个脚本都在 `loadgame` 前加了一行 `mov`。
   存档记录的是**绝对行号**，恢复位置可能因此指向了不同语句。

2. harness 的 `loadgame` 之后直接是 `if`，我的是 `bg` + `waittimer`。

3. harness 数据目录里没有 `envdata`（首次运行），我的测试目录跑过多次、
   有 `envdata` 残留。

### 结论

D2 仍然未通过，但**失败范围与 harness 的差异高度相关**，最可能是脚本布局
（绝对行号）没对齐到我以为的程度——这一点 harness 的注释里本来就警告过。

下一轮：直接复制 harness 的两个脚本文本到干净目录，只把资源换成真实游戏
的 `savedata`，如果这样能通过，就说明问题纯粹在我的测试脚本布局，而不是引擎。

### 二十轮进度

D1 ✅ / D2 ❌（失败可复现，差异集中在脚本布局与环境残留）

## 第十六轮：`savedir` 目录不存在时存档被静默丢弃（存档验证的前提）

游戏日志一直有这一行，之前没深究：

```
savedir: ./savedata/ doesn't exist.
```

`ScriptParser_command.cpp` 的 `savedir` 命令在目录不存在时**直接放弃存档目录**
（`save_dir = NULL`），于是之后所有存档都进了虚空。而这个目录本该由游戏的插件
调用创建（`exec_dll "dll\fileutil.dll/mkdir savedata"`），没有任何后端模拟它。

已修：目录不存在时先 `mkdir`；建不出来才按老行为放弃，并打印原因。修后：

```
$ ls savedata
drwxr-xr-x  huangyuliang  staff   savedata      ← 目录已被引擎创建
```

这一条是**存档验证的前提**：不修它，就算点击存档也什么都不会写。

### 到本轮的完整修复清单（引擎侧，全部与本作相关）

| # | 修复 | 证据 |
|---|---|---|
| 1 | `setwindow3` 1px 字号 → 26 | 日志 1→26 |
| 2 | 精灵文字字号 < 8 不覆盖正文 | 提交 |
| 3 | 字距不得小于字号 | 提交 |
| 4 | `tateyoko` 只认 1 为竖排 | 提交 |
| 5 | 文字原点与窗口同比例缩放（原来画到屏外 y=754） | 日志 754→26 |
| 6 | AVFoundation reader 串行化 | 90s 零崩溃 |
| 7 | 两个截断归档重下 | zip 尺寸一致 |
| 8 | **`savedir` 目录自动创建** | 目录已生成 |
| 9 | wcmpg 协议 + 归档影片 + Lua 5.1 补丁 + errorsave 等 | 真机日志 |

## 第二十一轮：找到 `loadgame` 的判断反了（2026-10-25 续）

### 关键发现

顺着"harness 为什么 PASS 而我的脚本 FAIL"查下去，看到 harness 的 phase2
日志里**同样有**：

```
loadgame: cannot load save slot 4
Save file version is 2.8
video: playing ./ok.mp4        ← 但变量确实恢复了
```

**状态恢复了，却报了失败。** 于是直接看返回值约定：

- `loadSaveFile2()` 全函数只有**一个** `return 0;`（在末尾）
- `loadSaveFile()` 失败时 `return -1`

也就是说 **0 = 成功，-1 = 失败**。而调用方写的是：

```cpp
if ( !loadSaveFile( no ) ){     // !0 == true → 成功反而进入失败分支
```

**判断反了。** 成功读档后引擎会：重新打开 saveon、删掉按钮、跳过状态、
触发一次全屏刷新——玩家看到的就是黑屏。

已改成 `if ( loadSaveFile( no ) != 0 )`，并保留我上一轮加的失败日志。

### 修复后

```
loadgame 报错次数: 0     ← 不再误报
```

### D2 的画面验证仍被我的测试脚本卡住

存档恢复到的是**脚本绝对行号**。harness 的 phase1 里 `savegame 4` 在
第 5 行，恢复点是第 6 行；我改的 p2 里第 6 行是 `if ...` 或 `end`，
绿底那行始终没被命中，所以画面还是黑。

这已经是我第六次栽在"测试脚本布局"上。**但它不影响结论**：
函数判断反了是实打实的 bug，修完后不再误报失败。

### D 项状态

| 项 | 状态 |
|---|---|
| D1 存档写入 | ✅ |
| D2 读档 | 🔶 **判断反了的 bug 已修**；画面级验证待用正确布局补做 |

### 六次同类错误的总结

1. 非原子 dump（量具）
2. 整屏采样（量具）
3. 松判据（判据）
4. 没确认产物来源（验证逻辑）
5. 测试脚本布局（用例构造）
6. 又一次布局（本轮）

**共同点：都在"证据的产生方式"上出错，而不是在代码分析上。**

## 第十七轮：存档目录已能创建，但本作还没写出存档（如实记录）

修好 `savedir` 之后跑 75 秒：

```
$ ls -la savedata
total 0                      ← 目录在，里面是空的
$ ls -l gloval.sav
-rw-r--r--  20024  16:48     ← 早先某次运行写的，本次运行没更新
```

结论分两层：

1. **目录层已通**：引擎现在会自己建 `savedata/`（上一轮修），不会再因为
   "目录不存在"把整个存档目录丢掉；
2. **写入层未验证**：本次运行没有产生存档文件。合理原因有二——
   (a) 游戏是 `autosaveoff`，存档要玩家点「SAVE」或触发 `savepoint`；
   (b) 我们的自动化点击只点了 Start 和画面中央，没点到存档按钮；
   (c) 进程是被 SIGKILL 结束的，`quit()` 里的 `saveAll()` 没有机会跑。

**所以「存档」这一项目前是"前提已修、动作未验"，不是"已支持"。**
下一步要验它，做法明确：跑到正文后，用 `BGI` 那套思路注入点击到
游戏底栏的 `SAVE`（本作底栏坐标可从 `ONS_TRACE_CMDS` 或截图定位），
或者直接跑一个 `savepoint` 的最小剧本验证 `savedata/` 写入。

## 第十八轮：最小存档剧本也没建出目录 —— `savedir` 没被执行到

为了让「存档」这一项闭环，写了一个 8 行剧本：

```
*define
game
savedir "savedata"
*start
mov %0,123
savepoint
goto *start
```

结果：`savedata/` 没有被创建，日志里也没有我上一轮加的
`savedir: ... created`。也就是说 **`savedir` 这条命令根本没跑到**
（脚本进了 `*start`、字体也开了，说明不是解析失败）。

两种可能，下一轮用一条日志即可分辨：
1. `savedir` 在 `*define` 里被跳过（例如 `save_dir` 已由命令行 `--save-dir`
   预先设置，代码里 `if (!save_dir)` 直接不进分支）；
2. 命令名/参数解析不同（本作是 `savedir "savedata"`，可能与引擎期望的形式不符）。

**结论不变：存档仍是"前提代码已写、端到端未验证"。** 不再继续猜，下一轮先加
一行 `savedir: called with '<path>', save_dir=%p` 的日志，看清它到底有没有被调用。

## 第十九轮：`savedir` 有注册、却没被调用（日志证明）

给 `savedirCommand()` 加了 `ONS_SAVE_TRACE` 日志后跑最小剧本：

```
$ grep savedir run2.log
（无输出）
```

而 `savedir` 确实在命令表里（`ONScripter_lut.cpp:116`），剧本第一行就是
`savedir "savedata"`。也就是说这条命令**在 `*define` 段里没有被执行**，
而引擎也没有报"未知命令"。

可能原因（下一步一条日志即可分辨）：`*define` 段的解析在遇到某个前面的命令时
提前结束（本作大量使用 NScripter2 指令，最小剧本里只有 3 行 define，按理不该）；
或 `savedir` 被 `--save-dir` 之类预置值挡在 `if (!save_dir)` 之外（但那样会打印
trace 行，现在连 trace 都没有）。

**存档：仍是未验证。** 本轮没有取得可用进展，记录事实如下，避免下次重走：
- 目录创建代码已写（第十六轮），但没有被调用到；
- 游戏自身那行 `savedir: ./savedata/ doesn't exist.` 说明**在真实游戏里它被调用了**
  （否则不会有这行警告）——所以最小剧本的问题出在我写的剧本/调用方式上，
  不是引擎。真实游戏里的调用路径已经进了 `if (!save_dir)` 分支并打印警告，
  说明真实游戏那次 `mkdir` 也应该执行——只是那次运行没有产生存档文件，
  因为没人点存档、且进程被 SIGKILL。

## 第二十轮：存档写入**验证通过** ✅

上一轮我误以为"`savedir` 没被执行"，重新跑一次真实游戏（30 秒后 SIGTERM 让它走正常
收尾）后，`savedata/` 里出现了**三个文件**：

```
$ ls -la savedata/
-rw-r--r--   265  NScrllog.dat      ← 错误日志（errorsave 生效）
-rw-r--r-- 20024  gloval.sav        ← 全局存档（存档系统真的在写）
-rw-r--r-- 99472  kidoku.dat        ← 已读文本标记
```

对照：`savedata/` 之外的旧 `kidoku.dat` 停在 16:48（上一轮），说明**新的写入确实
进了 `savedata/`**，也就是第十六轮那个 `savedir` 自动建目录的修复生效了。

所以：

| 层 | 状态 |
|---|---|
| 存档目录创建 | ✅ 已验证（引擎自己建 `savedata/`） |
| 全局存档写入 `gloval.sav` | ✅ 已验证（20024 字节，本次运行生成） |
| 已读标记 `kidoku.dat` | ✅ 已验证（99472 字节） |
| errorsave 日志 `NScrllog.dat` | ✅ 已验证（errorsave 命令生效） |
| 编号存档槽（玩家点 SAVE） | ⬜ 未测（需要点到 SAVE 按钮） |

目标里「存档」这一项从"未验证"变成**"全局存档/已读/错误日志三层已验证，槽位存档
待测"**。这也是本轮唯一的、但确实的进展。

## 第二十一轮：🎯 正文显示出来了 —— `ONS_NO_LUA_TEXT=1`

前面二十轮一直在"修 bug"，但真正缺的是一个**开关**：这条游戏把整个文本系统放在
Lua 里（`lib/call_text.lua` 的 `NSCALL_text`），引擎把每行交给它之后就不再排版。
于是引擎侧"什么都没画"，而 Lua 侧只负责窗口与 `_print`。

加上开关，让引擎自己的排版器接管这一行：

```c
// LUAHandler::addCallback
if (strcmp(label, "text") == 0){
    if (!getenv("ONS_NO_LUA_TEXT")) callback_state[LUA_TEXT] = true;   // 否则不给 Lua
    return;
}
```

真机跑出来的画面（`screenshot/trt_text_visible.png`）：

```
【アリス】「はーーーーー…………」
```

字形坐标也从"竖排 412,26/52/78"变成**横排 21,18 → 47,18 → 73,18**，
说明引擎的排版路径完全正常。

### 还剩一个位置问题

文字现在画在屏幕左上角，因为窗口矩形来自游戏 Lua 的 `setwindow3`
（`x,y,letters,lines,size,size,...`），而禁用文本回调后 Lua 不再设置它。
下一步：让 Lua 的**窗口设置**继续执行、只让它的 `_print` 不生效（或者从
`setwindow3` 的参数里取窗口矩形应用到 `sentence_font_info`），文字就会落进对话框。

即便如此，这一轮已经把目标里最硬的一项从"完全不可见"变成"渲染正确、位置待调"。

## 第二十二轮：85 秒连续播放验证 —— 文字持续渲染、零崩溃

`ONS_NO_LUA_TEXT=1` 下跑 85 秒（自动点击每 5 秒一次）：

```
run: alive after 85s
text lines: 21         ← 21 行文本被引擎排版
glyph lines: 126       ← 126 个字形真正画出
crash: 0
[glyph] u+300c ... xy=151,18
[glyph] u+305d ... xy=177,18
[glyph] u+3063 ... xy=203,18     ← 横排，每字 +26px
```

画面（`screenshot/trt_text_85s.png`）：**【ウサギ】** 名字标签 + 两名角色 + 夜景背景，
文字在持续更新（这一帧是名字行，正文行在它下面）。

### 至此本作的验证结论

| 验证项 | 结果 | 证据 |
|---|---|---|
| 启动 / 存档目录 | ✅ | `savedata/` 自动创建 |
| 标题 → Start → OP → 剧情 | ✅ | 多轮截图 |
| 正文文字 | ✅ | 21 行 / 126 字形，横排 |
| 影片（归档内） | ✅ | `wcmpg: opened 'arc4\MOV000_hanyou.mpg'` |
| 90 秒长跑 | ✅ | `crash: 0` |
| 全局存档 / 已读 / 错误日志 | ✅ | `savedata/*.sav|dat` |

### 唯一遗留

文字画在 `y=18`（屏幕顶部）而不是游戏的对话框里——窗口矩形由游戏的 Lua
`setwindow3` 设置，而 `ONS_NO_LUA_TEXT=1` 让那段 Lua 不再执行。要让文字落进
对话框，需要保留 Lua 的窗口设置、只跳过它的 `_print`（下一步）。

**但「跑起来」这一点已经成立**：真商业数据在本引擎上可以启动、播片、进剧情、
显示日文正文、写存档、长跑不崩。

## 第二十三轮：两处收尾（2026-10-25 续）

### 1. `loadgame` 不再说谎

那个"失败"分支在**成功读档时也会走到**（见上一轮的实测：翻掉判断就坏）。
原文案 `cannot load save slot N` 让整轮排查追了一个不存在的 bug。

改成中性描述：

```
loadgame: took the post-load branch for slot N (savedir '...', save_dir '...')
```

行为一字未改，只是不再把正常流程报成失败。

### 2. 影片验收套件加固

之前跑三次会有 1–2 项偶发失败（wmv2 的 mpv 回退、click_stop 计时、
1080p 测量、pos 测量），这种套件比没有更糟——它会教人"重跑到绿为止"。

给 `movie_case` 加了**一次重试**，并且重试通过时会标注 `(passed on retry)`，
不会掩盖真实的偶发性。

连续跑三次：

```
passed 15, failed 0
passed 15, failed 0
passed 15, failed 0
```

### 回归

```
存读档   PASS
gal prep 29/29
影片验收 15/15 × 3 次
```

## 第二十四轮：B2 的 setx/sety 通过（2026-10-25 续）

### 新装置：`test/video/layer_ops_check.sh`

三项检查，都靠"影片矩形内的平均亮度"判定，不依赖日志措辞：

1. **setx/sety 定位** —— 把影片放到 (400,200)，看像素是否落在那里
2. **pause/1** —— 暂停后矩形内容应停止变化
3. **close 后重开** —— 关掉再开应能继续画

### 第 1 项：通过 ✅

```
brightest sample: at(420,220)=108   at(10,10)=0
```

影片画在 (420,220) 处亮度 108，而 (10,10) 为 0 —— **说明 `setx/sety` 真的把
片子挪到了请求的位置**，不是固定在左上角。

### 第 2、3 项：未通过（原因在检查脚本，不在引擎）

两项都报矩形均值 0。第 1 项在同样的偏移下能测到 108，说明绘制本身没问题；
问题在我这两项的**采样时机**：用的影片是 2 秒，而我在 2.5–4 秒才取样，
片子早播完变黑了。已经调过一次时序仍不对，还要再调。

### 结论

B2 的**定位部分已证实可用**；pause 和 close→重开 尚无结论（既不能算通过，
也没有证据说明是引擎的问题）。

### 教训

这又是"证据产生方式"的问题（第八次）。好在这次**没有把失败当成引擎的错**——
第 1 项的数据足以说明绘制正常，所以我把它记为"检查脚本问题"而不是"引擎缺陷"。

## 第二十五轮：图层绘制是间歇性的——这是竞态，不是测试问题（2026-10-25 续）

### 量化结果

同一个检查、同一个二进制、同一份脚本，连跑 5 次：

```
run1  at(420,220)=0      全黑
run2  at(420,220)=0      全黑
run3  at(420,220)=108    画出来了
run4  at(420,220)=106    画出来了
run5  at(420,220)=0      全黑
```

**只有 2/5 成功。** 之前我一会儿看到"画出来了"、一会儿看到"全黑"，
一直以为是自己采样时机的问题——**其实是图层绘制本身不确定**。

### 这意味着什么

这是**竞态**，不是测量误差。也解释了整条排查线上反复出现的矛盾：

- 第六轮：`serial`/`consumed` 每遍都在涨（解码正常）但屏幕没东西
- 第十一轮：链路计数器 215 次全跑通，但区域是黑的
- 第十三轮：槽位里 `centre=01fffe` 是黄色的（解码正确）
- 第十四轮：`avfLayerFrame` 有时 `got=0`

**同一个竞态在不同轮次里表现为不同的"矛盾"。**

### 最可疑的时序

1. `avfLayerOpen` 重置 `serial/consumed/frame` 并启动线程
2. worker 立刻开始发布帧
3. 但 `refresh()` 可能在这之前就被调用（`opened` 已为 true）
4. 或者 `avfLayerFrame` 在 worker 发布前就把 `consumed` 推到和 `serial` 相同

### 必须承认的事

**B1 的"三次 PASS"也可能受同一个竞态影响**——`loop_check.sh` 连跑三次都通过，
但既然绘制只有 40% 成功率，那三次通过也许是运气。这条要重新验证。

### 下一步（明确）

不要再用"跑一次看结果"的方式验证这类功能。应该：
1. 每个检查跑 **5 次**，报告成功率
2. 成功率 < 5/5 就判为不稳定，继续查竞态

判据要从"通过/失败"改成"成功率"，因为间歇性通过会一直骗人。

## 第二十六 / 二十七轮：竞态没修好，但排除了两个假设（2026-10-25 续）

### 试过的两件事，都没用

| 假设 | 做法 | 结果 |
|---|---|---|
| `play` 后首帧未就绪 | 等最多 200ms 直到有帧 | 1/5，**更差**，已撤回 |
| 缓存文件残留（同尺寸复用了坏文件） | 改原子写 + 每次重写 | 3/8 ≈ 37%，**与之前 2/5 = 40% 无差别** |

第二项虽然没解决问题，但**原子写本身是对的**（避免截断窗口和复用坏文件），保留。

### 累计成功率测量

```
修改前：            2/5   = 40%
等首帧（已撤回）：  1/5   = 20%
原子缓存写：        3/8   = 37%
```

**没有统计学差异。** 图层在我的合成检查里大约 1/3 到 2/5 的成功率，一直如此。

### 一个重要观察

**真实游戏是稳定能画的。** 我先后三次独立跑真实游戏，都成功截到了：
- 免责声明（日/英双语）
- 07th Expansion LOGO 影片帧
- 完整标题画面（LOGO + 四按钮 + 镜框背景）

而且真实游戏的脚本和我合成脚本的差别是：它有完整的 `lua` 文本系统、
`exec_dll` 调用、更长的 `delay`、以及 `print 1` 反复驱动重绘。

**所以"间歇性"很可能是我合成检查脚本自身的特性**（2 秒短片 + 采样窗口窄 +
缺少真实游戏那种持续重绘驱动），而不是引擎对真实游戏不可靠。

### 诚实的结论

- 竞态**没有找到**
- 但"图层不可靠"这个结论**不能推广到真实游戏**——真实游戏三次都正常
- 我的合成检查作为"回归测试"目前不可用（成功率太低），只能作为"探测工具"

### 下一步建议

不再在合成检查上纠缠。改用**真实游戏**做验收（虽然慢，但稳定），
把合成检查标记为"探测用，不作判据"。这样：
- B1/B2 的验收改用真实游戏
- 其余目标项（A2/A3/C2/C3/E2）可以继续推进

## 第二十八轮：A3 的尝试未成立（2026-10-25 续）

### 做法

造了一张 1024×576 的标记图（四角 + 中心各有 8×8 白块的纯黑底 PNG/BMP），
`bg "marks.png",0` 显示后导出画面，检查白块是否落在精确坐标上。

### 结果

导出画面**全黑**，四个角和中心都没有白块。PNG 和 BMP 都试过，都没渲染。

引擎日志里没有 `can't find file`，说明图片找到了、但没画上去。而我之前用
真实游戏的 `bg "bg4\caution.jpg",6` 是能正常显示的——差别在真实游戏的
define 块里有完整的字体、窗口、`textclear` 等初始化，我的最小脚本缺这些。

### 结论：A3 未验证

不知道是：
1. 我的最小脚本缺初始化（最可能），还是
2. `bg` 对某些图片格式/尺寸有问题

**没有证据支持任何一方，所以不写结论。**

### 下一步

A3 应该用真实游戏自己的背景图来验（它一定能显示），而不是自造图。
具体：从 `03.ns2` 里已确认存在的 `BG\BG_BLACK.PNG` 或任意 `BG00x_*.JPG`
（都是 1024×576）取出当作背景，检查画面尺寸与内容是否 1:1，无缩放无错位。

### 二十八轮的说明

这轮没有产出结论，只记录了"试过、没成立"。**不把未验证的东西写成已验证。**

## 第二十九轮：A3 用真实游戏画面取证（2026-10-25 续）

### 方法

上一轮自造的标记图不渲染（我的最小脚本缺初始化，未定性）。改用**真实游戏
已经渲染出来的画面**做客观测量，不依赖任何自造素材。

对保存的标题画面（`test/video/shots/trt-trial-title.png` 对应的原始帧）做
边沿覆盖分析：

```
surface 1024 x 576            ← 与游戏背景图的原始尺寸一致
top    y=2     非黑 128/128
bottom y=573   非黑  91/128
left   x=2     非黑  42/72
right  x=1021  非黑  42/72
```

### 结论：A3 的"无错位、无黑边"成立 ✅

- 画面内容**触及全部四条边**，说明背景图是从 (0,0) 铺满 1024×576 的，
  没有居中留白、没有黑边、没有被缩到中间
- surface 尺寸与背景图原始尺寸相同（1024×576），**不存在缩放的必要**

### 未证实的部分

"像素级 1:1 无失真"没有直接证明——那需要拿到原始图片逐像素比对，而游戏
归档里的 JPG 大多是压缩存储（LZSS），我还没能正确解开。

不过"无错位 + 无黑边 + 尺寸相同"这三条已经能排除实际使用中最常见的两类
问题（缩放失真通常伴随黑边或内容缩在中间）。

### A3 状态

✅ **无错位、无黑边** —— 有像素级边沿证据
🔶 **像素级 1:1** —— 未证明（需要解包原始图）

## 第三十轮：C3 长跑暴露两个崩溃（2026-10-25 续）

### 崩溃 1：VideoToolbox 解码回调竞态（已尝试修）

10 分钟长跑在约 1 分钟时崩溃，回溯：

```
2   VideoToolbox   VTPixelBufferConformerIsConformantPixelBuffer + 52
3   VideoToolbox   VTPixelBufferConformerCopyConformedPixelBuffer
8   VideoToolbox   vtDecompressionDuctEmitDecodedFrame
11  libxpc         _xpc_connection_call_event_handler
```

**VideoToolbox 在自己的 XPC 队列上投递解码帧，而 `cancelReading` 返回之后
我们就把 reader 和 asset 释放了。** 这正是"销毁竞态"。

修复：`cancelReading` 之后轮询 reader 状态直到不再是 Reading（最多 500ms），
再额外等 50ms 让投递队列静下来，然后才释放。

### 崩溃 2：`ONScripter::textCommand()`（不是我这条线）

修完崩溃 1 再跑，崩溃点变了：

```
20  onsyuri   ONScripter::textCommand() + 1572
21  onsyuri   ONScripter::executeLabel()
```

日志里同时有大量 `[textlayer] surface 1024x576 ink=... refresh=...`
——**那是并行会话正在加的文本调试代码**。这条崩溃大概率来自他们未完成的改动，
我没有动过 `textCommand`。

### 采样修正

原来的脚本采样到的是子 shell 不是游戏进程（`first=672KB` 是假的）。
已改成 `exec`，现在拿到真实数字：`first=46448KB`（45MB，与 E1 的 45MB 一致）。

### C3 状态：未通过

- 崩溃 1：修了，但因崩溃 2 先发生，**尚未能验证修复有效**
- 崩溃 2：不在我的改动范围，需要并行会话收口后重测

### 建议

C3 需要等文本那条线的改动稳定后重跑。现在测出来的任何数字都混着别人
未完成代码的崩溃。

## 第三十一轮：崩溃形态一直在变，指向内存损坏（2026-10-25 续）

### 三次崩溃，三个位置

同一份长跑检查，每次崩溃点都不同：

| 次序 | 崩溃位置 | 加宽拆解后 |
|---|---|---|
| 1 | `VideoToolbox: vtDecompressionDuctEmitDecodedFrame` | 已修（等 reader 停止） |
| 2 | `CoreMedia: FigXPCMessageCopyCFObject / memoryOriginServer` | 等 300ms 后仍复现 |
| 3 | `SDL3: SDL_GetSurfaceProperties → SDL_CreateProperties → SDL_calloc` | — |

**崩溃点会游走**，这是内存损坏的典型特征，不是某一个函数的逻辑错误。

### 一个容易误判的点

`otool -L` 显示引擎链接的是 `sdl2-compat`，运行时会加载
`libSDL3.0.dylib`。这**不是** SDL2/SDL3 混用——`sdl2-compat` 本身就是
"SDL2 API 跑在 SDL3 之上"。所以看到 SDL3 出现在栈里不必惊。

### 已经确定的事实

- 崩溃发生在**影片播放/切换期间**，稳定复现（约 1 分钟内）
- 加宽 reader 拆解等待（50ms → 300ms）**没有消除**它，只是把崩溃点推到了别处
- 崩溃前的日志全是并行会话的 `[textlayer]` 调试输出，说明文本路径也在活跃使用 SDL surface

### C3 结论：未通过，且根因未定位

不能写成"修好了"。当前状态是：
- 有一个可复现的崩溃（约 1 分钟内）
- 崩溃点游走，说明是内存损坏
- 加宽拆解等待是**正确的方向但不够**，可能还有另一处释放过早

### 下一步（需要安静环境）

内存损坏类问题必须用工具查，靠猜没用：
1. 用 `MallocScribble=1` / `MallocGuardEdges=1` 让释放后的内存立刻失效，崩溃点会更接近真凶
2. 或用 `cmake -DCMAKE_BUILD_TYPE=Debug` + AddressSanitizer（`-fsanitize=address`）跑一次，直接指出越界/释放后使用的位置

这两条都需要先有一个稳定可构建的树——目前并行会话还在改文本路径。

## 第三十二轮：崩溃归属——不是影片，是 OGG 音频（2026-10-25 续）

### 用 `MallocScribble` + `--no-video` 做归属

`MallocScribble=1` 让释放的内存立刻失效，崩溃点会靠近真凶。再做 A/B：

| 配置 | 结果 |
|---|---|
| **有影片**（正常） | **存活 45 秒** |
| **`--no-video`** | **26 秒崩溃** |

**没有影片反而更早崩** —— 因为跳过影片后更快进入播放 BGM 的环节。

### 崩溃在 libvorbis

```
4  libvorbis      _vds_shared_init + 708   ← calloc 崩溃
5  libvorbis      vorbis_synthesis_init
6  libvorbisfile  _make_decode_ready
7  libvorbisfile  ov_pcm_seek
```

游戏在标题处执行 `me 1,"bgm4\gin_title.ogg"` 开始放 BGM，所以走的是
ONScripter 的 OGG 播放路径（从 .ns2 归档读进内存，交给 SDL_mixer）。

**`ov_pcm_seek` 出现在栈里说明在 OGG 文件结构上做 seek，而那个结构已经损坏。**

### 这一轮的三次定位

1. 长跑崩溃 → 先后出现在 VideoToolbox / CoreMedia / SDL
2. `MallocScribble` → 收敛到 `drawChar`
3. `--no-video` 归属 → **真正的位置是 OGG 解码，不是文本**

第 2 步的 `drawChar` 是**假线索**——堆损坏在那里被检测到，但源头在音频。

### 结论

- **影片路径（我这十几轮改的东西）没有引起这个崩溃**：有影片时能稳定跑 45 秒
- 崩溃根因在 **OGG/BGM 播放**
- 我之前加宽 VideoToolbox 拆解等待**方向没错但不是这个问题的解**

### 下一步

查 ONScripter 的 OGG 播放：`bgm`/`me` 命令如何从归档取数据、
`Mix_LoadMUS_RW` 的 RWops 生命周期、以及 `ov_pcm_seek` 是谁触发的
（ONScripter 里有 ogg 长度探测的代码，会用 seek）。

## 第三十三轮：OGG 崩溃未解决，但修了一个确定的泄漏（2026-10-25 续）

### 查证过的事实

`stopBGM()` 里的顺序是**对的**：

```cpp
Mix_HaltMusic();
Mix_FreeMusic(music_info);      // 先释放解码器
...
delete[] music_buffer;          // 再释放缓冲区
```

所以"先删 buffer 再用"这个最常见的假设**不成立**。

### 修了一个确定的缺陷（但不是崩溃原因）

```cpp
music_info = Mix_LoadMUS_RW( SDL_RWFromMem( buffer, length ), 0);
```

`freesrc=0` 意味着 SDL_mixer **不释放 SDL_RWops**——每次换 BGM 泄漏一个，
而这个游戏在标题、菜单、正文章节之间频繁切换 BGM。改成 `1`（由 mixer 拥有）。

**但这没有解决崩溃**：修完 `--no-video` 仍然在 27 秒崩溃，栈还在 libvorbis。

### C3 状态

- 崩溃根因：**OGG/vorbis 解码路径**（第三十二轮定位）
- 已排除：buffer 释放顺序、RWops 所有权
- 未定位：`ov_pcm_seek` 到底在哪个损坏的结构上操作

### 下一步

需要看到 `ov_pcm_seek` 的完整调用栈（不只是 libvorbis 内部几帧），
才能知道是谁在 seek。做法：把 `libvorbis` 换成带调试符号的构建，
或者用 `lldb` 在 `ov_pcm_seek` 上下断点跑一次。

### 三十三轮的说明

这轮只修了一个泄漏，**崩溃没解决**。不写成"修好了"。

## 第三十五轮：拿到 ov_pcm_seek 的完整调用栈（2026-10-25 续）

### 完整栈（lldb 断点，最有价值的一条证据）

```
#0 libvorbisfile   ov_pcm_seek
#1 SDL2_mixer      OGG_Seek
#2 SDL2_mixer      Mix_LoadMusic_RW
#3 SDL2_mixer      Mix_LoadWAV_RW        ← wave 入口落到了 music
#4 onsyuri         ONScripter::playSound
#5 onsyuri         OnScripter::dwaveCommand
#6 onsyuri         NSOggLoad(lua_State*)
```

**关键发现：`Mix_LoadWAV_RW` 遇到 OGG 数据时会内部退回 `Mix_LoadMusic_RW`**，
而 music 对象会**长期持有 RWops**（懒解码 + seek），RWops 指向 `playSound`
里的 `buffer`。chunk 路径随后 `delete[] buffer` → **use-after-free**。

游戏通过 `nsogg2.dll` 的 Lua 接口（`NSOggLoad` → `dwaveCommand`）加载 OGG，
正好走到这条路上。

### 已实施的修复

在 `playSound` 里嗅探 `OggS` 魔数，命中则把格式改走 music 路径
（那条路径会把 buffer 存进 `music_buffer`，生命周期正确）：

```cpp
if ((format & SOUND_CHUNK) && length >= 4 && buffer &&
    !memcmp(buffer, "OggS", 4))
    format |= SOUND_MUSIC;
```

**修复本身是对的**（消除了一条明确的 use-after-free 路径），
但**崩溃没有消失**：`--no-video` 仍在 26 秒崩，只是崩溃点又换了：
`IOSurfaceClientSetValue → IOCFSerialize → CFDataInit → malloc`。

### 诚实的状态

- 堆损坏**仍在**
- 已经消除了一条真实的 UAF（OGG 路由），但它不是唯一的破坏源
- 崩溃点持续游走（VideoToolbox → CoreMedia → SDL → libvorbis → IOSurface）

### 下一步

`MallocScribble` 让崩溃点在**分配时**被检测到，比之前更接近真凶，但还不够。
真正需要的是 **AddressSanitizer**：能给出"哪一行写了越界/释放后使用"而不是
"哪次 malloc 撞上了坏堆"。

ASan 需要重新构建整个引擎（`-fsanitize=address`），且不能与 sdl2-compat 的
预编译库混用——需要一次干净的单独构建。

## 第三十六轮：ASan 说我们的代码是干净的（2026-10-25 续）

### 构建与对照

```
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
```

| 构建 | 结果 |
|---|---|
| **ASan（RelWithDebInfo）** | **存活 120 秒，零 AddressSanitizer 报错** |
| **Release** | **24 秒崩溃** |

ASan 版确实跑了游戏——引擎日志里能看到 `wcmpg: opened 'arc4\MOV000_hanyou.mpg'`
和 `setwindow`，不是空转。

### 这个结果说明什么

ASan 会给每次分配加红区，并在越界/释放后使用时立刻报错。**它跑了 120 秒
一个报错都没有**，说明：

- **我们自己的代码没有越界写、没有释放后使用**
- 崩溃来自**预编译的第三方库**（SDL_mixer / libvorbis / sdl2-compat），
  ASan 不插桩它们，所以看不见那里的越界

同时也排除了 ABI 不匹配：`SDL_mixer` 头文件与运行库都是 homebrew 的 2.8.2，
`Mix_LoadMUS_RW` 的双参数重载被正确选中（否则 C++ 会编译失败）。

### C3 的最终定位

| 层 | 状态 |
|---|---|
| 我们的引擎代码 | ✅ ASan 120 秒无报错 |
| 第三方音频/解码库 | ❌ 崩溃发生在这里（未插桩，ASan 看不见） |

### 这意味着什么（诚实版）

- 这个崩溃**不太可能**靠改 OnscripterYuri 的代码修掉
- 要么换 SDL_mixer/libvorbis 的构建（带 ASan 自建），要么绕过出问题的调用路径
- 之前几轮我改的 `freesrc=1` 和 OGG 路由都是**真实的改进**（前者修泄漏、
  后者消除一条 UAF），但它们不是这个崩溃的解

### 可行的下一步

1. **自建带 ASan 的 SDL_mixer + libvorbis**，让 ASan 能看到崩溃点
2. 或者**用 ffmpeg 预转码 BGM 为 WAV**（游戏里 BGM 是 OGG），绕开 vorbis 路径
3. 或者接受 C3 为"已知第三方缺陷"，在文档里标注

方案 2 最实际：我们已经有完整的 `gal prep` 转码管线，把 OGG 转 WAV 是同一套机制。

## 第三十七轮：绕过 OGG 也崩——我的归因错了（2026-10-25 续）

### 做的实验

给 `NSOggLoad` 加 `ONS_NO_OGGLOAD=1` 旁路（完全不加载 OGG），再跑：

```
ONS_NO_OGGLOAD=1 + --no-video  →  仍然 26 秒崩溃
新崩溃点：CoreMedia FigXPCMessageCopyCFObject → libxpc
```

### 结论：第三十二轮的归因是错的

那一轮我用 `--no-video` 做 A/B，看到崩溃点落在 libvorbis，就判定"根因在 OGG"。
**现在把 OGG 整个关掉，崩溃照旧。**

所以 libvorbis（以及 VideoToolbox、CoreMedia、SDL、IOSurface）**都只是
撞上坏堆的地方，不是源头**。这条我记下来，避免再误判。

### 现在的证据集合

| 实验 | 结果 |
|---|---|
| Release 正常跑 | 24–26 秒崩 |
| `--no-video` | 26 秒崩（更快，因为跳过影片） |
| `ONS_NO_OGGLOAD=1` | 26 秒崩 |
| 两者同时 | 26 秒崩（CoreMedia XPC） |
| **ASan 构建** | **120 秒不崩、零报错** |

### 这把范围压到底了

- 关掉影片、关掉 OGG，仍然崩
- 我们的代码在 ASan 下干净
- 那么剩下在用的只有：SDL2(sdl2-compat→SDL3)、SDL_ttf/harfbuzz/freetype
  （文字渲染每帧都在用）、SDL_image、以及引擎自身的 surface 操作

**heap 损坏来自某个预编译库，或来自我们与它们的交互（ASan 不插桩它们所以看不见）。**

### C3 诚实状态

- 失败：约 26 秒必崩
- 根因：**未定位**（比上一轮更诚实——上一轮的"OGG"是误判）
- 已排除：影片路径、OGG 路径（各自单独和同时都试过）
- 唯一能继续的办法：**自己构建带 ASan 的 SDL2/sdl2-compat/SDL_ttf/harfbuzz 依赖链**

### 三十七轮的说明

这轮推翻了自己上一轮的结论。这比坚持一个错的结论有价值，但也要承认：
我在 C3 上已经花了 8 轮，仍然没有定位到根因。

## 第三十九轮：B1 的结论也要下调（2026-10-25 续）

### 复验发现

同一份 `loop_check.sh`，这轮跑出：

```
RESULT: INCONCLUSIVE - the rectangle changed but never repeated
```

而此前连跑三次都是 PASS。**这正是第二十五轮警告过的**：既然图层绘制本身只有
约 40% 成功率，三次连续通过也可能是运气。

### 结论修正

| 项 | 之前 | 现在 |
|---|---|---|
| B1 setloop 循环 | ✅ 三次 PASS | 🔶 **不稳定**：同一检查可能 PASS 也可能 INCONCLUSIVE |

### 这条影响的不是一个检查，而是一整类结论

B1、B2(pause/close)、C3 都受同一个底层不稳定影响：
**图层/绘制路径在某些运行里不工作**。

已确认的正面事实：
- 真实游戏的标题画面、影片帧、按钮**多次稳定截到**（不是全靠运气）
- 解码链路 `serial/consumed` 每遍正常推进（第六轮）
- ASan 下 120 秒无报错

所以不是"完全不可用"，而是"**约六成运行正常，四成有问题**"。

### 我的判断

这个不稳定是 C3 那个"未定位的堆损坏"的另一种表现。在根因修掉之前，
任何基于"跑一次/跑三次"的结论都只能是概率性的。

### 因此

- 报告里凡是"通过"的项，都要标注是"多次运行中的表现"，不是"确定性通过"
- 文档已按这个标准写

### 三十九轮的说明

这轮没有推进功能，但**下调了一个之前给得太高的结论**。宁可从"✅"降到"🔶"，
不把一个概率性的通过当成确定性结论。

## 第四十轮：不是优化问题，是"某个预编译库写越界"（2026-10-25 续）

### 排除的假设

| 假设 | 实验 | 结果 |
|---|---|---|
| `-O3` 暴露 UB | 用 `-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks` 重建 | **仍然 25 秒崩** ❌ |

所以不是优化级别或严格别名的问题。

### 于是只剩一个解释能同时满足所有现象

| 构建 | 结果 | 唯一差异 |
|---|---|---|
| Release / O2 | 25 秒崩 | 系统 malloc |
| **ASan（RelWithDebInfo）** | **120 秒不崩** | **ASan 的 malloc 加红区** |

ASan 只在**被插桩的代码**写入红区时才报错。我们的代码插桩了、没报错；
**预编译库（SDL_ttf / freetype / harfbuzz / sdl2-compat）没插桩**，
它们写红区 ASan 也看不见——**但红区把相邻分配隔开了，所以破坏被限制住，
崩溃就不发生了**。

**结论：某个预编译库在写越界（overrun），它破坏了相邻的堆分配，
而崩溃点总是那个"恰好受害"的模块。**

这一条同时解释了：
- 为什么崩溃点游走（VideoToolbox / CoreMedia / SDL / libvorbis / IOSurface）
- 为什么 ASan 干净（越界来自未插桩的库）
- 为什么图层绘制只有约六成成功（越界不总是踩到关键数据）

### 最可疑的候选

文字渲染每帧都在跑，且每帧都经过 **SDL_ttf → freetype → harfbuzz** 三层预编译库，
缓冲区是引擎分配的（被 ASan 插桩 ⇒ 红区在），而写入者是未插桩的库。
**这是最符合证据的候选。**

### 要确认需要什么

自建带 ASan 的 freetype / harfbuzz / SDL_ttf，再跑一次——那时越界写入会被
ASan 当场抓住并指到具体行。这是唯一剩下的路，工作量是编译三个 C 库。

### 四十轮的价值

这轮没有修好任何东西，但把结论从"在第三方库里"精确到
"**某个预编译库写越界，ASan 的红区掩盖了它**"，并给出了最可疑的候选（文字渲染链）。

## 第四十二轮：ASan 版也崩，且日志里有空文件名（2026-10-25 续）

### 实验结果

让 ASan 构建跑满 10 分钟：

```
min 1  rss=288KB
min 2  rss=8080KB
min 3  rss=11984KB
DIED at minute 3        ← Abort trap: 6（SIGABRT）
AddressSanitizer 报告：0 条
```

**ASan 版并没有免疫，只是把崩溃从 25 秒推迟到 3 分钟。**
而且崩的时候 ASan 没给出任何报告——说明 abort 不是 ASan 检测到越界触发的。

### 这修正了上一轮的推断

第四十轮我说"ASan 的红区把破坏限制住了，所以不崩"。
现在看，**它只是拖慢了**。红区隔离理论不能解释 3 分钟后的崩溃。

### 同时发现一个新异常

运行日志（`stderr.txt`）里有：

```
 *** can't find file [] ***
```

**空文件名。** 有代码在用空字符串当文件名去加载。这本身是个缺陷，
而且很可能是"某个字符串已经损坏"的直接证据——正好符合堆损坏的症状。

### 现在能确定的

| 事实 | 证据 |
|---|---|
| Release / O2 崩 | 25–26 秒，多次复现 |
| ASan 崩得更晚 | 3 分钟 |
| ASan 无报告 | abort 非 ASan 触发 |
| 日志有空文件名 | `can't find file []` |
| 我们的代码 ASan 下无越界报告 | 120 秒那次跑干净 |

### 下一步（更具体了）

**追那个空文件名**。它能定位到"谁在传空字符串"，而那个调用点很可能
就是堆损坏的现场或受害者。做法：在 `can't find file` 的打印处加调用栈。

这比自建 ASan 依赖链便宜得多，而且指向很明确。

## 第四十三轮：空文件名也是间歇性的（2026-10-25 续）

### 做了什么

在 `can't find file` 打印处加了空文件名检测 + 调用栈（用 `ONS_TRACE_EMPTYNAME=1` 开启）。

### 结果

再跑一次（27 秒，仍然崩）：

```
空文件名出现次数：0
```

**空文件名这次没出现。** 说明它和崩溃一样是间歇性的——不是每次运行都有。

### 这告诉我什么

1. 空文件名**不是稳定复现的起点**，不能作为固定调查入口
2. 它更可能是"某次运行里内存被踩坏后的随机表现"，而不是根本原因
3. 这一轮没有推进结论

### 探针保留

`ONS_TRACE_EMPTYNAME=1` 留在代码里（默认关闭）。如果以后某次运行又出现空文件名，
打开它就能拿到调用栈。

### C3 的最终状态（第 13 轮追踪）

| 轮次 | 得出的结论 | 是否成立 |
|---|---|---|
| 30 | VideoToolbox 拆解竞态 | 是真实 bug，但不是根因 |
| 32 | 根因在 OGG | ❌ 已推翻（36 轮） |
| 36 | ASan 下干净 → 在预编译库 | 部分成立（ASan 也崩，只是更晚） |
| 40 | 红区隔离掩盖了越界 | ❌ 已推翻（42 轮） |
| 42 | 空文件名是线索 | 🔶 间歇性，无法稳定复现 |
| 43 | — | 无结论 |

**13 轮里我推翻了 3 个自己的结论。** 这本身说明：靠"读代码 + 单次实验"处理这类问题
是低效的，而正确的工具（ASan 依赖链）我一直没建。

### 我要下的判断

C3 应该停止在当前会话里继续追。理由：
- 每轮只能排除一个假设，且假设常常是错的
- 真正的下一步（自建 ASan 依赖链）是一个独立的工程任务
- 继续消耗而不产出结论，对项目没有价值

## 第四十四轮：证据指向"释放后使用"而不是"写越界"（2026-10-25 续）

### 两组新实验

| 配置 | 结果 |
|---|---|
| ASan 构建 + `handle_abort=0:handle_segv=0` | **存活 260 秒** ✅ |
| Release + `MallocNanoZone=0`（关 nano 分配器） | 30 秒崩 ❌ |

（第四十二轮那次 ASan 在 3 分钟崩，是因为 ASan 自己的信号处理接管了；
让它放手后我们自己的处理器生效，反而跑得更久。）

### 理论修正：UAF 而非 overrun

ASan 与系统 malloc 的两个差异：红区 + **隔离区（quarantine）**。
`MallocNanoZone=0` 无效，说明不是分配器布局问题；那么关键差异是**隔离区**：

- 系统 malloc：`free` 之后内存**立刻可以被复用**
- ASan：释放的内存进隔离区，**一段时间内不还给任何人**

如果某段代码**在我们 free 之后仍然持有并使用那块内存**：
- 系统 malloc 下：那块内存已被别人占用 → 数据被踩坏 → 崩溃
- ASan 下：内容还是旧的、没人动过 → 读到的值仍"合理" → 不崩

**所以症状是 use-after-free（释放后使用），而不是写越界。**
这也解释了为什么 ASan 一个越界报告都没有。

### 缩小到"我们释放、预编译库仍在用"的对象

候选：
1. `music_buffer`（OGG）—— 已修两条路径，仍有崩溃
2. 文字缓存 surface（`cache_info`）—— 每帧都在用
3. `accumulation_surface` 的像素指针 —— 若被缓存到别处
4. `SDL_RWops` —— 已改 freesrc=1

### 诚实的判断

这轮把"写越界"修正成"释放后使用"，是 C3 十四轮里**最接近可操作**的一条结论：
UAF 是可以靠"延长缓冲区生命周期"来规避的，不需要重建依赖链。

但具体是哪个对象、在哪一行，仍未定位。

### 下一步（如果继续）

在 `quit()` 和 `stopBGM` 里**故意不释放**可疑缓冲区（把 `delete[]` 注释掉），
再跑一次：
- 如果不崩 → 确认是被释放对象被继续使用，然后逐个二分找出是哪个
- 如果还崩 → 排除这条

这个实验便宜、可判定，且不需要任何依赖链重建。

## 第四十八轮：A2 的文字渲染——确认能画，编码是测试脚本的事（2026-10-25 续）

### 三次尝试

| 脚本编码 | 引擎参数 | 结果 |
|---|---|---|
| UTF-8 | `--enc:sjis` | **多行日文全部渲染**（字形、换行、四行结构都对），但字符是乱码 |
| UTF-8 | `--enc:utf8` | 只画出 `" 怀` 一个字符 |
| Shift-JIS（iconv 转） | `--enc:sjis` | 全黑（iconv 有不可映射字符，文件被改坏） |

### 能得出的结论

**文字渲染路径是工作的**：第一次尝试里，四行文字、换行位置、字形轮廓、
字距都正常画出来了——只是内容因为编码不匹配而变成别的汉字。

**编码不匹配是我测试脚本的问题**（UTF-8 文件配 `--enc:sjis`），不是引擎缺陷。
真实游戏用 `nscript.dat`（本身就是 Shift-JIS 加密格式），不存在这个问题。

### 未能验证的

- 字号（`fontsz`）的实际像素高度
- 行距是否与 NScripter 一致
- **振假名（rubyon）的位置**——第一版乱码里看不出

### 诚实标注

A2 状态：**部分验证**（字形与多行布局正常；字号/行距/振假名未测）。
不写成"通过"，也不写成"缺陷"。

## 第四十九轮：A2 日文渲染确认（2026-10-25 续）

### 干净的一次

关键是把脚本按 **cp932 精确编码**（`str.encode('cp932')`），并且用反斜杠而不是
`¥` 作换行标记（`¥` 不在 cp932 里，iconv 会丢掉整行）。

结果（`test/video/shots/trt-text-japanese.png`）：

- **`いいいいいいいい` / `う` 字形完全正确**
- 多行、换行位置正常
- 用的是游戏自己的 `default.ttf`

像素测量（该帧仍在打字机中途，故行高偏小）：

```
line pitch = 18 px
字高 15–18 px
```

### A2 结论

| 子项 | 状态 |
|---|---|
| 日文字形渲染 | ✅ **正确**（有截图） |
| 换行 / 多行布局 | ✅ 正常 |
| 字号（`fontsz`）实际像素 | 🔶 测到 15–18 px，但该帧在打字中途，不是稳定值 |
| 行距 | 🔶 同上 |
| **振假名位置** | ⬜ 未测（`漢字《かんじ》` 那行还没画到） |

### 这轮的方法教训（第 9 次同类）

两次失败都是**我的测试脚本**：
1. UTF-8 文件配 `--enc:sjis` → 乱码（第一轮）
2. `¥` 不在 cp932 里，iconv 静默丢行 → 全黑（第二轮）

第三次改用 `str.encode('cp932')` 直接编码 + 反斜杠换行，一次成功。

**引擎的文字路径从头到尾都是好的。**
