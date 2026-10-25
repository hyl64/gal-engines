# 从这里复现全部结果

这台机器：**Apple M1（arm64）/ macOS 27.0.1**。四个引擎都是原生 arm64 可执行文件，
没有 Rosetta、没有 Wine、没有虚拟机。

## 一条命令

```bash
cd ~/Documents/gal-engines
./build-all.sh          # 构建 + 自检
./build-all.sh --check  # 只跑自检
```

```
构建
  ✓ nscripter     build/onsyuri
  ✓ bgi           bin/bgi
  ✓ kirikiri      build/krkrrun
  ✓ artemis       build/agame
自检
  ✓ nscripter     official demo still running after 3s
  ✓ kirikiri      KAG3 displays its first line without script errors
  ✓ bgi           the trial reaches profile 1.669
  ✓ artemis       pfs, ast, runtime and 12 rendered fixture frames
通过 8，失败 0
```

## 各自的构建方式

| 引擎 | 构建系统 | 命令 | 依赖 |
|---|---|---|---|
| ① OnscripterYuri | CMake | `cmake -B build && cmake --build build -j2` | SDL2, SDL2_image, SDL2_mixer, SDL2_ttf, Lua, bzip2, libiconv |
| ② Kirikiroid2 | CMake | 同上 | SDL2, **SDL2_mixer**, jpeg-turbo, libpng, lz4, ffmpeg(swscale) |
| ④ OpenBGI | **自带 Makefile** | `make PLATFORM=darwin -j2` | SDL2 |
| ⑤ Artemis | CMake | 同上 | SDL2；系统框架 CoreText / CoreGraphics / CoreFoundation |

`brew install` 需要的包：`cmake sdl2 sdl2_image sdl2_mixer sdl2_ttf lua jpeg-turbo libpng lz4 ffmpeg bzip2`

（① 的 CMake 在找不到系统 Lua 时会用自带的副本。）

## 这台机器上的几个适配要点

- **`-D_DARWIN_C_SOURCE`**：glibc 风格的 `st_*timespec` 等符号
- **`-liconv`**：macOS 的 `iconv` 在 libiconv，不在 libc
- **keg-only 的 brew 头文件**：通过各引擎的 `platform/compat` 目录兜底
- **OpenCV 4**（如需）：`/opt/homebrew/opt/opencv@4`
- **arm64 没有 `CPUID`**：OpenBGI 用自写的 CPUID 仿真替代

## 已知的非阻塞缺口

1. **③ RealLive / RLVM**：目录里只有上游源码、无构建产物；按指示**不改动**（另一会话负责）
2. **② 台词自动推进**：KAG 的 `window.onPrimaryClick()` 目前不会被调用——
   能进正文、能显示台词、零报错，但每句要手动点一次
3. **⑤ 真实游戏数据**：Artemis 无公开可获取的游戏，五层自检通过但没有真实数据验收
4. **② 音频**：能加载能播放（`totalTime=220.54`、`status=1`），未做长时/音量实测

## 测试数据从哪来

见 `RESOURCES.md`。全部是**官方免费 SDK 体验版**或**公开授权可分发**的工程：

| 数据 | 来源 |
|---|---|
| NScripter 5 套 | NScripter 官方 SDK（nscripter.com） |
| KAG3 框架 | [krkrsdl2/kag3](https://github.com/krkrsdl2/kag3)（改変・配布自由） |
| 完整 KAG 游戏工程 | [CAWorks-ChrisA/kirikiri-kag-ACG](https://github.com/CAWorks-ChrisA/kirikiri-kag-ACG) |
| BGI 体验版 | Lump of Sugar 官方体験版 |

## 目录结构

```
gal-engines/
├── build-all.sh          一条命令构建 + 自检
├── gal/gal               统一管理器（建库/选引擎/启动/验收）
├── STATUS.md             五引擎状态（只写实测）
├── VERIFICATION.md       验收记录（自动生成，含机器与日期）
├── RESOURCES.md          真实数据的来源与现状
├── BUILD.md              本文件
├── OnscripterYuri/       引擎①
├── kirikiroid2-mac/      引擎②
├── rlvm/                 引擎③（未改动）
├── openbgi/              引擎④
├── artemis/              引擎⑤（从零写）
└── testdata/             测试数据
```
