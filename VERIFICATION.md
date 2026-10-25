# 验收记录

生成时间：2026-10-25 05:09　机器：Apple M1 / macOS 27.0.1

```
$ ./build-all.sh --check
构建
  (跳过，--check)
自检
  ✓ nscripter      official demo still running after 3s
  ✓ kirikiri       KAG3 loads its scenario
  ✓ bgi            the trial's profile is recognised
  ✓ artemis        pfs, ast and the runtime self tests

通过 4，失败 0

$ ./gal/gal verify <每个真实数据>
  lifegame                         结论: 可以运行
  kag3-sample                      结论: 可以运行
  BGI官方体验版-遥か碧の花嫁に   结论: 可以运行

$ ./gal/gal engines
引擎         状态       二进制
nscripter  可运行      ~/OnscripterYuri/build/onsyuri
           NScripter → OnscripterYuri
           the script is usually Shift-JIS; --enc:gbk is for the Chinese ports
kirikiri   可运行      ~/kirikiroid2-mac/build/krkrrun
           KiriKiri → Kirikiroid2
           TJS2, XP3, the software renderer, text drawing, the window and the layer tree all work, and KAG3 runs its own scenario: first.ks reaches the screen (krkrrun --screenshot shows it)
reallive   未构建      ~/rlvm/build/rlvm
           RealLive → RLVM
           olde C++ that needs its own port; the other session owns it
bgi        可运行      ~/openbgi/openbgi-src/bin/bgi
           Ethornell/BGI → OpenBGI
           the engine takes the game folder as its working directory; the title screen and the window both render (BGI_DUMP_STAGE / BGI_DUMP_RENDERED show what it drew)
artemis    部分       ~/artemis/build/agame
           Artemis (from scratch)
           written from scratch: .pfs, .ast, the scenario runtime and a window that plays a script; give agame a .ast to see it run
```

## 截图证据

- `openbgi/screenshots/haruka-title.png` — 真实商业游戏的标题画面（1920×1080）
- `openbgi/screenshots/window-presented.png` — 同一游戏送到窗口的画面
- `kirikiroid2-mac/screenshots/kag3-first-line.png` — KAG3 剧本「こんにちは。」上屏
- `kirikiroid2-mac/screenshots/minikag-scenario.png` — KAG 形状剧本三行台词
- `artemis/screenshots/first-screen.png` — Artemis 从零实现的第一屏
- `artemis/screenshots/typewriter.png` — 打字机效果三阶段

---

# 2026-10-25 下午：KiriKiri 长剧情验收翻案

同日晚些时候定位并修复了四个引擎缺陷，早上「长剧情不能验收」的结论被推翻：

1. **事件被同步派发**：`TVPPostEvent` 末尾遗留了一次强制 `TVPInvokeEvents()`，
   conductor 的 AsyncTrigger 因此在按下按钮的 mouseUp 处理器**内部**同步跑完——
   `[cm]` 同步失效按钮，`ButtonLayer.onMouseUp` 的下一句 `Butt_mouseDown = false`
   写在死对象上，即 "The object is already invalidated"。派发权归还宿主主循环
   （与上游一致），台词随即逐句推进。
2. **失效图层残留鼠标捕获**：`tTJSNI_BaseLayer::Invalidate` 先释放 Manager 再
   `Part()`，`NotifyPart`（内含 `ReleaseCaptureFromTree`）被跳过；改为先 Part
   再释放，失效图层的捕获不再悬垂。
3. **`TVPTerminateSync` 是空实现**：脚本异常后引擎不退出不恢复，验收只能靠超时
   强杀且报错被吞；现在真正 `_exit(code)`，`System.exit` 一并修复。
4. **宽字符输出丢字**：`%ls` 打印引擎宽串在 C locale 下对日文静默输出空串
   ——这就是此前「报错看不见」的原因。全部改走 UTF-8 窄串；另加了
   `KRKR_TJS_TRACE=1`（异常带 TJS 调用栈）与 invalidated 抛出点栈转储。

## 长剧情实测（`kirikiroid2-mac/tests/run-kag-story.sh`，一条命令）

真实 KAG 工程 testdata/kag-game（kirikiri-kag-ACG，含 BGM/音效/存档），两种
发行形态各跑一遍：

| 形态 | 结果 |
|---|---|
| 游戏文件夹 | 标题 → 点击 start → 1.ks 连续台词 → `[playbgm]` → 点击停 BGM → `[playse][ws]` → 4th/5th message → 循环回跳，零异常，exit 0 |
| data.xp3 归档（真实发行形态） | 同上，exit 0 |

存档 data999.ksd 正常写出；`build-all.sh --check` 新增 kirikiri-story 自检通过
（合计 5/5）。

## 音频专项（SDL_mixer 后端重写）

重写 `SoundBuffer_Darwin.cpp`：WAV 走 Mix_Chunk（独立通道/音量/暂停），OGG 等
容器走 Mix_Music，BGM 与音效可以同时出声；status 读混音器真实状态；fade 由宿主
帧循环步进并投递 `onFadeCompleted`（KAG 的 fadeOutAndStop 因此能完成）。

| 项 | 实测 |
|---|---|
| WAV play/pause/resume/stop | status 1→2→1→0 ✓ |
| 自然播完（[ws] 语义） | boom.wav 5.55s，5.6s 后 status=0 ✓ |
| OGG BGM + WAV SE 同时 | 两条 status 同时为 1 ✓ |
| 每缓冲独立音量 | SE -1200 / BGM -600 互不覆盖 ✓ |
| close/reopen/invalidate-while-playing | 全部安全 ✓ |
| fade(0,300) 完成事件 | 送达 ✓ |

## 余留缺口（诚实清单）

- 启动时书签自动保存报「ファイルに保存できません」：`saveBookMarkToFile` 的
  **缩略图分支**（saveLayerImage bmp 输出）抛异常，KAG 自己捕获后继续；
  saveStruct 本身（含 o128/co128 模式、双斜杠路径）验证可用，data999.ksd 本体
  写出正常。
- `WaveSoundBuffer.position` 恒为 0（SDL_mixer chunk 无进度查询）。
- 归档形态下存档落在进程当前目录，尚未做目录重定向。
- 本机沙箱内 `SDL_AUDIODRIVER=dummy` 会被系统 SIGKILL（平台限制），音频测试
  走真实音频子系统。

## 影片后端与存读档（2026-10-25 第二轮，task T1/T3）

生成时间：2026-10-25 13:55　机器：Apple M1 / macOS 27.0.1

### 影片：8/8

```
$ ./OnscripterYuri/test/video/run_acceptance.sh
== 1. mpegplay, MPEG-1 ==          ✓ played sample.mpg (AVFoundation)
== 2. avi, wmv2 -> mpv fallback == ✓ played sample_wmv2.avi (mpv backend)
== 3. mp4 direct path ==           ✓ played sample.mp4 (AVFoundation)
== 4. video_map.json routes ==     ✓ transcoded file was chosen
== 5. click_stop=1 ==              ✓ stopped at 1.28s (clip 3.00s)
== 6. click_stop=0 ==              ✓ played through (3.46s)
== 7. frame reaches surface ==     ✓ dumped frame has real content
== 8. no-movie regression ==       ✓ lifegame survives 3s
passed 8, failed 0
```

### 存读档：PASS

```
$ ./harness/run_savetest.sh
[1/2] save phase ...  save4.dat 35998 bytes
[2/2] load phase (fresh process) ...
      variable %0 was restored: the ok.mp4 branch ran
SUMMARY: PASS save+load round-trip (variable restored across processes)
```

```
$ ./gal/gal savetest lifegame
SUMMARY: PASS save+load round-trip (variable restored across processes)
```

### 效率基线

```
$ ./bench/bench.sh OnscripterYuri/build/onsyuri testdata/lifegame 20 -- --root . --enc:sjis --window
cpu_avg_pct=1.0 rss_peak_mb=32.9
verdict=PASS (baseline: static CPU<3%, RSS<150MB)
```

### gal 全量 verify（逐个串行）

```
lifegame                          可以运行
kag3-sample                       可以运行
kag3-arc                          可以运行
kag3-xp3                          可以运行
BGI官方体验版-遥か碧の花嫁に      可以运行
NScripter官方样本-Lua值传递       可以运行
NScripter官方样本-文字精灵化      可以运行
NScripter官方样本-跳动的方块      可以运行
NScripter官方样本-输入函数        可以运行
data                              跑不起来（KiriKiri 书签写入失败，既有缺陷）
```

### gal prep 自测

```
$ cd gal && ./prep/selftest.sh
selftest: 29 pass, 0 fail
```

## 剧情/对话流修复（2026-10-25 第三轮）

```
$ ./OnscripterYuri/test/video/run_acceptance.sh
...
== 4b. script continues past a movie (the dialogue-flow bug) ==
  ✓ script continued past the movie (red background drawn)
...
passed 9, failed 0
```

端到端对话场景（影片前台词 → 影片 → 影片后台词）：

```
video: playing ./clip.mp4 (2.00s @25fps, audio=silent)
video: played ./clip.mp4 (AVFoundation)
colours: 1151  near-white samples: 1533
RESULT: AFTER-MOVIE DIALOGUE RENDERED
```

回归（同一构建）：

```
存读档      SUMMARY: PASS save+load round-trip
gal prep    selftest: 29 pass, 0 fail
效率        cpu_avg_pct=1.02  rss_peak_mb=43.8  verdict=PASS
verify      lifegame / 4 个官方 SDK 样例 全部「可以运行」
```

## 2026-10-25 傍晚：第二款真实游戏全程验收

外部站点当天基本不可达（archive.org DNS 被污染、itch.io 拒连、Vector 维护、freem 下架），
但 GitHub 通畅，据此拿到正规来源的第二款真实吉里吉里作品：
**krkrsdl2 官方 KAG3 演示游戏**（`testdata/krkrsdl2-demo-game/`，许可与 KAG3 相同）。

它与第一款工程（kirikiri-kag-ACG）的差异是真实存在的：

| 维度 | kirikiri-kag-ACG | krkrsdl2 官方演示 |
|---|---|---|
| 分辨率 | 1280×720 | 640×480 |
| 背景 | PNG | JPEG（`_24_*.jpg`） |
| 消息框 | PNG | **TLG**（`messageframe.tlg`） |
| 转场 | 简单 | `@trans crossfade` + `@wt` 多处 |
| 逻辑 | 纯 KAG | TJS `SnowPlugin` + `Timer` 驱动雪粒 |
| 交互 | 图形按钮 | `[link target=...]` 文字超链接菜单 |

实测（`tests/run-demo-game.sh`，已并入 `build-all --check` 的 kirikiri-demo 项）：

- 菜单画面正确渲染：307 种抽样颜色、双语标题与四个超链接条目
  （证据 `screenshots/demo-menu.png`）。
- `first.ks` 与 `snow.ks`（TJS 插件剧本）都加载执行；**零脚本异常、零 invalidated**。
- 20 秒自动点击跑 8 次点击，场景正常。
- 60 秒长跑后画面仍是完整菜单（`screenshots/demo-menu-bg.png`）。

### 本轮定位到的新缺口：`[link]` 文字热区（province 文本绘制）

探针实测（点击链接位置时）：

```
st=2 w_click=1 w_trans=1  prov160=0 prov300=0
msgHit=0/0 msgSize=16,16 608x448 msgVis=1
```

- `prov160=0`：链接所在坐标的 **province 像素为 0**，即文字热区没有写进 province 图。
- 命中测试因此落到最底层的 base 层（日志 `down on layer ... at 0,0 size 640,480`），
  链接永远点不中。
- 根因：`tTJSNI_BaseLayer::DrawText` 只处理 `dfAlpha/dfAddAlpha/dfOpaque`，
  对 `dfProvince` 直接抛 `TVPNotDrawableFaceType`；而 KAG3 正是用
  `face = dfProvince` 画链接文字来生成热区。

同时确认一处**点击穿透**（非阻塞，属 KAG 语义边界）：

```
click at inSleep=0 st=2(mWait) w_click=1   <- 陈旧 click 等待仍在 waitUntil
```

在转场（`@wt`）期间点击，`waitUntil` 里上一轮遗留的 `click` 信号会让
`conductor.trigger('click')` 提前恢复剧本，跳过本应停住的 `@s`。
正常游玩（等画面稳定再点）不受影响，但连点会跳过菜单。

两项的后续处理（2026-10-25 傍晚）：

- `drawText` 的 `dfProvince` 分支已补上（把字形覆盖率盖写成 province 值）；
- `operateRect` 的 province 面已修正（原先混进 MainImage，热区被静默丢弃），
  并把源图层的 province 图一起传过去。

修完之后，链接单元格**确实被填上了 province 值**（观察到 value=1 的 73 个单元格），
但仍是落在 KAG 命中测试**不查的那一层**：点击时 message 层的 province 仍是 0，
`findLink` 全为 -1，点击继续穿透到背景层。所以「链接可点」**仍未达成**，
只能算把底层能力补齐了一半。

### 2026-10-25 晚：链接点击已修复 ✅

上面两个缺口的第 (2) 项已经真正解决，而且是三层独立故障叠加：

1. **province 位图读写走错了通道**——`AllocateProvinceImage` 请求 8bpp，
   但本后端实际返回 32bpp 纹理，`Fill()` 又走渲染管线的 `FillARGB`，
   写进的是 CPU 侧读不到的纹理。现改为直接按扫描线读写
   （`ProvinceFillRect` / `ProvinceReadPixel`），fill / colorRect /
   setProvincePixel / 命中测试读取全走同一条路。
2. **热区不是 `drawText` 画的**——KAG3 用 `face=dfProvince` + `fillRect` /
   `colorRect` 逐格涂链接编号；两条路径都改为写入热区值而不是颜色。
3. **合成点击缺鼠标移动**——KAG 的防误点保护（`selClickLock`）会丢弃
   "没有移动过光标" 的点击，自动点击 harness 必须先发 mouse move。

修复后实测（`tests/run-demo-game.sh` 新增断言）：

```
[click] t=... at 320,145 front=... match=0
HT ... link=0                 <- findLink 返回 0，命中链接
first.ks : jumped to : *about_kirikiri
first.ks : @changebg_and_clear storage="_24_4"
```

即：点击菜单文字 → 选中第一个链接 → 跳转 `*about_kirikiri` → 换背景 →
章节正文开始执行。截图 `screenshots/demo-section.png`：
「吉里吉里は、TJSというスクリプト言語を使っていろいろな事をするためのソフトウェアです。」

第 (1) 项（转场期间点击穿透 `@s`）仍是已知缺口，未修。


## 2026-10-25 深夜：存档修复 ✅

KAG3 存书签的写法是：先画缩略图，再 `Dictionary.saveStruct(fn, "o<缩略图字节数>")`
把结构化数据写在图后面；读档用 `Scripts.evalStorage(fn, "o<偏移>")`。

本后端上这一步**每次存档都失败**（`Write error`）：偏移模式打开存储时走 update
模式，该模式在这台机器上要么不可写、要么返回只读视图，第一次写就抛。

修复：文本流与二进制流两个工厂都改成"就地更新"的实现——先读出文件已有字节，
用普通写流重建文件并写回这些字节，再定位到偏移继续写。读侧本来就支持 `o`，未改。

验证两条：

| 验证 | 结果 |
|---|---|
| 真实 KAG 工程启动存档 | `data999.ksd` 64KB 正常写出，不再出现「ファイルに保存できません」 |
| 新增 headless 回归 `tests/run-save-check.sh` | 缩略图 + 偏移结构写 + `evalStorage` 读回，全通过 |

`build-all.sh --check` 现有 **7 项自检全部通过**（新增 kirikiri-save）。

## 当前 KiriKiri 能力清单（真实数据实测）

| 环节 | 状态 | 证据 |
|---|---|---|
| TJS2 脚本引擎 | ✅ | tjsc 全套语法/类/异常用例 |
| XP3 归档（读/列/解包、从归档启动） | ✅ | data.xp3 直接启动跑完整章 |
| 图像 PNG/JPEG/BMP/TLG | ✅ | 两个真实游戏的背景与消息框 |
| 图层、合成、转场（crossfade） | ✅ | 演示游戏菜单与章节 |
| 文字渲染（日文、抗锯齿、描边） | ✅ | 截图 `demo-menu.png` / `demo-section.png` |
| 鼠标输入 + 点击路由 | ✅ | 按钮、正文推进 |
| **超链接 `[link]` 热区** | ✅ | 点击 → `jumped to : *about_kirikiri` |
| **存档 / 读档（缩略图 + 偏移结构）** | ✅ | run-save-check.sh |
| 音频（BGM/SE、暂停、独立音量、fade、[ws]） | ✅ | 音频专项探针 6 项全过 |
| 定时器 / AsyncTrigger / 事件队列 | ✅ | 长剧情逐句推进 |
| 影片播放 | ❌ | `VideoOverlay_Darwin.cpp` 仍是状态记录桩 |
| MIDI / CDDA | ❌ | 未实现（现代游戏基本不用） |
| XP3 加密变体 | ⚠️ | 仅未加密归档；商业游戏的加密 xp3 打不开 |


## 2026-10-25 深夜续：影片、存档、补丁、发布形态

### 影片播放 ✅（新）

`VideoOverlay` 原先只是状态记录器，现在接 FFmpeg 真解码：

- 影片经引擎存储层读取，**XP3 归档里的影片**与散文件一样能播
- 帧按墙上时钟推进（超前帧暂存到下一拍），不会「一帧当全部」
- 在设备合成图层树**之后**绘制（run loop 里 Update → TickVideo → Show），
  并通过纹理自身的 Update 上传；位图扫描线不是窗口读取的那块内存
- overlay 持有窗口引用并注册到帧循环；销毁时自动摘除——否则脚本丢掉
  窗口后一秒内必崩（这是实测到的真实崩溃）

验证：`tests/run-video-check.sh` 用「0.7 秒红 + 0.7 秒绿」的剪辑，
断言最终帧变绿（即真的推进了帧），已接入 `build-all --check`。

未做：影片**音轨**仍未解码进混音器（`numberOfAudioStream` 能报告数量）。

### 存档 ✅（新）

KAG 存书签 = 缩略图 + `saveStruct(fn, "o<偏移>")`。偏移写在 update 模式下
每次都抛 `Write error`。改为「读出已有字节 → 普通写流重建 → 定位到偏移」，
文本流与二进制流两个工厂都改。真实工程 `data999.ksd` 正常写出 64KB。

### 补丁与发布形态 ✅（新）

- 新增 `tTVPOverlayArchive`：把 data.xp3 与 patch.xp3..patch4.xp3 合并成一个
  视图，后者覆盖同名条目。因为存储层**当前目录优先于 auto-path**，只挂
  auto-path 的补丁永远不会生效。
- `krkrrun <游戏文件夹>` 现在能识别「发布形态」：chdir 进目录（KAG 的
  `System.exePath` 取自进程 cwd）、挂 data.xp3、叠加补丁。
- 验证：把演示游戏打成 data.xp3 + 改过标题的 patch.xp3，运行后屏幕上是
  补丁里的标题。

## KiriKiri 能力清单（更新）

| 环节 | 状态 |
|---|---|
| TJS2 脚本、XP3、图层合成、转场、日文文字 | ✅ |
| 鼠标输入、按钮、正文推进 | ✅ |
| 超链接 `[link]` 热区（province） | ✅ |
| 存档 / 读档（缩略图 + 偏移结构） | ✅ |
| 音频（BGM/SE、暂停、独立音量、fade、[ws]） | ✅ |
| **影片播放（画面）** | ✅ |
| **发布形态 data.xp3 + patch.xp3** | ✅ |
| 影片音轨 | ❌ 未解码进混音器 |
| XP3 加密变体（商业游戏常见） | ❌ 仅未加密归档 |
| MIDI / CDDA | ❌ 未实现 |


## 2026-10-25 收尾：交互、菜单、存档往返

又修掉两处真实缺陷，并把三款真实数据上的交互路径全部纳入自动验收。

### `MenuItem.caption` 是只读的 ✅

KAG 每次重建右键菜单都会给菜单项赋 `caption`，而我们的 MenuItem 只有 getter，
于是每次重建都抛 `Invalid operation for Read-only or Write-only property`。
这个异常还会连带打断 `loadBookMark`（读档要重建菜单）。现在 caption 可写。

顺带补上 `KRKR_AUTO_RCLICK`：自动点击只按左键，右键（KAG 的菜单入口）之前
根本没法测。

### 书签存读档往返 ✅

用游戏**自己的**代码验：

```
kag.saveBookMark(0)   -> ok（写出 data999.ksd）
kag.loadBookMark(0)   -> 1
```

`run-save-check.sh` 现在同时覆盖底层（缩略图 + 偏移结构 + 读回）和游戏级往返。

### 故事内文字链接 ✅

ACG 工程正文里右上角的 `setting` 链接，点中后 `setting.ks : jumped to : *setting`
（设定画面加载并渲染）。已加进 `run-kag-story.sh` 的第二阶段。

### 验收现状：9 项自检全绿

```
nscripter / kirikiri / kirikiri-story / kirikiri-demo /
kirikiri-save / kirikiri-video / kirikiri-audio / bgi / artemis   9/9
```

## KiriKiri 能力清单（最终）

| 环节 | 状态 | 验证方式 |
|---|---|---|
| TJS2 脚本引擎 | ✅ | tjsc 语法/类/异常用例 |
| XP3 归档（未加密） | ✅ | 从 data.xp3 启动跑完整章 |
| 发布形态 data.xp3 + patch*.xp3 | ✅ | 补丁覆盖标题的实测 |
| 图像 PNG/JPEG/BMP/TLG | ✅ | 两款真实游戏素材 |
| 图层合成、crossfade 转场 | ✅ | 演示游戏菜单/章节 |
| 日文文字渲染（描边、抗锯齿） | ✅ | 截图证据 |
| 鼠标左键、按钮、正文推进 | ✅ | 长剧情逐句推进 |
| 鼠标右键（KAG 菜单入口） | ✅ | onPrimaryRightClick 路径 |
| 文字超链接热区（province） | ✅ | 菜单四链接 + 正文 setting 链接 |
| 存/读档（缩略图 + 偏移结构） | ✅ | 底层 + 游戏级往返 |
| 音频（BGM/SE、暂停、独立音量、fade、[ws]、position） | ✅ | kirikiri-audio 9 项断言 |
| 影片画面 + 音轨 | ✅ | 双色剪辑逐帧 + 音频样本数 |
| 定时器 / AsyncTrigger / 事件队列 | ✅ | 长剧情、fade、影片共同依赖 |

### 仍缺（诚实清单）

| 缺口 | 说明 |
|---|---|
| XP3 加密变体 | 只能打开未加密归档；商业游戏的加密 xp3 打不开。需要移植 xp3filter（依赖 ncbind）并拿真实加密样本验证，当前无样本 |
| MIDI / CDDA | 未实现；现代 KAG 游戏基本不用 |
| 影片 mixer 模式 | 只有 overlay 模式（画到主图层）；KAG 在 mixer 失败时会回退到 overlay，所以实际可用 |
| 字体嵌入 / 非日文字形回退 | 仅用系统字体链，未做游戏自带字体加载 |


## 2026-10-25 性能：帧循环对齐时钟

跑循环用的是「干完活再固定睡 16ms」，一帧实际是 16ms + 工作量 + 调度开销：

| 场景 | 改前 | 改后 |
|---|---|---|
| 空窗口（无游戏） | 51.6 fps | **61.0 fps** |
| 官方演示 640×480 | 49.6 fps | **61.8 fps** |
| ACG 工程 1280×720 | 37.8 fps | **57.1 fps** |

改成「记录帧截止时间，只补睡剩余时间」后，两条真实工程都回到 60fps 附近。
`KRKR_STATS=1` 会在退出时打印帧数/秒数/fps，`tests/run-perf-check.sh`
用下限定住它，已接入 `build-all --check`。

## 验收现状：10 项全绿

```
nscripter / kirikiri / kirikiri-story / kirikiri-demo / kirikiri-save /
kirikiri-video / kirikiri-audio / kirikiri-perf / bgi / artemis     10/10
```
