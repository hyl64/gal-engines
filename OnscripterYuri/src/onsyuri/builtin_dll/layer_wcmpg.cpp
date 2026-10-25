/* ------------------------------------------------------------------ */
/* Layer-plugin playback: setlayer N,..,"dll\\wcmpg.dll"                */
/*                                                                     */
/* Real NScripter games do not use mpegplay or movie at all -- they put */
/* a movie into a *layer* and drive it with layermessage.  From the     */
/* 07th Expansion trial (trt_trial), decrypted script:                   */
/*                                                                      */
/*   setlayer 10,20,"dll\\wcmpg.dll"                                     */
/*   layermessage 10,"close"                                            */
/*   layermessage 10,"open/arc\\MOV001_100ten.mpg"                       */
/*   layermessage 10,"setloop/1"                                        */
/*   layermessage 10,"setx/0"   layermessage 10,"sety/0"                */
/*   layermessage 10,"play"                                             */
/*   lsp 983,"*10"        ; bind the layer to a sprite so saving works   */
/*   waittimer 1980       ; the script times the movie itself           */
/*                                                                      */
/* Two things the first cut got wrong and this one does not:            */
/*                                                                      */
/*  1. The movie is usually *inside the game's archive* (`arc\...`,      */
/*     `arc4\...`, .ns2 files), not a loose file.  fopen() misses those  */
/*     completely, so the bytes now come from the engine's own reader    */
/*     (BaseReader) - the same path every image and sound takes.          */
/*  2. Order varies: trt_trial writes setloop BEFORE open              */
/*     arrives, so the `play` that follows is the one that starts it.     */
/*                                                                      */
/* The decode/present split is the async player's: decoding runs on a     */
/* worker thread (the script keeps running), and the engine's             */
/* Layer::refresh() pulls the newest frame on the main thread.            */
/* ------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "../builtin_layer.h"
#include "../video_player_frame.h"
#include "../video_player.h"
#include "../BaseReader.h"
#include "../Utils.h"

#ifdef MACOSX

/* Decoder is provided by video_player_mac.mm (framework-only TU). */
extern "C" int  avfLayerOpen(const char *abs_path, int loop);
extern "C" int  avfLayerOpenMemory(const char *entry_name, const void *data, int size, int loop);
extern "C" int  avfLayerPlay(void);
extern "C" int  avfLayerPause(int pause);
extern "C" void avfLayerClose(void);
extern "C" int  avfLayerFrame(const unsigned char **px, int *w, int *h, int *pitch);
extern "C" int  avfLayerHasFrame(void);

class WcmpgLayer : public Layer {
public:
    WcmpgLayer(int w, int h, const char *root)
        : pos_x(0), pos_y(0), paused(true), loop_flag(false), opened(false),
          visible(false), last_w(0), last_h(0)
    {
        width = w;
        height = h;
        if (root) game_root = root;
    }

    ~WcmpgLayer()
    {
        if (opened) avfLayerClose();
    }

    void update()
    {
        /* Decoding happens on the worker thread; nothing to pump here. */
    }

    char *message(const char *msg, int &ret_int)
    {
        ret_int = 0;
        if (!msg || !*msg) return NULL;

        /* Messages are "verb" or "verb/arg"; the plugin is case-sensitive
         * about the verbs the games actually use. */
        char verb[64];
        const char *slash = strchr(msg, '/');
        size_t n = slash ? (size_t)(slash - msg) : strlen(msg);
        if (n >= sizeof(verb)) n = sizeof(verb) - 1;
        memcpy(verb, msg, n);
        verb[n] = 0;
        const char *arg = slash ? slash + 1 : "";

        if (!strcmp(verb, "open")){
            openMovie(arg);
        }
        else if (!strcmp(verb, "setloop")){
            loop_flag = (arg[0] == '1');
            /* open precedes setloop in the scripts; restart paused so the
             * decoder picks the flag up, keeping the frame on screen */
            if (opened && !movie.empty()){
                if (avfLayerOpenMemory(entry_name.c_str(), movie.data(),
                                       (int)movie.size(), loop_flag ? 1 : 0) == 0){
                    paused = true;
                    avfLayerPause(1);
                }
            }
        }
        else if (!strcmp(verb, "setx")){
            pos_x = atoi(arg);
        }
        else if (!strcmp(verb, "sety")){
            pos_y = atoi(arg);
        }
        else if (!strcmp(verb, "play")){
            if (!opened){
                utils::printInfo("wcmpg: play before a successful open\n");
                return NULL;
            }
            paused = false;
            avfLayerPause(0);
            avfLayerPlay();
            /* Wait a bounded moment for the first decoded frame.  Without this the
             * first refresh can arrive before the worker has published anything,
             * and whether the movie ever appears then depends on timing -- the
             * intermittency measured at 2 successes in 5 runs. */
            for (int w8 = 0; w8 < 200 && !avfLayerHasFrame(); w8++)
                SDL_Delay(1);
            ret_int = 1;
        }
        else if (!strcmp(verb, "pause")){
            paused = true;
            avfLayerPause(1);
        }
        else if (!strcmp(verb, "close")){
            if (opened) avfLayerClose();
            opened = false;
            visible = false;
            last_w = last_h = 0;
        }
        else if (!strcmp(verb, "getx")){
            ret_int = pos_x;
        }
        else if (!strcmp(verb, "gety")){
            ret_int = pos_y;
        }
        else if (!strcmp(verb, "getw") || !strcmp(verb, "getwidth") ||
                 !strcmp(verb, "getvideowidth")){
            ret_int = last_w;
        }
        else if (!strcmp(verb, "geth") || !strcmp(verb, "getheight") ||
                 !strcmp(verb, "getvideoheight")){
            ret_int = last_h;
        }
        else if (!strcmp(verb, "getstatus")){
            ret_int = opened ? (paused ? 2 : 1) : 0;
        }
        else {
            utils::printInfo("wcmpg: unhandled message '%s'\n", msg);
        }
        return NULL;
    }

    /* Called by the engine on the main thread with the screen surface; the
     * newest decoded frame goes at the layer's position. */
    void refresh(SDL_Surface *surface, SDL_Rect &clip)
    {
        (void)clip;
        {
            static long n = 0;
            static int st = -1;
            if (st < 0){ const char *e = getenv("ONS_TRACE_LAYER_FRAME"); st = (e && *e) ? 1 : 0; }
            if (st && (++n % 30) == 0){
                FILE *f = fopen("ons_layerframe.log", "a");
                if (f){ fprintf(f, "refresh#%ld surf=%d opened=%d visible=%d paused=%d movie=%zu\n",
                                n, surface?1:0, (int)opened, (int)visible, (int)paused,
                                movie.size()); fclose(f); }
            }
        }
        if (!surface || !opened || !visible) return;

        const unsigned char *px = NULL;
        int w = 0, h = 0, pitch = 0;
        /* Refresh may run more often than the decoder produces frames; only
         * redraw when something new arrived, otherwise keep what is there. */
        const int got = avfLayerFrame(&px, &w, &h, &pitch);
        {
            static long n = 0;
            static int st = -1;
            if (st < 0){ const char *e = getenv("ONS_TRACE_LAYER_FRAME"); st = (e && *e) ? 1 : 0; }
            if (st && (++n % 60) == 0){
                FILE *f = fopen("ons_layerframe.log", "a");
                if (f){ fprintf(f, "refresh-fetch#%ld got=%d px=%p w=%d h=%d pitch=%d pos=%d,%d surf=%dx%d\n",
                                n, got, (const void*)px, w, h, pitch,
                                pos_x, pos_y, surface->w, surface->h); fclose(f); }
            }
        }
        if (!got || !px || w <= 0 || h <= 0) return;
        last_w = w; last_h = h;

        SDL_LockSurface(surface);
        uint8_t *base = (uint8_t*)surface->pixels;
        const int spitch = surface->pitch;
        const bool swap_rb =
            (surface->format->format == SDL_PIXELFORMAT_ABGR8888 ||
             surface->format->format == SDL_PIXELFORMAT_RGBA8888);

        for (int y = 0; y < h; y++){
            const int dy = pos_y + y;
            if (dy < 0 || dy >= surface->h) continue;
            const uint8_t *s = px + (size_t)y * pitch;
            uint8_t *d = base + (size_t)dy * spitch + (size_t)pos_x * 4;
            for (int x = 0; x < w; x++){
                const int dx = pos_x + x;
                if (dx < 0 || dx >= surface->w) continue;
                const uint8_t *p = s + (size_t)x * 4;
                if (!swap_rb){
                    d[x*4+0] = p[0];
                    d[x*4+1] = p[1];
                    d[x*4+2] = p[2];
                } else {
                    d[x*4+0] = p[2];
                    d[x*4+1] = p[1];
                    d[x*4+2] = p[0];
                }
                d[x*4+3] = 0xff;
            }
        }
        SDL_UnlockSurface(surface);
    }

private:
    /* Load the entry the way the engine loads its own assets: through the
     * archive reader (which also finds loose files), then hand the bytes to the
     * decoder.  A name the reader does not know is reported and leaves the layer
     * closed, which the script's `play` then notices. */
    void openMovie(const char *name)
    {
        if (opened){ avfLayerClose(); opened = false; }
        visible = false;
        if (!name || !name[0]) return;

        const size_t name_len = strlen(name);
        (void)name_len;
        movie.clear();
        entry_name = name;

        /* The transcode pipeline (gal prep) writes H.264 copies of the archive's
         * movies into _video/ with a sidecar map; the fullscreen path consults
         * that map and so must the layer: the raw archive bytes are MPEG-1/2,
         * which AVFoundation cannot decode, so a layer fed straight from the
         * archive shows nothing while its decoder sits at EOF. */
        if (!game_root.empty()){
            char *mapped = videoMapLookup(game_root.c_str(), name);
            if (mapped){
                std::string rel = mapped;
                free(mapped);
                std::string full = game_root;
                if (!full.empty() && full[full.size()-1] != '/') full += '/';
                for (const char *p = rel.c_str(); p && *p; p++) full += (*p == '\\' ? '/' : *p);
                FILE *fp = fopen(full.c_str(), "rb");
                if (fp){
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz > 0 && sz < (long)256 * 1024 * 1024){
                        movie.resize((size_t)sz);
                        if (fread(movie.data(), 1, (size_t)sz, fp) != (size_t)sz) movie.clear();
                    }
                    fclose(fp);
                    if (!movie.empty()){
                        utils::printInfo("wcmpg: '%s' -> %s (transcoded)\n", name, rel.c_str());
                        // the mapped bytes win: the archive copy is MPEG-1/2, which
                        // AVFoundation cannot decode -- falling through would
                        // overwrite the playable data with unplayable bytes
                        if (avfLayerOpenMemory(name, movie.data(), (int)movie.size(),
                                               loop_flag ? 1 : 0) == 0){
                            opened = true;
                            visible = true;
                            paused = true;
                        }
                        return;
                    }
                }
            }
        }

        if (reader){
            size_t len = reader->getFileLength(name);
            if (len > 0 && len < (size_t)256 * 1024 * 1024){
                movie.resize(len);
                size_t got = reader->getFile(name, movie.data());
                if (got != len) movie.resize(got);
            }
        }
        /* a layer made before the reader was attached still has the game
         * directory: read the loose file straight from there */
        if (movie.empty() && !game_root.empty()){
            std::string path = game_root;
            for (size_t i = 0; i < name_len; i++) {}
            if (!path.empty() && path[path.size()-1] != '/') path += '/';
            for (const char *p = name; p && *p; p++) path += (*p == '\\' ? '/' : *p);
            FILE *fp = fopen(path.c_str(), "rb");
            if (fp){
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz > 0 && sz < (long)256 * 1024 * 1024){
                    movie.resize((size_t)sz);
                    if (fread(movie.data(), 1, (size_t)sz, fp) != (size_t)sz) movie.clear();
                }
                fclose(fp);
            }
        }
        if (movie.empty()){
            utils::printError("wcmpg: cannot find '%s'\n", name);
            return;
        }
        /* the container check: MPEG program/sequence start, ISO-BMFF or AVI.
         * Anything else is not a movie, and saying so beats a decoder error. */
        const unsigned char *d = movie.data();
        const size_t n = movie.size();
        bool looks_movie =
            (n > 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x01 &&
             (d[3] == 0xba || d[3] == 0xb3)) ||
            (n > 12 && !memcmp(d + 4, "ftyp", 4)) ||
            (n > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "AVI ", 4));
        if (!looks_movie)
            utils::printError("wcmpg: '%s' is not a movie this build can play "
                              "(%u bytes, %02x %02x %02x %02x)\n", name,
                              (unsigned)n, d[0], d[1], d[2], d[3]);

        if (avfLayerOpenMemory(name, movie.data(), (int)movie.size(), loop_flag ? 1 : 0) == 0){
            opened = true;
            visible = true;
            paused = true;
            utils::printInfo("wcmpg: opened '%s' (%u bytes)\n", name, (unsigned)movie.size());
        } else {
            utils::printError("wcmpg: cannot decode '%s'\n", name);
        }
    }

    int  pos_x, pos_y;
    bool paused, loop_flag, opened, visible;
    int  last_w, last_h;
    std::vector<unsigned char> movie;
    std::string entry_name;
    std::string game_root;
};

/* NScripter hands a layer plugin the game's files; in ONScripter that is the
 * reader the whole engine uses.  ScriptParser sets it right after the handler
 * is created (reader is a Layer member). */
Layer *createWcmpgLayer(int w, int h, const char *game_root)
{
    return new WcmpgLayer(w, h, game_root);
}

#else  /* !MACOSX -- the plugin needs a decoder; other platforms keep the old
                    "not implemented" path. */

Layer *createWcmpgLayer(int, int, const char *) { return NULL; }

#endif
