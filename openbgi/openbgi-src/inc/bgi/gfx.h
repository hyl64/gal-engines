/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gfx.h - the graphics subsystem as seen from the VM core.
 *
 * The full object model (bitmaps, display objects, sprites, windows,
 * effectors, the compositor) is declared in the headers under inc/bgi/gfx/;
 * this file collects the handful of entry points the scheduler and the
 * system-control instructions need, so that src/vm/ does not depend on the
 * renderer's internals: the creation and teardown of the manager set, the
 * movie player, the per-pass services of the particle and rain screens
 * and the knob registry of the main window.
 */
#ifndef BGI_GFX_H_
#define BGI_GFX_H_

#include "bgi/common.h"
#include "bgi/gfx/gfxmgr.h"

struct Thread;
struct Bitmap;
struct FileSet;

// ---- the manager itself ------------------------------------------------
/* gGfx and every Gfx_* method are declared in gfx/gfxmgr.h.  The window
 * code calls them directly with gGfx, as the original does. */

// the cdecl wrappers used by the opcode handlers and the cursor code
#include "bgi/gfx/gfxcall.h"

// ---- life cycle ---------------------------------------------------------
/* the graphics half of the engine's core set-up: the worker pool, the
 * graphics manager gGfx and the bitmap manager gBmpMgr; bandPixels is the
 * compositor band size derived from the CPU cache, cpus the number of
 * worker threads */
void Gfx_Create(uint32_t bandPixels, int cpus);
/* tear down gGfx, gBmpMgr, the archive manager, the static caches of the
 * text engine and the loader, and the worker pool */
void Gfx_Destroy(void);
extern struct WorkerPool* gWorkerPool;
void Blit_InitMmxTables(void); // the scalers' MMX weight table (blit_scale.c); once at start-up
void Ptcl_InitGlobals(void);   // the particle pattern tables; once per process
void Ptcl_FreeGlobals(void);   // free them at shutdown
// the module copies of the manager pointers (image decoders / particles)
void Panel_SetGfxPtr(struct Gfx* g);
void Panel_SetBmpMgrPtr(struct BmpMgr* m);
void Ptcl_SetBmpMgrPtr(struct BmpMgr* m);
void Pattern_SetBmpMgrPtr(struct BmpMgr* m);

//  --- movies ("90 F0" .. "90 F3" and "92 F0"; src/gfx/movie.c) --
extern int gMoviePlaying; // a movie is running (the window paints black, no style toggle)
/* play `path` in the (x, y, w, h) rectangle of the window; 1 when
 * playback started (*outLength receives the length in ms), 0 otherwise.
 * A missing audio device or renderer is a script error. */
int Movie_Play(int32_t* outLength, const char* path, int x, int y, int w, int h);
/* the same for a path of the host's own file system (an archive entry
 * unpacked to a temporary file, "92 F0"); it skips the engine's path
 * mapping, so the player can hand it to ffmpeg as it is */
int Movie_PlayNative(int32_t* outLength, const char* nativePath, int x, int y, int w, int h);
int Movie_Stop(void); // 1 when there was a graph to release
int Movie_IsPlaying(void);
int Movie_SetVolume(uint32_t v);   // 0 .. 0x80; 0 for anything above
void Movie_OnEvent(int completed); // the WM_APP notification of the window procedure
void Movie_Tick(void);             // every pass: the ffmpeg player's frame, and its end

/* "92 F0": play the movie entry (arc, name) of the archive manager in the
 * rectangle; 1 when playback started, *outLength then the length in ms.
 * The entry is unpacked into the cache directory of MovieArc_Extract()
 * first, so a large movie costs that copy once per run (per name and
 * size); 0 when the entry cannot be read or no player could be started. */
int Movie_PlayArc(int32_t* outLength, const char* arc, const char* name, int x, int y, int w, int h);
/* The unpack cache of "92 F0" (src/gfx/movie.c): write the entry (arc,
 * name) of archive manager `mgr` - gArcMgr when NULL - as the host file
 * `nativeOut` (n bytes; "$TMPDIR/openbgi-movies/..." when it is NULL or
 * n is 0).  An existing file of the entry's size is reused.  1 ok, 0 when
 * the entry cannot be read or the file cannot be written. */
int MovieArc_Extract(struct FileSet* mgr, const char* arc, const char* name, char* nativeOut, size_t n);
/* the number of unpackings and of cache hits of MovieArc_Extract since
 * start-up (diagnostics and tests; BGI_DEBUG_MOVIE prints them) */
void MovieArc_Stats(int* extracts, int* hits);

/* The ffmpeg player (src/sys/moviefx.c): used when the OS layer's own
 * player cannot play the file - always on POSIX.  Needs the ffmpeg
 * program (and ffprobe for the length) on the PATH or beside the binary. */
int MovieFx_Start(const char* path, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
/* the same for a host path (see Movie_PlayNative) */
int MovieFx_StartNative(const char* nativePath, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void MovieFx_Stop(void);
int MovieFx_IsRunning(void);
void MovieFx_SetVolume(int32_t volume);
int MovieFx_Tick(void); // 1 while running, 0 when it just ended or nothing runs
int MovieFx_FramesShown(void); // frames blitted since the current movie started (diagnostics)

//  --- video textures ("92 F2 / F4 / F5", "91 F0 .. F7"; src/gfx/vidtex.c) --
/* A bitmap slot can carry a video texture: a BF_Movie (inc/bgi/bfmovie.h)
 * attached by "92 F2" that decodes its frames into the slot's pixels on
 * the engine's pass, at the movie's frame rate, from the moment "92 F4" /
 * "91 F3" starts it.  "91 F1" reports the duration, "92 F5" the played
 * position, "91 F7" the frame decoded last; "91 F2" / "91 F6" release it.
 * The result codes are the scripts': 0 ok, 1 the slot has no texture,
 * 4 no such bitmap slot; VidTex_Attach's own codes are 1 the player could
 * not be created, 2 the slot is empty, 3 the slot does not match the
 * movie, 4 the first frame could not be decoded. */
int VidTex_Attach(int bmp, const char* arc, const char* name, int flags, int volume);
void VidTex_Release(int bmp); // drop the texture of `bmp` if it has one
void VidTex_FreeAll(void);    // drop every texture (shutdown, restart)
int VidTex_State(int bmp, int32_t* outDurationMs);  // "91 F1": duration
int VidTex_Position(int bmp, int32_t* outMs);       // "92 F5": position
int VidTex_Control(int bmp, int v);                 // "92 F4" / "91 F3": v 0 starts; 1 when ready
int VidTex_Advance(int bmp);                        // "91 F5": decode the next frame now
int VidTex_Frame(int bmp, int32_t* outFrame);       // "91 F7": last decoded frame, 1 / 0 for no bitmap
void VidTex_Tick(void);                             // every pass: decode the frame that is due

// ---- manager-wide operations --------------------------------------------
void Gfx_ResetAll(void);      // reset + free every bitmap slot
void BmpMgr_SetOption(int v); // "90 0B": re-created bitmap slots keep their generation

// ---- per-pass services called from Vm_FrameServices ---------------------
void Ptcl_UpdateAll(void); // bring every particle engine up to the present
void Ptcl_AutoClear(void); // forget the automatic redraw list ("C0 09")
void Ptcl_AutoTick(void);  // redraw the screens on that list when due
void Rain_UpdateAll(void); // run the stages of every rain screen
void Rain_FrameTick(void); // redraw the rain screens at the effect frame rate
// Panel_DeleteAll / Panel_PollAll: panel.h

// the knob (slider) registry of the main window (src/sys/knobreg.c)
typedef struct KnobReg KnobReg_t;
int Knob_Register(uint32_t h);                   // "90 D0": 1 ok, 0 not a knob
int Knob_Unregister(uint32_t h);                 // "90 D1": 1 if it was registered
KnobReg_t* Knob_UnderMouse(void);                // the registered knob under the mouse, NULL when none
void Knob_BeginDrag(KnobReg_t* k, int x, int y); // start a drag at (x, y); NULL ends the drag
KnobReg_t* Knob_ActiveDrag(void);                // the knob being dragged, NULL when none
void Knob_MarkChanged(KnobReg_t* k);             // k->changed = 1 (right button, window procedure)
DispObj_t* Knob_TopCaptured(void);               // the most recently captured knob, NULL when none
int Knob_NudgeCaptured(int down);                // the wheel moves the top captured knob a step; 1 if a knob is captured
void Knob_UnregisterAll(void);                   // drop every registration (shutdown, restart)
void Knob_ReleaseAll(void);                      // empty the captured list (shutdown)
int Knob_Release(uint32_t h);                    // "90 DF": 1 if it was captured
int Knob_Capture(uint32_t h);                    // "90 DE": 1 if the handle is a knob
uint32_t Knob_PollChanged(void);                 // "90 DB": last flagged handle, flags cleared
uint32_t Knob_DragHandle(void);                  // "91 DB" (1.588 on): the handle of the knob being dragged, 0 when none
void Knob_SetGlobal(int on);                     // "90 DD": the wheel goes to the captured knob
int Knob_GetGlobal(void);
extern int gKnobGlobal;
void Knob_Update(void); // per pass: the dragged knob follows the mouse

// icon / menu / panel selection requires the window to be active ("90 AF")
void Select_RequireActive(int on); // the three below
void IconSelect_SetRequireActive(int on);
void MenuSelect_SetRequireActive(int on);
void Panel_SetRequireActive(int on);

// ---- 3-D sprites ("90 5D", 1.588 on) -----------------------------------
/* The cdecl wrapper of the 3-D projected sprite.  It lives in
 * src/gfx/gfxcall.c (the other "90 5x" forwarders are there too); the
 * manager's sprite table, src/gfx/mgr/sprites.c, only has the "90 5C"
 * entry point, so this one is assembled from Gfx_FindSprite /
 * Sprite_SetProjected3DEx.  Same result codes as GfxCall_SpriteSetProjected:
 * 0 ok, 1 a bitmap is unusable, 9 the second bitmap differs in size or mode,
 * 8 an empty projection, 0xFF unknown handle. */
int GfxCall_SpriteSetProjected3D(uint32_t h, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2,
	int mixRatio, int levelRoute, int ox, int oy, int32_t angle, int32_t rotX, int32_t rotY, int32_t rotZ,
	int projDist, int projPos, int smooth, int effect, int level, int prio); // "90 5D"

#endif // BGI_GFX_H_
