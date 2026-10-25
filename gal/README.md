# gal — one command for the galgame runtimes in this tree

Each engine in `~/Documents/gal-engines` ships its own binary and its own idea
of how to be started: OnscripterYuri wants `--root`, OpenBGI wants the game
folder as its working directory.  `gal` looks at a game folder, works out which
engine it belongs to, remembers it, and starts the right binary the right way.

```bash
./gal engines             # what is built, where, and how far along
./gal add <folder>        # detect the engine and add the game to the library
./gal list                # the library
./gal info <game>         # why the engine was recognised
./gal launch <game>       # start it      (--dry-run prints the command)
./gal verify <game>       # start it headless for a few seconds and report
./gal remove <game>       # drop it
./gal doctor              # check the binaries and the folders
```

The library is JSON under `~/Library/Application Support/gal/library.json`;
the game folders stay where they are.

## Where the engines stand, verified by `gal verify`

Measured, not claimed - the output of `gal verify` on every data set in the
library at the end of round 37:

| engine | data set | verdict |
|---|---|---|
| NScripter | `testdata/lifegame` (the official OnscripterYuri demo) | **runs** - enters its main loop |
| NScripter | `testdata/nscripter-sdk/.../サンプル1` | **runs** |
| NScripter | `.../サンプル2-1`, `2-2`, `3` | **runs** |
| Ethornell/BGI | the official Lump of Sugar trial (1.669) | **not yet** - the engine identifies it, opens a window and decodes its data, then stops on opcodes that upstream has not implemented for 1.669 (200 of them) |
| KiriKiri | - | TJS2, XP3, storage, the software renderer, images and text all work; no game data to run, and the layer system still stops on its first fill |
| RealLive | - | ported in another session, no binary yet |
| Artemis | - | written from scratch: `.pfs` and `.ast` read and write, no game data to run |

Two engines run real data; the others are honest about where they stop.

## How detection works

Each engine declares signature files with weights; the engine with the most
evidence wins, so one generic name does not mis-file a game.

| engine | signature files |
|---|---|
| NScripter | `nscript.dat`, `nscr_sec.dat`, `00.txt`, `0.txt`, `*.nsa`, `*.ns2`, `*.nt2`, `*.nt3` |
| KiriKiri | `data.xp3`, `patch.xp3`, `startup.tjs`, `*.xp3`, `*.tjs` |
| RealLive | `Seen.txt`, `Gameexe.ini`, `*.g00`, `*.nwk`, `*.koe`, `*.ovk` |
| Ethornell/BGI | `system.arc`, `sysprg.arc`, `comap.dat`, `BGIError.txt`, `BGI.dat`, `*.arc` |
| Artemis | `*.pfs`, `*.ast`, `*.iet` |

## Engine status

| engine | binary | state |
|---|---|---|
| NScripter → OnscripterYuri | `OnscripterYuri/build/onsyuri` | **runnable** |
| Ethornell/BGI → OpenBGI | `openbgi/openbgi-src/bin/bgi` | **runnable** |
| KiriKiri → Kirikiroid2 | `kirikiroid2-mac/build/tjsc` | partial: TJS2 + XP3 work, no window yet |
| RealLive → RLVM | `rlvm/build/rlvm` | owned by the other session |
| Artemis | — | not implemented, the from-scratch target |

`launch` refuses the engines that cannot run a game yet and says why, instead
of failing halfway.

## Real data it runs today

Five data sets are registered and pass `gal verify` (headless, no window):

| data set | source |
|---|---|
| `testdata/lifegame` | the official OnscripterYuri web demo (`onscript.nt2`, an encrypted script) |
| `testdata/nscripter-sdk/n18/.../サンプル1` | NScripter final SDK, Lua value passing |
| `.../サンプル2-1` | NScripter final SDK, a square bouncing around |
| `.../サンプル2-2` | NScripter final SDK, text turned into sprites |
| `.../サンプル3` | NScripter final SDK, the input functions |

The NScripter2 SDK samples are *not* in the list: they are written in
NScripter 2's Lua dialect, which `verify` correctly reports as
`Script error, Label "define" is not found`.

## How verify judges

`verify` starts the engine headless and reads both the output and the clock:

| what happened | verdict |
|---|---|
| still running when the window closed | ✓ entered its main loop |
| exited on its own after 2 s or more | ✓ the script ended normally |
| exited on its own in under 2 s | ⚠ gave up: missing data or a suppressed error |

The third row is the important one: the engines report errors through a
message box, and a headless run has none, so a quiet early exit is the only
evidence that something was missing.  The BGI trial below is exactly that case
and `verify` refuses to call it a pass.

## Verified

```
$ ./gal add ../testdata/lifegame        # the real NScripter data set
已加入: lifegame
  引擎: NScripter → OnscripterYuri
  依据: onscript.nt2

$ ./gal launch lifegame --dry-run
cwd:  .../testdata/lifegame
argv: .../OnscripterYuri/build/onsyuri --root .../testdata/lifegame --enc:sjis

$ ./gal launch lifegame                 # really starts the engine:
ONScripter Yuri 0.7.7 (Jh0.8.0,Ons20181218,NSC2.96)
Display: 800 x 600 (32 bpp)
Audio: 44100 Hz 16 bit stereo
```

## gal prep — 影片预处理（task-2）

NScripter 时代的影片是 `.avi/.mpg/.mpeg`，Mac 端口放不了（没有 smpeg，没有
avifile，Homebrew 两边都已废弃）。`gal prep` 提前把它们一次性转成 H.264 MP4
放进 `<游戏目录>/_video/`，并写出 `_video/video_map.json`，让引擎侧按
"脚本里写的名字 → 真正能播的名字" 查表播放。map 的格式由
`../DESIGN-VIDEO.md` 的契约锁定（version 1），本工具不擅自加字段。

```bash
./gal prep <game|目录>            # 转码 + 写 map（table 报告）
./gal prep <game> --report json   # 机器可读：源codec/分辨率/时长/耗时/产物大小
./gal add <目录>                  # 识别为 NScripter 时自动跑 prep
./gal add <目录> --no-prep        # 跳过自动预处理
```

- **扫描**：游戏根目录递归的 `*.avi *.mpg *.mpeg`，大小写都算（`.MPEG` 也收）；
  `_video/`（自己的产物）与点开头的目录不扫。
- **探测**：`ffprobe` 拿 codec / 编码分辨率 / SAR / DAR / fps / 场序 / 音轨 / 时长。
- **转码**：`ffmpeg` → `yadif=0:-1:0`（只在源不是 progressive 时加）+ `scale`
  到按 SAR 换算出的显示尺寸 + `setsar=1`（保持显示宽高比，像素转正方），
  视频 `h264_videotoolbox` 硬编（`-realtime 0`，按分辨率/帧率算码率），音频 `aac 128k`，
  `-movflags +faststart`。
- **失败重试**：硬编任一环节失败 → 同样的滤镜链改用 `libx264 -preset veryfast -crf 23
  -pix_fmt yuv420p`；两个都失败才判 failed，`.part` 临时文件被清掉，map 里不留这条。
- **幂等**：产物已存在、非空、比源新，且 `ffprobe` 还能读出视频流 → 跳过（状态
  `skipped`），只重新登记映射。源被 `touch`、产物被截断或写坏，都会自动重转。
  源坏得探测不了、但旧产物仍满足这条幂等判据时，旧映射保留（状态 `unprobeable`，
  产物大小照报），引擎侧还能播上一次的成功转码。
- **命名**：`_video/<原名>.mp4`；嵌套目录里重名的（`clip.mpg` 与 `sub/clip.mpg`）
  第二个展平成 `sub_clip.mp4`。文件名里的 `\` 在产物名里折成 `_`，map 的 `from`
  保留脚本视角写法（`\` 归一为 `/`，与契约里 T1 的大小写不敏感/分隔符等价一致）。
- **写入边界**：只往 `<游戏>/_video/` 写，游戏其余文件一律只读。

### savetest 扩展点（留给 task-3）

`gal savetest <game>` 已就位，但它只做转发：解析库名或目录后调用
`<ENGINES_ROOT>/harness/run_savetest.sh <游戏目录> [-- 之后的参数]`（可用
`GAL_SAVETEST_SH` 覆盖脚本路径）。该脚本由 task-3 提供；**现在还不存在，
命令会报"待接入"并返回 2**。gal 侧不需要再改，脚本落地即通。

### 复现与自测（/tmp 假游戏，不碰 testdata）

```bash
cd ~/Documents/gal-engines/gal
FAKE=/tmp/gal-prep-fakegame; rm -rf "$FAKE"; mkdir -p "$FAKE/sub"
printf 'stub' > "$FAKE/nscript.dat"; printf 'stub' > "$FAKE/00.txt"   # nscript 签名
# 320x240 mpeg1 + mp2 音频
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=3" \
  -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" \
  -c:v mpeg1video -q:v 5 -c:a mp2 -b:a 128k "$FAKE/movie.mpg"
# wmv3 in avi
ffmpeg -loglevel error -y -f lavfi -i "smptebars=size=320x240:rate=24:duration=2" \
  -f lavfi -i "sine=frequency=660:sample_rate=44100:duration=2" \
  -c:v wmv2 -c:a wmav2 -b:a 96k "$FAKE/intro.avi"
# 大写扩展名 + 嵌套目录 + 非方像素（352x240，SAR 200:219）
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=352x240:rate=29.97:duration=2" \
  -c:v mpeg1video -aspect 4:3 "$FAKE/sub/Parade.MPEG"
# 坏文件（无视频流）
printf '\x00\x1a\x45\xdf not really' > "$FAKE/broken.avi"

./gal add "$FAKE" --name prep-fake     # 识别 nscripter 并自动 prep
./gal prep prep-fake                   # 第二次：全部 skipped(幂等)
./gal prep prep-fake --report json     # 机器可读报告
cat "$FAKE/_video/video_map.json"
```

实测输出（ffmpeg 9.0.2，h264_videotoolbox 可用；`add` 里的自动 prep 与单独 `gal prep` 同一套代码）：

```
文件             源          分辨率           时长  状态      编码器             耗时  产物
---------------  ----------  ---------------  ----  --------  -----------------  ----  --------
broken.avi       -           -                -     不可解码  -                  -     -
intro.avi        wmv2        320x240→320x240  2.1s  转码      h264_videotoolbox  0.6s  38.7KiB
movie.mpg        mpeg1video  320x240→320x240  3.0s  转码      h264_videotoolbox  0.2s  175.5KiB
sub/Parade.MPEG  mpeg1video  352x240→320x240  2.0s  转码      h264_videotoolbox  0.3s  130.7KiB
共 4 个影片：转码 3，跳过 0，失败 0，映射 3 条 -> _video/video_map.json
  ! broken.avi: broken.avi: Invalid data found when processing input
  预处理有未成功项（不影响入库），可单独跑 gal prep prep-fake 重转

$ ./gal prep prep-fake          # 第二次：幂等
文件             源          分辨率   时长  状态        编码器  耗时  产物
---------------  ----------  -------  ----  ----------  ------  ----  --------
broken.avi       -           -        -     不可解码    -       -     -
intro.avi        wmv2        320x240  2.1s  跳过(幂等)  -       -     38.7KiB
movie.mpg        mpeg1video  320x240  3.0s  跳过(幂等)  -       -     175.5KiB
sub/Parade.MPEG  mpeg1video  352x240  2.0s  跳过(幂等)  -       -     130.7KiB
共 4 个影片：转码 0，跳过 3，失败 0，映射 3 条 -> _video/video_map.json
  ! broken.avi: broken.avi: Invalid data found when processing input
（存在不可解码文件时 `gal prep` 返回 1，错误行不泄漏游戏绝对路径）
```

产物经 `ffprobe` 复核：`movie.mp4` / `intro.mp4` = h264 + aac，`320x240`，SAR `1:1`，
DAR `4:3`；`Parade.mp4` 由 352x240 SAR 200:219 转成 320x240 SAR 1:1（DAR 不变）；
经典 PAL 720x576 SAR 16:15 转成 768x576 SAR 1:1（仍是精确 4:3）；无音轨源只带 video 流。

```bash
cd ~/Documents/gal-engines/gal
./prep/selftest.sh            # 25 项断言，全绿返回 0；KEEP=1 保留 /tmp/gal-prep-selftest
```

**失败重试路径**用 ffmpeg shim 注入（不需要真坏机器），shim 就在 `prep/` 下：

```bash
# shim1：h264_videotoolbox 报错退出，其余原样放行
GAL_FFMPEG_BIN=prep/shim-ffmpeg-no-hw ./gal prep /tmp/gal-prep-retry --report json
# → attempts: [{h264_videotoolbox, ok:false, "Encoder not available"},
#              {libx264, ok:true}]，status transcoded，encoder libx264，产物 High profile

# shim2：所有编码器都失败
GAL_FFMPEG_BIN=prep/shim-ffmpeg-all-fail ./gal prep /tmp/gal-prep-retry
# → status failed，_video 里无产物无 .part，该条从映射里移除
```

其它已验证分支：`--no-prep` 不建 `_video/`；非 NScripter 引擎（如 KiriKiri）add 时不自动
prep；曾成功的源变更后重转失败 → 映射条目被移除，源恢复后重跑映射回来；库里没有且不是目录
→ 报错返回 1；`broken.avi` 这类无视频流文件标记 `unprobeable` 并让 `gal prep` 返回 1，
但 `gal add` 里的自动 prep 不影响入库。

## gal verify — 判定分级（工作树里的预留位，未提交，等 task-3 语料）

`228eb58`（已被 `e447a8d` revert）把"缺任何文件"都算失败，误杀了官方 demo：
OnscripterYuri 对可选资源（`cursor0/1.bmp`、`u/d on/offcur.bmp`、`system.lua`）打的
`*** can't find file [...] ***` 是 BUILD_NOTES 定性过的"无害警告，引擎优雅跳过"。
分级方案等 T3 的必需资源坏例 stderr 语料 + lead 的白名单定稿后落地，现在先把**空位留好**：

- `VERIFY_ERROR_MARKERS`：判失败的标记，**与 228eb58 之前的旧集合逐字相同**，行为零变更。
- `VERIFY_WARN_MARKERS`：**目前为空元组**。接入方式是只扩这个元组（或改成按引擎的字典），
  匹配逻辑不动；warn 行在 table 输出里带 `(可选资源/噪音)` 前缀、在 json 里进
  `warnings[]`，永不计败。error/warn 互斥（同一行先归 error）。
- `gal verify <game> --report json`：与 `gal prep --report json` 同风格，字段
  `game / engine / seconds / elapsed_sec / returncode / timed_out / line_count /
  verdict(pass|fail) / ok / errors[] / warnings[]`；json 模式不打人类文本，
  返回码 0=通过、1=不通过。
- 回归现状：lifegame、SDK 样本、kag3、BGI 体验版在这处改动下全部维持"可以运行"，
  人类可读输出与旧版逐字一致；`./prep/selftest.sh` 仍全绿。


