# 引擎③ RealLive / RLVM：现状与可行性（只读评估，未改动）

`rlvm/` 是上游源码（GPL-3+，作者说它能把 Kanon/Air/CLANNAD 等 Key 系游戏跑通）。
**本会话从未改动这个目录**——它由另一个会话负责移植。

## 现状（2026-10-25）

- 目录里只有上游源码，**没有构建产物**（无 `build/`、无 `bin/rlvm`）
- git 只有一个上游合并提交（`e38cda7`）
- `gal engines` 里显示「未构建」

## 构建它需要什么

| 依赖 | 状态 |
|---|---|
| scons | ✅ 已装（4.11.1） |
| boost ≥ 1.40 | ✅ 已装（1.92）——但老代码对新 boost 常需小修 |
| **SDL 1.2** | ❌ 未装（系统只有 SDL2）；可装 `sdl12-compat`（SDL1.2 → SDL2 的桥） |
| SDL_image / SDL_mixer 1.2 | 源码自带副本，也可用 brew 的 |
| GLEW | ✅ 已装（2.3.1） |
| libvorbis | ✅ 已装 |

## 评估

**可行，但不是一条命令的事**：

1. `scons --release` 的 SConscript 面向 Linux/旧 macOS，Apple Silicon 需要改架构与库路径
2. SDL 1.2 → 用 `sdl12-compat` 桥接，事件/全屏/音频路径都要实测
3. boost 1.92 与这份十多年前的代码，大概率要修几处头文件/接口
4. 跑通之后还需要**一款 RealLive 游戏的真实数据**（Kanon/Air/CLANNAD 等），
   这些是商业作品，本机没有

**所以即使构建成功，验收也要等游戏数据。**

## 建议

如果那个会话已经不活跃，我可以接手（大约需要：装 sdl12-compat → 调 SConscript →
修 boost 兼容 → 构建 → 用 `gal` 接入）。但这会和那个会话冲突，
**需要你确认再动 `rlvm/` 目录。**
