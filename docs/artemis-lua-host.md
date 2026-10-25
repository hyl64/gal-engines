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
