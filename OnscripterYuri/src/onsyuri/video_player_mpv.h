/* Copyright (C) gal-engines T4
 *
 * libmpv fallback video backend for the NScripter movie/avi commands.
 *
 * This backend has NO compile-time or link-time dependency on libmpv:
 * the library is located and loaded at run time with dlopen()/dlsym()
 * (see DESIGN-VIDEO.md "T4 的 mpv 接入方式"). The libmpv client headers
 * used here are vendored under OnscripterYuri/mpvdev/ (ISC-licensed
 * client API; note that the mpv *core* is GPLv2+/LGPLv2+ depending on
 * build flags — we only *load* it at runtime, we never link against it,
 * which keeps this source tree free of a build-time mpv dependency).
 *
 * If libmpv.2.dylib cannot be found (mpv not installed), every entry
 * point below simply returns false / does nothing — the engine keeps
 * its previous "disabled" behavior.
 */
#ifndef ONSCRIPTER_VIDEO_PLAYER_MPV_H
#define ONSCRIPTER_VIDEO_PLAYER_MPV_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional per-frame callback. Invoked from the thread that called
 * mpvPlayBlocking() every time a *new* video frame has been rendered
 * into the backend's internal RGBA8888 buffer.
 *
 *   rgba : pointer to the internal buffer; layout = 4 bytes per pixel,
 *          byte order in memory R,G,B,A (SDL_PIXELFORMAT_RGBA8888 on
 *          little-endian). Row pitch is (w*4) rounded up to 64 bytes.
 *          The pointer is ONLY valid during the callback — copy/blit it,
 *          do not retain it.
 *   w,h  : frame dimensions in pixels.
 *   user : opaque pointer passed to mpvSetFrameCallback().
 *
 * The engine side (T1) is expected to blit this into its SDL surface and
 * repaint. This backend never touches SDL surfaces itself.
 */
typedef void (*mpv_frame_cb)(const unsigned char *rgba, int w, int h,
                             void *user);

/*
 * Install (or with cb==NULL remove) the frame callback. Thread-safe
 * enough for the intended use: set it before calling mpvPlayBlocking().
 * Default is NULL — frames are then still decoded and timed but no
 * pointer is handed out (mpvPlayBlocking still blocks normally).
 */
void mpvSetFrameCallback(mpv_frame_cb cb, void *user);

/*
 * Polling alternative to the callback (the "dirty 标志" form the task
 * asks for): returns true once per NEW video frame. Fills rgba/w/h from
 * the internal buffer; same lifetime rules as mpv_frame_cb — the pointer
 * is valid until the frame is overwritten by the next one, and both the
 * callback and this poll run on the mpvPlayBlocking() thread, so blit
 * right there. Returns false when there is no new frame (cheap: one
 * atomic load) or when no playback is active.
 */
bool mpvPollFrame(const unsigned char **rgba, int *w, int *h);

/*
 * Cheap probe: returns true if a libmpv shared library can be dlopen'ed
 * and exports all required client-API symbols. Result is cached.
 * Used only for logging/decisions; mpvPlayBlocking() works regardless.
 */
bool mpvIsAvailable(void);

/*
 * Blocking fallback playback of any media file libmpv can decode.
 * Contract (DESIGN-VIDEO.md):
 *   - returns true  => the file was played on this machine (fully, or
 *                      stopped early by a click when click_stop==true);
 *                      the caller should treat it as "played" (endCommand).
 *   - returns false => backend unavailable / file not playable; caller
 *                      keeps its old "disabled" path.
 *   - click_stop==true : a left mouse button press (edge), ESC, SPACE or
 *                        LCTRL (engine skip key) inside the loop ends
 *                        playback early (still returns true).
 *   - click_stop==false: clicks are ignored, playback must run to EOF.
 *   - loop             : mpv loop-file=inf. NOTE: loop with
 *                        click_stop==false would never terminate; in that
 *                        combination we fall back to single playback.
 *
 * Internals: SW render API (MPV_RENDER_API_TYPE_SW), no mpv window is
 * ever created; audio goes through mpv's own ao (coreaudio). Events are
 * pumped with SDL_PumpEvents() in the render loop (when built with SDL2;
 * define ONSYURI_MPV_NO_SDL to compile without SDL — then click_stop
 * never triggers, everything else works).
 *
 * Environment knobs (for diagnostics / CI, safe to ignore in production):
 *   ONSYURI_MPV_DISABLE=1      -> always return false (kill switch)
 *   ONSYURI_MPV_LIB="a:b.dylib"-> colon-separated dlopen candidate list,
 *                                 replaces the built-in candidates.
 *   ONSYURI_MPV_AO=null        -> force mpv null audio output (headless CI)
 *   ONSYURI_MPV_MAX_PLAY=N     -> stop after N seconds (harness/demo cap)
 *   ONSYURI_MPV_MAXW/_MAXH=N   -> SW render size cap (default 1280x720;
 *                                 set 1920/1080 to measure 1080p fallback)
 *
 * abs_path must be an existing path (filesystem or game-dir joined);
 * a missing file makes this return false without crashing.
 */
bool mpvPlayBlocking(const char *abs_path, bool click_stop, bool loop);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ONSCRIPTER_VIDEO_PLAYER_MPV_H */
