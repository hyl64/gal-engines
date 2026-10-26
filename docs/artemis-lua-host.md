# Artemis Lua 宿主：规格与工作量评估（2026-10-26）

第二部作品（pf8，`artemis-raspberry-web`）的 UI 由 Lua 驱动：`ui.asb` + 53 个 `.lua`
+ `psw/ui/**`。要让它出画面，宿主必须内嵌一个真 Lua 解释器并实现引擎 API。
本文用**留存的真实样本**给出 API 面、全局状态模型与工作量分解。

样本：`testdata/artemis-raspberry-web/samples/system_ui_title.lua`
（标题画面的完整初始化逻辑，约 100 行有效代码）。

## 观测到的 API 面（真实调用，非猜测）

从样本中提取的**引擎函数**（游戏 Lua 自己定义的 title_* 等不含）：

| 类别 | 函数 |
|---|---|
| 求值/状态 | `get_eval(expr)`、`tn(v)`（转数字助手）、`ResetStack`、`vartable_init` |
| 全局状态 | `gscr`（表：scene/clearflag/allclear/…）、`flg`（表）、`sv`（表：checkopen/delpoint/load） |
| 界面控制 | `estag`、`flip`、`message`、`ui_message`、`uitrans`、`screen_crop`、`mulpos` |
| 按钮 | `csvbtn3`、`btn_active`、`delbtn`、`setBtnStat`、`getBtnID`、`getBtnInfo`、`setonpush_ui` |
| 音频 | `se_ok`、`se_cancel`、`sysvo`、`title_bgm` |
| 键盘 | `allkeyoff`、`allkeyon`、`key_reset` |
| 系统 | `patch_check`、`get_language`、`get_trial`、`get_savedatatime`、`getScriptStartup`、`getTitle`、`getExtra`、`loading_off`、`reset_backlog` |
| 流程 | `adv_config`、`adv_exit`、`adv_flagreset`、`adv_info`、`adv_load`、`adv_qload`、`titlecall` |

**规模**：单文件 ~70 个调用；53 个 .lua 全量后引擎函数预计 **80–120 个**。

## 全局状态模型

```
gscr  全局游戏状态（场景、clearflag、每角色完成标记…）
flg   局部标记表
sv    存档视图（槽位检查/删除/读取）
g     求值命名空间（get_eval("g.rur") 直接读引擎变量）
```

宿主必须让 Lua 侧的 gscr/flg 改动**持久回引擎变量区**（`get_eval`/`set_eval` 对偶）。

## .asb 容器

`ASB\0` 魔数 + 版本 + 计数 + 字符串表（`calltag`、`wt`…）——标签/回调的打包格式。
宿主要能解出 handler 名并绑定到 Lua 函数。

## 工作量分解（诚实评估）

| 阶段 | 内容 | 估时 |
|---|---|---|
| 1 | 内嵌 Lua 5.1 解释器（vendor lua-5.1 源码，C API 绑定） | 1–2 轮 |
| 2 | 状态对偶（gscr/flg ↔ 引擎变量区，get_eval/set_eval） | 1 轮 |
| 3 | UI 类 API（按钮/界面/翻转）×~30 | 2–3 轮 |
| 4 | 音频/系统/流程类 ×~30 | 2 轮 |
| 5 | .asb 容器解析 + handler 绑定 + ui.asb 加载 | 1–2 轮 |
| 6 | 联调：标题画面出图（`title_init` → `title_anime` → 立绘/LOGO 上屏） | 2 轮+ |

**总计约 9–12 轮**（每轮=一次专注会话），且阶段 6 的不可控因素在素材格式
（.psb 立绘——三套语料里数量为 0，只能按规格实现）。

## 结论

- 宿主是**可行但独立**的工程项目（与 T3 对 NScripter C3 的评估同级）。
- 本仓库已交付：API 面规格（本文）、真实样本、.asb 魔数与结构首查。
- 在宿主完成前，第二部作品的验收标注为「格式全覆盖 + UI Lua 规格就绪」，
  不标注「可玩」。


## 阶段 1 已完成（2026-10-26）：Lua 5.1.5 内嵌 + 真实游戏 Lua 执行

`artemis/tools/luahost.cpp`：vendor 的 Lua 5.1.5（`artemis/third_party/lua/`，arm64
构建通过）、引擎 API 桩（按本文 API 面）、全局状态表（`ONS_LUA_TABLES` 名单，自动
生成 metatable）、`get_eval` 已实现为**真正的路径游走**（状态对偶的雏形）。

实测：pf8 标题画面的 `system_ui_title.lua` 加载成功，`title_init` 执行到第 57 行，
途中引擎桩被真实调用（`get_eval`×4、`tn`×4、`message`×1）。

**边界**：第 57 行需要 `scr.mw` 是表——那由另外 52 个 .lua 在前面的加载顺序里填充。
留存样本只有 1/53；按 `download.md` 配方重下 pf8 全量后，把 53 个文件按序加载，
`title_init` 应能跑完（这就是阶段 2 的第一项）。

**驱动技巧**：缺失的全局表名可以通过解析
`attempt to index global 'X' (a nil value)` 逐个发现并加入 `ONS_LUA_TABLES`——
本轮就是这样发现 `scr` 的。


## 阶段 2 进行中（2026-10-26）：51/54 个 Lua 按序加载，title_init 到第 64 行

`luahost` 新增：`ONS_LUA_ORDER`（按 `system_loadinglua()` 的顺序逐个
`luaL_dofile`）、`ONS_LUA_ROOT`（归档根）、`e` 对象（冒号方法桩，__index 发放
具名闭包——真实调用被追踪为 `e`）。

实测：**51/54 个 Lua 文件加载成功**（3 个失败是顺序提取的杂质条目），
`vartable_init()` 运行（游戏状态表 scr/vari 创建），`title_init` 推进到第 64 行。

**第 64 行的边界**：`csvbtn3("ttl1","500",csv["ui_title"])` 需要 `csv` 表里的
按钮定义数据——由引擎把 `system/table/*.tbl`（Lua 赋值文件）解析进 `csv` 全局表。
宿主还没有实现这个 csv 数据层（这也解释了之前 "got string" 的错误）。

**阶段 2 剩余**：csv 数据层（解析 tbl → csv 表）→ title_init 跑完 →
阶段 3 的 UI 类 API 实装。


## 阶段 2 完成（2026-10-26）：pf8 标题的完整启动链跑通，title_init 正常返回

引擎的启动序列全部在宿主里执行：

```
init.lua → system_loadinglua()（54 个文件经真实的 e:include、game 全局、
           平台探测、list_wasm.tbl）→ system_initialize()（vartable_init、
           storage_path→set_uipath、字体、键位）→ title_init()
```

`title_init` 完整执行（csv["ui_title"]、按钮表、screen_crop、
title_load2info……），exit 0，**872 次引擎调用**。

宿主实装（非桩）：`e:include`、`e:var`（屏幕尺寸/game.os）、
`e:getScriptStack`、`get_language`、`get_eval`。其余 API 留给阶段 3。

过程中修掉的五个真实缺陷（每次都付出了崩溃的代价）：
1. 桩返回 false 而非 0（Lua 里 0 是真值，getTrial() 误入体验版分支）
2. `luaL_dofile` 失败后必须 pop 错误消息（否则栈失衡，csv 被赋成错误文本）
3. `lua_upvalueindex` 超过闭包 upvalue 数是越界访问
4. auto-vivify 破坏 `conf.language or default` 链——conf 用普通空表
5. 各平台 tbl 定义同名全局——全加载会让 readdir 顺序决定平台，只载 wasm

**阶段 3**：title_init 里 e:tag 发出的精灵命令（bg01/char/logo/trial 等，
`csv["ui_title"]` 里有完整定义）接到真实渲染——出画面。


## 阶段 3 达成（2026-10-26）：标题画面渲染成功

`luahost` 实装渲染器：
- `e:tag{"lyc", id=..., file=...}` → pngio 加载 png 入层表（`:bg/black` 为内置纯色）
- `e:tag{"lyprop", ...}` → 位置（x/y/left/top）、clip（"x,y,w,h"）、alpha、visible
- `e:tag{"lydel", id=...}` → 删除
- 运行结束：全部可见层按插入序（=z 序）合成到 1280×720 画布，alpha 混合，
  `ONS_RENDER=<path>` 写 PNG

**结果**：pf8 web 体验版的标题画面从它自己的启动链渲染出来——
init.lua → system_loadinglua（54 文件）→ system_initialize → title_init，
775 个 e:tag 命令，合成图即标题美术：四角色立绘、
ラズベリーキューブ LOGO（体験版徽章）、波点背景。

证据：`artemis/screenshots/pf8-title-rendered.png`。

已知小瑕疵：内置 `:bg/black` 底层与 png alpha 的 z 序——左上 UI 有一部分
透出（应为黑色衬底）。属化妆级问题，下一轮修。

**目标 4 的「出画面」达成**。


## 阶段 4（进行中）：标题可交互

`ONS_CLICK="x,y"` 命中测试 lyevent 绑定 → 调用游戏自己的
btn_over（设 btn.cursor）→ btn_click（执行按钮）——与真实鼠标一致的链路。

点击 bt_start (130,652)：游戏状态机推进——按钮焦点切换、开始面板出现、
背景转场（1570 采样点差异，帧哈希完全改变）。

实现的两条引擎事实：
1. 层树：对逻辑父层（500.d.2，从未 lyc）的 lyprop 会传递给子层
   （500.d.2.0，可点击切片）
2. 游戏函数是冒号风格：btn_over(e, param) 需要 e 作为 self

遗留：btn_over 内部的 se.lua 音效路径需要音量状态（阶段 5 的音频桥）。
