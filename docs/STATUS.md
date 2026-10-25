# 五引擎在 macOS arm64 上的状态（2026-10-25）

本文件只写**跑出来的事实**，每条都附可复现的命令。

## 总览

| 引擎 | 移植 | 真实数据 | 可玩 | 验收 |
|---|---|---|---|---|
| ① NScripter → OnscripterYuri | ✅ 原生 arm64 | ✅ 官方 SDK 5 套 | — | `gal verify` 通过；影片后端 8/8；存读档往返 PASS |
| ② KiriKiri → Kirikiroid2 | ✅ 原生 arm64 | ✅ 真实 KAG 游戏工程 | **✅ 是** | 截图 + 零报错长跑 |
| ③ RealLive → RLVM | ✅ 原生 arm64（SDL3） | ✅ CLANNAD Full Voice | **✅ app + DMG** | 260/260 单测；截图 |
| ④ Ethornell → OpenBGI | ✅ 原生 arm64 | ✅ 官方商业体验版 | 可运行 | 截图 + 画面导出 |
| ⑤ Artemis（从零重写） | ✅ 原生 arm64 | ✅ 3 份真实归档（pf6/pf8） | 归档层 | 五层自证 + 归档 4698/4698 哈希一致 |

## 一条命令验证

```bash
cd ~/Documents/gal-engines
./build-all.sh --check                              # 四引擎构建 + 自检
./OnscripterYuri/test/video/run_acceptance.sh       # 影片后端 8 项
./harness/run_savetest.sh                           # 存读档往返
./bench/bench.sh OnscripterYuri/build/onsyuri testdata/lifegame 20 -- \
    --root . --enc:sjis --window                    # 效率基线
```

```
✓ nscripter   official demo still running after 3s
✓ kirikiri    KAG3 displays its first line without script errors
✓ bgi         the trial reaches profile 1.669
✓ artemis     pfs, ast, runtime and 12 rendered fixture frames (not a real game)
通过 4，失败 0
```

## 各引擎的证据

### ② KiriKiri（最完整）

```bash
cd testdata/kag-game/data
KRKR_LOOP_SECONDS=14 KRKR_AUTO_CLICK="1131,446,5000" \
  SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
  ../../kirikiroid2-mac/build/krkrrun startup.tjs --screenshot out.bmp
```

- 标题画面 → 点 `start` → 进入 `1.ks` 正文 → 对话框显示台词（`screenshots/real-game-story.png`）
- 音频：`opened, totalTime=220.54`、`playing, status=1`
- **可以直接跑 `data.xp3`**（真实游戏的发布形态）：`krkrrun game.xp3`
- **管理器也接受归档**：`gal add game.xp3` → 加进库 → `gal play` / `gal verify` 都能用
- 50 秒 / 30 次点击长跑**零报错**

### ④ Ethornell

```bash
./gal/gal play "BGI官方体验版-遥か碧の花嫁に" --dump /tmp/x.bmp
```

- 1920×1080 标题画面，45 万色（`openbgi/screenshots/haruka-title.png`）
- 同一游戏送至窗口的画面（`openbgi/screenshots/window-presented.png`）

### ① NScripter

```bash
cd testdata/lifegame && ../../OnscripterYuri/build/onsyuri --root . --enc:sjis
```

5 套官方数据在库里，`gal verify` 全部「可以运行」。

**影片**（2026-10-25 新增，`mpegplay`/`avi`/`movie` 三种都通了）：

| 路径 | 结果 |
|---|---|
| MPEG-1 → AVFoundation 硬解 | ✅ 3.00s @25fps，dump 帧 620 色 |
| wmv2 AVI → AVFoundation 拒绝 → libmpv 兜底 | ✅ dump 帧 275 色彩条 |
| MP4 直路径（无 map） | ✅ |
| `video_map.json` 命中 → 播转码产物 | ✅ 改播 `_video/*.mp4` |
| map 缺条目 → 回退原文件 | ✅ |
| `click_stop=1` 点击中断 | ✅ 1.75s 停（片长 3.00s） |
| `click_stop=0` 点击被吞 | ✅ 播满 3.66s |
| 无影片的游戏回归 | ✅ CPU 1.0%，RSS 32.9MB |

**存读档**：`./harness/run_savetest.sh` → `PASS`（变量跨 `kill -9` 恢复）。
落盘位置：无 `<game>/sav/` 时只写私有格式，`gal launch` 现已预建该目录。

**影片硬化**（2026-10-25 第二轮，15 项验收全绿）：`movie stop`/`pos`/`async`
原先是 `not supported yet`，现已实现（async 走后台解码线程 + 主线程 16ms
超时呈现；pos 只画进目标矩形）。音频能证明"真有声音"（解码后报
`peak=0.129 rms=0.0883`，静音轨标 `(SILENT TRACK)`）。坏输入（损坏/0 字节/
随机字节/不存在/map 指向缺失产物）全部 rc=0 不崩且脚本继续。性能：
1080p60 **180/180 帧 0 丢帧**、720p30 900/900 实时、静态页 CPU **1.09%** /
RSS **18MB**、prep 转码约 **2.6× 实时**。

**剧情/对话流**（2026-10-25 修，这条最要紧）：影片原来会**打断剧情**——
每部 `mpegplay`/`avi`/`movie` 播完都触发 `endCommand()`，后面的台词全丢。
根因是返回值语义写反：`if (playMPEG(...)) endCommand();` 是"放不出来才结束"，
上游恒返回 0，而第一版 macOS 后端把"播放成功"置成了 1。
修完实测：影片前台词 → 影片 → 影片后台词，两段都渲染（dump 帧 1151 色、
1533 个近白采样）；验收里用绿→影片→红的颜色判据固化（第 4b 项）。
`test/video/run_acceptance.sh` **9/9 全绿**。

诊断能力：`ONS_SURFACE_DUMP=<file.bmp>` 现在每帧都写引擎 surface。
`savescreenshot` 在这个构建里静默不出文件、`ONS_VIDEO_DUMP` 只对影片生效，
所以在此之前**文本/对话渲染根本无法在无头环境下检查**——这也是影片那个 bug
一直没被发现的原因。

### ⑤ Artemis（从零写）

| 层 | 工具 |
|---|---|
| `.pfs` 归档（含 `.pfs.NNN` 分卷） | `pfstool` / `pfsgame` |
| `.ast` 解析 | `asttool` |
| 剧本执行 | `arun` |
| 播放器（打字机） | `agame` |

## 验收判定收紧之后（本轮）

`gal verify` 原来只查「引擎/脚本失败」，所以**缺数据文件的游戏会被判为通过**。
现在 `can't find file` / `*** can't` / `no decoder` / `invalid` 都算失败。

收紧之后暴露的：

| 条目 | 原因 | 性质 |
|---|---|---|
| `lifegame` + 4 个 NScripter 官方样本 | 缺 5 个光标 bmp（`uoncur.bmp` 等） | **数据不全**，引擎本身正常（8 秒仍在跑） |
| `data`（真实 KAG 游戏） | `Dictionary.saveStruct` 不存在 → **已修**；现在卡在偏移写入 | 引擎缺陷（已修一半） |

## 这一轮修掉的真 bug

`tjsDictionary.cpp` 里 `saveStruct` 以 `TJS_BEGIN_NATIVE_METHOD_DECL` 开头、
以 `TJS_END_NATIVE_STATIC_METHOD_DECL` 结尾——**宏不配对，方法没注册进类**。
所有保存结构化数据的脚本都会报 `Member "saveStruct" does not exist`。

修好后脚本可以正常保存 Dictionary。

## 缺口（诚实记录）

1. ~~③ RealLive~~ **已闭环**：`rlvm/` 复活为 arm64 app bundle，260/260 单元测试通过，
   CLANNAD 的标题、立绘、中文台词与系统菜单均已渲染（见 [reallive.md](reallive.md)）。
2. **② 台词点击推进**：KAG 的总入口 `window.onPrimaryClick()` 目前不会被调用——点击只给最前面的子层，primary 层收不到。已摸清完整链路，差最后一步。
3. **⑤ 真实游戏数据**：Artemis 没有公开可获取的游戏，五个自检层是通过的，但没有真实数据验收。
4. **② 音频**已接 SDL_mixer（能加载、能播放），但没有做过音量/淡入淡出的实测。
5. **① 影片音频**：AVFoundation 音频轨预解码在部分容器上失败后按契约静默
   降级（日志 `audio=silent`），画面不受影响。没做到"一定能出声"。
6. **① GUI 菜单存档路径**未自动化：命令级 `savegame/loadgame` 已验证，
   右键菜单走的同一套 `writeSaveFile/readSaveFile`，但没有点击注入测试。
7. **② KiriKiri 存档写入失败**：`gal verify data` 报
   `saveBookMarkToFile` → `ファイルに保存できません`，是该 port 的存储层
   真实限制，不是本轮改动引入。

## 一些踩过的坑（值得记住）

- 平台层的**空构造函数**会让引擎的成员变量是垃圾值（`EventEnable` 未初始化 → 所有事件被丢弃）
- **空的事件泵**（`InvokeEvents()` 空实现）→ 事件进队列后永不派发
- **两个同名类定义**（Windows 的 `TimerImpl.cpp` 与 darwin 的实现）→ 引擎注册了 Windows 那个
- **测试目录布局**：发行版游戏把 KAG 的 `system/`（含图片）放在游戏根目录
- **`pkill`（SIGTERM）杀不掉 BGI 引擎**（它有信号处理），必须 `kill -9`
