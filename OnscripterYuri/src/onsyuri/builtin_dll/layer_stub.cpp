/* -*- C++ -*-
 *
 *  layer_stub.cpp - neutral implementations for NScripter layer plugins we do
 *  not emulate, so that a commercial game's `setlayer` block does not abort.
 *
 *  Why this is allowed to exist
 *  ---------------------------
 *  ONScripter can emulate a handful of Windows layer DLLs (oldmovie / snow /
 *  hana) because their behaviour is simple and documented.  Others are
 *  proprietary and their full protocol is not public.  A real commercial trial
 *  (07th Expansion's trt_trial) declares four of them in its define block:
 *
 *      setlayer 1,33,   "dll\anm.dll"        ; animation engine
 *      setlayer 7,100,  "dll\screen.dll"     ; screen-grab helper
 *      setlayer 9,1000, "dll\NSFukuro.dll"   ; blur / fukuro effect
 *      setlayer 10,20,  "dll\wcmpg.dll"      ; movie layer -- implemented
 *
 *  Counting the messages that trial actually sends to each:
 *
 *      anm.dll       2   ("free 1", "move 1,stop")
 *      screen.dll    0
 *      NSFukuro.dll  5   ("fontsize,24", "clear", "fOff")
 *
 *  i.e. they are peripheral effects here, not the main path.  Refusing to set
 *  them up makes the engine print an error and, worse, leaves the script with a
 *  layer number it believes is live.  A neutral layer that accepts those
 *  messages, draws nothing, and reports defined defaults keeps the game running
 *  and is honest about what it does: every message it receives is logged once so
 *  a missing effect is visible instead of silent.
 *
 *  Nothing here claims to reproduce the plugin's visuals.  If a game turns out
 *  to depend on one of these for its main presentation, that DLL needs a real
 *  implementation and this file is where the claim would be replaced.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../builtin_layer.h"
#include "../Utils.h"

#ifdef USE_BUILTIN_LAYER_EFFECTS

class StubLayer : public Layer {
public:
    StubLayer(const char *dll_name, int w, int h)
        : reported_(false)
    {
        width = w;
        height = h;
        name_ = dll_name ? dll_name : "?";
    }

    void update() {}

    char *message(const char *msg, int &ret_int)
    {
        ret_int = 0;
        if (!msg) return NULL;

        /* First message per layer is reported; repeats are dropped so a game
         * that polls this every frame does not flood the log. */
        if (!reported_){
            reported_ = true;
            utils::printInfo("layer plugin '%s': accepted ('%s') but draws nothing"
                             " -- this DLL is not emulated (see layer_stub.cpp)\n",
                             name_.c_str(), msg);
        }

        /* A few verbs have a meaningful answer for callers that ask. */
        if (!strncmp(msg, "getw", 4) || !strncmp(msg, "getwidth", 8))
            ret_int = width;
        else if (!strncmp(msg, "geth", 4) || !strncmp(msg, "getheight", 9))
            ret_int = height;
        else if (!strncmp(msg, "getx", 4))
            ret_int = 0;
        else if (!strncmp(msg, "gety", 4))
            ret_int = 0;
        return NULL;
    }

    void refresh(SDL_Surface *surface, SDL_Rect &clip)
    {
        (void)surface; (void)clip;   /* nothing to draw */
    }

private:
    bool        reported_;
    std::string name_;
};

Layer *createStubLayer(const char *dll_name, int w, int h)
{
    return new StubLayer(dll_name, w, h);
}

#else

Layer *createStubLayer(const char *, int, int) { return NULL; }

#endif
