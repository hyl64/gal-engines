# 找真实游戏数据的路子（2026-10-25 实探）

目标要求「每个引擎跑通至少一款真实游戏数据」。官方免费体验版与自由发布作品是正当来源。
这一轮去探了，结论：

## 已经拿到的

| 引擎 | 数据 | 来源 |
|---|---|---|
| ① NScripter | 5 套 | NScripter 官方 SDK（nscripter.com，免费） |
| ② KiriKiri | KAG3 3.32 框架 + 示例剧本 | [krkrsdl2/kag3](https://github.com/krkrsdl2/kag3)（W.Dee 原作，改変・配布自由） |
| ④ Ethornell | 《遥か碧の花嫁に》体验版 | Lump of Sugar 官方体验版 |

## 今天下不了的（都值得之后再试）

| 站点 | 状态 | 上面有什么 |
|---|---|---|
| [ふりーむ！](https://www.freem.ne.jp/win/game/7437) | **维护中**（返回 maintenance 页） | *Alice in Dreamworld 体験版* —— 制作工具标明「吉里吉里」，82 MB |
| [Vector](https://vector.co.jp/soft/dl/winnt/amuse/se503791.html) | **维护中**（10/22～未定） | *オービタルレインボウ-体験版-*（吉里吉里）、*ごらんょ!* 等 |
| [dimguilgames 体验版集](https://dimguilgames.jp/trial/) | 可访问 | 列出两个吉里吉里体验版（オービタルレインボウ、街痛），下载在作者页 |

## 有用的新发现

**[Kirikiri SDL2](https://krkrsdl2.github.io/krkrsdl2/en/)** —— 一个已经存在的
KiriKiri Z 的 macOS/Linux SDL2 移植。两点价值：

1. **参考实现**：它已经把「在 macOS 上跑 KiriKiri」做过一遍，卡住时可以对照
2. 它的 KAG3 分支就是我们正在用的框架来源
3. 官方明确说**不支持未修改的商业游戏**（建议用 Wine），说明「跑商业游戏」确实是难点

## 下一步（数据到手时）

拿到任意一个 `data.xp3`（或解开的游戏目录）后：

```bash
./gal/gal add <游戏目录>
./gal/gal verify <名字>          # 判定
./gal/gal play <名字> --dump x.bmp   # 看画面
```

引擎② 现在**能直接从 `data.xp3` 启动**（第 101 轮），所以真实游戏的数据形态已经支持。

## 2026-10-25 下午复测

- Vector 仍在维护（maintenance_php_2026.html）；freem 的 game/7437 已下架（重定向回首页）；
  freegame-mugen（夢現）的吉里吉里分类健在，但其下载全部指向外部站
  （Vector / fc2 / boy.jp），实测均不可达；dimguilgames 试炼页可达，两个吉里吉里
  体验版（オービタルレインボウ、街痛）的包仍藏在深层 frame 里且外链站点同样失联。
- 结论：今天「外部新资源」拿不到。改用**仓库内已有真实 KAG 工程**（kag-game）按
  真实发行形态做全程验收：tests/pack-game-xp3.py 把它打成 data.xp3，
  `krkrrun data.xp3` 从归档直接启动跑完整章——这正是玩家拿到的数据形态。
  固化为 `tests/run-kag-story.sh`（文件夹 + 归档双形态，接进 build-all --check）。

## 2026-10-25 下午：拿到第二款真实游戏（官方演示作品）

外部站点今天基本不可达（archive.org DNS 被污染指向 23.13.x、itch.io 连接被拒、
Vector 维护中、freem 下架、dimguilgames 外链全死），但 **GitHub 全链路可用**。
据此拿到了正规来源的第二款真实吉里吉里游戏：

| 项 | 内容 |
|---|---|
| 名称 | 吉里吉里SDL2・KAG3・デモンストレーション |
| 来源 | https://github.com/krkrsdl2/krkrsdl2-kag3-demo （演示数据） + https://github.com/krkrsdl2/kag3 （KAG3 系统，分支 krkrsdl2） |
| 许可 | 与 KAG3 相同（Copyright (C)2001-2009 W.Dee and contributors，改変・配布は自由） |
| 落盘 | `testdata/krkrsdl2-demo-game/`（kag3 系统文件 + `data-demo/` 合并，78 文件） |
| 形态 | 640×480、JPEG 背景、TLG 消息框、crossfade 转场、TJS 雪花插件（Timer 驱动）、超链接菜单 |
| release 资产 | `data.xp3` 是 65 字节占位，不可用；按仓库 `build.sh` 的配方本地合并得到可运行目录 |

复现：

```bash
cd testdata/krkrsdl2-demo-game
KRKR_LOOP_SECONDS=8 SDL_VIDEODRIVER=dummy \
  ../../kirikiroid2-mac/build/krkrrun startup.tjs --screenshot /tmp/menu.bmp
# 验收脚本（已并入 build-all --check）：
../../kirikiroid2-mac/tests/run-demo-game.sh
```
