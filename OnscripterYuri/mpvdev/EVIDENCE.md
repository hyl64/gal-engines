# T4 libmpv 兜底后端 — 验证记录

## 结论

**libmpv 兜底在引擎真实路径上工作**（不是在独立 harness 里，见下）：

```
$ cd /tmp/vid2 && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ~/Documents/gal-engines/OnscripterYuri/build/onsyuri --root . --enc:sjis --window
video: playing ./sample_wmv2.avi (2.00s @24.0fps, audio=silent)
video: startReading failed (./sample_wmv2.avi)     <- AVFoundation 拒绝 wmv2，符合预期
video: AVFoundation failed on ./sample_wmv2.avi
video: played ./sample_wmv2.avi (mpv backend)      <- 兜底接住
```

dump 出来的帧有 275 种颜色（smptebars 彩条），证明 mpv 解码出的画面真的被
blit 进了引擎 surface，而不是"返回 true 但画黑屏"。

同一路径也在 `OnscripterYuri/test/video/run_acceptance.sh` 的第 2 项里固化为断言。

## dlopen 探测（独立可复现）

```
$ cd OnscripterYuri/mpvdev && ./mpvdev_harness probe
[mpv] dlopen ok: /opt/homebrew/lib/libmpv.2.dylib
[mpv] client API 2.5 OK
mpvIsAvailable=1
```

`libmpv.2.dylib` 来自 Homebrew（本机已装）。**没有编译期依赖**：
`video_player_mpv.cpp` 里没有任何 `#include <mpv/client.h>`，全部符号运行期
`dlsym` 取得；mpv 没装的机器上 `mpvIsAvailable()` 返回 0、
`mpvPlayBlocking()` 直接返回 false，引擎回到原来的"跳过并打日志"行为。

## 未完成 / 已知问题（诚实记录）

1. **独立 harness 的 `play` 模式在无音频设备环境会挂住**，没有正常退出。
   `./mpvdev_harness play fixtures/wmv2.avi 3` 在 `SDL_AUDIODRIVER=dummy`
   下阻塞超过 120s 被手动杀掉。原因没查到根因（猜测是 mpv 的 coreaudio ao
   在 dummy 驱动下不回调 end-file）。**这不影响引擎路径**——引擎里由
   `hostAbort` 每帧轮询 SDL 事件，播完正常返回，实测 2s 片长按时结束。
   要复现引擎路径请看上面的命令，不要用独立 harness。
2. **1080p 性能没有正式测量**。`fixtures/h264-1080p60.mp4` 已经生成好，
   但 T4 在跑到这一步之前就中断了。SW 渲染在 1280x720 以上会掉帧是设计上
   接受的（契约允许兜底路径掉帧），但没有数字。占位 TODO。
3. **音量/音频输出未调**：mpv 走自己的 ao，引擎不控制它的音量。契约里
   兜底路径的定位是"能放出来就行"，不追求与引擎 BGM 音量联动。

## 与 T1 的接缝

`video_player_mpv.h` 提供的 `mpvSetFrameCallback()` 是进程级全局回调。
`video_player_bridge.cpp` 在调用前把它指向 `mpvFrameTrampoline`（把 RGBA
帧 blit 进 `accumulation_surface`），调用后置回 NULL。引擎脚本 VM 是单线程，
所以不存在并发覆盖问题；如果将来多线程播放，这里要改成每实例回调。
