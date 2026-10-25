# NScripter 影片后端与 gal 预处理管线 — 跨任务契约（2026-10-25）

本文件是 T1/T2/T3/T4 的**唯一接口真源**。改接口必须先改本文件。
任何 teammate 不得改本文件；发现契约不合理 → 给 lead 发消息。

## 背景事实（已核实，勿再调查）

- `OnscripterYuri/src/onsyuri/ONScripter_sound.cpp`
  - `playMPEG()` (:256)：主体 `#if defined(USE_SMPEG)`；Mac 构建无此宏 → `#else` 打印 `mpegplay command is disabled.`
  - `playAVI()` (:390)：只有 WIN32(`system()`) / IOS / ANDROID 分支 → Mac 打印 `avi command is disabled.`
  - 返回值语义（**2026-10-25 修正，先前写反了**）：`play*` 返回**非 0 表示播放失败**，
    调用方是 `if (playMPEG(...)) endCommand();` —— 即"放不出来才结束脚本"。
    上游 `playMPEG` 以 `int ret = 0` 起步并在所有正常路径返回 0，所以**影片播完
    脚本必须继续**。把"播放成功"返回成 1 会让每部影片后都跑 `endCommand()`，
    影片后面的台词全部丢失（真实发生过的 bug，见 BUILD_NOTES「影片打断剧情」）。
- `movieCommand()`（ONScripter_command.cpp :1536）解析 click/loop 后同样调 `playMPEG`；`stop/pos/async` 未支持
- `CMakeLists.txt` `config_darwin()` (:85) 只定义 `MACOSX USE_PARALLEL USE_GLES USE_FILELOG`
- 调用路径：`playMPEG/playAVI` 先用 `script_h.cBR->getFileLength/Name` 查（视频通常在归档外的散文件），SMPEG 分支会 `getFile` 整读进内存
- NScripter 的 movie 文件不归档（原作限制），所以**文件名映射不需要改脚本**，运行时侧车映射即可
- Homebrew smpeg 已 deprecated（2026-12-01 disable），avifile 已死 —— 不要尝试接回
- ffmpeg 9.0.2 已装，带 `h264_videotoolbox` / `hevc_videotoolbox`

## 总路线

- **主路（转码）**：`gal` 在 add/prep 阶段把游戏目录里的 `.avi/.mpg/.mpeg`（含归档内的视频）转成 H.264 MP4 放 `_video/`，生成 `_video/video_map.json`。运行时原生 AVFoundation 后端查 map → 播转码产物（VideoToolbox 硬解）。
- **兜底（未转码/转码失败）**：libmpv，以 **dlopen 运行期加载**方式接入，无编译期依赖（brew 没有就自动降级为现状的 disabled+跳过）。
- 两条路都不可用 → 打日志、跳过、**返回 0 让脚本继续**（宽容策略，与 ONScripter 整体一致）。

## 接口契约（C++，引擎侧唯一头文件）

新文件 `OnscripterYuri/src/onsyuri/video_player.h`（**T1 所有权**）：

```cpp
#ifndef ONSCRIPTER_VIDEO_PLAYER_H
#define ONSCRIPTER_VIDEO_PLAYER_H

// 返回 true 表示"已在本机播放"；false 表示不可用/未播放，调用方走原禁用逻辑。
// 阻塞直到播放结束：
//   click_stop=true 且期间发生左键点击或 esc/skip → 提前结束，返回 true
//   click_stop=false → 必须播完，忽略点击（与 NScripter movie,click=0 一致）
// loop 仅 mpegplay/movie 使用；avi 传 false。
bool videoPlayBlocking(const char *abs_path, bool click_stop, bool loop);

// 侧车映射：若 game_root/_video/video_map.json 有 from -> to（to 为相对 game_root 的路径），
// 返回 malloc/new 拷贝的 to，否则返回 nullptr。大小写不敏感、'/'与'\\'等价。
char *videoMapLookup(const char *game_root, const char *script_path);

#endif
```

`playMPEG/playAVI` 的 Mac `#else` 分支改为（**T1 所有权，仅动这两个函数与 CMake**）：

```cpp
char *mapped = videoMapLookup(archive_path, absolute_filename);
const char *to_play = mapped ? mapped : absolute_filename;
if (!videoPlayBlocking(to_play, click_flag, loop_flag)) {   // false = 没播成
    utils::printError("mpegplay: could not play %s, skipped.\n", to_play);
}
free(mapped);
// ret 保持 0 —— 影片后脚本继续（这是原作行为，不是容错）
```

`videoPlayBlocking` 实现内部顺序：AVFoundation（`video_player_mac.mm`，T1）→ 失败/非 mp4 容器则尝试 mpv（T4 的 `.mm/.cpp` 通过 dlopen，接口见下）→ 都失败 return false。
事件泵：播放循环内必须 `SDL_PumpEvents` 轮询鼠标/键（SDL2 API），否则引擎外部点击无法感知。

## T4 的 mpv 接入方式（避免 CMake 冲突）

- **不碰** `CMakeLists.txt`。新文件 `OnscripterYuri/src/onsyuri/video_player_mpv.cpp` + `.h`：
  `bool mpvPlayBlocking(const char *path, bool click_stop, bool loop);`
- dlopen `libmpv.2.dylib`（候选路径：`/opt/homebrew/lib`、`/usr/local/lib`、`@rpath`）；拿不到 `mpv_client_api` 必需符号即返回 false。
- 视频帧用 `mpv_render_context`（MPV_RENDER_API_TYPE_SW，fmt=rgba）读进 CPU buffer，转成 SDL surface blit 到引擎 `accumulation_surface` 后 repaint —— **不要开 mpv 自己的窗口**（点击判定归引擎事件循环）。音频走 mpv 内部 audio（无窗口也有声）。
- T4 在 `OnscripterYuri/mpvdev/` 下自建最小 harness 验证解码正确性（用 testdata 或 ffmpeg 生成的样片）。集成调用点的 patch 由 lead 在合并阶段落。

## 构建目录隔离（硬规定）

`OnscripterYuri/` 是同一份源码树，**并行构建必须各自 build dir**：

| 任务 | cmake build dir | 二进制输出 |
|---|---|---|
| T1 | `OnscripterYuri/build-video/` | `build-video/onsyuri` |
| T3 | `OnscripterYuri/build-t3/` | `build-t3/onsyuri` |
| T4 | 不需要 cmake（单独 `c++ -c` 编译 + 自有 harness 二进制放 `OnscripterYuri/mpvdev/`） | — |
| T2 | 不碰 OnscripterYuri | — |

canonical `build/onsyuri` 最后由 lead 统一重建成含后端的版本。

## video_map.json 格式（T1 读 / T2 写）

```json
{ "version": 1,
  "mappings": [
    { "from": "af.mpg",        // 脚本里引用的相对路径原样（游戏根为基准，'\\'归一为'/'）
      "to":   "_video/af.mp4", // 相对游戏根
      "codec": "h264", "source": "loose|nsa:<archive>#<entry>" } ] }
```

T2 生成转码产物时**必须**同步写这个文件；T1 找不到文件/字段就忽略该条。

## 文件所有权矩阵（scope 外只读）

| 路径 | owner |
|---|---|
| `OnscripterYuri/src/onsyuri/video_player*`、`video_player_mac.*`、`ONScripter_sound.cpp`（两函数）、`CMakeLists.txt` | T1 |
| `OnscripterYuri/src/onsyuri/video_player_mpv.*`、`OnscripterYuri/mpvdev/**` | T4 |
| `gal/**`（含新子命令 `gal prep`）、`gal/README.md`、`testdata/` 之外不动游戏本体 | T2 |
| `harness/**`、`OnscripterYuri/build-t3/**`、`testdata/save-probe/**`（新建自证数据） | T3 |
| `rlvm/**` 任何人不动（另一会话） | — |

## 效率与验收基线

- 静态页 headless 30s：CPU 均值 <3%（`ps -o %cpu`），常驻 RSS <150MB，冷启动到标题 <0.5s
- 影片播放用 `SDL_AUDIODRIVER=dummy` 也能跑（CI 无麦）；1080p60 软解兜底路径可接受掉帧，硬解主路不允许
- 每个任务完成后给 lead：改动文件清单 + 复现命令 + 截图/日志证据（放各自目录，命名 `<task>-*`）
