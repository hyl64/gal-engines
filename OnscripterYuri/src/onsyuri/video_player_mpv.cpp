/* Copyright (C) gal-engines T4 — libmpv fallback backend, dlopen'd at runtime.
 *
 * NO compile-time / link-time dependency on libmpv: everything goes through
 * dlopen()/dlsym(). See video_player_mpv.h and DESIGN-VIDEO.md §"T4 的 mpv
 * 接入方式".
 *
 * License note (LGPL/GPL awareness): the libmpv *client API* headers are
 * ISC-licensed; a vendored copy lives under OnscripterYuri/mpvdev/ with its
 * original permission notice preserved verbatim at the top of those files.
 * The mpv *core* is GPLv2+ (LGPLv2+ when built with -Dgpl=false); we never
 * link or embed it at build time — it is loaded from the user's system at
 * run time, exactly like every other libmpv-based player does. When the
 * library is absent this backend is a silent no-op and the engine keeps its
 * previous behavior.
 *
 * We deliberately do NOT include the mpv client headers here (that would
 * force every builder to have mpv headers installed = a compile-time
 * dependency). Instead we mirror the ABI-stable subset we need below. The
 * libmpv 2.x ABI is append-only: enum values are frozen, and the layouts of
 * mpv_event / mpv_event_end_file / mpv_render_param / mpv_render_frame_info
 * are frozen for the whole 2.x series (cross-verified against mpv v0.38 =
 * client API 2.3 and mpv v0.41 = client API 2.5 headers).
 */

#include "video_player_mpv.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <atomic>

#if !defined(ONSYURI_MPV_NO_SDL)
#include <SDL2/SDL.h>
#define ONSYURI_MPV_HAVE_SDL 1
#endif

/* ------------------------------------------------------------------------ */
/* Minimal libmpv 2.x ABI mirror                                            */
/* (values copied from vendored mpvdev/client.h + mpvdev/render.h — keep in */
/*  sync when bumping the vendored headers)                                  */
/* ------------------------------------------------------------------------ */

struct mpv_handle;            /* opaque, passed through only */
struct mpv_render_context;    /* opaque, passed through only */

#define MPV_VER_MAJOR(v) ((unsigned long)(v) >> 16)
#define MPV_VER_MINOR(v) ((unsigned long)(v) & 0xffff)

/* enum mpv_format (client.h) */
#define FMT_INT64   4

/* enum mpv_event_id (client.h) */
#define EV_NONE            0
#define EV_SHUTDOWN        1
#define EV_START_FILE      6
#define EV_END_FILE        7
#define EV_FILE_LOADED     8
#define EV_VIDEO_RECONFIG  17

/* enum mpv_end_file_reason (client.h) */
#define END_REASON_EOF     0
#define END_REASON_STOP    2
#define END_REASON_ERROR   4

/* enum mpv_render_param_type (render.h) */
#define RP_API_TYPE              1
#define RP_ADVANCED_CONTROL      10
#define RP_NEXT_FRAME_INFO       11
#define RP_BLOCK_FOR_TARGET_TIME 12
#define RP_SW_SIZE               17
#define RP_SW_FORMAT             18
#define RP_SW_STRIDE             19
#define RP_SW_POINTER            20

/* mpv_render_context_update() / mpv_render_frame_info.flags bitsets */
#define UPDATE_FLAG_FRAME  (1u << 0)       /* MPV_RENDER_UPDATE_FRAME */
#define FINFO_PRESENT      (1ull << 0)
#define FINFO_REDRAW       (1ull << 1)

/* ABI-stable struct layouts for libmpv 2.x (field order/types verified
 * against v0.38 and v0.41 headers; newer 2.x only appends events/fields
 * that we ignore). */
struct abi_mpv_event {
    int event_id;             /* mpv_event_id */
    int error;
    uint64_t reply_userdata;
    void *data;
};
struct abi_end_file {
    int reason;               /* mpv_end_file_reason */
    int error;
    int64_t playlist_entry_id;
};
struct abi_render_param {
    int type;                 /* mpv_render_param_type */
    void *data;
};
struct abi_frame_info {
    uint64_t flags;
    int64_t target_time;      /* µs, same base as mpv_get_time_us() */
};

/* Required function-pointer table. DESIGN-VIDEO.md: "拿不到
 * mpv_client_api 必需符号即返回 false" — we never call a missing entry. */
struct MpvApi {
    unsigned long (*client_api_version)(void);
    const char *(*error_string)(int);
    void (*free_)(void *);
    mpv_handle *(*create)(void);
    int (*initialize)(mpv_handle *);
    void (*destroy)(mpv_handle *);
    void (*terminate_destroy)(mpv_handle *);
    int64_t (*get_time_us)(mpv_handle *);
    int (*set_option_string)(mpv_handle *, const char *, const char *);
    int (*set_property_string)(mpv_handle *, const char *, const char *);
    int (*get_property)(mpv_handle *, const char *, int /*mpv_format*/, void *);
    int (*command)(mpv_handle *, const char **);
    abi_mpv_event *(*wait_event)(mpv_handle *, double);
    int (*render_create)(mpv_render_context **, mpv_handle *, abi_render_param *);
    int (*render_get_info)(mpv_render_context *, abi_render_param);
    void (*render_set_update_cb)(mpv_render_context *, void (*)(void *), void *);
    uint64_t (*render_update)(mpv_render_context *);
    int (*render_render)(mpv_render_context *, abi_render_param *);
    void (*render_report_swap)(mpv_render_context *);
    void (*render_free)(mpv_render_context *);
};

/* ------------------------------------------------------------------------ */
/* dlopen plumbing                                                          */
/* ------------------------------------------------------------------------ */

static MpvApi g_api;
static std::atomic<bool> g_probe_done{false};
static std::atomic<bool> g_probe_ok{false};

static bool load_symbol(void *lib, const char *sym, void **slot)
{
    *slot = dlsym(lib, sym);
    if (!*slot) {
        fprintf(stderr, "[mpv] missing required symbol: %s\n", sym);
        return false;
    }
    return true;
}

static bool fill_api_table(void *lib)
{
    memset(&g_api, 0, sizeof(g_api));
    bool ok = true;
    ok = ok && load_symbol(lib, "mpv_client_api_version", (void **)&g_api.client_api_version);
    ok = ok && load_symbol(lib, "mpv_error_string", (void **)&g_api.error_string);
    ok = ok && load_symbol(lib, "mpv_free", (void **)&g_api.free_);
    ok = ok && load_symbol(lib, "mpv_create", (void **)&g_api.create);
    ok = ok && load_symbol(lib, "mpv_initialize", (void **)&g_api.initialize);
    ok = ok && load_symbol(lib, "mpv_destroy", (void **)&g_api.destroy);
    ok = ok && load_symbol(lib, "mpv_terminate_destroy", (void **)&g_api.terminate_destroy);
    ok = ok && load_symbol(lib, "mpv_get_time_us", (void **)&g_api.get_time_us);
    ok = ok && load_symbol(lib, "mpv_set_option_string", (void **)&g_api.set_option_string);
    ok = ok && load_symbol(lib, "mpv_set_property_string", (void **)&g_api.set_property_string);
    ok = ok && load_symbol(lib, "mpv_get_property", (void **)&g_api.get_property);
    ok = ok && load_symbol(lib, "mpv_command", (void **)&g_api.command);
    ok = ok && load_symbol(lib, "mpv_wait_event", (void **)&g_api.wait_event);
    ok = ok && load_symbol(lib, "mpv_render_context_create", (void **)&g_api.render_create);
    ok = ok && load_symbol(lib, "mpv_render_context_get_info", (void **)&g_api.render_get_info);
    ok = ok && load_symbol(lib, "mpv_render_context_set_update_callback", (void **)&g_api.render_set_update_cb);
    ok = ok && load_symbol(lib, "mpv_render_context_update", (void **)&g_api.render_update);
    ok = ok && load_symbol(lib, "mpv_render_context_render", (void **)&g_api.render_render);
    ok = ok && load_symbol(lib, "mpv_render_context_report_swap", (void **)&g_api.render_report_swap);
    ok = ok && load_symbol(lib, "mpv_render_context_free", (void **)&g_api.render_free);
    return ok;
}

/* Try candidate paths in order. Returns the opened handle or NULL. */
static void *open_libmpv(void)
{
    static const char *kDefault[] = {
        "/opt/homebrew/lib/libmpv.2.dylib",
        "/usr/local/lib/libmpv.2.dylib",
        "libmpv.2.dylib",          /* DYLD search fallback */
        "@rpath/libmpv.2.dylib",
    };

    const char *candidates[8];
    int n = 0;
    const char *env = getenv("ONSYURI_MPV_LIB");
    if (env && *env) {
        /* colon-separated override (DIY installs / CI fixtures). Leaked on
         * purpose: candidate strings must stay valid; one-time probe. */
        char *copy = strdup(env);
        char *save = NULL;
        if (copy) {
            /* tokens point into `copy`; kept alive intentionally (one-time
             * probe; a few dozen bytes, never freed — dlclose-free design) */
            for (char *tok = strtok_r(copy, ":", &save);
                 tok && n < 8; tok = strtok_r(NULL, ":", &save))
                candidates[n++] = tok;
        }
    }
    if (n == 0) {
        for (size_t i = 0; i < sizeof(kDefault) / sizeof(kDefault[0]) && n < 8; i++)
            candidates[n++] = kDefault[i];
    }

    for (int i = 0; i < n; i++) {
        void *lib = dlopen(candidates[i], RTLD_LAZY);
        if (lib) {
            fprintf(stderr, "[mpv] dlopen ok: %s\n", candidates[i]);
            return lib;
        }
    }
    fprintf(stderr, "[mpv] libmpv.2.dylib not loadable (%s)\n", dlerror());
    return NULL;
}

static bool probe_open(void)
{
    void *lib = open_libmpv();
    if (!lib) return false;

    if (!fill_api_table(lib)) {
        dlclose(lib);
        return false;
    }

    unsigned long v = g_api.client_api_version();
    if (MPV_VER_MAJOR(v) != 2) {
        fprintf(stderr, "[mpv] unsupported client API %lu.%lu (need 2.x)\n",
                MPV_VER_MAJOR(v), MPV_VER_MINOR(v));
        dlclose(lib);
        return false;
    }
    fprintf(stderr, "[mpv] client API %lu.%lu OK\n",
            MPV_VER_MAJOR(v), MPV_VER_MINOR(v));
    /* Intentionally never dlclose(): libmpv keeps internal threads alive;
     * unloading mid-run is a known crash source. Leak one handle per proc. */
    return true;
}

bool mpvIsAvailable(void)
{
    if (g_probe_done.load(std::memory_order_acquire))
        return g_probe_ok.load(std::memory_order_acquire);
    bool ok = false;
    const char *dis = getenv("ONSYURI_MPV_DISABLE");
    if (dis && dis[0] == '1') {
        fprintf(stderr, "[mpv] disabled via ONSYURI_MPV_DISABLE=1\n");
    } else {
        ok = probe_open();
    }
    g_probe_ok.store(ok, std::memory_order_release);
    g_probe_done.store(true, std::memory_order_release);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Public frame buffer + dirty flag + optional callback                      */
/* ------------------------------------------------------------------------ */

static std::atomic<mpv_frame_cb> g_cb{nullptr};
static std::atomic<void *> g_cb_user{nullptr};

/* Internal RGBA8888 buffer (the "公开缓冲"): mpv SW-renders straight into
 * it — that conversion-copy IS the frame transfer (cheaper than rendering
 * and then memcpy-ing a second time). g_dirty is set for every fresh frame
 * and can be polled on this same thread via mpvPollFrame(). */
static unsigned char *g_px = NULL;
static int g_w = 0, g_h = 0;
static size_t g_stride = 0;
static std::atomic<bool> g_dirty{false};
static std::atomic<bool> g_active{false};   /* playback session lifetime */

extern "C" void mpvSetFrameCallback(mpv_frame_cb cb, void *user)
{
    g_cb_user.store(user, std::memory_order_release);
    g_cb.store(cb, std::memory_order_release);
}

extern "C" bool mpvPollFrame(const unsigned char **rgba, int *w, int *h)
{
    if (!g_active.load(std::memory_order_acquire)) return false;
    if (!g_dirty.load(std::memory_order_acquire)) return false;
    if (rgba) *rgba = g_px;
    if (w) *w = g_w;
    if (h) *h = g_h;
    g_dirty.store(false, std::memory_order_release);
    return true;
}

/* 64-byte aligned pointer AND stride, per render.h SW docs ("pointer and
 * stride should be a multiple of 64"). */
static bool fb_ensure(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    if (g_px && g_w == w && g_h == h) return true;
    size_t stride = ((size_t)w * 4 + 63) & ~(size_t)63;
    void *px = NULL;
    if (posix_memalign(&px, 64, stride * (size_t)h) != 0 || !px) return false;
    memset(px, 0, stride * (size_t)h);
    free(g_px);
    g_px = (unsigned char *)px;
    g_w = w; g_h = h; g_stride = stride;
    return true;
}

static void fb_release(void)
{
    free(g_px);
    g_px = NULL; g_w = 0; g_h = 0; g_stride = 0;
    g_dirty.store(false, std::memory_order_relaxed);
}

/* mpv render-update callback: fires on an mpv-internal thread and MUST NOT
 * call any mpv API — it only flips a hint flag. The render loop polls
 * mpv_render_context_update() itself, so nothing else is needed here. */
static std::atomic<bool> g_update_hint{false};
static void on_mpv_update(void *)
{
    g_update_hint.store(true, std::memory_order_relaxed);
}

/* ------------------------------------------------------------------------ */
/* SDL event pump — ALWAYS pump while playing (see note below), then peek    */
/* for stop keys only when click_stop is on (peek, never consume).           */
/*                                                                            */
/* On macOS libmpv 0.41's vo init does dispatch_sync onto the MAIN dispatch   */
/* queue (cocoa_cb / GPU context probing). If our thread blocks in a sync     */
/* mpv call without ever running the Cocoa pump, init deadlocks (vo parked in */
/* dispatch_sync, core holding the mpv dispatch lock, us queued on the lock). */
/* SDL_PumpEvents() drains the Cocoa queue, so it MUST run every iteration    */
/* regardless of click_stop.                                                  */
/* ------------------------------------------------------------------------ */

static void pump_sdl(void)
{
#if defined(ONSYURI_MPV_HAVE_SDL)
    if (SDL_WasInit(SDL_INIT_EVENTS))
        SDL_PumpEvents();
#endif
}

static bool poll_stop_request(bool enabled)
{
    if (!enabled) return false;
#if defined(ONSYURI_MPV_HAVE_SDL)
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return false;
    /* Peek only (never consume): the engine must still receive the
     * click/key after playback returns (NScripter movie/skip semantics).
     * Scan a small window so e.g. a right-click queued before a
     * left-click does not hide the stop request. */
    SDL_Event ev[16];
    int n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
    for (int i = 0; i < n; i++) {
        if (ev[i].type != SDL_KEYDOWN) continue;
        SDL_Keycode k = ev[i].key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_LCTRL)
            return true;
    }
    n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_MOUSEBUTTONDOWN,
                       SDL_MOUSEBUTTONDOWN);
    for (int i = 0; i < n; i++) {
        if (ev[i].type == SDL_MOUSEBUTTONDOWN &&
            ev[i].button.button == SDL_BUTTON_LEFT)
            return true;
    }
#endif
    return false;
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static int env_int(const char *name, int def)
{
    const char *s = getenv(name);
    if (!s || !*s) return def;
    int v = atoi(s);
    return v > 0 ? v : def;
}

static bool get_i64(mpv_handle *mh, const char *prop, int64_t *out)
{
    return g_api.get_property(mh, prop, FMT_INT64, out) >= 0;
}

/* SW surface size: display size dw×dh (aspect-corrected by mpv) clamped to
 * a max box. DESIGN-VIDEO efficiency line: SW 480p~720p is the accepted
 * zone; 1080p fallback is allowed to drop frames. Default cap 1280×720;
 * override with ONSYURI_MPV_MAXW / ONSYURI_MPV_MAXH (use 1920/1080 for the
 * documented "1080p 兜底" measurement — see mpvdev/EVIDENCE.md). */
static void pick_size(int64_t dw, int64_t dh, int *w, int *h)
{
    const int maxw = env_int("ONSYURI_MPV_MAXW", 1280);
    const int maxh = env_int("ONSYURI_MPV_MAXH", 720);
    int64_t W = dw, H = dh;
    if (W > maxw || H > maxh) {
        double s1 = (double)maxw / (double)W;
        double s2 = (double)maxh / (double)H;
        double s = s1 < s2 ? s1 : s2;
        W = (int64_t)((double)W * s);
        H = (int64_t)((double)H * s);
    }
    if (W < 16) W = 16;
    if (H < 16) H = 16;
    *w = (int)W; *h = (int)H;
}

/* ------------------------------------------------------------------------ */
/* mpvPlayBlocking                                                           */
/* ------------------------------------------------------------------------ */

bool mpvPlayBlocking(const char *abs_path, bool click_stop, bool loop)
{
    if (!abs_path || !*abs_path) return false;
    if (!mpvIsAvailable()) return false;

    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[mpv] not a regular file: %s\n", abs_path);
        return false;
    }

    /* loop + "must play to end" (click ignored) would never terminate →
     * degrade to a single playback instead of hanging the script. */
    bool loop_on = loop;
    if (loop && !click_stop) {
        fprintf(stderr, "[mpv] loop without click_stop never self-terminates;"
                        " playing once instead\n");
        loop_on = false;
    }

    mpv_handle *mh = g_api.create();
    if (!mh) return false;

    /* Deterministic headless core: no user config, no bundled scripts, no
     * OSC/OSD (would burn controls into SW frames), no window ever. */
    g_api.set_option_string(mh, "config", "no");
    g_api.set_option_string(mh, "terminal", "no");
    g_api.set_option_string(mh, "msg-level", "all=error");
    g_api.set_option_string(mh, "force-window", "no");
    g_api.set_option_string(mh, "keep-open", "no");
    g_api.set_option_string(mh, "osc", "no");
    g_api.set_option_string(mh, "load-scripts", "no");
    /* SW renderer efficiency knob (recommended in render.h docs). */
    g_api.set_option_string(mh, "sw-fast", "yes");
    /* Bounded demuxer cache → small RSS (DESIGN baseline <150MB). */
    g_api.set_option_string(mh, "demuxer-max-bytes", "32MiB");
    g_api.set_option_string(mh, "demuxer-max-back-bytes", "8MiB");
    /* Audio: mpv default ao (coreaudio) — sound with no window. Headless CI
     * can force silence: ONSYURI_MPV_AO=null. */
    const char *ao = getenv("ONSYURI_MPV_AO");
    if (ao && *ao) g_api.set_option_string(mh, "ao", ao);

    if (g_api.initialize(mh) < 0) {
        fprintf(stderr, "[mpv] mpv_initialize failed\n");
        g_api.destroy(mh);
        return false;
    }

    if (loop_on) {
        int rc = g_api.set_property_string(mh, "loop-file", "inf");
        if (rc < 0)
            fprintf(stderr, "[mpv] loop-file=inf rejected (%s)\n",
                    g_api.error_string(rc));
    }

    /* Render context — MPV_RENDER_API_TYPE_SW. Created on the calling
     * thread; every mpv_render_* call below stays on this thread
     * (render.h threading rules). No mpv window is ever opened.
     *
     * CRITICAL ORDERING: rctx must exist BEFORE loadfile and
     * mpv_render_context_update() must be called regularly — the mpv core
     * thread blocks in vo_create()/mp_rendezvous() until the render API
     * attaches (attach happens via update/render calls). Creating the
     * context after loadfile is a race that can hang the core thread AND
     * hold the dispatch lock, dead-locking mpv_command()/mpv_get_property().
     * (Observed & diagnosed on libmpv 0.41, see mpvdev/EVIDENCE.md.) */
    mpv_render_context *rctx = NULL;
    const char *api_sw = "sw";   /* MPV_RENDER_API_TYPE_SW */
    int adv = 0;                  /* default control */
    abi_render_param create_params[3];
    create_params[0].type = RP_API_TYPE;          create_params[0].data = (void *)api_sw;
    create_params[1].type = RP_ADVANCED_CONTROL;  create_params[1].data = &adv;
    create_params[2].type = 0 /* MPV_RENDER_PARAM_INVALID = terminator */;
    create_params[2].data = NULL;
    if (g_api.render_create(&rctx, mh, create_params) < 0 || !rctx) {
        fprintf(stderr, "[mpv] SW render context create failed\n");
        g_api.terminate_destroy(mh);
        return false;
    }
    g_api.render_set_update_cb(rctx, &on_mpv_update, NULL);

    const char *load_args[] = { "loadfile", abs_path, "replace", NULL };
    int rc = g_api.command(mh, load_args);
    if (rc < 0) {
        fprintf(stderr, "[mpv] loadfile rejected: %s\n", g_api.error_string(rc));
        g_api.render_free(rctx);
        g_api.terminate_destroy(mh);
        return false;
    }

    bool have_video = false;
    bool eof_reached = false;
    bool file_failed = false;
    bool got_file_loaded = false;
    bool stop_clicked = false;
    int64_t last_target = 0;
    long frames = 0;

    const double SLICE = 0.004;        /* ~250 Hz wake ceiling, ≤4ms input lag */
    double open_budget_s = 15.0;       /* file must resolve (loaded/error) */
    const int64_t max_play_us =
        (int64_t)env_int("ONSYURI_MPV_MAX_PLAY", 0) * 1000000;
    const int64_t t_start = g_api.get_time_us(mh);

    g_active.store(true, std::memory_order_release);

    /* Drain all currently queued events. timeout: seconds to wait for the
     * first one. Returns false when playback must abort (load error). */
    const bool dbg = getenv("ONSYURI_MPV_DEBUG") != NULL;
    auto drain_events = [&](double timeout) -> bool {
        abi_mpv_event *e = g_api.wait_event(mh, timeout);
        while (e && e->event_id != EV_NONE) {
            if (dbg) fprintf(stderr, "[mpv.dbg] ev id=%d err=%d\n",
                             e->event_id, e->error);
            switch (e->event_id) {
            case EV_FILE_LOADED:
                got_file_loaded = true;
                break;
            case EV_END_FILE: {
                abi_end_file *ef = (abi_end_file *)e->data;
                const int why = ef ? ef->reason : -1;
                if (why == END_REASON_ERROR) {
                    fprintf(stderr, "[mpv] end-file error: %s\n",
                            g_api.error_string(ef ? ef->error : -20));
                    file_failed = true;
                    return false;
                }
                /* EOF/STOP/QUIT — current file finished (loop may requeue) */
                eof_reached = true;
                break;
            }
            case EV_SHUTDOWN:
                eof_reached = true;
                break;
            default:
                break;   /* append-only ABI: ignore unknown/newer events */
            }
            e = g_api.wait_event(mh, 0);
        }
        return !file_failed;
    };

    while (!file_failed) {
        /* 1) user input stop (left click / esc / space / ctrl) */
        if (poll_stop_request(click_stop)) {
            fprintf(stderr, "[mpv] stop requested by user input\n");
            stop_clicked = true;
            break;   /* early stop still counts as "played" → true */
        }

        /* 2) wait for mpv events / frame queueing (~SLICE sleep) */
        if (!drain_events(SLICE)) break;

        /* 3) drive the render API EVERY iteration, unconditionally.
         * The mpv core thread blocks in vo_create()/mp_rendezvous() until
         * the render API attaches, and attachment happens on the first
         * mpv_render_context_update()/render() call from this thread —
         * while holding the dispatch lock. Skipping update() until
         * FILE_LOADED therefore dead-locks the core (diagnosed via
         * `sample` on libmpv 0.41: core parked in mp_rendezvous, client
         * parked in mp_dispatch_lock). */
        uint64_t uflags = g_api.render_update(rctx);
        g_update_hint.store(false, std::memory_order_relaxed);

        if (!got_file_loaded) {
            open_budget_s -= SLICE;
            if (open_budget_s <= 0) {
                fprintf(stderr, "[mpv] open timed out: %s\n", abs_path);
                file_failed = true;
                break;
            }
        }
        if (max_play_us > 0 && g_api.get_time_us(mh) - t_start >= max_play_us) {
            fprintf(stderr, "[mpv] ONSYURI_MPV_MAX_PLAY cap reached\n");
            break;
        }
        if (file_failed) break;

        /* 3) target frame size from video-out-params (dw/dh = aspect
         * corrected display size). */
        int64_t dw = 0, dh = 0;
        if (got_file_loaded && !have_video) {
            have_video = get_i64(mh, "video-out-params/dw", &dw) &&
                         get_i64(mh, "video-out-params/dh", &dh) &&
                         dw > 0 && dh > 0;
        }
        if (have_video &&
            (!get_i64(mh, "video-out-params/dw", &dw) ||
             !get_i64(mh, "video-out-params/dh", &dh) || dw <= 0 || dh <= 0)) {
            have_video = false;    /* video track ended (audio tail) */
        }

        if (!have_video) {
            if (eof_reached) {
                if (loop_on) eof_reached = false;  /* requeued by loop-file */
                else break;                        /* audio-only finish */
            }
            continue;
        }

        int w, h;
        pick_size(dw, dh, &w, &h);
        if (!fb_ensure(w, h)) {
            fprintf(stderr, "[mpv] frame buffer alloc %dx%d failed\n", w, h);
            file_failed = true;
            break;
        }

        /* 4) frame available? (uflags from the unconditional step-3 update;
         * the core thread only proceeds once that update attached the ctx.) */
        if (!(uflags & UPDATE_FLAG_FRAME)) {
            if (eof_reached) {
                if (loop_on) eof_reached = false;
                else break;    /* EOF, nothing left to draw */
            }
            continue;
        }

        /* 5) what kind of frame + intended display time */
        abi_frame_info info;
        info.flags = 0; info.target_time = 0;
        abi_render_param gp;
        gp.type = RP_NEXT_FRAME_INFO; gp.data = &info;
        g_api.render_get_info(rctx, gp);
        const bool is_new = (info.flags & FINFO_PRESENT) &&
                            !(info.flags & FINFO_REDRAW);
        if (is_new && info.target_time > 0)
            last_target = info.target_time;

        /* 6) pace ourselves (render is invoked with block-for-target=0 so a
         * click preempts within ≤SLICE of the display time). Sleeps double
         * as event drains so a fast EOF isn't missed. */
        bool aborted = false;
        while (last_target > 0) {
            const int64_t now = g_api.get_time_us(mh);
            if (now >= last_target) break;
            if (poll_stop_request(click_stop)) {
                stop_clicked = true;
                aborted = true;
                break;
            }
            double wait = (double)(last_target - now) * 1e-6;
            if (wait > SLICE) wait = SLICE;
            if (!drain_events(wait)) { aborted = true; break; }
            g_api.render_update(rctx);   /* keep API driven while pacing */
            if (eof_reached) {
                if (loop_on) eof_reached = false;
                last_target = 0;   /* draw what we have, then end/requeue */
            }
        }
        if (aborted || file_failed) break;

        /* 7) SW render into the RGBA buffer. fmt "rgba" (task contract):
         * 4 B/px, bytes R,G,B,A in increasing address = SDL_PIXELFORMAT_
         * RGBA8888 byte order on LE. Some 2.x builds only accept the rgb0
         * family — retry once with "bgr0" and shout in the log so T1 can
         * pick the matching SDL surface format. */
        static const char *kFmtTry[] = { "rgba", "bgr0" };
        int sz[2];
        int zero = 0;
        sz[0] = w; sz[1] = h;
        rc = -20;
        for (int fi = 0; fi < 2; fi++) {
            abi_render_param rp[5];
            rp[0].type = RP_SW_SIZE;     rp[0].data = &sz[0];
            rp[1].type = RP_SW_FORMAT;   rp[1].data = (void *)kFmtTry[fi];
            rp[2].type = RP_SW_STRIDE;   rp[2].data = &g_stride;
            rp[3].type = RP_SW_POINTER;  rp[3].data = g_px;
            rp[4].type = RP_BLOCK_FOR_TARGET_TIME; rp[4].data = &zero;
            rc = g_api.render_render(rctx, rp);
            if (rc >= 0) {
                if (fi > 0)
                    fprintf(stderr, "[mpv] NOTE: SW fmt is bgr0 (bytes B,G,R,X)"
                                    " — use SDL_PIXELFORMAT_ARGB8888 on LE\n");
                break;
            }
            if (fi == 0)
                fprintf(stderr, "[mpv] SW fmt 'rgba' rejected (%s), retry bgr0\n",
                        g_api.error_string(rc));
        }
        if (rc < 0) {
            fprintf(stderr, "[mpv] SW render failed: %s\n", g_api.error_string(rc));
            file_failed = true;
            break;
        }
        g_api.render_report_swap(rctx);
        if (is_new) last_target = 0;    /* frame consumed */

        /* 8) publish: dirty flag (poll path) + optional same-thread callback.
         * Buffer stays valid until the NEXT render; consumers must copy/blit
         * inside the callback or before the next mpvPollFrame(). */
        frames++;
        g_dirty.store(true, std::memory_order_release);
        mpv_frame_cb cb = g_cb.load(std::memory_order_acquire);
        if (cb) cb(g_px, g_w, g_h, g_cb_user.load(std::memory_order_acquire));

        if (eof_reached) {
            if (loop_on) eof_reached = false;
            else break;     /* played to the end */
        }
    }

    /* Teardown order (render.h): stop playback → free render ctx BEFORE the
     * core → terminate_destroy. */
    if (!eof_reached) {
        const char *stop_args[] = { "stop", NULL };
        g_api.command(mh, stop_args);
        for (double t = 1.0; t > 0; t -= SLICE) {
            abi_mpv_event *e = g_api.wait_event(mh, SLICE);
            if (e && (e->event_id == EV_END_FILE || e->event_id == EV_SHUTDOWN))
                break;
        }
    }
    g_api.render_free(rctx);
    g_api.terminate_destroy(mh);

    g_active.store(false, std::memory_order_release);
    fb_release();

    if (file_failed) {
        fprintf(stderr, "[mpv] playback failed: %s\n", abs_path);
        return false;
    }
    fprintf(stderr, "[mpv] %s: %ld frames delivered%s\n", abs_path, frames,
            stop_clicked ? " [stopped by input]" :
            eof_reached  ? " [EOF]" : " [ended]");
    return true;
}
