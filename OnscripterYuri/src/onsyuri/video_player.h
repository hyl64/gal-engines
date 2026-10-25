#ifndef ONSCRIPTER_VIDEO_PLAYER_H
#define ONSCRIPTER_VIDEO_PLAYER_H

/* The macOS movie backend's public surface, as fixed by
 * gal-engines/DESIGN-VIDEO.md.  Two implementations share it:
 *   video_player_bridge.cpp  videoPlayBlocking (engine half)
 *   video_player_mac.mm      videoMapLookup   (sidecar parser, no engine deps)
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 true 表示"已在本机播放"；false 表示不可用/未播放，调用方走原禁用逻辑。
 * 阻塞直到播放结束：
 *   click_stop=true 且期间发生左键点击或 esc/skip → 提前结束，返回 true
 *   click_stop=false → 必须播完，忽略点击（与 NScripter movie,click=0 一致）
 * loop 仅 mpegplay/movie 使用；avi 传 false。 */
bool videoPlayBlocking(const char *abs_path, bool click_stop, bool loop);

/* 侧车映射：若 game_root/_video/video_map.json 有 from -> to（to 为相对
 * game_root 的路径），返回 malloc 拷贝的 to，否则返回 NULL。调用方负责 free()。
 * 大小写不敏感、'/' 与 '\\' 等价。 */
char *videoMapLookup(const char *game_root, const char *script_path);

/* movie ... async / movie stop.
 * videoStartAsync returns true when a background decode actually started.
 * videoTickAsync is called from the engine's event loop on the MAIN thread and
 * presents the newest decoded frame; it returns nonzero if it drew one. */
bool videoStartAsync(const char *abs_path, bool loop);
void videoStopAsync(void);
bool videoIsAsyncPlaying(void);
bool videoTickAsync(void);

#ifdef __cplusplus
}
#endif

#endif /* ONSCRIPTER_VIDEO_PLAYER_H */
