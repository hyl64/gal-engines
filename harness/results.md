# 存读档验证记录（task T3）

自动复现：`./harness/run_savetest.sh`
判据是**行为**而非"文件存在"：变量 `%0=12345` 写入 slot 4 → `SIGKILL`
→ 新进程 `loadgame 4` → 变量值决定播哪个片源 → 日志显示实际走了哪条分支。

## 结论（2026-10-25 实测）

```
$ ./harness/run_savetest.sh
[1/2] save phase ...
      save4.dat 35998 bytes, phase ended: exited
[2/2] load phase (fresh process) ...
      variable %0 was restored: the ok.mp4 branch ran
SUMMARY: PASS save+load round-trip (variable restored across processes)
```

**存读档在 macOS arm64 上是通的**，且是真正的跨进程往返：前一个进程被
`kill -9` 强杀（没有机会做收尾写盘），后一个进程仍然读回了变量。

## 存档落在哪里（三条实测，这条最容易踩坑）

| 场景 | `sav/` 目录 | 行为 |
|---|---|---|
| A 无 `sav/` | 不存在 | 打印 `can't open save file sav/saveN.dat for writing (not an error)`，**只写** `<game>/saveN.dat` |
| B 有 `sav/` | 预先建好 | **双写**：`<game>/sav/saveN.dat`（NScripter 兼容格式）+ `<game>/saveN.dat`（ONScripter 自有格式） |
| C `--save-dir <外部>` | 不存在 | 写到外部目录，`sav/` 那条仍按相对路径尝试并静默失败 |

复现命令：

```bash
cd /tmp && mkdir -p savloc && cd savloc
printf '*define\ngame\n*start\nmov %%0,777\nsavegame 2\nend\n' > 00.txt
B=~/Documents/gal-engines/OnscripterYuri/build/onsyuri
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $B --root . --enc:sjis --window & sleep 3; kill -9 %1
find . -name 'save*.dat'
mkdir -p sav && rm -f save2.dat
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $B --root . --enc:sjis --window & sleep 3; kill -9 %1
find . -name 'save*.dat'      # 现在两份都有
```

**实践含义**：想让存档能跟 Windows 原版互通（NScripter 只认 `sav/` 里那份），
启动前必须 `mkdir -p <game>/sav`。否则玩家看到的存档永远只在 ONScripter 私有
格式里，带回原版读不了。

## 其它已验证的事实

- `savegame N` / `loadgame N` 与右键菜单的 SAVE/LOAD 走同一套
  `writeSaveFile` / `readSaveFile`（见 `ONScripter_rmenu.cpp:463` / `:362`），
  所以命令级验证对玩家点菜单同样成立。
- 存档文件里记录的是**脚本绝对行号**，因此测试剧本两个阶段的布局必须
  逐行对齐；改一行就会让 `loadgame` 落到别处。T3 在
  `testdata/save-probe/00.txt` 头部留了同样的警告。
- 引擎会在游戏目录顺手写 `envdata`（环境变量/设置）、`stdout.txt`、
  `stderr.txt`；`gloval.sav`、`kidoku.dat` 等只在用到对应命令时才出现。
- 存读档不依赖视频后端：上面这轮验证跑在**含视频后端的 canonical
  build** 上，日志里没有任何 `video:` 行，说明无影片路径未被改动。

## 没有做到的（诚实记录）

- **GUI 菜单点击路径**没有自动验证。要走到 `rmenu → SAVE → 槽位 → Yes`
  需要往引擎注入 SDL 事件；T3 尝试过用 `libons_driver.dylib` interpose
  `SDL_WaitEventTimeout`，没跑通就中断了，半成品已删除。命令级路径与它
  共用同一套存档读写函数，所以风险低，但不是零。
- **真实商业游戏的存档兼容性**没有测：库里目前没有带存档的商业作品数据。
  等有数据时用同一 harness（`./harness/run_savetest.sh <game_dir>`）补测。
