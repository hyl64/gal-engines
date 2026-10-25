# 引擎③ RealLive → RLVM

上游 [rlvm](https://github.com/eglaysher/rlvm) 2015 年前后停止可构建；这里的分支把它
在现代工具链上救活，并在 Apple Silicon 上跑通真实商业作品。

## 现状（实测）

| 项 | 结果 |
|---|---|
| 构建 | `scons --release --no-bundle`，Apple clang + Homebrew |
| 单元测试 | **260/260 通过**（`build/rlvm_unittests`） |
| 真实数据 | CLANNAD Full Voice（中文化，231 个剧本，4 GB 资源）：**231 个剧本全部解析** |
| 渲染 | 标题画面、立绘、文字、中文台词、系统菜单 |
| 音频 | BGM 可播放 |
| 空闲开销 | 0.4% CPU、8.7 MB RSS（静态场景）|
| 打包 | `rlvm.app` 自包含（6 个 dylib 带进 `Contents/Frameworks`）+ DMG |

技术路线：SDL 1.2 API 由 **sdl12-compat** 提供，底层是 SDL3，渲染走 GL-on-Metal 桥。
截图与性能记录在 [rlvm/README.md](../rlvm/README.md)、[rlvm/PERF.md](../rlvm/PERF.md)。

## 与构建有关的两个坑

1. **`--no-bundle`**：组装 `.app` 会用 `install_name_tool` 改写 Homebrew dylib 的 load
   command，而 Xcode 15 的版本拒绝处理「已带签名」的库（`file not in an order that can
   be processed`）。这是打包步骤，不是构建步骤，所以放在开关后面；`build-all.sh` 与 CI
   用 `--no-bundle`，只验证可执行文件与单元测试。
2. **单元测试要在自己的目录里跑**：它们按相对路径读 `test/` 下的夹具，换个 cwd 会大面积
   失败——那不是回归。

## 数据

本仓库不含任何游戏数据。`build-all.sh` 对 RLVM 的检查是 260 个单元测试，**不需要游戏**；
要跑真实作品，请自备一份含 `Seen.txt` 与 `Gameexe.ini` 的目录。获取与校验方式见
[DATA.md](DATA.md)。
