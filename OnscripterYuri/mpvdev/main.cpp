/* T4 harness — standalone verification for the libmpv fallback backend.
 *
 * Links ONLY against SDL2 + system libs. There is deliberately NO -lmpv:
 * video_player_mpv.cpp loads libmpv.2.dylib at run time via dlopen.
 *
 * Usage:
 *   ./mpvdev_harness probe
 *   ./mpvdev_harness play <media> [seconds] [--loop] [--click-stop after_ms]
 *                         [--out PREFIX] [--lib PATHS]
 *
 *   play   : runs mpvPlayBlocking(path, click_stop, loop) for at most
 *            <seconds> (ONSYURI_MPV_MAX_PLAY). The frame callback captures
 *            the FIRST and LAST frame, saved as PREFIX.ppm / PREFIX-last.ppm.
 *            Prints frames/size/fps (efficiency evidence for EVIDENCE.md).
 *   probe  : mpvIsAvailable() → exit 0/1 (dlopen failure path check).
 *
 *   --click-stop N : push a synthetic SDL left-button-down after N ms and
 *                    use click_stop=true (tests the SDL_PumpEvents peek path).
 *   --loop         : loop-file=inf (implies click_stop so it can terminate).
 */

#include "video_player_mpv.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static char g_out[400] = "frame";

/* captured copies of first/last frame (callback buffer is transient!) */
static unsigned char *cap_first = nullptr, *cap_last = nullptr;
static size_t cap_stride = 0;
static int cap_w = 0, cap_h = 0;
static std::atomic<bool> got_first{false};
static std::atomic<long> nframes{0};
static std::chrono::steady_clock::time_point t0;

static void save_ppm(const unsigned char *px, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[harness] cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", cap_w, cap_h);
    for (int y = 0; y < cap_h; y++) {
        const unsigned char *row = px + y * cap_stride;
        for (int x = 0; x < cap_w; x++)
            fwrite(row + (size_t)x * 4, 1, 3, f);  /* RGBA8888(LE) → rgb24 */
    }
    fclose(f);
    printf("[harness] saved %s (%dx%d)\n", path, cap_w, cap_h);
}

static void on_frame(const unsigned char *rgba, int w, int h, void *user)
{
    (void)user;
    if (!rgba || w <= 0 || h <= 0) return;
    size_t stride = (((size_t)w * 4 + 63) & ~(size_t)63);

    if (!got_first.exchange(true)) {
        cap_w = w; cap_h = h; cap_stride = stride;
        cap_first = (unsigned char *)malloc(stride * (size_t)h);
        cap_last  = (unsigned char *)malloc(stride * (size_t)h);
        if (cap_first && cap_last) memcpy(cap_first, rgba, stride * (size_t)h);
        nframes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (cap_last) memcpy(cap_last, rgba, stride * (size_t)h);
    nframes.fetch_add(1, std::memory_order_relaxed);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "probe";

    if (!strcmp(mode, "probe")) {
        bool ok = mpvIsAvailable();
        printf("mpvIsAvailable=%d\n", ok ? 1 : 0);
        return ok ? 0 : 1;
    }
    if (strcmp(mode, "play")) {
        fprintf(stderr, "mode must be 'probe' or 'play'\n");
        return 2;
    }

    const char *path = nullptr;
    int seconds = 3;
    bool loop = false, click_stop = false;
    long click_after_ms = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--loop")) { loop = true; click_stop = true; }
        else if (!strcmp(argv[i], "--click-stop") && i + 1 < argc) {
            click_stop = true; click_after_ms = atol(argv[++i]);
        } else if (!strncmp(argv[i], "--out=", 6)) {
            snprintf(g_out, sizeof g_out, "%s", argv[i] + 6);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            snprintf(g_out, sizeof g_out, "%s", argv[++i]);
        } else if (!strncmp(argv[i], "--lib=", 6)) {
            setenv("ONSYURI_MPV_LIB", argv[i] + 6, 1);
        } else if (!path) {
            path = argv[i];
        } else {
            seconds = atoi(argv[i]);
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s play <media> [seconds] [--loop]"
                        " [--click-stop ms] [--out PREFIX] [--lib PATHS]\n", argv[0]);
        return 2;
    }

    /* Event pump needs SDL_INIT_EVENTS; VIDEO only opens a cocoa connection
     * — fine on desktop, and harmless with SDL_VIDEODRIVER=dummy for CI. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        fprintf(stderr, "[harness] SDL_Init failed: %s\n", SDL_GetError());

    /* sanity: poll API must say "no frame" when nothing is playing */
    {
        const unsigned char *px = nullptr; int w = 0, h = 0;
        if (mpvPollFrame(&px, &w, &h)) {
            fprintf(stderr, "[harness] FAIL: mpvPollFrame true outside playback\n");
            return 1;
        }
    }

    std::thread clicker;
    if (click_stop && click_after_ms > 0) {
        clicker = std::thread([click_after_ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(click_after_ms));
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_MOUSEBUTTONDOWN;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.state = SDL_PRESSED;
            e.button.x = 64; e.button.y = 64;
            SDL_PushEvent(&e);   /* backend peeks, never consumes */
            printf("[harness] pushed synthetic left-click at %ldms\n",
                   click_after_ms);
        });
    }

    mpvSetFrameCallback(&on_frame, nullptr);
    if (seconds > 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", seconds);
        setenv("ONSYURI_MPV_MAX_PLAY", buf, 1);
    }

    t0 = std::chrono::steady_clock::now();
    bool played = mpvPlayBlocking(path, click_stop, loop);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();

    long fr = nframes.load();
    printf("RESULT played=%s frames=%ld size=%dx%d wall=%.0fms fps=%.1f\n",
           played ? "true" : "false", fr, cap_w, cap_h, ms,
           ms > 0 ? fr / (ms / 1000.0) : 0.0);

    if (cap_first) save_ppm(cap_first, (std::string(g_out) + ".ppm").c_str());
    if (cap_last)  save_ppm(cap_last,  (std::string(g_out) + "-last.ppm").c_str());

    free(cap_first); free(cap_last);
    if (clicker.joinable()) clicker.join();
    SDL_Quit();
    return played ? 0 : 1;
}
