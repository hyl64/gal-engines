# 五个引擎的状态（2026-10-25）

本文件只写**跑出来的事实**。每条结论后面要么是命令，要么是对应的验收记录；
没有跑通的不写「支持」，跑了一半的写明「跑到哪」。

```bash
./build-all.sh --check --data <数据目录>     # 通过 11，失败 0
```

## 总览

| 引擎 | 移植形态 | 真实作品 | 可玩程度 | 验收证据 |
|---|---|---|---|---|
| ① NScripter → OnscripterYuri | arm64 原生，CMake | 官方 SDK 5 套 + 《トライアンソロジー》官方体验版 | 标题→Start→OP 影片→剧情，日文正文正常 | 85 秒连续运行零崩溃；存档写盘；存读档跨进程往返 |
| ② KiriKiri → Kirikiroid2 | arm64 原生，CMake | 真实 KAG 工程 + 官方 KAG3 demo | 标题→点击→整章正文，目录与 `data.xp3` 两种形态 | story/demo/save/video/audio/perf 六项全绿 |
| ③ RealLive → RLVM | arm64 原生，scons | CLANNAD Full Voice（231 剧本，4 GB） | 标题、立绘、文字、中文台词、系统菜单、BGM | 260/260 单元测试；app bundle + DMG |
| ④ Ethornell → OpenBGI | arm64 原生，自带 Makefile | 《遥か碧の花嫁に》官方体验版 | 引导→标题画面完整渲染 | 1920×1080 标题帧（45 万色）；列出 profile 1.669 |
| ⑤ Artemis（从零重写） | arm64 原生，约 1.5 万行 | 三套真实归档（pf6/pf8，含 2,884 句台词） | 标题→Start→正文 60 FPS；存档可续跑 | 11 个工具 9 项自检；独立验收 30 项 0 失败 |

**五个引擎都在同一台 M1 上跑真实数据，没有 Wine、没有 Rosetta、没有虚拟机。**

## 构建与自检

```bash
brew install cmake scons sdl2 sdl2_image sdl2_mixer sdl2_ttf sdl3 sdl12-compat \
             lua jpeg-turbo libpng lz4 ffmpeg bzip2 boost gettext libogg \
             libvorbis libsndfile libarchive opencv@4

./build-all.sh                          # 五个引擎构建 + 全部自检
./build-all.sh --check                  # 只跑自检
./build-all.sh --data ~/gal-data        # 数据放在别处
./build-all.sh --skip kirikiri-perf     # 临时跳过某一项
```

没有游戏数据时，需要数据的检查报「跳过」而不是失败——**一个全新 clone 必须能构建，
并且诚实地说出哪一项没法验证**。CI（GitHub Actions，macos-14 arm64）跑的就是这条链。

## 各引擎的能力与证据

### ① NScripter — 五套官方数据 + 一款官方体验版

- **官方 SDK 5 套**全部进入主循环；`gal verify` 全绿。
- **《トライアンソロジー ～三面鏡の国のアリス～》官方体验版**：启动 → 标题 → Start →
  OP 影片 → 剧情场景，日文正文正常排版
  （截图见 [test/video/shots](../OnscripterYuri/test/video/shots/)）。
  本仓库的自动验收只能做到「启动并存活」：无头环境没有点击注入，游戏停在空文本层
  （`[textlayer] ink=0`），实测连续 **110 秒不崩**。
- **✅ C3 稳定性缺陷已修复**（原：Release 构建下约 25 秒必崩）。根因是
  `Page::add` 的 lookback 缓冲在 `max_text == 0` 时永远长不出来——Lua 驱动的文本路径
  传 0×0 字号，`clearCurrentPage()` 分配零长缓冲，之后每个字符都往零大小分配外写
  1 字节，堆被踩坏后崩溃点随机游走（malloc freelist / VideoToolbox / libxpc /
  libvorbis / IOSurface）。**Guard Malloc 抓到写入者**（drawChar 内联的 add），
  修复后三连跑全部越过原崩溃点 1.7 倍时长，并推进到 15 分钟深处的剧情；
  随后在 **~15 分钟、44 部影片之后**退出，且三次运行**一致地**停在同一个脚本错误
  （`NSPopComma` 无逗号，honpen_inaka1:4592）——盲点击踩进分支的脚本级兼容问题，
  可复现、非内存崩溃。原 [C3-HANDOFF.md](../OnscripterYuri/C3-HANDOFF.md)
  保留作历史记录。
- **存档**：`savedata/gloval.sav`、`kidoku.dat`、`NScrllog.dat` 均落盘；
  [harness/run_savetest.sh](../harness/run_savetest.sh) 证明**跨进程往返**——
  前一个进程被 `kill -9`，新进程仍读回变量。
- **影片**：AVFoundation 主路 + libmpv 兜底（dlopen，无编译期依赖）；
  `wcmpg.dll` 协议（`open/setloop/setx/sety/play/pause/close`）从引擎的 BaseReader 读档案，
  所以 `arc\`、`arc4\` 里的影片能取出来。[证据](../OnscripterYuri/mpvdev/EVIDENCE.md)
- **踩过的坑**：数据本身残缺（zip 只解出一半，`00.ns2` 记录 223 MB、实际 177 MB）——
  按 HTTP Range 只取中央目录、再按 member 偏移重新下载才修好；
  引擎侧修了 `setwindow3` 字号、字距不得小于字号、`tateyoko`、文字原点缩放、
  `savedir` 自动建目录、AVFoundation 双 reader 的 SIGBUS。
- **正文进对话框（原生 Lua 路径）**：修完内存/事件缺陷后，游戏自己的文本流程
  （`NSCALL_text → mode_normal → _setwindow3 → _print`）已可用——
  对话框、气泡纵排台词、立绘场景全部正确（11/16 分钟帧：
  `test/video/evidence/luapath-*.png`）。`ONS_NO_LUA_TEXT` 不再是本作必需，
  保留为其它作品的后备开关。
- **未完**：`setloop` 的画面上屏；编号存档槽未测；振假名（`ruby`）未验成。

### ② KiriKiri — 一款真实 KAG 作品，整章可玩

- **真实 KAG 工程**：标题 → 点击 → 进入正文 → 多句台词，**目录与打包成 `data.xp3` 两种形态都过**。
- **官方 KiriKiri SDL2 / KAG3 demo**：不同分辨率、JPEG + TLG 素材、交叉淡入、
  TJS 雪花插件跑在 Timer 上、超链接菜单。
- **存档**：书签是「缩略图 + 偏移写入的结构化数据」，写入与读回均验证。
- **影片**：`VideoOverlay` 真的解码上屏（ffmpeg），音轨走同一条时间线。
- **音频**：暂停、`[ws]` 等播放结束、SE 与 BGM 同时、每缓冲音量、淡入淡出。
- **帧率**：两个真实工程都保持可玩帧率。
- **踩过的坑**（都是引擎自身的缺陷）：`EventEnable` 未初始化让**所有事件被丢弃**；
  `InvokeEvents()` 空实现；`TVPPostEvent` 自己泵队列导致重入；图层销毁时鼠标捕获悬空；
  日文错误信息用 `%ls` 在 C locale 下**什么都不打印**；纹理路径把每种图片格式都判成 PVR；
  `Dictionary.saveStruct` 宏不配对从未注册；`patch.xp3` 不覆盖基础归档。

### ③ RealLive — CLANNAD 跑起来了

- **260/260 单元测试**通过。
- 真实数据 CLANNAD Full Voice（中文化，231 剧本，4 GB）：**231 个剧本全部解析**，
  标题画面、立绘、文字、中文台词、系统菜单全部渲染，BGM 可播。
- 空闲 0.4% CPU / 8.7 MB RSS；`rlvm.app` 自包含，另有 DMG。
- 技术路线：SDL 1.2 API 由 **sdl12-compat** 提供，底层 SDL3，渲染走 GL-on-Metal。
- 细节见 [reallive.md](reallive.md)。

### ④ Ethornell / BGI — 商业体验版的标题画面

- 列出引擎 profile `1.669`，加载 `data*.arc`（2.1 GB，23 个归档）。
- **1920×1080 标题画面完整渲染**（45 万色、日文正确），另有一帧送进窗口的证据。
- 自带 darwin 后端：CPUID 仿真、SDL2 窗口、字体、音频、存档、UI、影片。
- 踩过的坑：游戏表里缺**产品名**会让引导程序主动退出；`TVPInternalLoadTexture`
  在 Windows 上是通用上传步骤，被当成「PVR 解码器」桩掉后**所有图片都读不出来**。

### ⑤ Artemis — 从零写出来的实现

- **11 个工具**：`pfstool asttool asbtool iettool fxtool audiotool savetool movietool agame arun pfsgame`，
  其中 9 个有自检。
- **三套真实归档**（作者/发行方免费发布）：pf6 125 条、pf8 2891 条、trial 604 MB；
  **4698/4698 个文件的哈希**与归档索引独立算出的值一致。
- **可玩**：标题 → Start → 正文，60 FPS（帧间隔 p50 = p95 = 17.00 ms），
  真声卡出声 underrun 0；存档 → **换一个新解释器** → 读档续跑，逐页相同。
- **格式覆盖**：第二部作品 21 个 `.ast` 解析出 **2,884 句台词**、3 个 `.asb`、4 个 `.ipt`；
  `colortone`×141、`extrans`×34、`excall`×56 全部实现。
- **鲁棒性**：五万条变异输入，5 个发现全部修掉（4 个同一类错误：**解析器相信了文件写的数字**），
  含「条目名 `..\..\x` 会写到目录外」。
- **性能**：打开 202 MB 归档 0.40 ms、常驻 +540 KiB、取 2 MB CG 0.12 ms；
  逐字节 XOR 改分组后解档 792 → 5,405 MiB/s（先证等价：368,734 例 0 字节差异）。
- **未完**：第二部作品的画面要 Lua 宿主 API；`.psb` 在三套语料里数量为 0，只做了识别；
  `movie` 的解码器就绪但语料里没有视频成员；`scetween` 的槽位到图层未接。
  见 [artemis 文档](../artemis/docs/) 与 [数据说明](../artemis/DATA_SOURCES.md)。

## 统一管理器（gal）

```bash
./gal/gal engines                  # 五个引擎与其二进制状态
./gal/gal add <目录|data.xp3>      # 建库，按文件特征识别引擎
./gal/gal play <名字>              # 启动
./gal/gal verify <名字>            # 跑一遍并按行为判定（有 warn/error 白名单）
./gal/gal play <名字> --dump x.bmp # 无头导出引擎画出的画面
./gal/gal prep <名字>              # 把游戏里的影片转码到 _video/
./gal/gal savetest <名字>          # 存读档往返
./gal/gal doctor                   # 环境自检
```

管理器按脚本自身位置定位引擎目录（`GAL_ENGINES_ROOT` 可覆盖），换台机器、换个路径都能用。

## 数据从哪里来

**本仓库不含任何游戏数据。** 全部是官方免费 SDK、官方体验版、作者免费发布的作品；
获取方式、哈希与许可姿态见 [DATA.md](DATA.md)。

## 版本与可复现

[engines.lock](../engines.lock) 记录每份引擎副本来自哪个提交，
[scripts/sync-engines.sh](../scripts/sync-engines.sh) 按锁同步；
本地修补放在 `patches/`，同步后自动重放，不会被下一次同步悄悄覆盖。

## 已知缺口（一条没藏）

1. **NScripter**：~~C3 崩溃~~ 已修复；~~NSPopComma 分歧~~ **也已修复**——
   第二个根因是 `NSPopInt` 用 `lua_pushnumber` 弹参数，Lua 5.3+ 把整数 1 渲染成
   字符串 "1.0"，游戏 Lua 用它拼接嵌套命令 `cutin_associate 1.0,""`，引擎 tokenizer
   对 `1.0` 的 end-status 推进与 `1` 不同，内层 handler 的 `NSPopComma` 失败退出。
   改为 `lua_pushinteger` 后，盲点击连续运行 **41:21 零错误、37 部影片**
（原 15 分钟即死；30 分钟验收线以 30:49 首次越过，随后主动结束）。
   ~~剩余：正文画进对话框~~ **已通**（原生 Lua 路径，见上）。
   ~~剩余：编号存档槽~~ **已打通**：时序点击（`ONS_AUTOCLICK_SEQ`）走完
   菜单 → セーブモード → 槽位 1 → 写盘（`savedata/save1.dat` 51596 B +
   `thumb1.bmp` 缩略图 + `gloval.sav`）；重新启动 → Load Mode → 确认
   「このデータをロードしますか？」→ **恢复到存档时刻的场景并继续播放**
   （证据：`test/video/evidence/load-restored-scene.png` 等 4 帧）。
   根因之一是 `savedir "savedata"` 的目录从未被创建——`setSaveDir` 现在会建。
   ~~剩余：振假名~~ **已验证**：`rubyon` 命令 + `(本文/ルビ)` 语法 →
   假名以半尺寸画在正文上方（探针剧本 `testdata/ruby-probe/00.txt`，
   证据 `OnscripterYuri/test/video/evidence/ruby-above-text.png`）。
   之前的失败是缺 `rubyon`（rubyon_flag 默认关闭，假名整段跳过或内联）。
   剩余：`setloop` 上屏。
2. **KiriKiri**：自动播放（每句需点击）；依赖 Lua 的脚本未覆盖。
3. **RealLive**：`--no-bundle` 在旧版 `install_name_tool` 上是必需的（那是打包步骤）。
4. **Artemis**：第二部作品的画面（Lua 宿主）、`.psb`、`movie` 语料、`scetween` 槽位。
5. **通用**：GUI 菜单点击路径未自动验证（命令级路径与它共用同一套读写函数）。
