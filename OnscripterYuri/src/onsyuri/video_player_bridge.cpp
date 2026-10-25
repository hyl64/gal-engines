/* -*- C++ -*-
 *  video_player_bridge.cpp - engine half of the macOS movie backend.
 *
 *  Owns everything that needs to know about ONScripter: presenting a frame
 *  into accumulation_surface, uploading it to the engine texture, presenting
 *  through the renderer, and pumping SDL events to detect the click that
 *  aborts playback (DESIGN-VIDEO.md contract).
 *
 *  Decoding lives in video_player_mac.mm (AVFoundation) and
 *  video_player_mpv.cpp (libmpv, dlopen).  They are framework-only TUs and
 *  must never include ONScripter.h: AVFoundation reaches Quickdraw.h's
 *  `struct FontInfo`, which collides with the engine's `class FontInfo`.
 *  video_player_frame.h is the seam between the halves.
 *
 *  Route, fixed by the contract: AVFoundation -> libmpv -> give up (false),
 *  where false keeps the caller's original "disabled" log+skip behaviour.
 *
 *  Copyright (c) 2026 gal-engines project (task T1).
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "video_player.h"
#include "video_player_frame.h"
#include "video_player_mpv.h"
#include "Utils.h"

/* Engine internals (accumulation_surface / texture / renderer /
 * render_view_rect / gles_renderer / ctrl_pressed_status) are private or
 * protected.  Rather than edit ONScripter.h (outside T1's ownership matrix)
 * we relabel access for THIS compilation unit only; C++ name mangling does
 * not encode access specifiers, so the link stays consistent with every
 * other object file.  The decoder TUs never do this -- keeping the relabel
 * to a single engine-only file is the point of the split. */
#define private public
#define protected public
#include "ONScripter.h"
#undef protected
#undef private

extern ONScripter ons;

/* Decoder half (video_player_mac.mm). */
extern "C" int avfPlayFrames(const char *abs_path, int click_stop, int loop,
                             const video_host *host);
extern "C" int  avfStartAsync(const char *abs_path, int loop);
extern "C" void avfStopAsync(void);
extern "C" int  avfIsAsyncPlaying(void);
extern "C" int  avfTickAsync(const video_host *host);

/* ------------------------------------------------------------------ */
/* Presentation                                                        */
/* ------------------------------------------------------------------ */
static void presentSurface()
{
    if (!ons.texture || !ons.renderer) return;
    SDL_SetRenderDrawColor(ons.renderer, 0, 0, 0, 255);
    SDL_RenderClear(ons.renderer);
#if defined(USE_GLES)
    if (!isnan(ons.sharpness) && ons.gles_renderer){
        ons.gles_renderer->copy(ons.render_view_rect.x, ons.render_view_rect.y);
    }
    else
#endif
    {
        SDL_RenderCopy(ons.renderer, ons.texture, NULL, &ons.render_view_rect);
    }
    SDL_RenderPresent(ons.renderer);
}

/* Copies (and nearest-neighbour scales) a decoded frame into the engine's
 * accumulation surface, uploads it to the engine texture and presents.
 * pixel_order follows video_player_frame.h: VIDEO_PIX_BGRA from AVFoundation,
 * VIDEO_PIX_RGBA from libmpv's sw renderer. */
static void presentFrame(const unsigned char *src, int w, int h, int src_pitch,
                         int pixel_order)
{
    SDL_Surface *dst = ons.accumulation_surface;
    if (!dst || dst->format->BytesPerPixel != 4 || w <= 0 || h <= 0 || !src)
        return;

    /* kCVPixelFormatType_32BGRA is memory order B,G,R,A -- exactly the
     * little-endian byte layout of SDL_PIXELFORMAT_ARGB8888, which is the
     * engine texture_format on macOS.  Swizzle only when the target surface
     * is RGBA/ABGR ordered (the GLES path picks those), or when the producer
     * already handed us RGBA and the surface wants BGRA. */
    const bool dst_is_rgba =
        (dst->format->format == SDL_PIXELFORMAT_ABGR8888 ||
         dst->format->format == SDL_PIXELFORMAT_RGBA8888);
    const bool swap_rb = (pixel_order == VIDEO_PIX_RGBA) ? !dst_is_rgba
                                                         : dst_is_rgba;

    /* NOTE: a "render straight to the renderer, skip accumulation_surface"
     * fast path was tried and reverted.  It measured no frame-rate gain
     * (decode, not scaling, is the bottleneck: 180/180 either way) and it
     * broke everything that reads the surface -- ONS_SURFACE_DUMP,
     * savescreenshot and the save-thumbnail path all saw a stale frame.
     * Correct and fast beats clever here. */

    const size_t dw = (size_t)dst->w, dh = (size_t)dst->h;
    const size_t dpitch = (size_t)dst->pitch;

    SDL_LockSurface(dst);
    uint8_t *dpx = (uint8_t*)dst->pixels;

    /* movie ...,pos,x,y,w,h : draw into that rectangle of the surface instead
     * of filling it.  Coordinates are game-space (0..screen_width), so they go
     * through the same ratio the engine uses for sprites.  Everything outside
     * the rectangle keeps whatever the engine drew there (normally a bg). */
    size_t tx = 0, ty = 0, tw = dw, th = dh;
    if (ons.movie_pos_flag && ons.screen_width > 0 && ons.screen_height > 0){
        const size_t x0 = (size_t)ons.movie_pos_x * dw / (size_t)ons.screen_width;
        const size_t y0 = (size_t)ons.movie_pos_y * dh / (size_t)ons.screen_height;
        size_t w0 = (size_t)ons.movie_pos_w * dw / (size_t)ons.screen_width;
        size_t h0 = (size_t)ons.movie_pos_h * dh / (size_t)ons.screen_height;
        if (w0 > 0 && h0 > 0 && x0 < dw && y0 < dh){
            tx = x0; ty = y0;
            tw = (x0 + w0 <= dw) ? w0 : dw - x0;
            th = (y0 + h0 <= dh) ? h0 : dh - y0;
        }
    }

    if (tw == (size_t)w && th == (size_t)h && tx == 0 && ty == 0){
        /* 1:1 copy -- the common full-screen case, and also a pos rect that
         * happens to match the frame size exactly. */
        for (size_t y = 0; y < th; y++){
            const uint8_t *s = src + y * (size_t)src_pitch;
            uint8_t *d = dpx + (ty + y) * dpitch + tx * 4;
            if (!swap_rb){
                memcpy(d, s, tw * 4);
            }
            else {
                for (size_t x = 0; x < tw; x++){
                    d[x*4+0] = s[x*4+2];
                    d[x*4+1] = s[x*4+1];
                    d[x*4+2] = s[x*4+0];
                    d[x*4+3] = 0xff;
                }
            }
        }
    }
    else {
        /* nearest-neighbour scale; the soft-decode fallback path tolerates
         * this cost (contract allows drops at 1080p on that path). */
        for (size_t y = 0; y < th; y++){
            size_t sy = (y * (size_t)h) / th;
            const uint8_t *s = src + sy * (size_t)src_pitch;
            uint8_t *d = dpx + (ty + y) * dpitch + tx * 4;
            for (size_t x = 0; x < tw; x++){
                size_t sx = (x * (size_t)w) / tw;
                const uint8_t *p = s + sx * 4;
                if (!swap_rb){
                    d[x*4+0] = p[0];
                    d[x*4+1] = p[1];
                    d[x*4+2] = p[2];
                }
                else {
                    d[x*4+0] = p[2];
                    d[x*4+1] = p[1];
                    d[x*4+2] = p[0];
                }
                d[x*4+3] = 0xff;
            }
        }
    }

    /* ONS_VIDEO_PIXEL_LOG=1: report one pixel inside the movie's target rect
     * and one well outside it, read straight from the surface we just wrote.
     * This is how `movie ...,pos` gets verified -- SDL_SaveBMP on this surface
     * does not reliably reflect what was written, so a dump-based check would
     * report a false negative. */
    {
        const char *e = getenv("ONS_VIDEO_PIXEL_LOG");
        if (e && *e){
            static int logged = 0;
            if (logged++ < 1){
                const size_t ix = tx + tw / 2, iy = ty + th / 2;
                const size_t ox = dw > 20 ? dw - 10 : 0, oy = dh > 20 ? dh - 10 : 0;
                if (ix < dw && iy < dh){
                    const uint8_t *a = dpx + iy * dpitch + ix * 4;
                    const uint8_t *b = dpx + oy * dpitch + ox * 4;
                    utils::printInfo("video: pos rect=%zu,%zu %zux%zu inside=#%02x%02x%02x outside=#%02x%02x%02x\n",
                                     tx, ty, tw, th, a[2], a[1], a[0], b[2], b[1], b[0]);
                }
            }
        }
    }

    SDL_UnlockSurface(dst);

    if (ons.texture)
        SDL_UpdateTexture(ons.texture, NULL, dpx, (int)dpitch);
    presentSurface();
}

/* ------------------------------------------------------------------ */
/* Event pump -- contract: SDL_PumpEvents inside the playback loop.    */
/* Mirrors the USE_SMPEG branch: left/right click or RETURN/SPACE/     */
/* ESCAPE end playback; skip/automode (engine getSkip) also counts.    */
/* click_stop=false: everything except SDL_QUIT is ignored, exactly    */
/* like NScripter movie,click=0.                                       */
/* ------------------------------------------------------------------ */
static bool pollAbortEvent(bool click_stop)
{
    SDL_PumpEvents();

    SDL_Event ev;
    bool got_click = false;
    bool quit = false;
    while (SDL_PollEvent(&ev)){
        switch (ev.type){
          case SDL_MOUSEBUTTONDOWN:
          case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT ||
                ev.button.button == SDL_BUTTON_RIGHT)
                got_click = true;
            break;
          case SDL_KEYDOWN:
          case SDL_KEYUP:
            if (ev.key.keysym.sym == SDLK_RCTRL && ev.type == SDL_KEYUP)
                ons.ctrl_pressed_status &= ~0x01;
            if (ev.key.keysym.sym == SDLK_LCTRL && ev.type == SDL_KEYUP)
                ons.ctrl_pressed_status &= ~0x02;
            if (ev.key.keysym.sym == SDLK_ESCAPE ||
                ev.key.keysym.sym == SDLK_RETURN ||
                ev.key.keysym.sym == SDLK_SPACE)
                got_click = true;
            break;
          case SDL_QUIT:
            quit = true;
            break;
          default:
            break;
        }
    }
    if (quit){
        /* re-post so the engine's own loop still sees it after the movie */
        SDL_Event q;
        SDL_zero(q);
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
        return true;
    }
    if (!click_stop) return false;      /* movie,click=0: ignore clicks */
    if (got_click) return true;
    if (ons.getSkip()) return true;     /* skip/automode act as a click */
    return false;
}

/* ------------------------------------------------------------------ */
/* video_host glue                                                     */
/* ------------------------------------------------------------------ */
struct BridgeState {
    bool click_stop;
};

static void hostFrame(void *user, const unsigned char *px,
                      int w, int h, int pitch, int pixel_order)
{
    (void)user;
    presentFrame(px, w, h, pitch, pixel_order);
}

static int hostAbort(void *user)
{
    BridgeState *st = (BridgeState*)user;
    return pollAbortEvent(st ? st->click_stop : false) ? 1 : 0;
}

static void maybeDumpLastFrame()
{
    const char *e = getenv("ONS_VIDEO_DUMP");
    if (!e || !*e || !ons.accumulation_surface) return;
    if (SDL_SaveBMP(ons.accumulation_surface, e) == 0)
        utils::printInfo("video: last frame dumped to %s\n", e);
    else
        utils::printInfo("video: frame dump to %s failed: %s\n", e, SDL_GetError());
}

/* libmpv hands frames over through a process-global callback, so stash the
 * active bridge state here.  Playback is strictly single-threaded. */
static void mpvFrameTrampoline(const unsigned char *rgba, int w, int h, void *user)
{
    (void)user;
    presentFrame(rgba, w, h, w * 4, VIDEO_PIX_RGBA);
}

/* ------------------------------------------------------------------ */
/* Contract entry point                                                 */
/* ------------------------------------------------------------------ */
bool videoPlayBlocking(const char *abs_path, bool click_stop, bool loop)
{
    if (!abs_path || !abs_path[0]) return false;

    BridgeState st;
    st.click_stop = click_stop;

    video_host host;
    host.user     = &st;
    host.frame_cb = &hostFrame;
    host.abort_cb = &hostAbort;

    /* 1. AVFoundation (hardware decode, the transcode-first main route). */
    if (avfPlayFrames(abs_path, click_stop ? 1 : 0, loop ? 1 : 0, &host)){
        utils::printInfo("video: played %s (AVFoundation)\n", abs_path);
        maybeDumpLastFrame();
        return true;
    }

    /* 2. libmpv fallback (dlopen; no compile-time dependency). */
    mpvSetFrameCallback(&mpvFrameTrampoline, &st);
    const bool mpv_ok = mpvPlayBlocking(abs_path, click_stop, loop);
    mpvSetFrameCallback(NULL, NULL);
    if (mpv_ok){
        utils::printInfo("video: played %s (mpv backend)\n", abs_path);
        maybeDumpLastFrame();
        return true;
    }

    utils::printInfo("video: no backend available for %s\n", abs_path);
    return false;
}

/* ------------------------------------------------------------------ */
/* movie ... async / movie stop                                        */
/* ------------------------------------------------------------------ */
static video_host s_async_host;   // filled once at first use

bool videoStartAsync(const char *abs_path, bool loop)
{
    if (!abs_path || !abs_path[0]) return false;
    return avfStartAsync(abs_path, loop ? 1 : 0) == 0;
}

void videoStopAsync(void)
{
    avfStopAsync();
}

bool videoIsAsyncPlaying(void)
{
    return avfIsAsyncPlaying() != 0;
}

/* Main thread, called from runEventLoop's timeout.  Async movies have no
 * click-to-skip semantics (the script keeps running), so abort stays NULL. */
bool videoTickAsync(void)
{
    static bool inited = false;
    if (!inited){
        inited = true;
        s_async_host.user     = NULL;
        s_async_host.frame_cb = &hostFrame;
        s_async_host.abort_cb = NULL;
    }
    return avfTickAsync(&s_async_host) != 0;
}
