/* video_player_frame.h -- the seam between the two halves of the macOS
 * movie backend.
 *
 * Why this file exists
 * --------------------
 * AVFoundation's umbrella header pulls in ApplicationServices -> Quickdraw.h,
 * which defines `struct FontInfo` with no include guard.  The engine defines
 * `class FontInfo` in FontInfo.h.  The two cannot coexist in one translation
 * unit: whichever comes second is a redefinition error, and suppressing
 * Quickdraw entirely breaks HIServices (RgnHandle/RGBColor).
 *
 * So the backend is split in two, and this header is the only thing they
 * share:
 *
 *   video_player_mac.mm     framework-only TU (AVFoundation, CoreMedia,
 *                           CoreVideo).  Never includes ONScripter.h.
 *   video_player_bridge.cpp engine-only TU (ONScripter.h, Utils.h).  Never
 *                           imports a framework.
 *
 * Decoding lives on the framework side, presentation and event handling on
 * the engine side; `video_host` carries the engine's callbacks across.
 */
#ifndef ONSCRIPTER_VIDEO_PLAYER_FRAME_H
#define ONSCRIPTER_VIDEO_PLAYER_FRAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel order of the buffer handed to video_host::frame_cb. */
enum {
    VIDEO_PIX_BGRA = 0,   /* AVFoundation kCVPixelFormatType_32BGRA */
    VIDEO_PIX_RGBA = 1    /* libmpv sw render, rgba8888 */
};

typedef struct video_host {
    void *user;

    /* Called once per decoded frame, on the thread running playback.
     * `px` is only valid for the duration of the call -- blit, do not keep.
     * `pitch` is the source row stride in bytes. */
    void (*frame_cb)(void *user, const unsigned char *px,
                     int w, int h, int pitch, int pixel_order);

    /* Polled once per frame.  Nonzero ends playback early (click, ESC,
     * skip/automode, SDL_QUIT).  The implementation is expected to run
     * SDL_PumpEvents() itself -- the decoder halves never pump events. */
    int (*abort_cb)(void *user);
} video_host;

/* Implemented in video_player_mac.mm (framework-only TU).
 * Returns true if at least one frame was presented. */
int avfPlayFrames(const char *abs_path, int click_stop, int loop,
                  const video_host *host);

/* ------------------------------------------------------------------
 * Asynchronous playback -- `movie "file",...,async` and `movie stop`.
 *
 * Decoding runs on a worker thread that never touches SDL; it only fills a
 * single "latest frame" slot under a mutex.  The engine's main thread owns
 * rendering and calls the tick below once per loop iteration.  That split is
 * deliberate: SDL's renderer is not safe to call off the main thread.
 *
 * avfStartAsync   starts (replacing any current async movie). 0 = started.
 * avfStopAsync    stops and joins; safe to call when nothing is playing.
 * avfIsAsyncPlaying  nonzero while a movie is being decoded.
 * avfTickAsync    main thread: present the newest decoded frame, if any.
 *                 Returns 1 if a frame was presented.
 * ------------------------------------------------------------------ */
int  avfStartAsync(const char *abs_path, int loop);
void avfStopAsync(void);
int  avfIsAsyncPlaying(void);
int  avfTickAsync(const video_host *host);

/* ------------------------------------------------------------------
 * Layer-plugin playback -- `setlayer N,..,"dll\wcmpg.dll"` followed by
 * layermessage open/play/pause/close.  Same worker thread as the async
 * player (a game only drives one movie layer at a time in practice), but
 * instead of rendering, the engine's Layer::refresh() pulls the newest
 * decoded frame and blits it wherever the layer says.
 *
 * avfLayerOpen   starts decoding (paused, unlooped); 0 = ok
 * avfLayerPlay   starts/resumes advancing
 * avfLayerPause  0 resumes, nonzero pauses
 * avfLayerClose  stops and joins
 * avfLayerFrame  newest frame, or 0 if none new since the last call.
 *                The pointer is valid until the next decode into the slot;
 *                copy or blit immediately.
 * ------------------------------------------------------------------ */
int  avfLayerOpen(const char *abs_path, int loop);
/* Same, from bytes the engine's archive reader handed us (a wrong offset is
 * not a reason to give up: the bytes are checked for a movie container
 * signature before use).  The bytes are copied to a cache file under TMPDIR
 * named after the entry, so a repeated open of the same movie is cheap. */
int  avfLayerOpenMemory(const char *entry_name, const void *data, int size, int loop);
int  avfLayerPlay(void);
int  avfLayerPause(int pause);
void avfLayerClose(void);
int  avfLayerFrame(const unsigned char **px, int *w, int *h, int *pitch);

#ifdef __cplusplus
}
#endif

#endif /* ONSCRIPTER_VIDEO_PLAYER_FRAME_H */
