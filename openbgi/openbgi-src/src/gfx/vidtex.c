/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vidtex.c - video textures: a BF_Movie attached to a bitmap slot and
 *            decoded into its pixels on the engine's pass.  Interface:
 *            bgi/gfx.h (VidTex_*), instructions "92 F2 / F4 / F5" and
 *            "91 F0 .. F7".
 *
 * The original creates a DirectShow player for the movie and has it paint
 * the decoded frames into the slot's bitmap; the scripts then draw that
 * bitmap like any other (a sprite or a window frame), poll the played
 * position ("92 F5") and release the texture when the movie is over.  The
 * player object is what "91 F1" asks for the duration and "91 F7" for the
 * frame number shown last.
 *
 * Here the movie is a BF_Movie file (inc/bgi/bfmovie.h) read from an
 * archive entry and registered with the BF_Movie registry of
 * src/gfx/bmseq.c - the same registry "90 F4" .. "90 F7" use - so the
 * frame decoding is BmSeq_Decode: frame 0 is a key frame and the others
 * are deltas, hence a texture decodes every frame it passes, never just
 * the due one, and the slot must keep the movie's size and pixel mode.
 * The clock starts when the script starts the player ("92 F4" with 0, or
 * "91 F3"); VidTex_Tick is called every pass from Movie_Tick (the
 * scheduler's per-pass movie service) and brings the bitmap up to the
 * frame that is due.  A slot that is freed or re-created (its generation
 * changes) drops its texture on the next pass.
 *
 * BGI_DEBUG_MOVIE=1 reports attach / release and the frame numbers on
 * stderr; off by default and one getenv per run.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/file.h"
#include "bgi/os.h"

#include <stdio.h>

#define VIDTEX_MAX 64 // the scripts use up to 21 slots; more than 64 textures at once is a leak

typedef struct VidTex
{
	int bmpNo;          // the bitmap slot the frames decode into
	int32_t seqNo;      // the BF_Movie record of the registry (src/gfx/bmseq.c)
	int32_t w, h, mode; // the movie's size and pixel mode
	int32_t fps;        // frames per second (0: no clock)
	int32_t frames;     // frame count
	int32_t gen;        // the slot's generation at attach
	int32_t frame;      // frame decoded last
	int32_t startTick;  // when playback started, 0 while it has not
	int32_t volume;     // "92 F2"'s volume (the BF_Movie carries no sound; kept for the state)
	int flags;          // "92 F2"'s fourth operand (unused by the decode)
	struct VidTex* next;
} VidTex_t;

static VidTex_t* gTextures; // the attached textures, newest first
static int gTexCount;

// BGI_DEBUG_MOVIE=1: log attach, release and the frame progress (off by default, one getenv per run)
static int TexDebug(void)
{
	static int on = -1;
	if(on < 0)
		on = getenv("BGI_DEBUG_MOVIE") != NULL;
	return on;
}

static VidTex_t* Find(int bmp)
{
	VidTex_t* t;
	for(t = gTextures; t; t = t->next)
		if(t->bmpNo == bmp)
			return t;
	return NULL;
}

// the duration of a movie: frames at fps, rounded to the millisecond
static int32_t DurationOf(const VidTex_t* t)
{
	if(t->fps <= 0)
		return 0;
	return (int32_t)(((int64_t)t->frames * 1000 + t->fps / 2) / t->fps);
}

/* Drop the record and its BF_Movie (the registry's copy of the file).  The
 * bitmap is left alone: release never touches the pixels it decoded
 * into, as the original's player did not. */
static void Drop(VidTex_t* t)
{
	VidTex_t** p;
	for(p = &gTextures; *p && *p != t; p = &(*p)->next)
		;
	if(*p)
		*p = t->next;
	if(t->seqNo)
		BmSeq_Free(t->seqNo);
	if(TexDebug())
		fprintf(stderr, "[vidtex] bmp %d: released at frame %d of %d\n", t->bmpNo, (int)t->frame, (int)t->frames);
	BGI_Free(t);
	gTexCount--;
}

// the slot still holds the bitmap the texture was attached to
static int SlotAlive(const VidTex_t* t)
{
	Bmp_t b;
	return BmpMgr_GetInfo(gBmpMgr, &b, t->bmpNo) && BmpMgr_Generation(gBmpMgr, t->bmpNo) == t->gen;
}

// decode the frames up to and including `frame` (the deltas need every step)
static int DecodeTo(VidTex_t* t, int32_t frame)
{
	Bmp_t b;
	if(frame >= t->frames)
		frame = t->frames - 1;
	if(frame <= t->frame)
		return 1;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, t->bmpNo) || BmpMgr_Generation(gBmpMgr, t->bmpNo) != t->gen)
		return 0;
	while(t->frame < frame)
	{
		if(BmSeq_Decode(t->bmpNo, t->seqNo, (uint32_t)(t->frame + 1)) != 0)
			return 0;
		t->frame++;
	}
	return 1;
}

/* "92 F2": read the BF_Movie entry (arc, name) and attach it to bitmap
 * slot `bmp`, making the slot show its first frame.  0 ok, 1 the player
 * could not be created (the entry is missing or not a movie), 2 the slot
 * is empty, 3 the slot does not match the movie, 4 the first frame could
 * not be decoded. */
int VidTex_Attach(int bmp, const char* arc, const char* name, int flags, int volume)
{
	Bmp_t b;
	VidTex_t* t;
	uint8_t* data;
	uint32_t size, got;
	int32_t no = 0, info[5];
	uint32_t r;

	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 2;
	if(gArcMgr == NULL || arc == NULL || name == NULL)
		return 1;
	size = ArcMgr_SizeOf(gArcMgr, arc, name);
	if(size == ARC_ERR_NOT_FOUND || size == ARC_ERR_OPEN || size == 0)
		return 1;
	data = (uint8_t*)BGI_Alloc(size);
	got = ArcMgr_Read(gArcMgr, data, arc, name);
	if(got != size)
	{
		BGI_Free(data);
		return 1;
	}
	// the registry keeps its own copy, as for "90 F4"
	r = BmSeq_Register(data, size, &no, info);
	BGI_Free(data);
	if(r != 0)
		return 1;
	if(info[0] != b.w || info[1] != b.h || info[2] != b.mode || b.bpp != 4)
	{
		BmSeq_Free(no);
		return 3;
	}
	r = BmSeq_Decode(bmp, no, 0);
	if(r != 0)
	{
		BmSeq_Free(no);
		return r == 0x80000005u ? 3u : 4u;
	}
	// a slot carries one texture: an attachment replaces the earlier one
	{
		VidTex_t* old = Find(bmp);
		if(old)
			Drop(old);
	}
	if(gTexCount >= VIDTEX_MAX)
	{
		BmSeq_Free(no);
		return 1;
	}
	t = (VidTex_t*)BGI_Calloc(sizeof *t);
	t->bmpNo = bmp;
	t->seqNo = no;
	t->w = info[0];
	t->h = info[1];
	t->mode = info[2];
	t->fps = info[3];
	t->frames = info[4];
	t->gen = BmpMgr_Generation(gBmpMgr, bmp);
	t->frame = 0;
	t->startTick = 0;
	t->volume = volume;
	t->flags = flags;
	t->next = gTextures;
	gTextures = t;
	gTexCount++;
	if(TexDebug())
		fprintf(stderr, "[vidtex] bmp %d: attached %s:%s, %dx%d mode %d fps %d frames %d (duration %d ms)\n", bmp,
			   arc, name, (int)t->w, (int)t->h, (int)t->mode, (int)t->fps, (int)t->frames, (int)DurationOf(t));
	return 0;
}

void VidTex_Release(int bmp)
{
	VidTex_t* t = Find(bmp);
	if(t)
		Drop(t);
}

void VidTex_FreeAll(void)
{
	while(gTextures)
		Drop(gTextures);
}

// "91 F1": the duration of the slot's texture in ms; 0 ok, 1 no texture, 4 no such slot
int VidTex_State(int bmp, int32_t* outDurationMs)
{
	Bmp_t b;
	VidTex_t* t;
	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 4;
	t = Find(bmp);
	if(t == NULL)
		return 1;
	if(outDurationMs)
		*outDurationMs = DurationOf(t);
	return 0;
}

// "92 F5": the played position in ms; 0 ok, 1 no texture, 4 no such slot
int VidTex_Position(int bmp, int32_t* outMs)
{
	Bmp_t b;
	VidTex_t* t;
	int32_t pos = 0, dur;
	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 4;
	t = Find(bmp);
	if(t == NULL)
		return 1;
	dur = DurationOf(t);
	if(t->startTick)
	{
		pos = (int32_t)(OS_TicksMs() - (uint32_t)t->startTick);
		if(pos < 0)
			pos = 0;
		if(pos > dur)
			pos = dur;
	}
	if(outMs)
		*outMs = pos;
	return 0;
}

/* "92 F4" / "91 F3": control the player.  0 starts it (it is ready at
 * once: the first frame was decoded by the attach; 1 is the answer the
 * scripts' wait loop wants), 1 pauses it, 2 restarts it from frame 0; any
 * other value is accepted without an effect.  Always 1 for a texture whose
 * player exists, 1 for a slot without one (as the original's codes say)
 * and 4 for a slot without a bitmap. */
int VidTex_Control(int bmp, int v)
{
	Bmp_t b;
	VidTex_t* t;
	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 4;
	t = Find(bmp);
	if(t == NULL)
		return 1;
	if(v == 0)
	{
		if(t->startTick == 0)
			t->startTick = (int32_t)OS_TicksMs();
	}
	else if(v == 1)
		t->startTick = 0;
	else if(v == 2)
	{ // restart: frame 0 is the key frame, the deltas cannot be replayed from the current picture
		t->frame = 0;
		if(BmSeq_Decode(bmp, t->seqNo, 0) != 0)
		{
			Drop(t);
			return 1;
		}
		t->startTick = (int32_t)OS_TicksMs();
	}
	if(TexDebug())
		fprintf(stderr, "[vidtex] bmp %d: control %d -> started %d\n", bmp, v, t->startTick ? 1 : 0);
	return 1;
}

// "91 F5": decode the next frame at once; 0 ok, 1 no texture, 4 no such slot
int VidTex_Advance(int bmp)
{
	Bmp_t b;
	VidTex_t* t;
	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 4;
	t = Find(bmp);
	if(t == NULL)
		return 1;
	if(!SlotAlive(t))
	{
		Drop(t);
		return 1;
	}
	if(!DecodeTo(t, t->frame + 1))
		Drop(t);
	return 0;
}

/* "91 F7": the frame number decoded last into *outFrame, 1 for a slot with
 * a bitmap (0 without one); a slot with a texture reports its frames, one
 * without reports 0 as the original does. */
int VidTex_Frame(int bmp, int32_t* outFrame)
{
	Bmp_t b;
	VidTex_t* t;
	if(gBmpMgr == NULL || !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 0;
	t = Find(bmp);
	if(outFrame)
		*outFrame = t ? t->frame : 0;
	return 1;
}

/* Every pass (from Movie_Tick): decode the frame that is due on the
 * texture's clock, and drop the textures whose slot is gone. */
void VidTex_Tick(void)
{
	VidTex_t* t;
	uint32_t now;
	if(gTextures == NULL)
		return;
	if(gBmpMgr == NULL)
	{
		VidTex_FreeAll();
		return;
	}
	now = OS_TicksMs();
	for(t = gTextures; t;)
	{
		VidTex_t* next = t->next;
		if(!SlotAlive(t))
			Drop(t);
		else if(t->startTick && t->fps > 0)
		{
			int32_t dur = DurationOf(t);
			int32_t pos = (int32_t)(now - (uint32_t)t->startTick);
			int32_t due;
			if(pos < 0)
				pos = 0;
			if(pos > dur)
				pos = dur;
			due = (int32_t)(((int64_t)pos * t->fps) / 1000);
			if(due < 0)
				due = 0;
			if(due > t->frames - 1)
				due = t->frames - 1;
			if(!DecodeTo(t, due))
				Drop(t);
		}
		t = next;
	}
}
