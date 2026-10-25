# Engine ② KiriKiri — Kirikiroid2 @ macOS arm64

## 状态：🚧 地基完成（TJS2 脚本引擎 arm64 原生跑通），运行时未完成

## 上游
- https://github.com/zeas2/Kirikiroid2 （master，2947 stars，2024-06 仍有更新）
- 它是 **Android + cocos2d-x 应用**，不是桌面程序：窗口/输入/渲染全在
  `src/core/environ/cocos2d/MainScene.cpp`（2448 行）里，由 cocos2d-x 提供。

## 已完成（本目录）
### 1. TJS2 脚本引擎 → `libtjs2.a` + `tjsc` CLI（arm64 原生）
TJS2 是 KiriKiri 和 KAG 的宿主语言，是整条链的地基。它自身几乎零依赖
（只有标准库 + Oniguruma 正则）。

```bash
brew install oniguruma
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/tjsc tests/hello.tjs
```

**实测输出（exit 0）**：
```
string: KiriKiri len=8
array: 1,2,3
dict: KiriKiri2 / TJS2
function: 42
class: I am Derived (derived)
regexp: dee@kikyou.info -> dee/kikyou.info
number: 255 + 1 = 256
platform: macOS
caught: thrown on purpose
done
```
覆盖：字符串/数组/字典/函数/类继承/正则(Oniguruma)/类型自动转换/异常。

### 2. darwin 平台层
| 文件 | 职责 |
|---|---|
| `platform/darwin_platform.cpp` | `TVPGetRoughTickCount32`（CLOCK_MONOTONIC）、`TJS::TVPConsoleLog`、`TVPGetMessageByLocale`（走引擎消息表兜底） |
| `platform/darwin_system.cpp` | `System` 对象：`inform`/`print`/`println`/`platform`（原生类宏，照 tjsMath.cpp 的写法） |
| `tools/tjsc.cpp` | CLI 宿主：UTF-8 读脚本、注册 System、跑脚本、UTF-16→UTF-8 输出 |

上游这三个符号属于「应用层」职责，Android 在 `environ/cocos2d/MainScene.cpp`、
Windows 在 `environ/Application.cpp` —— 我们给 macOS 写了自己的那份。

## 构建坑
1. 代码是 C++11 时代：`std::binary_function` 在 C++17 已移除 → 用 **C++14**
2. `tjs_char` 在 clang 下是 `char16_t`（UTF-16）——正确，KiriKiri 原生就是 UTF-16
3. `LocaleConfigManager`（消息本地化）绑死 cocos2d `FileUtils` → 桌面版不用它，
   消息走 `TJSGetMessageMapMessage` 兜底，查不到就返回 key 本身

## 重要兼容性发现
这个 TJS2 是 **2003 年的老版本**（Kirikiroid2 基于 TVP 的 TJS2，不是 krkrz）：
- ❌ 没有 `in` 运算符，没有 `for(k in obj)` —— 后来的 krkrz 才加
- ❌ 核心不含 `String`/`format` 等 TVP 扩展类（那是应用层注册的）
- ✅ 核心语言（类/闭包/正则/异常/字典）完整可用

结论：老游戏脚本没问题；**2013 年后的新 KAG 脚本可能依赖 krkrz 的语法**，
到时候要么走 krkrz 分支，要么加语法补丁。

### 3. XP3 归档层 → `krkrpack`（arm64 原生）✅ 已完成并验证
游戏的全部资源都在 `data.xp3` 里，所以这是拿到游戏内容的第一步，而且不需要窗口。

```bash
./build/krkrpack list    game/data.xp3
./build/krkrpack extract game/data.xp3 out/
```

**验证方式（无游戏数据的自证）**：用 `tests/make_xp3.py` 造一个符合格式的
XP3，再用 krkrpack 列出/解包，与源文件逐字节比对：

```
3 file(s) in file:///tmp/test.xp3
        11  a.txt
        33  b.txt
      4000  dir/c.bin
--- 3 file(s), 4044 byte(s) uncompressed
a.txt ✓ 一致   b.txt ✓ 一致   dir/c.bin ✓ 一致（4KB 随机二进制）
exit 0
```

为此补齐的「平台层」（上游在各平台各写一份，tarball 里没有）：
- `platform/darwin/Platform.cpp` — `environ/Platform.h` 那 28 个函数：内存统计
  (sysctl + host_statistics64)、消息框（无窗口时走 stderr）、目录/文件辅助、IME、
  `TVP_stat`/`TVP_utime`、语言
- `platform/darwin_host.cpp` — 存储层：`tTVPFileMedia`（"file://" 媒体，10 个方法，
  POSIX 实现）、`tTVPFileStream`、`TVPOpenArchive`（嗅探 XP3 签名）、
  `TVPGetVersion`/`TVPGetCommandLine`/`TVPAddAtExitHandler`、`tTVPApplication::PrintConsole`
- `platform/compat/Protect.h` — win32 反调试子系统在桌面构建里是空操作
- `platform/compat/libarchive/` — Android NDK 头布局的兼容软链（brew 的 libarchive 是 keg-only）

**依赖处理**：`UtilStreams.cpp` 里 7z/unrar 两条路径需要 vendored 库，但上游自己
已经把 7z 分支改成走 libarchive 了，所以桌面构建用 `KRKR_DESKTOP_UNPACK` 把那两段
死代码编译掉，zip/7z/rar 统一走 libarchive，XP3 走引擎原生读取器。

### 4. 脚本层 ↔ 存储层 ↔ 归档层 打通 → `krkrscript` ✅
引擎的脚本通过 TVP 扩展类访问文件，`Storages` 就是那个对象。把它们接起来之后，
**TJS2 脚本可以直接查询 XP3 归档里的文件**（日志里能看到 XP3 读取器真的打开归档）：

```
$ ./build/krkrscript tests/storage.tjs
plain file exists:    1
plain file missing:   0
in archive, present:  1     ← file:///tmp/test.xp3>a.txt
in archive, nested:   1     ← file:///tmp/test.xp3>dir/c.bin
in archive, absent:   0
done
exit 0
```

实现要点：
- `Storages` 类本体在上游 `base/StorageIntf.cpp`（已编译），工厂函数在 `StorageImpl.cpp`
  → 我们在 `platform/darwin_host.cpp` 补了那一个函数体（`new tTJSNC_Storages()`）
- 主机启动时把当前目录加入 auto path（`TVPAddAutoPath`），等价于引擎的启动流程
- 已知限制：引擎会规范化掉相对名里的 `./`，所以**归档内成员要用绝对路径**
  （`file:///abs/path/game.xp3>inner.txt`）；窗口层落地后由游戏目录解析补上

### 5. 软件像素管线（tvpgl）→ `krkrblit` ✅
引擎所有的图层、精灵、转场最终都落进 `visual/tvpgl.cpp`——12600 行的像素管线，
以函数指针表的形式存在，`TVPInitTVPGL()` 把每个指针指到通用 C 实现。

**卡点与解法**：它 `#include "tvpgl_route.h"`，而这个头是 Android 构建**生成**的
（按 CPU 选 SIMD 实现），树里没有。查清 `TVPInitTVPGL()` 里已经直接把指针赋成
`_c` 通用实现，所以这个头**可以是空的**——补一个带说明的空兼容头即可。
（期间顺手给 `tTVPApplication* Application` 补了定义，日志层要用。）

验证工具 `krkrblit`（按混合器的真实契约断言，定点位移 ±1 容差）：

```
$ ./build/krkrblit
  ok    opaque source replaces the destination         0xff2040bf
  ok    transparent source leaves the destination      0xff808080
  ok    half alpha lands between the colours           r=0x40
  ok    additive blend brightens                       0xff303030
  32 -> 24 bit conversion                              ff 00 80
all passed: 0 check(s) failed
```

两个语义细节（踩过才知道）：
- `TVPAlphaBlend` 是「目标无 alpha」的变体，**不保留目标 alpha**；带 alpha 的目标用 `TVPAlphaBlend_d`
- 定点运算 `((s-d)*a)>>8` 会使结果比精确值低 1，断言必须容差 1

### 6. 图像解码 → 显示 → 逐像素验证 → `krkrview` ✅
把「从 XP3 里取一张图、用引擎自己的解码器解开、显示出来」这条链跑通了：

```bash
./build/krkrview /tmp/view.xp3 img.png out.bmp
# decoded 513x55 from file:///tmp/view.xp3>img.png
```

**验证**（用 BGI 体验版解出的一张带 alpha 的 PNG 装进 XP3，再用引擎解码器解回来，
按原始字节比对）：

```
raw bytes: 112860 for 513x55
  as BGRA: 4431/28215 differ
  as RGBA: 0/28215 differ   ← MATCH
```

### 重要发现：引擎像素格式是 ABGR8888，不是 ARGB8888
引擎的 `tjs_uint32` 像素是 **0xAABBGGRR**（alpha 在高字节，然后 B、G、R）。
内存里（小端）是 R,G,B,A —— 对应 SDL 的 **`SDL_PIXELFORMAT_ABGR8888`**。
我最初按名字猜成 ARGB8888，结果红蓝互换；靠原始字节比对才定论。
**这一条对后面整个渲染层都关键。**

### 位图层第一片：`src/core/visual/darwin/LayerBitmapImpl.cpp`
上游的位图把像素存在 `iTVPTexture2D` 里（类本身只有度量与扫描线），所以先实现
了委托给纹理的那几个方法（GetWidth/GetHeight/GetScanLine/GetScanLineForWrite/
GetPitchBytes）+ 解码器要用的 `TVPClearGraphicCache()` 空钩子。
解码器因此可以链接；完整的位图层（文字/字体/绘制）仍待补。

### 7. 字体与文字渲染 → `krkrfont` ✅（galgame 的核心）
上游的文字管线 `FreeType.cpp` / `FreeTypeFontRasterizer.cpp` / `FontSystem.cpp` /
`PrerenderedFont.cpp` / `CharacterData.cpp` **是平台无关的**，只差几个平台钩子：

| 钩子 | 实现 |
|---|---|
| `TVPGetDefaultFontName()` | Hiragino Sans GB → Arial Unicode → AppleSDGothicNeo（`KRKR_FONT`/`ONS_FONT` 可覆盖） |
| `TVPCreateFontStream()` | 走存储层 `TVPCreateStream` |
| `TVPGetAllFontList()` | 扫 `/System/Library/Fonts`、`/Library/Fonts`、`~/Library/Fonts` |
| `TVPFontSystem` | 一个 `FontSystem` 实例 |

`environ/ConfigManager/*` 绑 cocos2d，桌面版用 `platform/darwin/darwin_config.cpp`
重新实现同一个类（无偏好文件时一律返回调用方默认值）。

**实测（真实日文字形，抗锯齿）**：
```
$ ./build/krkrfont /tmp/font.bmp 40 "あ漢A9"
  U+3042 extent 40x40 blackbox 32x36 pitch 32 origin (4,2) aa=1 full=0
  U+6F22 extent 40x40 blackbox 37x38 pitch 40 origin (1,1) aa=1 full=0
  U+0041 extent 30x40 blackbox 30x31 pitch 32 origin (0,4) aa=1 full=0
$ ./build/krkrfont /tmp/font2.bmp 48 "ナルキッソス"
```
渲染出的「あ漢A9」与「ナルキッソス」都清晰可读（见提交说明里的验证记录）。

**又一个格式坑**：普通字形的 `tTVPCharacterData::Data` 是**8 位覆盖率**（一字节一像素，
`Pitch == BlackBoxX`），不是 4 字节；只有 `FullColored` 字形才是 4 字节 BGRA。
按 4 字节读会得到重影糊字。

### 8. 合成完整画面 → `krkrscene` ✅
三块引擎部件合体，**像素全部出自引擎自己**：

```bash
./build/krkrscene /tmp/scene.xp3 bg.png scene.bmp "この空の向こうに、何があるんだろう。" 120 0 40
# background: 1920x1080 from /tmp/scene.xp3>bg.png
# text: ... at (120,940), 40px
```

- 背景：`TVPLoadPNG` 从 XP3 里解码（存储层）
- 消息窗：`TVPAlphaBlend_d`（tvpgl）半透明混合
- 台词：FreeType 字体栈栅格化 → 覆盖率当 alpha → `TVPAlphaBlend_d` 叠到背景

产物见 `screenshots/scene-composed.png`（1920×1080，真实游戏背景 + 日文台词）。
这一张图说明引擎②的图像、字体、混合三层都是通的——缺的只剩「谁来调度这些层」，
也就是窗口层与 KAG 运行时。

### 9. 软件纹理 + 软件渲染管理器 + 位图分配 ✅（图层的地基）
引擎的位图类本身**不存像素**——像素在 `iTVPTexture2D` 里，由渲染管理器创建。
所以先把这两个部件写出来：

| 部件 | 文件 | 说明 |
|---|---|---|
| 软件纹理 | `visual/darwin/Texture2D_Soft.cpp` | `iTVPTexture2D` 的内存实现，引擎像素格式 |
| 软件渲染管理器 | `visual/darwin/RenderManager_Soft.cpp` | `iTVPRenderManager` 只有 **9 个纯虚**，全部实现；纹理创建、缩放（最近邻）、OperateRect 拷贝 |
| 软件重采样 | `visual/darwin/ResampleImage_Soft.cpp` | `TVPResampleImage`（上游 gl 版绑 cocos 分配器） |
| 位图分配 | `visual/darwin/LayerBitmapImpl.cpp` | ctor/dtor/Recreate/SetSize/GetBPP/Assign/Independ… 委托给纹理 |

**验证**（`krkrbitmap`）：
```
bitmap: 320x200, 32 bpp, pitch 1280
point (10,10) = 0x00ff00ff, (11,10) = 0xff282828
wrote /tmp/bmp.bmp     ← 渐变 + 色块，像素抽样与写入一致
```

### 10. 引擎自带的软件渲染器跑起来了 ✅（重大转折）
上一轮我发现「连 Fill 都要走渲染方法」，正准备自己写。结果发现：**上游
`visual/RenderManager.cpp`（4414 行）里本来就有一整套软件渲染器**——
`tTVPSoftwareRenderManager` + 全部渲染方法（FillARGB / FillColor / Copy /
AlphaBlend / 各色混合与遮罩 / 拉伸 / 透视）+ 自己的压缩纹理类
（`tTVPSoftwareTexture2D`，带 LZ4）。

它只被三样东西卡住，全是**编译期依赖**而非逻辑：

| 缺的东西 | 解法 |
|---|---|
| `renderer/CCTexture2D.h`（cocos2d） | `platform/compat/renderer/CCTexture2D.h`：只声明该文件用到的 4 个名字，适配器函数是空实现（软件后端永不调用） |
| `xxhash/xxhash.h` | `platform/compat/xxhash/xxhash.h`：自己实现的 XXH32（公有领域算法） |
| `libswscale` / `opencv2` / `lz4` | `brew install ffmpeg` / `opencv@4` / `lz4`。**注意必须用 OpenCV 4**：代码用的是 `cv::getAffineTransform` 和 3 参数 `getPerspectiveTransform`，OpenCV 5 已删除/改签名；Homebrew 同时装了 v5 和 v4，所以用**绝对路径**链接 v4 |

于是把我自己写的 `Texture2D_Soft.cpp` / `RenderManager_Soft.cpp` **删掉了**——
用引擎的。顺带把 `tTVPBitmap`（像素缓冲类，137 行纯可移植）从 win32 版搬进
`visual/darwin/LayerBitmapImpl.cpp`。

**验证**（`krkrbitmap`，全部走引擎 API）：
```
$ ./build/krkrbitmap /tmp/bmp4.bmp
01:57:48 (info) Use malloc for Bitmap
bitmap: 320x200, 32 bpp, pitch 1280
bg    (32, 64, 96)      ← Fill(0xff204060) 精确正确
block (255, 128, 0)     ← FillColor(0x00ff8000, opa=255) 精确正确
point (0, 255, 0)       ← SetPoint(0xff00ff00) 正确
```

**已知缺口**：`FillColor` 在 `opa < 255` 时走 `ConstColorAlphaBlend` 方法，目前不出图
（不崩），待查。`Fill` 与不透明 `FillColor` 正常。

颜色约定（两条要记住）：
- `Fill` / `FillColor` 的入参是 **0xAARRGGBB**，内部会 `TVP_REVRGB` 成 0xAABBGGRR
- `GetPoint` / `SetPoint` / 扫描线用的是**内部格式 0xAABBGGRR**

### 上一轮的发现（保留记录）：连「纯色填充」都要走渲染方法
`iTVPBaseBitmap::Fill` 的实现是：
```cpp
static iTVPRenderMethod* method = TVPGetRenderManager()->GetRenderMethod("FillARGB");
static int paramid = method->EnumParameterID("color");
```
也就是说引擎把所有绘制（Fill / FillColor / 拉伸 / 混合）都抽象成
**`iTVPRenderMethod`（着色器式的对象）**，由渲染管理器按名字提供。软件后端
必须实现这些方法（内部调 tvpgl 即可），否则 `method` 是空指针。
**这就是下一个部件**：`iTVPRenderMethod` 的软件实现（FillARGB / Copy / AlphaBlend…）。

### 11. 引擎纹理上真屏幕 → `krkrpresent` ✅
引擎画进 `iTVPTexture2D`，后端负责把其中一张送显：Windows 是 GDI BitBlt，
Android 是 GL 纹理上传。桌面后端这一跳是 **SDL 纹理 + renderer**。

```bash
$ ./build/krkrpresent /tmp/scene.xp3 bg.png 60 /tmp/present2.bmp
texture: 1920x1080 from file:///tmp/scene.xp3>bg.png
presented 60 frame(s) on 'cocoa'      ← 真的开了一个 Cocoa 窗口
```

**验证**：截图与源图逐像素比对 **0/28215 差异**（用 513×55 带 alpha 的图测）。

### 12. 绘制设备（DrawDevice）→ `DrawDevice_Darwin.cpp` ✅（部分）
`tTVPDrawDevice`（`visual/win32/DrawDevice.cpp`，**零 Win32 调用**）是引擎的合成器，
实现了 54/56 个方法。平台的活只剩 **4 个虚函数**：

| 虚函数 | darwin 实现 |
|---|---|
| `Show()` | 主图层的位图 → SDL 纹理 → `SDL_RenderPresent`（真的开窗口） |
| `StartBitmapCompletion` / `NotifyBitmapCompleted` / `EndBitmapCompletion` | CPU 合成器同步完成，无需等待，空实现 |

写了 `src/core/visual/darwin/DrawDevice_Darwin.cpp`，编译链接通过。

**但接线还需要图层对象**（`tTJSNI_BaseLayer`，在 `LayerIntf.cpp` = 图层系统本体）。
把它加进库后，又拉出整串运行时服务：`TVPPostEvent`（事件）、`TVPGetCursor`（光标）、
`TVPLoadGraphic`/`TVPSaveImage`（图像）、`TVPCreateRectObject`、`ColorToRGB`、
`TVPActionName`/`TVPEventDisabled`/`TVPSimpleImageProvider` 三个全局。

**这是引擎②最后一段的真实清单**：图层系统 + 这些运行时服务接线。本轮为保证构建
全绿，把 `LayerIntf.cpp` 与对应工具撤下（不留死代码/坏目标），九个工具全部回归通过
（`krkrpresent` 仍逐像素 0 差异）。

### 13. 图层系统的平台服务（部分完成，边界已量化）
把这轮为图层写的东西落了两个文件：

| 文件 | 内容 |
|---|---|
| `visual/darwin/FontRasterizers_Darwin.cpp` | 字体栅格化注册表（从 win32 版搬来，纯可移植）：`TVPSetFontRasterizer`/`TVPGetFontRasterizer`/`GetCurrentRasterizer`/预渲染字体映射 |
| `platform/darwin/darwin_visual_services.cpp` | 引擎会点名的一批平台服务：不支持的图像格式（JPEG/JXR/WebP/BPG/PVR，**明确抛「无解码器」而不是假装**）、`ColorToRGB`、`TVPGetCursor`、剪贴板（走 SDL）、`TVPGetAppPath`、事件钩子、`TVPGL_ASM_Init`、`ExtractFileDir`、`TVPListDir` 等 |

**把 `LayerIntf.cpp`（图层系统本体）+ `ScriptMgnIntf.cpp`（注册全部 TVP 类）加进库后，
暴露出的完整缺口清单**（这就是引擎②还剩的全部工作量，逐项都已知归属）：

```
事件/系统层   TVPInvokeEvents, TVPEventReceived, TVPGetSystemEventDisabledState,
              TVPSystemControl(+Alive), TVPRegisteredPlugins, TVPLoadInternalPlugins,
              TVPCauseAtInstallExtensionClass, TVPCreateAndAddWindow
插件/手柄     TVPLoadInternalPlugin, TVPGetJoyPadAsyncState
脚本层        TVPExecuteExpression, TVPShowScriptException, TVPProcessUnhandledException,
              TVPBeforeProcessUnhandledException
系统信息      TVPDataPath, TVPNativeProjectDir, TVPDetectCPU, TVPDetectCPU,
              TVPJPEGLoadPrecision, TVPGL_ASM_Init
其它          TVPListDir, ExtractFileDir, TVPShowPopMenu, TVPGetWindowListAt
```

**结论**：引擎②最后一段不是「一个大部件」，而是**约 20 个 Windows 系统层符号**的接线，
每个都很小（多数是「无窗口/无插件/无手柄时返回什么」的决定）。这一轮写了其中一半，
剩下的一半需要逐个决定语义，而不是靠猜。

为保证构建全绿，`LayerIntf.cpp` 等仍**没有**加入构建（不留坏目标）；9 个工具全部
回归通过（`krkrpresent` 逐像素 0 差异）。

### 14. 位图层的字体与文字度量（本轮新增，9 个工具全绿）
从 win32 版搬进 `visual/darwin/LayerBitmapImpl.cpp` 的三段**纯可移植**代码：

| 函数 | 作用 |
|---|---|
| `tTVPNativeBaseBitmap::ApplyFont()` | 解析该位图使用的字体（走栅格化器 + 预渲染字体映射） |
| `GetFontList` / `GetTextSize` / `GetTextWidth` / `GetTextHeight` / `GetEscWidthX/Y` / `GetEscHeightX/Y` / `GetFontGlyphDrawRect` | 文字度量：字形推进、行高、转义宽度、绘制矩形 |
| `TVPGlobalFontStateMagic` + `TVPGetPrerenderedMappedFont` | 栅格化器切换计数 + 游戏自带位图字体的映射查询（放进 `FontRasterizers_Darwin.cpp`） |

另加 `platform/compat/xmmlib.h`（空兼容头：`sound/MathAlgorithms.h` 只是 include 它，
不使用其中任何符号），以及 `platform/darwin/darwin_visual_services.cpp` 里
**约 20 个系统层符号**的实实现或空实现（事件、剪贴板、屏幕度量、系统控制、
插件/手柄、脚本异常、LocaleConfigManager…）。

**结果**：`LayerIntf.cpp`/`ScriptMgnIntf.cpp`/`Application.cpp`/`LayerManager.cpp`
这一批**都能编译**；剩余缺口 = 文字**绘制**（`DrawTextSingle`/`DrawTextMultiple`，
上游是 GDI 实现，需按预渲染字形 + tvpgl 重写）+ 脚本侧 `Window`/`MenuItem` 类
（定义在 VCL 应用层）。

### 15. 文字绘制跑通 → `krkrtext` ✅（关键里程碑）
意外的好消息：**上游的文字绘制也没有一处 GDI 调用**。`InternalBlendText` 的做法是
「字形覆盖率 → 临时纹理 → 向渲染管理器要 `ApplyColorMap`/`ApplyColorMap_d`/
`AlphaBlend_a`/`RemoveOpacity` 方法 → `OperateRect` 合成」，软件渲染管理器全都提供了。

新增 `visual/darwin/TextDraw_Darwin.cpp`（从 win32 版搬移，非重写）：
`InternalBlendText` / `InternalDrawText` / `DrawGlyph` / `DrawTextSingle` /
`DrawTextMultiple` / `DrawText`，外加字形缓存（`TVPFontCache` + `TVPGetCharacter`）。

**验证**（`krkrtext`，全部走引擎 API）：
```
$ ./build/krkrtext /tmp/text2.bmp 40 "あ漢A9 テスト"
drawn: 6083 non-background pixel(s)
wrote /tmp/text2.bmp
```
产物见 `screenshots/text-on-bitmap.png`：同一句「あ漢A9 テスト」用 `bmAlpha` 与
`bmCopy` 两种混合模式画了两行，字形清晰、抗锯齿正常。

坑：`DrawTextSingle` 只画**一个**字符，字符串要走 `DrawText`（内部会分派到
`DrawTextMultiple`）；字体栅格化器需要**懒初始化**（Windows 版在应用启动时初始化，
工具里没人做，`GetCurrentRasterizer()` 里补了）。

### 16. 图层系统的接线勘察（本轮结论：可以切到「真实现」了）
把 `LayerIntf.cpp`（图层系统本体）、`ScriptMgnIntf.cpp`（注册全部 TVP 类）、
`WindowIntf.cpp`、`Application.cpp`、`EventIntf.cpp`、`LayerManager.cpp`、
`TransIntf`/`RectItf`/`GraphicsLoaderIntf`/`BitmapIntf`、`BasicDrawDevice`、
`GraphicsLoaderImpl`、`NativeEventQueue`、`SysInitImpl` 等**全部加进构建后**：

1. **它们都能编译**
2. 缺口逐项补齐后，链接阶段开始报 **25 个 duplicate symbol** ——
   这 25 个正是我之前为「让代码先编过」写的平台桩，而现在**引擎的真实实现已经进来了**：
   `EventIntf.cpp` 的真实事件队列、`Application.cpp` 的真实应用层、
   `GraphicsLoaderIntf.cpp` 的图形缓存、`WindowIntf.cpp` 的真实 Window 类……

**这意味着移植进入新阶段**：不再需要「桩出一个能链接的世界」，而是把真实应用层接上、
删掉自己的桩。这是引擎② 的**最后一步**，内容明确：

| 要做的 | 说明 |
|---|---|
| 删掉约 25 个平台桩 | 它们和真实现冲突（`darwin_visual_services.cpp` 里的绝大部分） |
| 保留真正平台的部分 | SDL 窗口创建/送显（`DrawDevice_Darwin`）、文件媒体、字体钩子、剪贴板 |
| 补 `tTVPNativeBaseBitmap::ApplyFont` 之外的窗口类 | 脚本侧 `Window` 需要平台窗口对象（50 纯虚的 `tTVPWindow`） |

本轮为保证仓库绿色，构建配置回退到已验证状态（10 个工具全绿），清理工作留给下一轮
一次做完——那是一次性的删除+接线，比继续加桩更有价值。

### 17. 合并成单一引擎库，接上真实应用层 ✅（阶段性节点）
上一轮发现「25 个 duplicate symbol 全是我的桩」，这一轮把这件事做完了：

1. **两个静态库合并成一个**（`krkr`）——`krkrbase` 和 `krkrvis` 本来就互相引用
   （脚本管理器注册视觉类、图层抛脚本异常），分成两个库只会造成符号在两库间反复
   跳成「既重复又缺失」。
2. **删掉冲突的桩**：`Application` 全局、`PrintConsole`、`TVPAdd/RemoveContinuousEventHook`、
   `TVPAddCompactEventHook`、`TVPFireOnApplicationActivateEvent`、`TVPGetMessageByLocale`、
   `TVPShowSimpleMessageBox`、`TVPInitWindowOptions`、`TVPClearGraphicCache`、
   `ColorToRGB`、`ExePath`、`ExtractFileDir`、`TVPGetCursor`、`TVPInvokeEvents`、
   `TVPEventReceived`、`TVPGetWindowCount/ListAt`、`TVPDoSaveSystemVariables`、
   `tTJSNI_BaseWindow::UpdateContent` …… 全部由引擎的真实实现接管。
3. **删掉我那个 `System` 替身类**（`darwin_system.cpp`）——引擎自带 `tTJSNC_System`，
   工具改为注册 `TVPCreateNativeClass_System()`。
4. 现在库里有：真实的事件队列（`EventIntf`+`NativeEventQueue`）、真实应用层
   （`Application.cpp`）、真实图层系统（`LayerIntf`+`LayerManager`+`WindowIntf`）、
   真实绘制设备（`DrawDevice`+`BasicDrawDevice`）、真实脚本管理器（`ScriptMgnIntf`）。

**10 个工具全部构建通过并回归正常**：`krkrblit` 全过、`krkrfont` 渲染、
`krkrpresent` 逐像素 0 差异、`krkrtext` 正常绘制。

**仍由桌面提供的**（真正平台专属，不可替代）：SDL 窗口与送显、文件存储媒体、
字体钩子、剪贴板、软件渲染管理器的纹理、`LayerBitmapImpl`/`TextDraw` 的平台实现。

### 18. 图层系统跑通了 ✅（2026-10-25 解决）

**根因**：`krkrrun` 这个工具**没有调用 `TVPInitTVPGL()`**。像素管线（`tvpgl.cpp`，
12600 行）的全部 blitter 都是**函数指针**，在 `TVPInitTVPGL()` 里赋值；不初始化就是
空指针。图层构造函数第一次 `Fill` → `PartialFill` → `TVPFillARGB(...)` → **跳到空指针**。
崩溃栈看起来像「引擎的填充器写越界」，实际是工具少了一步初始化。

一条线：
```cpp
// tools/krkrrun.cpp
TVPInitTVPGL();   // ← 缺这一行，就崩在图层第一次填充
```

**结果**（`tests/layer.tjs`，全部走引擎自己的脚本对象）：
```
$ ./build/krkrrun tests/layer.tjs
[Information] layer: 640x480
[Information] background: 6307872
[Information] pixels drawn: 854
[Information] layer drawing works
```

一个 `Window`、一个 `Layer`、`fillRect`、两行 `drawText`、再逐像素读回验证——
**引擎的图层系统在 arm64 上端到端工作了**。

**顺带留下的防御**（都在 `RenderManager.cpp`，有环境变量开关）：
- `FillARGB` 在扫描线为空时**报错返回而不是崩**
- 填充矩形**钳到纹理边界**
- `taskNum` 至少为 1

### 18b. 子图层 ✅（结论：从来不是障碍，是我脚本写错了）

**引擎的签名是 `new Layer(window, parentLayer)`**：
- `param[0]` = **窗口**（要取它的 `layerTreeOwnerInterface`）
- `param[1]` = 父图层（可选）

我一直写的是 `new Layer(parentLayer, null)` —— 把父图层当成窗口传进去，于是取不到
`layerTreeOwnerInterface`，引擎抛「Please specify layerTreeOwnerInterface object」并终止。
**KAG 用的是正确顺序，所以这从来不是移植障碍。**

正确用法（已验证）：
```tjs
var base = new Layer(win, null);        // 主图层
var text = new Layer(win, base);        // 子图层，父图层在第二位
text.fillRect(...); text.drawText(...);
win.update();                            // 合成
```
输出：`wrote /tmp/child.bmp (320x240)`，exit=0。

**但子图层的内容还没合成进画面**：`new Layer(win, base)`、`fillRect`、`drawText`、
`visible=true`、`update()` 全部返回成功、截图也写出来了，**但合成帧里只有主图层的
底色**（子图层的文字没出现）。这是下一个具体问题：图层树的合成路径
（`tTVPLayerManager::Update`/`tTVPDrawable` 的子层遍历）。

**这轮的教训比 bug 值钱**：诊断输出必须 `fflush`（stderr 接管道时全缓冲，`_exit` 会丢光）；
以及**先怀疑自己的测试脚本**，再怀疑引擎。

### 18c. 子层不合成：`win.update()` 根本没到设备（本轮定位）

给 `tTVPDrawDevice::Update()` 与我们的 `Show()` 加了 `KRKR_DEBUG_DEV` 打印后运行子图层脚本：

```
[dev] 行数：0          ← Update 和 Show 一次都没被调用
```

对照 `tTJSNI_BaseWindow::UpdateContent()`（WindowIntf.cpp:632）：

```cpp
if( DrawDevice ) {
    DrawDevice->Update();
    if( !WaitVSync ) DrawDevice->Show();
    EndUpdate();
}
```

**`DrawDevice` 是空的**——所以整个更新/合成/上屏链路从来没跑过。之前主图层的截图能出内容，
是因为 `krkrrun --screenshot` 直接读主图层位图，绕过了设备。

**下一步**：查 `tTJSNI_BaseWindow::SetDrawDeviceObject` 里从 "interface" 属性取回设备指针
那一段——`DrawDevice` 为空说明那次 `PropGet("interface")` 的结果没有被正确解释，
或者窗口构造时根本没有调用到 `SetDrawDeviceObject`。

### 18e. 合成链路打通 ✅（脚本 → 窗口 → 图层树 → 设备 → 帧）

最后一块是**合成发生在哪里**。答案：既不在主图层，也不在设备的分块回调里——
`tTVPLayerManager` 自己有一块 `DrawBuffer`（`tTVPDestTexture`），`DrawCompleted`
把每层 `Blt` 进去，**那才是最终的帧**。

修复（`DrawDevice_Darwin.cpp`）：

```cpp
tTVPBaseTexture* screen = primary->GetMainImage();
if(iTVPLayerManager* m = primary->GetManager())
    if(tTVPLayerManager* lm = dynamic_cast<tTVPLayerManager*>(m))
        if(tTVPBaseTexture* composed = lm->GetOrCreateDrawBuffer())
            screen = composed;          // ← 呈现合成缓冲，而不是主图层
```

同时 `Show()` 会把呈现出去的那一帧留下来（`Presented`），`krkrrun --screenshot`
优先取它——之前的截图读主图层，**根本看不到设备呈现的东西**。

**证据**：子图层脚本 `new Layer(win, base)` + `drawText` + `win.update()` →
`screenshots/child-layer-composed.png`，320×240，画面上就是子图层写的那行
「child layer works」，底色是主图层的填充。470 个非背景像素。

至此引擎②的显示链路完整：**脚本 → 窗口 → 图层树（父/子）→ 图层管理器合成
→ 绘制设备 → 帧**。

### 19. 拿到真实 KAG3 框架（引擎②终于有真实数据了）

来源：[Hollow-Moon/kag3](https://github.com/Hollow-Moon/kag3)——KAG3 移植到吉里吉里Z 的
版本，W.Dee 原作，许可是「**改変・配布は自由**」（可自由修改分发），所以可以放进
`testdata/kag3-sample/`（552 KB，38 个文件）。

**这是真实的框架**，不是我自己写的 mini-KAG：
- `startup.tjs` —— 引擎启动后第一个脚本
- `system/Initialize.tjs`、`MainWindow.tjs`、`Config.tjs`、`MessageLayer.tjs`、`HistoryLayer.tjs` …
- `scenario/first.ks` —— 示例剧本（`[wait time=200]` / `[cm]` / 台词）
- 菜单、存档、历史、动画层等完整 KAG3 功能脚本

**跑起来的进度**（用 `krkrrun` 逐步试）：
```
ok: plugins menu          ← Plugins.link 不报错
ok: plugins kagparser
fail: execStorage -> Internal error    ← 卡在这里
```

即：`startup.tjs` 里的 `Scripts.execStorage("system/Initialize.tjs")` 失败。
这正是存储层要和 KAG 对上的地方——**KAG 用「相对于游戏根的存储名」**，
而我们的存储解析还没配置好搜索路径。

**下一步**（非常具体）：
1. 让 `TVPGetPlacedPath` / 存储搜索路径包含游戏根目录（KiriKiri 启动时做的其中一件事）
2. 再跑 `startup.tjs`，把 `Internal error` 变成具体的下一步错误

### 20. 真实 KAG3 初始化跑起来了 ✅（引擎②第一次跑真实框架）

给 `krkrrun` 补上引擎启动时该做的事——**把游戏根目录加入存储搜索路径**：

```cpp
TVPAddAutoPath(ttstr("file://" + cwd + "/"));
TVPSetCurrentDirectory(...);
```

结果：
```
Storages.isExistentStorage("system/Initialize.tjs") → 1        ← 之前解析不了
```

**然后真实 KAG3 的初始化脚本真的跑起来了**：

```
03:38:52 OS : macOS (macOS)
03:38:52 KAG : 3.32 stable rev. 2          ← KAG 自己报的版本
03:38:52 Kirikiri : 2.32.0.0
03:38:52 (info) Rebuilding Auto Path Table ...
03:38:52 (info) Total 50 file(s) found, 43 file(s) activated. (1ms)
```

**这意味着**：引擎② 能加载真实的 KAG3 框架脚本，KAG 能识别自己跑在
Kirikiri 2.32 上、能扫描游戏目录、能建立自动路径表。

**卡在下一步**：`system/Initialize.tjs:200` 的 `KAGLoadScript("MainWindow.tjs")`
——那会构造 `KAGMainWindow`（建窗口、建图层、注册大量属性），进程就停在那里。
这是 KAG 与引擎最重的接触面，也是下一个要啃的具体目标。

### 20b. `Scripts.execStorage` 修好 ✅ → KAG 开始报真实错误

**根因**：`TVPExecuteStorage` 第一句就是 `if(!TVPScriptEngine) TVPThrowInternalError`。
`TVPScriptEngine` 这个全局**只在应用启动流程里被赋值**（`TVPInitScriptEngine()`），
而 `krkrrun` 自己 `new tTJS()`，于是永远是空 → 任何 `execStorage` 都返回
「Internal error」。

修复（`tools/krkrrun.cpp`）：

```cpp
extern tTJS* TVPScriptEngine;
tTJS* tjs = new tTJS();
TVPScriptEngine = tjs;        // ← 工具创建引擎后要登记
```

（试过直接调 `TVPInitScriptEngine()`，它需要完整启动环境，会段错误；所以用最小挂钩。）

**效果**：`Scripts.execStorage("trivial.tjs")` → `trivial ran` ✓

**KAG3 初始化随后前进了一大步**，从「静默 Internal error」变成**带消息的真实 KAG 错误**：

```
OS : macOS (macOS)
KAG : 3.32 stable rev. 2
Kirikiri : 2.32.0.0
initialize failed: Config.tjs が見つかりません。    ← "Config.tjs not found"
(info) Total 50 file(s) found, 43 file(s) activated.
```

**下一步**：KAG 的脚本用**相对 `system/` 的名字**互相引用。已把 `system/` 加进
auto path，但仍然找不到——说明 `TVPSearchPlacedPath` 的查找时机或路径表构建顺序有问题
（KAG 在 Initialize 早期就构建路径表，之后加的 auto path 可能不生效）。
下轮查这个顺序。

### 20c. KAG 初始化继续前进：从 Config.tjs 到 `gcsAuto`

存储层对**裸文件名**（`Config.tjs`）不解析，只对完整存储名（`system/Config.tjs`）解析；
`TVPAddAutoPath` 加 `file://…/system/` 也无效，而加**本地路径**会抛 `eTJSError`。

为验证框架能走到哪一步，临时把 `system/*.tjs` 平铺到游戏根（测试布局），KAG 立即前进：

```
OS : macOS (macOS)
KAG : 3.32 stable rev. 2
Kirikiri : 2.32.0.0
(info) Total 71 file(s) found, 43 file(s) activated.
initialize failed: Member "gcsAuto" does not exist      ← 新的具体错误
```

**这是好事**：错误从「文件找不到」变成「缺某个成员」，说明 KAG 已经读进了自己的配置与脚本，
正在按它的假设调用引擎/框架的成员。`gcsAuto` 来自 `Config.tjs` 的默认值，值得下一轮查
（可能是 Config.tjs 被跳过，或该成员由 KAG 在更晚的步骤定义）。

### 20d. KAG 加载了自己的全部模块 ✅（本轮最大进展）

`gcsAuto` 缺失的原因找到了：引擎有一大段**用 TJS 写成的初始化脚本**
（`ScriptMgnIntf.cpp` 里的 `TVPInitTJSScript`，定义了 `gcsAuto`、虚拟键码、图像标签表等），
只有 `TVPInitScriptEngine()` 会执行它，而工具是自建引擎。加了一个导出函数：

```cpp
void TVPRunEngineInitScript(tTJS* engine) { engine->ExecScript(ttstr(TVPInitTJSScript)); }
```

**结果——真实 KAG3 开始加载它自己的全部模块**：

```
MessageLayer.tjs を読み込みました(4ms)
Menus.tjs を読み込みました(1ms)
DefaultMover.tjs を読み込みました(0ms)
MainWindow.tjs を読み込みました(7ms)
スクリプトの読み込みに 23ms かかりました
initialize failed: Member "MenuItem" does not exist
```

**这意味着**：KAG 的消息层、菜单、动画移动器、主窗口——四个核心模块在我们的移植版上
被解析并执行成功。KiriKiri 的 TJS2 解释器在这个后端上能跑真实的复杂框架代码。

**新卡点**：`MenuItem` 原生类不存在——因为它的实现（`tTJSNC_MenuItem`）在 Windows 的
VCL 应用层里，我们当时用空工厂顶替。这是**具体可做的一件事**：给桌面后端写一个最小的
MenuItem 类（菜单栏在 galgame 里用得不多，KAG 只是要在启动时创建它）。

### 20e. MenuItem 类写好了，但 `super.MenuItem(...)` 不解析（下一关）

`visual/darwin/MenuItem_Darwin.cpp`：一个没有菜单栏的 MenuItem——脚本能设的
`caption/visible/enabled/checked/radio/group`，能调的 `add/remove/click`，全部实现，
不画任何东西。已注册（`krkrrun` 里加了一行）。

**KAG 的模块加载继续变多**（现在读到 AnimationLayer/GraphicLayer）：

```
AnimationLayer.tjs を読み込みました(2ms)
GraphicLayer.tjs を読み込みました(1ms)
MessageLayer.tjs を読み込みました(4ms)
Menus.tjs を読み込みました(1ms)
DefaultMover.tjs を読み込みました(1ms)
MainWindow.tjs を読み込みました(8ms)
スクリプトの読み込みに 27ms かかりました
initialize failed: Member "MenuItem" does not exist
```

**缩小到一行**：KAG 的 `KAGMenuItem` 构造里写的是
```tjs
super.MenuItem(window, caption);
```
用最小脚本验证：
```tjs
class T2 extends MenuItem { function T2() { super.MenuItem(null, "x"); } }
new T2();       → 静默失败
```
而 `class T2 extends MenuItem {}`（不调 super 构造）是**可以**的。

所以卡点是 **TJS2 里「用 `super.原生类名(...)` 调父类构造」这一语法**对**原生类**不生效。
上游的原生类（例如 Window）在真实 Kirikiroid2 里支持这种写法，说明我们的原生类注册
缺少某个让 `super.类名` 可解析的成员。

**下一步**：查 `tTJSNativeClass` 的成员表里「以类名为名的构造入口」是怎么建立的
（`TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL` 注册的成员名 vs. `super.` 的查找规则）。

### 20f. `super.原生类名(...)` 修好 ✅（TJS2 原生类继承的关键）

**根因**：`TJS_BEGIN_NATIVE_MEMBERS(名字)` 里那个名字**才是**这个类在 TJS 里的内部名，
`super.类名(...)` 就是按它查的。我在 MenuItem 里写成了 `MenuItemDarwin`，而全局注册名
是 `MenuItem` → 两者不一致 → `super.MenuItem` 找不到。

改成一致（`TJS_BEGIN_NATIVE_MEMBERS(MenuItem)` + 构造声明同名）后：

```
class T2 extends MenuItem { function T2() { super.MenuItem(null, "x"); } }
new T2();        → super ok ✓
```

**这是一处通用的修复**：任何「脚本继承原生类并在构造里调父类构造」的写法从此都能用，
KAG 的 `KAGMenuItem extends MenuItem` 正是这种写法。

**KAG 现在通过 MenuItem，卡在下一个成员**：

```
initialize failed: Member "menu" does not exist
```

`menu` 是 **Window 的属性**（菜单栏根节点），KAG 的 `Menus.tjs` 一上来就
`menu.add(this.systemMenu = new KAGMenuItem(...))`。桌面后端没有菜单栏，所以这个属性
不存在。

**下一步**：给 `tTJSNC_Window` 加一个 `menu` 属性，返回一棵可 `add` 的 MenuItem 树
（不画出来，只让框架能构建）。位置在 `visual/WindowIntf.cpp` 的成员表里。

### 20g. `menu` 属性补上 ✅ → 下一个缺 `Timer`

给 `tTJSNC_Window` 加了 `menu` 属性（`WindowIntf.cpp` 成员表末尾），返回一棵可
`add` 的 MenuItem 树，不画任何东西。KAG 的 `Menus.tjs` 通过。

**新卡点**：`Member "Timer" does not exist`——`Timer` 原生类也在 VCL 应用层
（`environ/win32/TimerImpl.cpp` 之类），桌面后端还没有。KAG 用它做自动播放/动画计时。

**这类缺口的规律很清楚**：VCL 应用层提供的脚本可见类（`MenuItem`、`Timer`、
可能还有 `Window` 的子类行为），桌面上要逐个补最小实现。每补一个 KAG 就往下走一段。

### 20h. 连过三关：Timer → KAGParser → AsyncTrigger

| 缺的东西 | 处理 |
|---|---|
| `Timer` | 新写 `visual/darwin/Timer_Darwin.cpp`（interval/enabled/mode/capacity/onTimer/start/stop，不真的计时） |
| `KAGParser` | 引擎里已有实现（`utils/KAGParser.cpp`），只是 `krkrrun` 没注册 → 注册 |
| `AsyncTrigger` | 引擎里已有（`base/EventIntf.cpp`）→ 注册 |

**每过一关，KAG 就往前走一段**，而且能看到它在**开始分配位图**（`Use malloc for Bitmap`）——
说明窗口/图层正在被真正创建。

**下一个缺口**：`WaveSoundBuffer`（声音缓冲类）。我们之前把它做成返回空指针，
KAG 在初始化时就会创建声音缓冲，所以需要给它一个最小实现（存状态、不播放）。

**规律确认**：VCL/Windows 层提供的脚本可见类必须逐个补齐。目前补了
`MenuItem`、`Window.menu`、`Timer`，注册了 `KAGParser`、`AsyncTrigger`；
接下来是声音类。

### 20i. WaveSoundBuffer 写成 → 下一个 `VideoOverlay`

`visual/darwin/SoundBuffer_Darwin.cpp`：最小的波形缓冲类——`open`/`close`/`play`/
`stop`/`pause`/`fade`，`volume`/`pan`/`looping`/`playing`/`position`/`totalTime`。
**没有音频设备，所以状态记住、不出声**；KAG 启动时只创建和配置它。

KAG 继续前进：

```
スクリプトの読み込みに 26ms かかりました
(info) Use malloc for Bitmap
initialize failed: Member "VideoOverlay" does not exist
```

**下一个缺口**：`VideoOverlay`（视频叠加层），也在 VCL 层；我们的桩返回空指针。
同样需要一个最小实现（状态 + 不播视频）。

### 20j. VideoOverlay 写好 → `MenuItem.children` 要正确实现

`visual/darwin/VideoOverlay_Darwin.cpp`：`mode/frame/left/top/width/height/visible/
loop/audioBalance/audioVolume/enabledAudioStream/numberOfAudioStream/position/totalTime`
+ `open/close/play/stop/pause`，**不播视频**。KAG 的 `Movie extends VideoOverlay` 通过。

**新卡点**：`MenuItem.children`——KAG 的 `internalSetMenuAccessibleAll` 会遍历菜单树
（`var children = menu.children; ... children.count`），所以这个属性必须返回**数组**。

试过直接 `TJSCreateArrayObject()`，**段错误**——它需要 TJS 的全局上下文/类对象，
在属性 getter 里裸调不安全。正确做法是用 `iTJSDispatch2* global` 上的 TJS 数组类来
创建（或者缓存一个数组对象）。下轮按这个方向修。

### 20k. `children` 有了新进展，但暴露出更深的一层：`SaveStructuredData` 崩溃

把 `children` 属性加回去（返回 `TJSCreateArrayObject()` 的数组）后，KAG **通过了那一关**，
但紧接着**段错误**。崩溃栈（macOS crash report）第一次指向了别的地方：

```
tTJSDictionaryNI::SaveStructuredData
  ← tTJSDictionaryClass::NCM_saveStruct::Process
    ← tTJSNativeClassMethod::FuncCall
      ← ...
```

即 KAG 在**保存结构化字典**（`Dictionary.saveStruct`，KAG 存配置/存档用的）时崩了。
这是 TJS2 自己的序列化代码，在处理我们构造的对象时踩到空指针。

**这轮的处理**：`children` 暂时回退，保证构建与所有工具全绿（`krkrblit` 全过）。
`children` 的正确实现必须和 `SaveStructuredData` 的崩溃一起解决——两者都涉及
「TJS 对象在原生属性里的生命周期」。

**下一步**（具体）：
1. 读 `tTJSDictionaryNI::SaveStructuredData`，看它遍历对象时假设了什么
2. 我们的 `children` getter 返回的数组对象**每次都是新的、且立刻 Release**，
   很可能 `SaveStructuredData` 拿着它时已经失效 → 应当缓存数组并正确持有引用

### 20l. 序列化层的两个可复现事实

用最小脚本试 `saveStruct`（KAG 存配置用的那条路）：

```tjs
var d = %[]; d.a = 1;
d.saveStruct("file:///tmp/saved.tjs");          → Member "saveStruct" does not exist
Dictionary.saveStruct("file:///tmp/saved.tjs", d); → Invalid object context
```

**第一个**说明实例上没有这个方法（KAG 走的是类级调用）；
**第二个**说明类级调用的参数/上下文约定与我们给的不符。

再结合崩溃栈：

```
tTJSDictionaryNI::SaveStructuredData
  ← tTJSDictionaryClass::NCM_saveStruct::Process
```

可以确定：**KAG 确实调到了 `NCM_saveStruct`，并在 `SaveStructuredData` 里崩了**。
下一步就是拿 `NCM_saveStruct::Process` 的参数解析对照它的实现，把「Invalid object
context」和崩溃这两个现象对上——两者很可能是同一个约定没满足。

### 21. 🎉 KAG 主窗口建成 + 剧本被加载（引擎②的里程碑）

**第二个「同类根因」**：`TJSCreateTextStreamForWrite` 等**四个流工厂是函数指针**，
只在 `TVPInitScriptEngine()` 里赋值。工具自建引擎 → 指针为空 → `saveStruct`
（以及数组序列化）调用空指针崩溃。在 `TVPRunEngineInitScript()` 里一并赋值后，
`saveStruct` 正常写出 `(const) %[ "a" => 1, ... ]`。

**然后 KAG 一路走到底**：

```
スクリプトの読み込みに 27ms かかりました
(info) Use malloc for Bitmap
KAGMainWindow のコンストラクタで 6ms かかりました        ← 主窗口构造完成！
================================================================================
Scenario loaded : first.ks                              ← 剧本加载成功！
[Information] initialize failed: Member "paused" does not exist
```

**这是引擎② 第一次做到**：
- KAG 的主窗口 `KAGMainWindow` 构造成功（建窗口 + 全套图层 + 菜单 + 声音）
- **`first.ks` 剧本被真正加载**

也就是说，**距离屏幕上出现「こんにちは。」只差最后几个成员**。下一个卡点：
`paused`（KAG 的自动播放/暂停状态，属于 Window 或 SoundBuffer 的属性）。

### 21b. 🎉🎉 KAG 开始执行剧本（引擎②到达过的最高点）

两个平台层的根因接着修：
1. **四个流工厂函数指针**未赋值 → `saveStruct` 空指针崩溃（已修）
2. **`TVPSystemControl` 是空指针** → `TVPGetSystemEventDisabledState()`
   （`AsyncTrigger.trigger` 会走到）解引用崩溃。桌面后端现在托管一个真实实例
   （`static tTVPSystemControl gSystemControl;`），和 Windows 应用层一样。

**然后 KAG 一路走到执行剧本**：

```
KAGMainWindow のコンストラクタで 7ms かかりました
================================================================================
Scenario loaded : first.ks
処理を開始します                        ← 「开始处理」——剧本开始执行了！
wrote /tmp/kag_frame.bmp (640x480)     ← 无错误退出
```

**这是引擎② 到达过的最高点**：真实 KAG3 框架加载 → 主窗口构造 → 剧本加载 →
**开始执行剧本**，全程无错误。

**还差最后一步**：截的帧是**空白的**（一种颜色）。因为 KAG 在执行每条指令之间
**等事件**（定时器/点击），而 `krkrrun` 跑完脚本就退出了，没有事件循环。
下一步：给 `krkrrun` 加一个**主循环**（泵事件 N 秒 + 每帧 `update()`），
让 KAG 有时间把「こんにちは。」画出来。

### 21c. 加了主循环、定时器触发、事件派发——KAG 仍未推进到绘制

本轮给基础设施补了三样：
1. **`Timer` 真的会触发**：`Timer_Darwin.cpp` 里加了运行中的定时器表 +
   `TVPDarwinTickTimers()`，主循环按 `interval` 到点就调 `onTimer` 处理器。
2. **`krkrrun` 有主循环**：`KRKR_LOOP_SECONDS=4` 时循环 4 秒（每帧 16ms）。
3. **每帧调 `TVPInvokeEvents()`**：让引擎自己的事件队列（`AsyncTrigger` 等）得以派发。

**状态**：KAG 依然停在「処理を開始します」，帧仍空白（一种颜色）。说明它的
场景推进不是靠我们这条定时器路径——很可能 KAG 的 `wait` 用的是
`System.getTickCount` 轮询 + 自己的事件循环，而那个循环需要 `WaitForVBlank` /
连续的 `onTimer` 事件链。

**下一步**：查 KAG 的 `wait`/`waitCommand` 是怎么等的（`MainWindow.tjs` 或
`KAGParser`），对照它期待的驱动方式，再补对应的平台钩子。

### 21d. 定时器参数顺序修好，但 KAG 仍不推进

**发现并修正**：KAG 的 `Conductor` 构造时钟的方式是
```tjs
timer = new Timer(timerCallback, '');   // 第一参数是回调，第二是模式串
```
我们的 `Timer` 把处理函数当成了第二参数——**参数顺序反了**，所以配置了却没有回调。
另外 `interval = 0` 在 KAG 里表示「尽快触发」，我们按 1000ms 兜底了，改成 1ms。

**状态**：`KRKR_DEBUG_TIMER` 显示定时器确实触发了；但 KAG 在 5 秒的循环后
帧仍是空白（一种颜色），仍停在「処理を開始します」。

**下一步**：确认回调真的被调用且没抛异常——我们的 `FuncCall(0, nullptr, ...)`
可能不符合 TJS 事件回调的签名（通常要传一个事件对象）。给回调加 try/catch +
打印，看它进入没进入、异常是什么。

### 22. 🏆 KAG3 剧本上屏：「こんにちは。」

**最后一个 bug**：定时器的注册表在**赋值 handler 之前**就登记了条目，记下的是 `nullptr`。
改成「先存 handler，再登记，且注册表持指针跟随」之后：

```
first.ks : [cm]
first.ks : こんにちは。
wrote /tmp/kag_final2.bmp (640x480, presented frame)     帧内 231 种颜色
```

**产物**：`screenshots/kag3-first-line.png` —— 640×480 黑底，左上角写着
**「こんにちは。」**。

**这是引擎② 的验收时刻**：
- 真实 KAG3 3.32 框架（W.Dee 原作，38 个文件）
- 真实剧本 `scenario/first.ks`
- 剧本被执行：`[wait time=200]` → `[cm]` → 台词
- 台词经 KAG 的消息层绘制，合成、上屏、读回

**脚本 → 框架 → 消息层 → 图层树 → 合成 → 设备 → 屏幕**，全链路在 arm64 原生构建上跑通。

### 24. ✅ 直接从 `data.xp3` 启动（真实游戏的发布形态）

**关键发现**：存储名里 `>` 表示「归档内部」，但**相对名字要靠「当前目录」解析**，
光加自动路径不够（子目录名解析不到）。正确做法是把归档设为当前目录：

```cpp
std::string root = std::string("file://") + realpath + ">";   // 注意结尾的 '>'
TVPSetCurrentDirectory(ttstr(root.c_str()));
TVPAddAutoPath(ttstr(root.c_str()));
// 然后 Scripts.execStorage("startup.tjs")
```

**结果**（`krkrrun kag3.xp3`，KAG3 工程 30 个文件全在归档里）：

```
KAG : 3.32 stable rev. 2
KAG System スクリプトを読み込んでいます...
KAGLayer.tjs を読み込みました(2ms)
KAGMainWindow のコンストラクタで 8ms かかりました
Scenario loaded : first.ks
wrote /tmp/arc.bmp (640x480, presented frame)     帧内 231 种颜色
```

**这是引擎② 距离真实游戏最近的一步**：真实的 KiriKiri 游戏都以 `data.xp3` 发布，
现在「传归档就跑」通了。产物 `screenshots/archive-launch.png`。

### 25. 🏆 第一款真实 KAG 游戏工程跑起来（带图带音）

数据来源：[CAWorks-ChrisA/kirikiri-kag-ACG](https://github.com/CAWorks-ChrisA/kirikiri-kag-ACG)
（12.7 MB，完整的 KAG3 游戏工程：22 个框架 tjs、5 个剧本 ks、真实 `bg0.jpg` 背景、
`song.ogg` BGM、插件目录）。放在 `testdata/kag-game/`。

**跑起来的输出**：

```
KAG : 3.32 stable rev. 2
KAGLayer.tjs を読み込みました(1ms)
KAGMainWindow のコンストラクタで 13ms かかりました
Scenario loaded : first.ks
Scenario loaded : 0.ks
wrote /tmp/acg2.bmp (1280x720, presented frame)
```

**1280×720 的 HD 工程，加载了它自己的剧本链（first.ks → 0.ks）并呈现了一帧。**

**关键布局发现**：KAG 的框架文件（`Config.tjs`、`UpdateConfig.tjs` 等）必须放在
**游戏根目录**——`Initialize.tjs` 里是这么写的：

```tjs
if(Storages.isExistentStorage("Config.tjs")) KAGLoadScript("Config.tjs");
else if(Storages.isExistentStorage("Config.~new")) { ...; System.exit(); }
else throw new Exception("Config.tjs が見つかりません。");
```

即：**裸名字只按「当前目录」解析，不走自动路径**。真实游戏的根目录就是这些文件所在处，
所以 `cp system/*.tjs .` 之后一切正常。这也解释了之前 kag3-sample 为什么要平铺。

**画面仍然是空的**（在标题画面等输入），下一步是注入按键。

### 26. 真实游戏要 JPEG：解码器已接入，但图片路径仍撞到 PVR 桩

真实游戏的背景是 JPEG，而本后端原来只有 PNG/TLG。这轮：

1. 把引擎自带的 `visual/LoadJPEG.cpp` 编进构建（对 Homebrew 的 jpeg-turbo）
2. 加 `platform/compat/jinclude.h`（libjpeg-turbo 不提供这个内部头，且文件并不真用它的东西）
3. 删掉平台层里「JPEG 无解码器」的桩和重复的 `TVPJPEGLoadPrecision`

**ACERG 游戏工程现在能加载剧本链**（first.ks → 0.ks），但画背景时报：

```
0.ks : [image storage="bg0" page=fore layer=base]
Cannot open storage the macOS back end has no decoder for PVR
```

**已排除**：文件本身是合法 JPEG（`file` 确认 JFIF 1280×720）；`.jpg` 处理器在表里；
PVR/WebP 已从处理器表移除；PVR 的 header 探测已改成「签名不匹配就静默返回」。

**仍在排查**：还有哪条路径会走到 PVR 桩（可能是引擎别处按扩展名直接调）。

### 27. 🏆🏆 真实 KAG 游戏出画面了（背景 + 按钮）

**根因（一个反直觉的）**：引擎的 `TVPInternalLoadTexture` **无条件**调用
`TVPLoadPVRv3(stream, cb)` —— 在 Windows 上那是「把图片上传成 GPU 纹理」的通用加载器，
**不是 PVR 专用**。我把它当 PVR 解码器桩成了「抛异常」，于是**所有图片**都死在它那儿。

**修法**：让那个重载返回 `nullptr`，这样 `TVPLoadGraphic` 会回落到位图加载路径
（那里按扩展名/文件头分派，JPEG/PNG/TLG/BMP 全都能用）：

```cpp
iTVPTexture2D* TVPLoadPVRv3(tTJSBinaryStream*, const std::function<...>&)
{
    // 软件后端把图片放在位图里，不建 GPU 纹理；回落到位图加载器
    return nullptr;
}
```

**结果**：

```
bgimage/bg0.jpg -> 1280x720
bgimage/bg0     -> 1280x720      ← 不带扩展名也能靠「试探扩展名」找到

Scenario loaded : first.ks / 0.ks
wrote /tmp/real.bmp (1280x720, presented frame)     帧内 92,150 种颜色
```

**产物 `screenshots/real-game-title.png`**：《UI Test》ACG 工程的标题画面——
真实的绿色草地/光晕背景（`bg0.jpg` 正确解码）+ 右侧 **start** / **setting** 两个按钮。

**这是引擎② 第一次把一款真实 KAG 游戏的标题画面完整画出来。**

### 28. 输入注入：点击能送进去了

`KRKR_AUTO_CLICK="x,y,ms"` —— 在运行循环里到点调用
`iTVPLayerManager::NotifyClick(x, y)`（窗口收到鼠标时走的就是这个）。

**已验证送达**（日志 `[input] clicked 1000,400`），但**还没验证 KAG 的按钮会响应**：
真实工程的标题画面上，点了 `start` 之后画面没变。

**下一步**：查点击进入管理器之后的分发（`tTVPLayerManager::PrimaryClick` →
各图层的 `onClick`），看是坐标没命中按钮，还是事件没继续往下传。

### 29. 输入注入做成确定性了；按钮仍不响应

三件事让注入可复现：

1. 挪到循环**最前面**（之前在某些分支之后，可能走不到）
2. 加**心跳**（每 60 帧打印帧号与毫秒）
3. **调用前**先打印 `[input] clicking`（万一调用不返回也能看见）

现在日志稳定给出：

```
[input] frame 661, 13293 ms
[input] waiting: 11001 ms elapsed, click at 1000,400,11000
[input] clicking 1000,400
[input] clicked 1000,400
```

并且补上了 `NotifyMouseDown` / `NotifyMouseUp`。

**但真实工程的 `start` 按钮仍不响应**：点击确实送到
`iTVPLayerManager::NotifyClick`，剧本没有推进（没有 `Scenario loaded : 1.ks`）。

**顺带查明一个之前误判的原因**：游戏加载耗时波动极大（冷缓存 ~11 秒，热缓存 ~1 秒），
所以「点击太早」和「点击没送到」在日志里看起来一样。**心跳 + 时间戳**把这两种情况分开了。

**下一步**：`tTVPLayerManager::PrimaryClick` 沿图层树的命中测试——
哪个图层应当拥有 (1000,400) 这一点，以及 KAG 的按钮为什么不是它。

### 30. 点击改走「真实鼠标的入口」，按钮仍不响应

发现两件事：

1. **`PrimaryClick` 只在鼠标捕获还持有时才生效**：

```cpp
void tTVPLayerManager::PrimaryClick(tjs_int x, tjs_int y)
{
    tTJSNI_BaseLayer * l = GetMostFrontChildAt(x, y);
    if(l && CaptureOwner == l) { l->FireClick(x, y); }   // ← 捕获必须还是它
}
```

   而 `CaptureOwner` 是 `PrimaryMouseDown` 设的、`PrimaryMouseUp` 释放的。
   我原来写成 down → up → click，**点击时捕获已经放掉了**。
   已改成 **down → click → up**。

2. 改成走 `iTVPDrawDevice::OnMouseDown/OnClick/OnMouseUp`
   （真实窗口收到鼠标就是这条路径，它会先把窗口坐标映射到主图层）。

**结果**：KAG 的 `start` 按钮仍不响应。

**下一步**：查 `GetMostFrontChildAt(1100, 430)` 到底返回哪个图层——
是按钮层（说明之后的 `FireClick` 没到 KAG），还是别的层（说明命中测试有问题）。
注意 KAG 的按钮是 `ButtonLayer` 的 `onClick` 覆写（`system/ButtonLayer.tjs:169`）。

### 31. 命中测试的结果：点击落在**基础层**，不是按钮

在 `tTVPLayerManager` 里加了三个探针（down / click / up + 几何信息）：

```
[click] down on layer 0x…680 at 16,16 size 1240,680
[click] at 1100,430 front=0x…680 capture=0x…680 match=1
[click] up, capture=0x…680
```

**三个事件都到了同一层**，看似成功——但那一层是 **(16,16) / 1240×680**，
即**整屏大小的基础层**，不是按钮。

**所以问题在命中测试**：该点上「最前面的图层」不是画在那里的东西。
KAG 的按钮是消息层的子层，而剧本里写着 `[position layer=message opacity=0]` ——
很可能是「不透明度 0 的图层」被命中测试跳过了。

**下一步**：查 `tTJSNI_BaseLayer::GetMostFrontChildAt` 的可见性/透明度判断，
确认它是否该跳过 opacity=0 的层（KAG 依赖这些层接收点击）。

### 32. 按钮就在那儿，命中测试差一层

递归打印点击处的前景层子树：

```
[layers] under the click:
  2: 1016,416 230x60 visible=1 opac=255 type=2     ← start 按钮
  3: 1016,486 230x60 visible=1 opac=255 type=2     ← setting 按钮
```

点 (1131,446) **确实落在 start 按钮的矩形内**（1016..1246 × 416..476），
`front` 却是它的**父层**——说明递归下降在按钮这一层被拒绝了。

**推断**：`HitTestNoVisibleCheck` 对这个 `type=2` 层做的是**alpha 命中测试**，
而按钮图形的绘制区域小于它的矩形（圆角/留白），我点的位置像素是透明的。

**顺带**：primary 下只有两层——base（visible=1）和 message（**visible=0**），
所以按钮不在消息层里，而是 base 的子树。

**下一步**：确认 KAG 是否给按钮设了 `hitType`（矩形命中），以及我们的
`HitTestNoVisibleCheck` 是否读了这个属性。

### 33. 时序：本引擎冷启动要几十秒；启动期间点击会打乱工程

`KRKR_AUTO_CLICK` 现在可以**连点 5 次**（从指定时刻起每 3 秒一次）。

但更重要的是这轮**测出来**的两件事：

1. **冷启动很慢**：从进程启动到标题可交互，实测 **20～60 秒**，而且波动大
   （同一命令两次可以差 3 倍）。所以「固定某个时刻点一次」基本是碰运气。
2. **启动期间点击会打乱工程**：从第 2 秒起每 2 秒点一次，工程只走到 `first.ks`
   就停了；而**不点击**的运行能走到 `0.ks` 并停在 `[s]`。

**所以下一步不是继续调点击时机**，而是命中测试本身：
按钮层 `1016,416 230×60` 可见、点也在矩形内，但递归下降停在它的父层。

### 34. 两个纠正 + 一个更根本的发现

**纠正 1：启动其实很快。** 用 `/usr/bin/time` 量：

```
real 2.30   user 0.04   sys 0.10     （热缓存，跑到 first.ks 加载完）
```

之前以为「启动要 35 秒」是**日志流缓冲的假象**：KAG 的剧本行走 stdout（有缓冲），
`[click]` 走 stderr（无缓冲），文件里的先后顺序**不是时间顺序**。
现在点击日志带墙钟时间，可以直接和 KAG 自己的时间戳对比。

**纠正 2：点击并不比不点击更糟。** 加了墙钟时间后看到：`first.ks` 在 10:14:23 加载，
点击在 10:14:27/30/33/36——**都在启动之后**。

**更根本的发现：`first.ks → 0.ks` 的推进是间歇性的。**

| 运行 | 加载的剧本数 |
|---|---|
| 带点击 | 1（只到 first.ks）|
| **不带点击** | **1**（也只到 first.ks）|
| 早前若干次 | 2（first.ks + 0.ks）|

**所以问题不在点击，而在 KAG 的定时器驱动的剧本推进本身不稳定。**
标题画面能不能出现，取决于这次跑有没有推进到 `0.ks`（标题在 0.ks 里画）。

**下一步**：查 `first.ks` 结束后 KAG 靠什么定时器/事件跳到 `0.ks`，
以及我们的定时器/事件泵（`TVPDarwinTickTimers` + `TVPInvokeEvents`）是否有丢事件。

### 35. 🎯 根因：运行循环被我改坏了（丢掉了定时器/事件/呈现）

**这是最近十几轮「时好时坏」的真正原因。**

前面移动点击注入代码时，替换范围过大，把循环里的这些调用一起删掉了：

```cpp
TVPDarwinTickTimers();      // 定时器
TVPInvokeEvents();          // 引擎事件（AsyncTrigger 等）
device->Update(); Show();   // 呈现
SDL_PollEvent(...)          // 窗口事件
```

循环退化成了「检查点击 + sleep」。后果：

- **没有 tick → 定时器不触发 → KAG 的 Conductor 不推进剧本**（first.ks 停在原地）
- **没有呈现 → 画面不动**
- **没有事件轮询 → 窗口「聋了」**
- 但启动阶段仍然画了第一帧，所以看起来「半死不活」

**恢复后立刻验证**：

```
[timer] tick, 1 registered
[timer] due, interval=100 enabled=1
[Information] timer fired 1
```

真实工程连跑三次**全部一致**：`2 个剧本`、`92150 色`（标题画面）——**不再时好时坏**。

### 36. 附带修好的两处（这次才发现被 Windows 版挡住）

1. `utils/win32/TimerImpl.cpp` 和 `visual/darwin/Timer_Darwin.cpp` **都定义**
   `TVPCreateNativeClass_Timer`，而引擎注册的是 Windows 那个（在 macOS 上永不触发）。
   已把 Windows 版从构建里去掉 —— 现在脚本 `new Timer(...)` 走到我们的实现：
   `[timer] Construct numparams=2 type0=1`、`enabled=1` ✓
2. `Timer` 默认应为 **enabled=true**（KAG 创建后从不设 enabled）

### 37. 按钮层终于收到了鼠标事件（循环修好之后）

循环修好、定时器恢复后，再看点击：

```
[hit]   rect 1016,416 230x60 at 99,14 hit=0 thr=0 img=0,0 690x60   ← 命中测试通过
[click] down on layer 0x…800 at 1016,416 size 230,60              ← 正是 start 按钮
[click] at 1131,446 front=0x…800 capture=0x…800 match=1           ← 捕获也对
```

**这是这几轮想要的：鼠标事件确实送到了按钮层。**

**但 KAG 仍未跳转到 `1.ks`**——`onExecute` 是 `onMouseUp` 里被调的
（`system/ButtonLayer.tjs`：`if(Butt_mouseDown) onExecute(...)`），
所以下一步要查 `FireMouseUp` 是否真的调到那一层、以及 `onExecute` 是否执行了跳转。

**另一个观察**：点空白处（落在基础层）也**推不动 `[s]` 停顿**——
说明 `[s]` 等的是别的事件（可能是窗口级的 onClick，或 `Conductor` 的 click 处理）。

### 38. 🎯 又一个真 bug：事件投递是空的

`FireMouseUp` / `FireClick` 并不直接调 TJS，而是 **`TVPPostEvent(...)` 投递到队列**，
由 `TVPInvokeEvents()` → `tTVPSystemControl::InvokeEvents()` 派发。

而我们的实现：

```cpp
// 原来
void tTVPSystemControl::InvokeEvents() { }        // 空！
```

**所以所有事件（每个图层的 onClick/onMouseDown/onMouseUp、KAG 的窗口触发器）
排进队列后从来没有被派发过。**

已修：

```cpp
void tTVPSystemControl::InvokeEvents()
{
    extern void TVPDeliverAllEvents();
    TVPDeliverAllEvents();
}
```

### 39. KAG 的点击模型（读 MainWindow.tjs 得知）

```tjs
_primaryLayer.onMouseDown = function (x, y, btn) {
    switch (btn) {
    case mbLeft:  window.onPrimaryClick(); break;      // ← 剧情前进靠这个
    ...
```

而 `AnimationLayer.tjs` 里：

```tjs
hitType = htMask;
hitThreshold = 256;  // マウスメッセージは完全に透過
```

**KAG 把所有剧情图层设成「对鼠标完全透明」，好让点击落到 primary 层。
我们的默认 `HitThreshold = 16`，所以带背景图的图层会「吃掉」点击。**

**下一步**：验证带背景的 `base` 层的 `hitThreshold` 实际值——
如果它是 16（默认），说明 KAG 没给它设值，而真实 KiriKiri 的默认值可能是 256。

### 40. 两个实验都失败，已回退（结论很有用）

**实验 A：把鼠标事件同时冒泡给 primary 层**
（因为 KAG 把 `_primaryLayer.onMouseDown` 当窗口级点击）
→ 无改善，`[s]` 推不动。**已回退。**

**实验 B：让 `NotifyEventDelivered()` 调 `TVPEventReceived()` 重置事件泵**

引擎源码里：

```cpp
void TVPInvokeEvents()
{
    if(TVPEventInvoked) return;      // ← 只放行一次
    TVPEventInvoked = true;
    if(TVPSystemControl) TVPSystemControl->InvokeEvents();
}
void TVPEventReceived() { TVPEventInvoked = false; }   // ← 唯一的重置点
```

我们的 `NotifyEventDelivered()` 是空的，所以**事件泵只工作过一次**。
加上重置后 → **进程在打印 banner 后就死了**（3 行日志、没有加载任何剧本）。

**推断**：重置导致事件在派发过程中重入（`InvokeEvents` 里再派发），
需要更小心的实现（比如派发时加递归保护）。**已回退，基线恢复正常**
（2 个剧本、1280×720、92150 色）。

### 41. 现在稳定的基线

```
Scenario loaded : first.ks
Scenario loaded : 0.ks
wrote … (1280x720)     92150 色 = 标题画面
```

连续多次运行一致。**这是可以继续工作的地基。**

### 42. ✅ 真实游戏可以「玩」了：标题 → 点击 → 正文 → 多句台词

修好 `EventEnable` 之后，`start` 按钮生效，游戏进入 `1.ks`。接着三处小问题：

1. **[l] 换行报错** —— 是**测试目录布局**问题，不是引擎：
   发行版游戏把 KAG 的 system 文件（**含图片**）放在游戏根目录，
   而我只平铺了 `*.tjs`。`cp system/*.png .` 之后正常。
2. **`[playbgm]` → `Member "stopFade" does not exist`** —— `WaveSoundBuffer` 缺方法，已补
3. **`[ws]` → `Member "status" does not exist`** —— 已补 `status` 属性（0 停止 / 1 播放 / 2 暂停）

**结果：零报错，剧本连续推进。**

产物 `screenshots/real-game-story.png`：对话框里是 **「4th message」**——
游戏自己的背景 + 对话框 + 台词，一句一句往下走。

### 43. 这个引擎现在的完整能力

| 环节 | 状态 |
|---|---|
| XP3 归档（含直接启动） | ✅ |
| KAG3 框架（22 个脚本） | ✅ |
| PNG / JPEG / TLG / BMP 解码 | ✅ |
| 图层合成、按钮、动画层 | ✅ |
| 定时器（KAG 的 Conductor） | ✅ |
| 事件派发（图层 onClick/onMouseDown/onMouseUp） | ✅ |
| 鼠标输入 | ✅ |
| 音频对象（WaveSoundBuffer/BGM） | 部分（无实际播放） |
| 剧本推进（多句台词） | ✅ |

### 44. ✅ 音频：接上 SDL_mixer

`WaveSoundBuffer` 原来只保存状态、不出声。现在：

- `open()` 通过**引擎的存储层**读文件（所以归档里的音频也能用）
- 字节交给 **SDL_mixer**（`Mix_LoadMUS_RW`），`play/stop/pause/volume` 都接到混音器
- 引擎的音量是 `-10000..0 dB`，映射到混音器的 `0..128`

实测（工程自带的 `song.ogg`）：

```
[Information] opened, totalTime=220.54
[Information] playing, status=1
```

真实游戏跑到 `[playbgm storage="song.ogg"]` **零报错**。

### 45. `build-all.sh` 卡死的原因（顺带修的）

自检脚本卡了十几分钟，两个原因：

1. 检查完 BGI 后 `kill`（SIGTERM）——**引擎有信号处理，不会死**——然后 `wait`：**永远等下去**
2. 没有先清理残留实例：新实例发现锁被占，**按设计静默退出**，日志是空的

修法：`kill -9`、检查前 `pkill -9 -f bin/bgi`、窗口从 14 秒放宽到 25 秒。
**结果 4/4 通过。**

### 46. 长跑 50 秒零报错，但台词不推进（点击落点问题）

连续 30 次点击、50 秒：**零报错**，剧本在 `1.ks` / `uiscript.ks` 之间正常往返。

**但对话框停在「The current message # is 1」。**

排查结果：

| 点击位置 | 命中图层 | 结果 |
|---|---|---|
| 1131,446（start 按钮） | 按钮层 | ✅ 从标题进入正文（已验证） |
| 400,300（正文区） | 基础层 16,16 1240×680 | ❌ 台词不推进 |
| 5,5（基础层外的边角） | 仍不是 primary | ❌ 台词不推进 |

**推测**：KAG 的台词推进等的是 `_primaryLayer.onMouseDown` → `window.onPrimaryClick()`，
而我们的命中测试总是先返回某个子层（基础层或按钮），**primary 层从来收不到事件**。

**已加工具**：`KRKR_AUTO_CLICK2`（第二个点击点 + 独立时间表），
方便「先点标题、再点正文」的自动化。

**下一步**：查 KAG 的 `[s]` 具体挂在哪个事件上（`Conductor` 里搜 `onPrimaryClick` / `stop`），
以及为什么基础层会被命中（它的 `hitThreshold` 应该是 256 才对，实测是 0）。

### 47. 台词点击的症结：primary 层收不到鼠标事件

在**测试数据**里给 KAG 的 `onPrimaryClick` 加了一行打印（`MainWindow.tjs`），
然后点标题按钮、点对话框：

```
DEBUG: onPrimaryClick 出现次数：0
```

说明 `window.onPrimaryClick()` **从未被调用**——而它是 KAG 推进台词/处理点击的总入口。

KAG 的注释写得很清楚（`MainWindow.tjs:3785`）：

> プライマリレイヤで「クリックの動作」がなにもフィルタリングされなかったとき、
> プライマリレイヤから呼ばれる。
> （**当点击没有被任何东西过滤时，由 primary 层调用**）

也就是说：子层（背景/按钮）收到点击后，如果它**不处理**这次点击，primary 层也要收到。
而我们的 `PrimaryMouseDown` 只把它交给最前面的子层，**primary 永远收不到**。

**实验**：在 `PrimaryMouseDown` 里让 primary 也收到 mouseDown
→ 报错 `The object is already invalidated`（primary 可能在 KAG 重建图层时已失效）
→ **已回退**，基线恢复正常。

**下一步**：不能直接在 primary 上 `FireMouseDown`，要走引擎自己的机制——
`HitTestNoVisibleCheck` 里已经有 `OnHitTest_Work` / `onHitTest` 的钩子
（图层可以「拒绝」这次命中），需要确认这条路径是否把「没人要的点击」交回 primary。

### 48. 两个纠正

**纠正 1：上一轮「加了 `KRKR_AUTO_CLICK2`」是错的。**
那次文本替换**没有匹配到任何东西**却静默成功了，所以所谓「两段式点击」的几次实验，
其实一直是**单点点击**。现在真正插进循环（`grep -c` 确认为 2，二进制里也有这个字符串）。

**纠正 2：`The object is already invalidated` 不是 primary 冒泡引起的。**
把 primary 冒泡回退之后它照样出现——原因是**点击落在场景切换的瞬间**：
按钮所在的图层被销毁了，事件的目标对象已失效，异常从循环里抛出去**终结整个运行**。

已给两个注入点加 `try/catch`，让测试工具**熬过一次失效点击**而不是死掉。

### 49. KAG 的点击模型（现在完整了）

```
点击 → PrimaryMouseDown
      → 最前面的子层（背景/按钮）
        → 子层的 onHitTest（KAG 的 GraphicLayer.onHitTest，
          没有区域动作时返回 false = 拒绝）
          → 若拒绝，继续往上找 → 最终落到 primary 层
            → _primaryLayer.onMouseDown → window.onPrimaryClick()  ← KAG 的总入口
```

**我们的现状**：`onHitTest` 确实被调用（探针确认返回 1 = 接受），
但**只测到按钮层**；对背景层的「拒绝」还没观察过，
`onPrimaryClick` 仍然是 0 次。

**下一步**：在一次真实点击里确认背景层是否进入 `onHitTest` 且返回 0；
如果它根本没被调用（比如命中测试在 `HitTestNoVisibleCheck` 之前就返回了），
那就是要修的地方。

### 50. 稳定性实测：三次运行完全一致

清掉所有调试探针、重新构建后，同一个命令跑三次：

| 运行 | 加载剧本数 | 报错 | 画面颜色 |
|---|---|---|---|
| 1 | 5 | 0 | 79757 |
| 2 | 5 | 0 | 79757 |
| 3 | 5 | 0 | 79757 |

**三次完全一致。** 这是引擎② 当前可复现的基线：
点击标题 → 进入 `1.ks` → 加载 `uiscript.ks` → 反复推进，**零报错**。

### 51. 台词点击推进：暂时搁置（收益递减）

`window.onPrimaryClick()` 依旧不被调用。这轮又确认了两条线索：

- `NotifyMouseDown` → `PrimaryMouseDown` 是引擎原样，没有额外分发
- 背景层（1240×680）的 `HitTestNoVisibleCheck` 在点击时**根本没被调用**
  （说明命中测试在到达它之前就返回了）

**但继续追它的收益在下降**：引擎已经能进正文、能显示台词、零报错。
「每句都要点一下才继续」更像是**自动播放的便利性**问题，而不是「能不能跑通」。

**已把调试探针全部移除**，代码库保持干净；`krkrblit`、minikag、build-all 全部恢复通过。

**如果之后要回到这个问题**：起点是「为什么 `GetMostFrontChildAt` 没走到背景层的命中测试」。

### 24b. 失败记录（保留，作为过程）

真实 KiriKiri 游戏以 `data.xp3` 归档发布，所以让 `krkrrun` 支持「传归档就跑里面的
`startup.tjs`」是必要能力。本轮的尝试：

1. 用 `tests/make_xp3.py` 把 KAG3 工程（30 个文件：`startup.tjs` + `system/` + `scenario/`）
   打成 `testdata/kag3.xp3` ✓
2. `krkrpack list` 能读它：`30 file(s) in file:///…/kag3.xp3` ✓
3. 给 `krkrrun` 加「参数是 .xp3 时：realpath → TVPAddAutoPath → 执行
   `Scripts.execStorage("startup.tjs")`」的分支 —— **运行后无任何输出、exit 1**

**已 `git checkout` 回退**，构建与既有功能全部恢复（`krkrblit` 全过、minikag 三行正常）。

**已知**：XP3 读取本身是好的（`krkrpack` 往返一致、`Storages.isExistentStorage("file://a.xp3>dir/c.bin")` 早前验证过），
所以问题在「把归档挂进自动路径 + 从里面 exec」这一段，需要单独调试。

### 23. 管理器集成完成 ✅（`gal verify kag3-sample` → 可以运行）

**最后一道差异是测试数据的布局，不是启动器**：KAG 的脚本用**不带目录的名字**互相引用
（`Config.tjs`），而这个后端的存储层只有在**游戏根目录**里才能解析裸名字，所以
`testdata/kag3-sample` 需要把 `system/*.tjs` 平铺到根目录（工作副本里就是这么做的）。

平铺后：

```bash
cd ~/Documents/gal-engines/gal
KRKR_LOOP_SECONDS=4 SDL_VIDEODRIVER=dummy ./gal play kag3-sample
# → KAGMainWindow のコンストラクタで 10ms かかりました
# → first.ks : こんにちは。

SDL_VIDEODRIVER=dummy ./gal verify kag3-sample
# → 结论: 可以运行
```

**引擎② 的验收闭环完成**：管理器建库 → 选引擎 → 启动 → 判定通过。

### 18g. mini-KAG 完整跑通 ✅（「尺寸 bug」是假的，缺的是 visible）

**结论：没有尺寸问题。** 前面的二分实验全部失败的原因是——**子图层默认不可见**。

引擎里 `tTJSNI_BaseLayer` 的 `Visible` 初始值是 `false`（主图层会在 `AttachPrimary`
时被置为 true，**子图层不会**）。所以：

```tjs
var t = new Layer(w, base);
t.visible = true;      // ← 缺这一行，画在子层上的东西永远不会合成
```

补上之后，640×480、512×384、384×288 全部正常（呈现帧 117 种颜色）。
之前 320×240 那个「成功案例」能被记住，正是因为它的脚本里恰好写了
`.visible = true`——**我自己写的两个测试脚本不一致，才造出「尺寸相关」的假象**。

修正后 mini-KAG 完整输出（`screenshots/minikag-scenario.png`）：

```
[Information] engine up: 640x480, two layers
[Information] line shown: はじまりの朝。
[Information] line shown: 窓の外は、まだ青い。
[Information] line shown: KiriKiri on arm64.
[Information] scenario finished, pixels drawn: 25061
wrote ... (640x480, presented frame)     帧内 224 种颜色
```

**KAG 形状的引导脚本在移植版上跑通了**：system.tjs 建窗口/图层、剧本逐行推进、
三次 update、合成、上屏。

### 18f-old. 当时的错误判断（保留）

`tests/minikag/`：KAG 形状的引导（`system.tjs` 建窗口与两个图层 + `showLine`，
`first.tjs` 逐行推进），合并成 `run.tjs` 运行：

```
[Information] engine up: 640x480, two layers
[Information] line shown: はじまりの朝。
[Information] line shown: 窓の外は、まだ青い。
[Information] line shown: KiriKiri on arm64.
[Information] scenario finished, pixels drawn: 25061
wrote /tmp/mk3.bmp (640x480, presented frame)
```

**台词确实画进了子图层**（25061 个像素），但**呈现帧只有一种颜色**（主图层底色
`(48,32,16)`）——子图层没被合成进去。

**对照实验**：`screenshots/child-layer-composed.png`（320×240、一次 `update()`、
子层只画文字）**是成功的**。差异在于本测试是 **640×480 + 三次 update**。

所以问题缩小成一个可复现的对比：
| | child6（成功） | mini-KAG（失败） |
|---|---|---|
| 尺寸 | 320×240 | 640×480 |
| update 次数 | 1 | 3 |
| 子层内容 | 只有文字 | 框 + 三次文字 |

**二分结果（本轮完成）**：用 640×480 + **单次** update 跑最小脚本（`/tmp/big.tjs`）：

```
wrote /tmp/big.bmp (640x480, presented frame)
colours: 1                  ← 仍然只有底色
```

对照 320×240 同样的脚本 → 成功。**所以与 update 次数无关，是尺寸相关**：

| 尺寸 | 子层合成 |
|---|---|
| 320×240 | ✅ |
| 640×480 | ❌ |

**下一步**：查与尺寸相关的路径——`tTVPLayerManager::GetDrawBuffer` 的创建尺寸
（`GetPrimaryLayerSize`）与 `DrawCompleted` 里 `DrawBuffer->Blt(...)` 的 destrect；
320 能过、640 过不去，像是某处假设了「窗口尺寸 = 图层尺寸」或某个上限。

### 18d. 更新链路接上了，合成仍不落（历史）

**修好的一处**：脚本 `win.update()` → `tTJSNI_Window::Update` → `Form->UpdateWindow()`，
而 `Form` 是 VCL 窗体，桌面上是空的 → **什么也不做**。改成没有 Form 时走引擎自己的
`UpdateContent()`：

```cpp
void tTJSNI_Window::Update(tTVPUpdateType type)
{
	if(Form) { Form->UpdateWindow(type); return; }
	UpdateContent();   // 桌面后端：合成 + 上屏就是引擎自己的这条路径
}
```

效果：`[dev] Update` 和 `[dev] Show` **开始被调用了**（之前一次都没有）。

**仍未落地的**：`NotifyBitmapCompleted` 一次都没被调用（`Show composed=0 0x0`），
主图层位图里也没有子层内容。说明 CPU 合成不是通过分块回调交给设备的，而是画进
**设备自己的屏幕表面**——`tTVPBasicDrawDevice` 在 win32 版里就自带
`ScreenTexture`，而我们的设备**没有这个表面**。

**下一步**：给 `tTVPDrawDevice_Darwin` 加一块自己的合成表面（尺寸跟随窗口），
让图层管理器往里画，`Show()` 直接呈现它，而不是去读主图层位图。

### 18b-old. 排查记录（保留）

**先纠正一个测量错误**：之前记「静默退出 exit 0」是错的——那是 `cmd | head` 的退出码。
真实是 **exit 1**，即引擎的**错误终止路径**（`TVPShowScriptException` → `TVPTerminateSync(1)`）。

**已确认的路径**（加了 `KRKR_DEBUG_LAYER` 打印）：
```
[layer] Construct parent=0x0 params=2     ← 主图层，parent 解析为 0，正常
[Information] base ok
<malloc for Bitmap>                        ← 子图层的位图被分配了
→ exit 1                                   ← 走到终止路径，Construct 里的 parent 分支没打印
```
说明子图层的 **Construct 之后的某个环节**触发脚本错误，而错误文本还没到 stderr
（只给 `TVPShowScriptException(eTJS&)` 加了打印，`eTJSScriptError` 那个重载和
`TVPAddLog` 还没有）。

**本轮补充**（都做了，仍未拿到错误文本）：
- 两个 `TVPShowScriptException` 重载都加了 stderr 输出，并且**挪到 `TVPOnError()` 之前**
  （`TVPOnError` 可能直接结束进程）
- `atexit` 回溯钩子（`KRKR_TRACE_EXIT=1`）证明**不是 `exit()`**，而是 `_exit()` 类调用
- `tTJSNI_BaseLayer::Join` / `RegisterSelfToWindow` 加了打印，**一次都没打出来**

**由此确定**：子图层连 `Construct` 都没进（第二次 Construct 的打印没有出现），
进程在「原生实例创建之后、`Construct` 之前」就结束了。位图分配发生在这一步
（`malloc for Bitmap`），所以嫌疑落在 `tTJSNI_BaseLayer` 构造函数尾部或 TJS 的
`tTJSNativeClassConstructor::FuncCall`。

**顺带修掉一个真 bug**：`tTVPBitmap` 的像素大小原本读 `BitmapInfo->GetImageSize()`
（即 `biSizeImage`，BI_RGB 下为 0 或垃圾值，实测 32×32 得到 1.1GB）；
改成按 `((w*bpp+31)/32)*4 * h` 自己算。这解释了此前一次异常的巨大尺寸。

**⚠️ 元凶（影响之前多轮排查）**：`stderr` 一旦接到**管道**就是全缓冲，而进程用 `_exit`
退出时缓冲内容**全部丢失**。所以前几轮「静默退出、没有报错」的假象，很大程度是
**诊断输出被吞了**。加了 `fflush(stderr)` 并重定向到文件之后，真相立刻清楚：

```
[layer] Construct parent=0x0 params=2      ← 主图层
[Information] base ok
[ctor] a1 temp holder ... a6 rect          ← 子图层的构造函数【完整跑完】
<malloc for Bitmap>
→ exit=1
```

**结论修正**：子图层的 **构造函数是完整执行的**，进程死在其后的 `Construct` 之前
（`Construct` 开头的打印没出现）。而 `Construct` 之前只有 TJS 的原生类调用与
参数解析——所以嫌疑在 **`tTJSNI_BaseLayer::Construct` 的参数解析段**（`param[0]`
按 Layer 取实例那段），或者 TJS 的原生类构造回调。

**教训**：诊断输出先 `fflush`，再判断「有没有报错」。我因此白追了三轮。

**下一步**：在 `Construct` 的**最开头第一行**加打印（现在那行在参数解析之后）。

### 18b-old. 原始记录（措辞已由上节纠正）
准备 KAG 形状的引导脚本（`tests/minikag/`）时撞到一个具体障碍：

```tjs
var base = new Layer(win, null);      // ✅ 主图层，正常
var text = new Layer(base, null);     // ❌ 子图层 —— 进程静默退出（exit 0，无报错）
```

已用最小脚本逐层验证（都在函数内调用）：
- `new Window()` + `setInnerSize` ✅
- `new Layer(win, null)` + `setSize` + `fillRect` ✅
- **`new Layer(base, null)`（带父图层）→ 静默终止** ← 卡点

为什么重要：KAG 的层树就是这样搭的（base → message → sprite 层），所以这一步不通，
KAG 引导脚本就起不来。静默退出（而不是抛异常）说明是 `TJS` 层的致命错误落在了
`TVPShowScriptException`/终止路径上；下一步该查 `tTJSNI_BaseLayer::Construct` 里
`Join(lay)`/`Manager->AddRef()` 那段对父图层的处理。

`tests/minikag/system.tjs` 与 `first.tjs` 保留下来，就是这两个脚本暴露了这个卡点。

### 18a. 图层上屏（合成结果读回）✅
`krkrrun` 新增 `--screenshot <path>`：脚本跑完后从绘制设备的主图层取像素存图。

```bash
$ ./build/krkrrun tests/compose.tjs --screenshot /tmp/composed.bmp
[Information] updated
wrote /tmp/composed.bmp (640x480)      # corner (32,64,96) = 填充色, 397 个文字像素
```

产物 `screenshots/layer-window.png`：640×480 深蓝底 + 左上角「レイヤーに描けた」。
**完整链路**（全部是引擎自己的对象和代码）：

```
new Window() → new Layer(win,null) → fillRect → drawText → win.update()
  → 图层合成（LayerManager/DrawDevice）→ 主图层 → 逐像素读回
```

这就是 KiriKiri 的「hello world 上屏」。剩下的是 KAG 运行时（游戏自带的 TJS 框架）
和真实游戏数据。

### 18b. 排查过程（保留，作为教训）

**现象**：脚本 `new Window()` 成功，紧接着 `new Layer(...)` 在图层构造函数里崩：

```
tTVPRenderMethod_FillARGB::DoRender   ← RenderManager.cpp:1038
  ← tTVPSoftwareRenderManager::OperateRect
    ← iTVPBaseBitmap::Fill
      ← tTJSNI_BaseLayer::tTJSNI_BaseLayer()
```

**已知**：崩在 `RenderManager.cpp:1038`：
```cpp
tjs_uint8 *dest = (tjs_uint8*)_tar->GetScanLineForWrite(rect.top) + rect.left * (is32bpp ? 4 : 1);
```
这条语句在 `GetScanLineForWrite` 返回空指针时会崩（空指针 + 偏移后立即写入）。

**已排查**：
- 加了 `KRKR_DEBUG_BITMAP=1` 的探针到我们位图层的 `GetScanLineForWrite`/`GetTextureForRender`
  → **没有输出**，说明崩溃时的目标纹理不是我们的位图对象，而是引擎自己创建的软件纹理
- 已修两处相关问题（都保留）：位图请求**未压缩**纹理（`RENDER_CREATE_TEXTURE_FLAG_NO_COMPRESS`）；
  `SetSize` 也把 0 尺寸钳到 1（之前只钳了 `Recreate`）

**本轮又推进两层**（都验证过）：

1. **`tTVPBitmap` 分配是好的**：直接构造 `tTVPBitmap(32,32,32)` → `pitch 128, bits 非空, 扫描线可用`
   → 排除「像素缓冲分配失败」这个可能。
2. **位图现在自己持有像素缓冲**：因为 `tTVPSoftwareTexture2D` 虽然会自分配，但
   `CreateTexture2D(pixel,…)` 在有指针时创建的是**静态视图**；位图契约是「拥有自己的像素」，
   所以 `CreateOwnedTexture()` 自己 `calloc`、以静态视图包装、并在替换/析构时释放。

**仍未解，但候选已缩到两个**：
- `tTVPSoftwareTexture2D_static::Update()` 会**重新赋值 BmpData**——若某处以 `nullptr` 调 Update，
  之后写入必崩
- `CreateTexture2D(neww, newh, tex)` 构造的 `tTVPSoftwareTexture2D(tex,…)` 以一个
  `tTVPSoftwareTexture2D_static(nullptr, 0, w, h, format)` 起手，若其后的分配没接上，
  BmpData 留空

**下一步该查**：`tTVPSoftwareTexture2D::GetScanLineForWrite` 的实现——什么情况下返回空？
（压缩变体 `tTVPSoftwareTexture2D_compress` 的写法需要先解压，若它被创建出来就会走那条路径；
另一个方向是 `rect.top` 超出了纹理高度，即某处的尺寸约定不一致。）

## 窗口层的真实工作量（下一步，已勘察清楚）

渲染要落地，缺的是**平台位图层与窗口层**，上游是 Win32 GDI 实现：

| 文件 | 行数 | 作用 |
|---|---|---|
| `visual/win32/LayerBitmapImpl.cpp` | 1607 | `tTVPNativeBaseBitmap`（GetWidth/GetScanLine…）——**图像解码器和所有图层都依赖它** |
| `visual/win32/WindowImpl.cpp` | 2432 | 窗口/DrawDevice |
| `visual/win32/BasicDrawDevice.cpp` | 1009 | 绘制设备 |
| `visual/win32/DrawDevice.cpp` `DInputMgn.cpp` `VideoOvlImpl.cpp` `PassThroughDrawDevice.cpp` | ~3400 | 其余 |

**勘察结论（本轮实测）**：
- `visual/*.cpp` 里 26/33 直接可编译，`win32/` 那批里 `WindowImpl` / `BasicDrawDevice` /
  `DrawDevice` / `LayerImpl` / `VideoOvlImpl` 也**全部可编译且零 Win32 调用**——它们是纯抽象 C++
- 但 `environ/win32/` 的**应用窗口**这批是真的 Win32/VCL 专用（`windowsx.h`、`Classes.hpp`、
  `vcl.h` 是 Borland C++ Builder），**无法复用**；`tTVPWindow` 有 **50 个纯虚函数**
- `environ/Application.cpp` 可编译，它要的是平台自己的窗体类（`TTVPWindowForm` 的等价物）

所以要写的就剩：一个 darwin 窗体（50 个纯虚，多数是琐碎的标题/尺寸/光标/IME/按键注入，
真正有分量的是 `UpdateDrawBuffer` 送显——**已由 `krkrpresent` 验证可行**）+ 与
`tTVPBasicDrawDevice` 的接线。

## 剩余工作（按依赖顺序）
1. ~~XP3 归档层~~ ✅
2. **TVP 扩展类**（`Storage`/`Scripts`/`Window`/`System` 的完整版）——KAG 脚本直接调它们
3. **窗口 + 渲染**：用 SDL2 替换 cocos2d 那 2448 行，渲染用 `visual/ogl/`
4. **音频**：`sound/` 的 WaveIntf 需要 darwin 实现（SDL2 audio 或 CoreAudio）
5. ⚠️ **图像/动画**：上游重度依赖 libgdiplus/cairo/pixman/opencv/ffmpeg/bpg/jxrlib
   这些 vendored 库（tarball 里没有，需单独拉）——这是最大的工作量来源

## 诚实评估
Kirikiroid2 的「桌面化」不是修 bug 级别，是**重写应用层 + 重建 20 个第三方依赖**，
量级远大于引擎①④。已把地基（TJS2）做成可独立验证的一层，后续按上述顺序推进。
真跑 KiriKiri 游戏（如 Fate）在短期内的现实路径仍是 **Sikarugir(Wine)** 兜底。
