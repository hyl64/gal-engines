/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/sprite_3d.c - unit test of the 3-D projected sprite ("90 5D",
 *                     1.588 on): the perspective blitter src/gfx/blit_persp.c,
 *                     Sprite_SetProjected3D / Sprite_Draw in src/gfx/sprite.c
 *                     and the GfxCall wrapper; built as bin/test_sprite_3d by
 *                     `make test`
 *
 * The sprite machinery is driven directly (no game data, no window): a fake
 * Gfx_t with a back buffer and an empty compositor is enough, the bitmaps
 * are synthetic patterns with a unique colour per pixel, and the boxes are
 * rendered into private canvases that are compared byte for byte.
 *
 * The groups of tests, in the order they run:
 *
 *   fixtures    - the fake manager set: a pattern bitmap, a canvas, the
 *                 "90 5C" baseline really draws
 *   rot=0       - "90 5D" with the three rotations at 0 is the "90 5C"
 *                 picture: the box (size, anchor) and every pixel of the
 *                 rendered canvas are identical, over a sweep of origins,
 *                 angles, depths, projections, smooth on/off, a second bitmap
 *                 for the cross-fade and an effect that needs the temporary
 *                 path
 *   rotZ        - rotZ is the plane rotation of "90 5C": rotZ = 90 / 180 /
 *                 270 degrees is pixel-identical to the same angle in 90 5C,
 *                 and a unique mark pixel lands on the position the rotation
 *                 formula predicts (not merely "somewhere else")
 *   perspective - the depth scale of the perspective blitter: the drawn size
 *                 of a flat picture follows projDist / (projDist + fz) and
 *                 equals ProjectScale's factor behind the screen plane; a
 *                 rotation about Y puts one side farther away, so the Z value
 *                 takes part in the projection; projDist = 0 disables the
 *                 depth scaling
 *   rotXY       - a rotation about X and/or Y really moves pixels where the
 *                 documented projection matrix says: a unique mark pixel is
 *                 searched in the rendered box and compared with the forward
 *                 projection of its cell centre
 *   degenerate  - tiny pictures, edge-on rotations (a corner on or behind the
 *                 eye), a projection distance of 0 and huge magnifications:
 *                 the set call reports the empty box or accepts it, and
 *                 rendering never reads outside the pictures
 *   visual      - a 640 x 480 composite of rotated and perspective-scaled
 *                 sprites and a depth comparison, written as BMP under
 *                 ../screenshots (override with SPRITE3D_DIR)
 *   blit path   - the NEON blend / tint kernels of src/gfx/blit.c against a
 *                 copy of the scalar definition (odd widths, every level,
 *                 the dim / tint / over entry points); SPRITE3D_BENCH=1
 *                 (default off) also times both on a 1920 x 1080 picture
 */
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/compositor.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/sprite.h"
#include "bgi/gfx.h"
#include "bgi/version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// ========================================================================
// the fake graphics manager set
// ========================================================================

static BmpMgr_t* gMgr;
static Gfx_t gFakeGfx;
static Compositor_t gFakeComp;
static Gfx_t* gSavedGfx;
static BmpMgr_t* gSavedMgr;
static Bmp_t gCanvas; // 256 x 192 RGB32, the back buffer of the fake manager

#define CANVAS_W 256
#define CANVAS_H 192

static void SetupGfx(void)
{
	Blit_InitTables();
	gSavedGfx = gGfx;
	gSavedMgr = gBmpMgr;
	gMgr = BmpMgr_New(32);
	gBmpMgr = gMgr;

	memset(&gFakeGfx, 0, sizeof gFakeGfx);
	memset(&gFakeComp, 0, sizeof gFakeComp); // empty object list: Compositor_Resort of an unlisted object is a no-op
	Bmp_Alloc(&gCanvas, CANVAS_W, CANVAS_H, PM_RGB32);
	Bmp_Clear(&gCanvas, NULL);
	gFakeGfx.back = gCanvas;
	gFakeGfx.screen.l = 0;
	gFakeGfx.screen.t = 0;
	gFakeGfx.screen.r = CANVAS_W - 1;
	gFakeGfx.screen.b = CANVAS_H - 1;
	gFakeGfx.comp = &gFakeComp;
	gFakeGfx.projCentre[0] = gFakeGfx.projCentre[1] = -1;

	gGfx = &gFakeGfx;
	DispObj_SetGfxPtr(&gFakeGfx);
	DispObj_SetBmpMgrPtr(gMgr);
}

static void TeardownGfx(void)
{
	Bmp_Free(&gCanvas);
	BmpMgr_Delete(gMgr);
	gMgr = NULL;
	gGfx = gSavedGfx;
	gBmpMgr = gSavedMgr;
}

// register a sprite in the fake manager's table (the GfxCall wrappers look it up by handle)
static uint32_t RegisterSprite(Sprite_t* s, int idx)
{
	memset(s, 0, sizeof *s);
	Sprite_Ctor(s, idx);
	gFakeGfx.sprites[idx & 0xff] = &s->obj;
	return H_SPRITE | (uint32_t)(idx & 0xff);
}

static void UnregisterSprite(Sprite_t* s)
{
	int i;
	for(i = 0; i < GFX_SPRITES; i++)
		if(gFakeGfx.sprites[i] == &s->obj)
			gFakeGfx.sprites[i] = NULL;
	Sprite_Dtor(s);
}

// ========================================================================
// bitmaps and canvases
// ========================================================================

/* A deterministic pattern with a unique colour per pixel: the colour is
 * 1 + x + (y << 8) + (salt << 16), so two pictures with different salts can
 * never share a pixel value and a pixel found in a render identifies its
 * source position.  ARGB32 pictures are opaque. */
static void FillUnique(Bmp_t* b, uint32_t salt)
{
	int x, y;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
		{
			uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			uint32_t v = 1u + (uint32_t)x + ((uint32_t)y << 8) + (salt << 16);
			if(b->mode == PM_RGB16)
			{
				p[0] = (uint8_t)v;
				p[1] = (uint8_t)((v >> 8) & 0x7f);
			}
			else if(b->mode == PM_GRAY8)
			{
				p[0] = (uint8_t)v;
			}
			else
			{
				p[0] = (uint8_t)v;
				p[1] = (uint8_t)(v >> 8);
				p[2] = (uint8_t)(v >> 16);
				p[3] = b->mode == PM_ARGB32 ? 0xff : 0;
			}
		}
}

// create slot `no` as a w x h picture of `mode` filled by FillUnique; 1 ok
static int MakeUniqueBitmap(int no, int w, int h, int mode, uint32_t salt)
{
	Bmp_t b;
	if(!BmpMgr_Create(gMgr, no, w, h, mode))
		return 0;
	if(!BmpMgr_GetInfo(gMgr, &b, no))
		return 0;
	FillUnique(&b, salt);
	return 1;
}

/* The checkerboard of the visual evidence: 8 x 8 cells of two colours, a
 * white border and two diagonals, opaque, so a rotation / a perspective can
 * be read from the picture itself. */
static void FillChecker(Bmp_t* b)
{
	int x, y, w = b->w, h = b->h;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
		{
			uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			uint32_t v;
			if(x == 0 || y == 0 || x == w - 1 || y == h - 1)
				v = 0x00f0f0f0u; // border
			else if(abs(x - y) <= 2 || abs((w - 1 - x) - y) <= 2)
				v = 0x002020e0u; // the two diagonals
			else if(abs(x - w / 2) < 5 && abs(y - h / 2) < 5)
				v = 0x0040e040u; // a green square in the middle
			else
				v = (((x >> 3) + (y >> 3)) & 1) ? 0x00c07840u : 0x00304868u; // the checker
			p[0] = (uint8_t)v;
			p[1] = (uint8_t)(v >> 8);
			p[2] = (uint8_t)(v >> 16);
			p[3] = 0xff;
		}
}

/* Render the sprite alone into a fresh RGB32 bitmap of its box: this is the
 * piece the compositor would hand to Sprite_Draw (the whole local rectangle,
 * starting at 0, 0). */
static int RenderBox(Sprite_t* s, Bmp_t* out)
{
	Rect_t local;
	memset(out, 0, sizeof *out);
	if(s->outW < 1 || s->outH < 1)
		return 0;
	if(!Bmp_Alloc(out, s->outW, s->outH, PM_RGB32))
		return 0;
	Bmp_Clear(out, NULL);
	local.l = 0;
	local.t = 0;
	local.r = s->outW - 1;
	local.b = s->outH - 1;
	s->obj.vt->draw(&s->obj, out, &local, 0);
	return 1;
}

// the number of differing pixels of two equal-sized views (-1 when the sizes differ)
static int CountDiffs(const Bmp_t* a, const Bmp_t* b)
{
	int x, y, n = 0;
	if(a->w != b->w || a->h != b->h)
		return -1;
	for(y = 0; y < a->h; y++)
		for(x = 0; x < a->w; x++)
		{
			const uint8_t* pa = a->pixels + (size_t)y * (size_t)a->pitch + (size_t)x * (size_t)a->bpp;
			const uint8_t* pb = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			if(memcmp(pa, pb, (size_t)a->bpp) != 0)
				n++;
		}
	return n;
}

// the 32-bit colour word (B | G << 8 | R << 16) of a pixel of an RGB32 view
static uint32_t PixelAt(const Bmp_t* b, int x, int y)
{
	const uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

// the first occurrence of `colour` in an RGB32 view; 1 when found
static int FindColour(const Bmp_t* b, uint32_t colour, int* outX, int* outY)
{
	int x, y;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
			if(PixelAt(b, x, y) == colour)
			{
				*outX = x;
				*outY = y;
				return 1;
			}
	return 0;
}

// count the occurrences of `colour` in an RGB32 view
static int CountColour(const Bmp_t* b, uint32_t colour)
{
	int x, y, n = 0;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
			n += PixelAt(b, x, y) == colour;
	return n;
}

/* Render the picture through Blit_Persp about its own box: the box comes from
 * Blit_PerspBounds so the whole projected picture is inside.  Returns 1 and
 * the bitmap; `boxAnchor` receives the anchor. */
static int RenderPerspBox(Bmp_t* out, const Bmp_t* src, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t rotX, int32_t rotY, int32_t sx, int32_t sy, int32_t fz, int projDist, int smooth, int32_t* boxAnchor)
{
	int32_t outW = 0, outH = 0, anchor[2] = {0, 0};
	if(!Blit_PerspBounds(&outW, &outH, anchor, src->w, src->h, cx16, cy16, angle, rotX, rotY, sx, sy, fz,
		   projDist, 0, 0))
		return 0;
	if(outW <= 0 || outH <= 0 || outW > 1024 || outH > 1024)
		return 0;
	if(!Bmp_Alloc(out, outW, outH, PM_RGB32))
		return 0;
	Bmp_Clear(out, NULL);
	Blit_Persp(out, anchor[0] << 16, anchor[1] << 16, src, cx16, cy16, angle, rotX, rotY, sx, sy, fz, projDist, 0,
		smooth);
	if(boxAnchor)
	{
		boxAnchor[0] = anchor[0];
		boxAnchor[1] = anchor[1];
	}
	return 1;
}

// the extents of the pixels of `colour` in a view; 0 when there are none
static int ColourExtents(const Bmp_t* b, uint32_t colour, int* outW, int* outH)
{
	int x, y, l = b->w, t = b->h, r = -1, bot = -1;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
			if(PixelAt(b, x, y) == colour)
			{
				if(x < l)
					l = x;
				if(x > r)
					r = x;
				if(y < t)
					t = y;
				if(y > bot)
					bot = y;
			}
	if(r < l || bot < t)
		return 0;
	*outW = r - l + 1;
	*outH = bot - t + 1;
	return 1;
}

// ========================================================================
// BMP output for the visual evidence
// ========================================================================

static int WriteBmp24(const char* path, const Bmp_t* b)
{
	FILE* f;
	int x, y, row = (3 * b->w + 3) & ~3;
	uint8_t hdr[54];
	uint32_t fileSize = 54u + (uint32_t)row * (uint32_t)b->h;
	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'B';
	hdr[1] = 'M';
	hdr[2] = (uint8_t)fileSize;
	hdr[3] = (uint8_t)(fileSize >> 8);
	hdr[4] = (uint8_t)(fileSize >> 16);
	hdr[5] = (uint8_t)(fileSize >> 24);
	hdr[10] = 54; // pixel data offset
	hdr[14] = 40; // BITMAPINFOHEADER
	hdr[18] = (uint8_t)b->w;
	hdr[19] = (uint8_t)(b->w >> 8);
	hdr[20] = (uint8_t)(b->w >> 16);
	hdr[21] = (uint8_t)(b->w >> 24);
	hdr[22] = (uint8_t)b->h;
	hdr[23] = (uint8_t)(b->h >> 8);
	hdr[24] = (uint8_t)(b->h >> 16);
	hdr[25] = (uint8_t)(b->h >> 24);
	hdr[26] = 1; // planes
	hdr[28] = 24;
	f = fopen(path, "wb");
	if(!f)
		return 0;
	if(fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr)
	{
		fclose(f);
		return 0;
	}
	for(y = b->h - 1; y >= 0; y--) // bottom-up
	{
		int pad = row - 3 * b->w;
		for(x = 0; x < b->w; x++)
		{
			const uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			uint8_t bgr[3] = {p[0], p[1], p[2]};
			if(fwrite(bgr, 1, 3, f) != 3)
			{
				fclose(f);
				return 0;
			}
		}
		while(pad-- > 0)
			if(fputc(0, f) == EOF)
			{
				fclose(f);
				return 0;
			}
	}
	return fclose(f) == 0;
}

// ========================================================================
// fixtures
// ========================================================================

static void TestFixtures(void)
{
	Bmp_t box;
	Sprite_t s;
	uint32_t h;
	printf("fixtures\n");
	CHECK(MakeUniqueBitmap(0, 41, 29, PM_RGB32, 0));
	h = RegisterSprite(&s, 0);
	// the "90 5C" baseline: a flat picture at the screen plane, angle 0,
	// drawn opaque over a clean canvas
	CHECK(GfxCall_SpriteSetProjected(h, 0, 0, 0, 0, -1, 0, -1, 20, 14, 0, 0, 0, 0, 1, 0, 0) == 0);
	CHECK(s.mode == 5);
	CHECK(s.outW >= 41 && s.outH >= 29);
	CHECK(RenderBox(&s, &box));
	CHECK(box.w == s.outW && box.h == s.outH);
	CHECK(CountColour(&box, 0x00000001u) == 1); // the top-left pattern pixel reached the canvas
	Bmp_Free(&box);
	UnregisterSprite(&s);
}

// ========================================================================
// (a) rotX = rotY = rotZ = 0 is the "90 5C" picture, pixel for pixel
// ========================================================================

static void TestRotZeroMatches5C(void)
{
	static const int32_t angles[] = {0, 17 << 16, -(90 << 16), 45 << 16, (360 + 33) << 16};
	static const int32_t fzs[] = {0, 120 << 16, 200 << 16, -(80 << 16)};
	static const int32_t oys[] = {0, 7, -5};
	int ia, iz, io;
	printf("rot=0\n");
	CHECK(MakeUniqueBitmap(1, 41, 29, PM_RGB32, 1));
	CHECK(MakeUniqueBitmap(2, 41, 29, PM_RGB32, 2));
	CHECK(MakeUniqueBitmap(3, 41, 29, PM_ARGB32, 3));
	for(ia = 0; ia < (int)(sizeof angles / sizeof angles[0]); ia++)
		for(iz = 0; iz < (int)(sizeof fzs / sizeof fzs[0]); iz++)
			for(io = 0; io < (int)(sizeof oys / sizeof oys[0]); io++)
			{
				int32_t angle = angles[ia];
				int32_t fz = fzs[iz];
				int oy = oys[io];
				int projDist = (iz & 1) ? 300 : 0;
				int projPos = (iz == 2);
				int smooth = (io & 1);
				Sprite_t a, b;
				Bmp_t ba, bb;
				uint32_t ha = RegisterSprite(&a, 1), hb = RegisterSprite(&b, 2);
				CHECK(GfxCall_SpriteSetProjected(ha, 0, 0, fz, 1, -1, 0, -1, 20, 14 + oy, angle, projDist, projPos,
						  smooth, 1, 0, 0x100) == 0);
				CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, fz, 1, -1, 0, -1, 20, 14 + oy, angle, 0, 0, 0,
						  projDist, projPos, smooth, 1, 0, 0x100) == 0);
				CHECK(a.outW == b.outW);
				CHECK(a.outH == b.outH);
				CHECK(a.anchorX == b.anchorX);
				CHECK(a.anchorY == b.anchorY);
				CHECK(a.subX == b.subX && a.subY == b.subY);
				CHECK(RenderBox(&a, &ba));
				CHECK(RenderBox(&b, &bb));
				if(ba.w == bb.w && ba.h == bb.h)
				{
					int diffs = CountDiffs(&ba, &bb);
					if(diffs != 0)
						printf("  (angle=%d fz=%d projDist=%d smooth=%d: %d differing pixels)\n", (int)(angle >> 16),
							(int)(fz >> 16), projDist, smooth, diffs);
					CHECK(diffs == 0);
				}
				else
					CHECK(0);
				if(ia == 0 && iz == 0 && io == 0)
					CHECK(CountColour(&bb, 0x00010001u) == 1); // salt 1, source (0, 0)
				Bmp_Free(&ba);
				Bmp_Free(&bb);
				UnregisterSprite(&a);
				UnregisterSprite(&b);
			}

	// a second bitmap (the cross-fade cache) and an ARGB32 picture with an
	// effect that goes through Blit_XformCopy + Bmp_BlitEffect: still identical
	{
		Sprite_t a, b;
		Bmp_t ba, bb;
		uint32_t ha = RegisterSprite(&a, 3), hb = RegisterSprite(&b, 4);
		CHECK(GfxCall_SpriteSetProjected(ha, 0, 0, 90 << 16, 1, 2, 0x80, -1, 20, 14, 23 << 16, 300, 0, 1, 1, 0,
				  0) == 0);
		CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, 90 << 16, 1, 2, 0x80, -1, 20, 14, 23 << 16, 0, 0, 0, 300, 0,
				  1, 1, 0, 0) == 0);
		CHECK(a.outW == b.outW && a.outH == b.outH);
		CHECK(RenderBox(&a, &ba) && RenderBox(&b, &bb));
		CHECK(ba.w == bb.w && ba.h == bb.h && CountDiffs(&ba, &bb) == 0);
		Bmp_Free(&ba);
		Bmp_Free(&bb);
		UnregisterSprite(&a);
		UnregisterSprite(&b);
	}
	{
		Sprite_t a, b;
		Bmp_t ba, bb;
		uint32_t ha = RegisterSprite(&a, 5), hb = RegisterSprite(&b, 6);
		// effect 4 (multiply): the mode 5 draw renders into a temporary first
		CHECK(GfxCall_SpriteSetProjected(ha, 0, 0, 0, 3, -1, 0, -1, 20, 14, 12 << 16, 0, 0, 1, 4, 0x80, 0) == 0);
		CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, 0, 3, -1, 0, -1, 20, 14, 12 << 16, 0, 0, 0, 0, 0, 1, 4, 0x80,
				  0) == 0);
		CHECK(a.outW == b.outW && a.outH == b.outH);
		CHECK(RenderBox(&a, &ba) && RenderBox(&b, &bb));
		CHECK(ba.w == bb.w && ba.h == bb.h && CountDiffs(&ba, &bb) == 0);
		Bmp_Free(&ba);
		Bmp_Free(&bb);
		UnregisterSprite(&a);
		UnregisterSprite(&b);
	}
}

// ========================================================================
// (b) the Z rotation: 90 / 180 / 270 degrees and where a mark pixel lands
// ========================================================================

static void TestRotZ(void)
{
	Sprite_t a, b;
	Bmp_t ba, bb;
	uint32_t ha = RegisterSprite(&a, 7), hb = RegisterSprite(&b, 8);
	int32_t angle;
	printf("rotZ\n");
	CHECK(MakeUniqueBitmap(4, 41, 29, PM_RGB32, 4));
	for(angle = 0; angle < 360 << 16; angle += 90 << 16)
	{
		CHECK(GfxCall_SpriteSetProjected(ha, 0, 0, 0, 4, -1, 0, -1, 20, 14, angle, 0, 0, 0, 1, 0, 0) == 0);
		CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, 0, 4, -1, 0, -1, 20, 14, 0, 0, 0, angle, 0, 0, 0, 1, 0, 0) ==
			  0);
		CHECK(a.outW == b.outW && a.outH == b.outH && a.anchorX == b.anchorX && a.anchorY == b.anchorY);
		CHECK(RenderBox(&a, &ba) && RenderBox(&b, &bb));
		CHECK(ba.w == bb.w && ba.h == bb.h);
		if(ba.w == bb.w && ba.h == bb.h)
		{
			int diffs = CountDiffs(&ba, &bb);
			if(diffs != 0)
				printf("  (angle=%d: %d differing pixels)\n", (int)(angle >> 16), diffs);
			CHECK(diffs == 0);
		}
		Bmp_Free(&ba);
		Bmp_Free(&bb);
	}

	// the mark pixel of the source (2, 3) must land where the 90 degree
	// formula says: with the plane map dx = v, dy = -u the box position is
	// i = anchorX + (py - oy), j = anchorY - (px - ox)
	{
		Bmp_t src, box;
		uint32_t mark = 0x00abcdefu;
		int i = -1, j = -1;
		CHECK(BmpMgr_GetInfo(gMgr, &src, 4));
		{
			uint8_t* p = src.pixels + (size_t)3 * (size_t)src.pitch + (size_t)2 * (size_t)src.bpp;
			p[0] = (uint8_t)mark;
			p[1] = (uint8_t)(mark >> 8);
			p[2] = (uint8_t)(mark >> 16);
		}
		CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, 0, 4, -1, 0, -1, 20, 14, 0, 0, 0, 90 << 16, 0, 0, 0, 1, 0,
				  0) == 0);
		CHECK(b.subX == 0 && b.subY == 0); // the fake back buffer is centred on whole pixels
		CHECK(RenderBox(&b, &box));
		CHECK(FindColour(&box, mark, &i, &j));
		{
			int expI = b.anchorX + (3 - 14);
			int expJ = b.anchorY - (2 - 20);
			printf("  mark (2,3) -> (%d,%d), expected (%d,%d), anchor (%d,%d)\n", i, j, expI, expJ, b.anchorX,
				b.anchorY);
			CHECK(i == expI);
			CHECK(j == expJ);
			CHECK(CountColour(&box, mark) == 1);
		}
		Bmp_Free(&box);
	}

	// a 180 degree rotation: the source (0, 0) must land on the bottom-right
	// corner of the box, (anchorX + ox, anchorY + oy)
	{
		Bmp_t box;
		int px = -1, py = -1;
		CHECK(GfxCall_SpriteSetProjected3D(hb, 0, 0, 0, 4, -1, 0, -1, 20, 14, 0, 0, 0, 180 << 16, 0, 0, 0, 1, 0,
				  0) == 0);
		CHECK(RenderBox(&b, &box));
		CHECK(FindColour(&box, 0x00040001u, &px, &py));
		printf("  rotZ 180: source (0,0) at box (%d,%d), expected (%d,%d) of %dx%d\n", px, py,
			b.anchorX + 20, b.anchorY + 14, box.w, box.h);
		CHECK(px == b.anchorX + 20);
		CHECK(py == b.anchorY + 14);
		Bmp_Free(&box);
	}
	UnregisterSprite(&a);
	UnregisterSprite(&b);
}

// ========================================================================
// (c) the perspective scale of a flat picture: projDist / (projDist + fz)
// ========================================================================

static void TestPerspScale(void)
{
	Bmp_t src, box;
	static const int32_t cases[][2] = {
		// projDist, fz (16.16): the drawn size is 40 * projDist / (projDist + fz)
		{0, 0},
		{0, 300 << 16},
		{400, 0},
		{400, 400 << 16},
		{400, 1200 << 16},
		{400, -(200 << 16)},
		{100, 0},
		{100, 300 << 16},
	};
	int i;
	printf("perspective\n");
	CHECK(Bmp_Alloc(&src, 40, 40, PM_RGB32));
	Bmp_Fill(&src, NULL, 0x00404040u);
	for(i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
	{
		int pd = cases[i][0], fz = cases[i][1];
		int32_t k;
		double expect, got;
		int mw = 0, mh = 0;
		if(!RenderPerspBox(&box, &src, 20 << 16, 20 << 16, 0, 0, 0, 0x10000, 0x10000, fz, pd, 0, NULL))
		{
			printf("  projDist=%d fz=%d: empty projection\n", pd, (int)(fz >> 16));
			CHECK(0);
			continue;
		}
		CHECK(ColourExtents(&box, 0x00404040u, &mw, &mh));
		expect = 40.0 * ((pd == 0) ? 1.0 : (double)pd / ((double)pd + (double)fz / 65536.0));
		got = 0.5 * ((double)mw + (double)mh);
		printf("  projDist=%d fz=%d: drawn %dx%d, expected %.2f\n", pd, (int)(fz >> 16), mw, mh, expect);
		CHECK(fabs(got - expect) <= 2.0);
		// behind the screen plane the blitter must agree with ProjectScale
		if(pd != 0 && fz >= 0)
		{
			ProjectScale(&k, fz, pd);
			CHECK(fabs((double)k / 65536.0 - (double)pd / ((double)pd + (double)fz / 65536.0)) < 1e-4);
		}
		if(pd != 0 && fz > 0)
			CHECK(mw < 40 && mh < 40); // a depth behind the screen plane shrinks
		if(pd != 0 && fz < 0)
			CHECK(mw > 40 && mh > 40); // one in front grows
		Bmp_Free(&box);
	}
	Bmp_Free(&src);

	// a rotation about Y puts one side of the picture farther away: with a
	// short projection distance the far side is compressed by the Z value,
	// while a projection without it would only give 40 * cos(30 deg)
	{
		Bmp_t s2, box2;
		int32_t anchor[2];
		int mw = 0, mh = 0;
		CHECK(Bmp_Alloc(&s2, 40, 40, PM_RGB32));
		Bmp_Fill(&s2, NULL, 0x00404040u);
		CHECK(RenderPerspBox(&box2, &s2, 0, 20 << 16, 0, 0, 30 << 16, 0x10000, 0x10000, 0, 100, 0, anchor));
		CHECK(ColourExtents(&box2, 0x00404040u, &mw, &mh));
		printf("  rotY=30 projDist=100: drawn %dx%d (a projection ignoring Z would be %.1f wide), anchor %d\n",
			mw, mh, 40.0 * cos(30.0 * 3.14159265358979 / 180.0), anchor[0]);
		CHECK(mw >= 36); // more than the 34.64 of a rotation without the Z division
		CHECK(mh > 40);  // the near (right) side is magnified
		Bmp_Free(&box2);
		Bmp_Free(&s2);
	}
}

// ========================================================================
// (b/c) X and Y rotations really move the pixels where the matrix says
// ========================================================================

/* The documented forward projection of src/gfx/blit_persp.c, in the test:
 * texture offset (u, v) (pixels, y down) -> box offset (dx, dy). */
static void ExpectProject(double* dx, double* dy, double u, double v, int32_t angle, int32_t rotX,
	int32_t rotY, double fzPx, int projDist, double sx, double sy)
{
	const double rad = 2.663161090079238e-07;
	double A = (double)angle * rad, X = (double)rotX * rad, Y = (double)rotY * rad;
	double cA = cos(A), sA = sin(A), cX = cos(X), sX = sin(X), cY = cos(Y), sY = sin(Y);
	double x2, y2, z, k;
	u *= sx;
	v *= sy;
	x2 = u * cY + v * sX * sY;
	y2 = v * cX;
	z = -u * sY + v * sX * cY;
	k = projDist == 0 ? 1.0 : (double)projDist / ((double)projDist + fzPx + z);
	*dx = cA * k * x2 + sA * k * y2;
	*dy = -sA * k * x2 + cA * k * y2;
}

static void TestRotXY(void)
{
	static const int32_t params[][4] = {
		// rotX, rotY, angle, projDist
		{20 << 16, 0, 0, 300},
		{0, 30 << 16, 0, 300},
		{20 << 16, 30 << 16, 0, 300},
		{15 << 16, -(25 << 16), 40 << 16, 250},
	};
	int ip;
	printf("rotXY\n");
	CHECK(MakeUniqueBitmap(5, 41, 41, PM_RGB32, 5));
	for(ip = 0; ip < (int)(sizeof params / sizeof params[0]); ip++)
	{
		Sprite_t s;
		Bmp_t box, src;
		uint32_t h = RegisterSprite(&s, 10 + ip);
		int32_t rotX = params[ip][0], rotY = params[ip][1], angle = params[ip][2];
		int pd = params[ip][3];
		int i = -1, j = -1;
		uint32_t mark = 0x00fedcbau;
		double ex, ey;
		CHECK(BmpMgr_GetInfo(gMgr, &src, 5));
		{
			uint8_t* p = src.pixels + (size_t)12 * (size_t)src.pitch + (size_t)9 * (size_t)src.bpp;
			p[0] = (uint8_t)mark;
			p[1] = (uint8_t)(mark >> 8);
			p[2] = (uint8_t)(mark >> 16);
		}
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 5, -1, 0, -1, 20, 20, angle, rotX, rotY, 0, pd, 0, 0, 1,
				  0, 0) == 0);
		CHECK(s.subX == 0 && s.subY == 0);
		CHECK(RenderBox(&s, &box));
		CHECK(FindColour(&box, mark, &i, &j));
		// the destination pixel that shows source pixel (9, 12) is the one
		// whose inverse falls into its cell: the cell centre projected forward
		ExpectProject(&ex, &ey, 9.0 + 0.5 - 20.0, 12.0 + 0.5 - 20.0, angle, rotX, rotY, 0.0, pd, 1.0, 1.0);
		ex += s.anchorX + s.subX / 65536.0;
		ey += s.anchorY + s.subY / 65536.0;
		printf("  rotX=%d rotY=%d angle=%d: mark (9,12) at (%d,%d), matrix says (%.1f,%.1f) of %dx%d\n",
			(int)(rotX >> 16), (int)(rotY >> 16), (int)(angle >> 16), i, j, ex, ey, box.w, box.h);
		CHECK(i >= 0 && j >= 0 && i < box.w && j < box.h);
		CHECK(fabs((double)i + 0.5 - ex) <= 2.0);
		CHECK(fabs((double)j + 0.5 - ey) <= 2.0);
		CHECK(box.w == s.outW && box.h == s.outH);
		Bmp_Free(&box);
		UnregisterSprite(&s);
	}
}

// ========================================================================
// (d) tiny / degenerate / out-of-range projections are safe
// ========================================================================

static void TestDegenerate(void)
{
	printf("degenerate\n");
	CHECK(MakeUniqueBitmap(6, 1, 1, PM_RGB32, 6));
	CHECK(MakeUniqueBitmap(7, 41, 29, PM_RGB32, 7));
	{
		Sprite_t s;
		uint32_t h = RegisterSprite(&s, 20);
		// a 1 x 1 picture is below the 2 x 2 rule of "90 5C" and "90 5D"
		CHECK(GfxCall_SpriteSetProjected(h, 0, 0, 0, 6, -1, 0, -1, 0, 0, 0, 0, 0, 0, 1, 0, 0) == 8);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 6, -1, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0) == 8);
		// an edge-on picture (a quarter turn about X or Y) has an empty box
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 7, -1, 0, -1, 20, 14, 0, 90 << 16, 0, 0, 300, 0, 0, 1, 0,
				  0) == 8);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 7, -1, 0, -1, 20, 14, 0, 0, 90 << 16, 0, 300, 0, 0, 1, 0,
				  0) == 8);
		// a corner on or behind the eye: projDist + fz + z <= 0
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, -(300 << 16), 7, -1, 0, -1, 20, 14, 0, 0, 20 << 16, 0, 300,
				  0, 0, 1, 0, 0) == 8);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, -(299 << 16), 7, -1, 0, -1, 20, 14, 0, 0, -(20 << 16), 0, 300,
				  0, 0, 1, 0, 0) == 8);
		// a nearly edge-on but valid picture is accepted and renders small
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 7, -1, 0, -1, 20, 14, 0, 0, 80 << 16, 0, 300, 0, 0, 1, 0,
				  0) == 0);
		{
			Bmp_t box;
			CHECK(RenderBox(&s, &box));
			CHECK(box.w <= 10); // 41 * cos(80) ~ 7 columns
			Bmp_Free(&box);
		}
		// a huge magnification: the bounds guard keeps the box finite and the
		// never-rendered giant box does not cost any pixels
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, -(299 << 16), 7, -1, 0, -1, 20, 14, 0, 0, 1 << 16, 0, 300,
				  0, 0, 1, 0, 0) == 0);
		CHECK(s.outW > 0 && s.outH > 0 && s.outW < 2000000 && s.outH < 2000000);
		UnregisterSprite(&s);
	}
	// a projection distance of 0 disables the depth scaling even with rotX/rotY
	{
		Sprite_t s;
		Bmp_t box;
		uint32_t h = RegisterSprite(&s, 21);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 500 << 16, 7, -1, 0, -1, 20, 14, 0, 20 << 16, 15 << 16, 0, 0,
				  0, 0, 1, 0, 0) == 0);
		CHECK(RenderBox(&s, &box));
		Bmp_Free(&box);
		UnregisterSprite(&s);
	}
	// a "90 5D" sprite that goes through Sprite_ChangeBitmap keeps its
	// rotations (it must not fall back to the flat picture)
	{
		Sprite_t s;
		uint32_t h = RegisterSprite(&s, 22);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 7, -1, 0, -1, 20, 14, 0, 15 << 16, 25 << 16, 0, 300, 0,
				  0, 1, 0, 0) == 0);
		CHECK(s.is3D == 1);
		CHECK(GfxCall_SpriteChangeBitmap(h, 7) == 0);
		CHECK(s.is3D == 1 && s.rotX == 15 << 16 && s.rotY == 25 << 16);
		UnregisterSprite(&s);
	}
}

// ========================================================================
// the visual evidence: composites written as BMP
// ========================================================================

static void FillGradient(Bmp_t* canvas)
{
	int x, y;
	for(y = 0; y < canvas->h; y++)
		for(x = 0; x < canvas->w; x++)
		{
			uint8_t* p = canvas->pixels + (size_t)y * (size_t)canvas->pitch + (size_t)x * (size_t)canvas->bpp;
			p[0] = (uint8_t)((x + y) * 30 / (canvas->w + canvas->h) + 22);
			p[1] = (uint8_t)(y * 28 / canvas->h + 12);
			p[2] = (uint8_t)(x * 40 / canvas->w + 18);
		}
}

static void CompositeSprite(Bmp_t* canvas, Sprite_t* s, int x, int y)
{
	Bmp_t view = *canvas;
	Rect_t local;
	if(x < 0 || y < 0 || x + s->outW > canvas->w || y + s->outH > canvas->h)
		return;
	view.pixels += (size_t)y * (size_t)canvas->pitch + (size_t)x * (size_t)canvas->bpp;
	view.w = s->outW;
	view.h = s->outH;
	local.l = 0;
	local.t = 0;
	local.r = s->outW - 1;
	local.b = s->outH - 1;
	s->obj.vt->draw(&s->obj, &view, &local, 0);
}

static void TestVisual(void)
{
	const char* dir = getenv("SPRITE3D_DIR");
	char path[512];
	Bmp_t canvas;
	int i;
	static const struct
	{
		int rotX, rotY, rotZ, angle, fz, projDist, x, y;
	} scene[] = {
		{15 << 16, 0, 0, 0, 0, 500, 16, 26},                // rotX: the top recedes
		{0, 30 << 16, 0, 0, 0, 500, 246, 26},               // rotY: the right side comes forward
		{12 << 16, 25 << 16, 20 << 16, 0, 0, 450, 476, 26}, // all three
		{0, 0, 35 << 16, 35 << 16, 0, 400, 16, 246},        // a Z rotation: the same as 90 5C's angle
		{0, 0, 0, 0, 0, 400, 246, 246},                     // flat at the screen plane
		{0, 0, 0, 0, 400 << 16, 400, 476, 246},             // the same at depth = projDist: half size
	};
	if(!dir || !*dir)
		dir = "../screenshots";
	printf("visual\n");
	CHECK(Bmp_Alloc(&canvas, 640, 480, PM_RGB32));
	FillGradient(&canvas);
	CHECK(MakeUniqueBitmap(8, 140, 100, PM_ARGB32, 8));
	{
		Bmp_t pic;
		CHECK(BmpMgr_GetInfo(gMgr, &pic, 8));
		FillChecker(&pic);
	}
	for(i = 0; i < (int)(sizeof scene / sizeof scene[0]); i++)
	{
		Sprite_t s;
		uint32_t h = RegisterSprite(&s, 30 + i);
		int r = GfxCall_SpriteSetProjected3D(h, 0, 0, scene[i].fz, 8, -1, 0, -1, 70, 50, scene[i].angle, scene[i].rotX,
			scene[i].rotY, scene[i].rotZ, scene[i].projDist, 0, 1, 1, 0, 0x100);
		printf("  scene %d: rotX=%d rotY=%d rotZ=%d angle=%d fz=%d -> %d (%dx%d)\n", i, (int)(scene[i].rotX >> 16),
			(int)(scene[i].rotY >> 16), (int)(scene[i].rotZ >> 16), (int)(scene[i].angle >> 16),
			(int)(scene[i].fz >> 16), r, s.outW, s.outH);
		CHECK(r == 0);
		if(r == 0)
			CompositeSprite(&canvas, &s, scene[i].x, scene[i].y);
		UnregisterSprite(&s);
	}
	snprintf(path, sizeof path, "%s/sprite3d_composite.bmp", dir);
	printf("  writing %s\n", path);
	CHECK(WriteBmp24(path, &canvas));
	Bmp_Free(&canvas);

	// a depth comparison: the same picture at the screen plane and at depth
	// projDist (half size), side by side
	{
		Bmp_t out;
		Sprite_t s1, s2;
		uint32_t h1 = RegisterSprite(&s1, 41), h2 = RegisterSprite(&s2, 42);
		CHECK(Bmp_Alloc(&out, 640, 320, PM_RGB32));
		FillGradient(&out);
		CHECK(MakeUniqueBitmap(9, 160, 110, PM_ARGB32, 9));
		{
			Bmp_t pic;
			CHECK(BmpMgr_GetInfo(gMgr, &pic, 9));
			FillChecker(&pic);
		}
		CHECK(GfxCall_SpriteSetProjected3D(h1, 0, 0, 0, 9, -1, 0, -1, 80, 55, 0, 0, 0, 0, 400, 0, 1, 1, 0,
				  0x100) == 0);
		CHECK(GfxCall_SpriteSetProjected3D(h2, 0, 0, 400 << 16, 9, -1, 0, -1, 80, 55, 0, 0, 0, 0, 400, 0, 1, 1, 0,
				  0x100) == 0);
		CompositeSprite(&out, &s1, 40, (320 - s1.outH) / 2);
		CompositeSprite(&out, &s2, 360, (320 - s2.outH) / 2);
		UnregisterSprite(&s1);
		UnregisterSprite(&s2);
		snprintf(path, sizeof path, "%s/sprite3d_perspective.bmp", dir);
		printf("  writing %s (near %dx%d, far %dx%d)\n", path, s1.outW, s1.outH, s2.outW, s2.outH);
		CHECK(WriteBmp24(path, &out));
		Bmp_Free(&out);
	}

	// a close-up of a 42 degree Y turn at a short projection distance: the
	// left side is far and small, the right side near and large
	{
		Bmp_t out;
		Sprite_t s;
		uint32_t h;
		CHECK(Bmp_Alloc(&out, 640, 320, PM_RGB32));
		FillGradient(&out);
		CHECK(MakeUniqueBitmap(10, 200, 140, PM_ARGB32, 10));
		{
			Bmp_t pic;
			CHECK(BmpMgr_GetInfo(gMgr, &pic, 10));
			FillChecker(&pic);
		}
		h = RegisterSprite(&s, 43);
		CHECK(GfxCall_SpriteSetProjected3D(h, 0, 0, 0, 10, -1, 0, -1, 100, 70, 0, 22 << 16, 42 << 16, 0, 300, 0,
				  1, 1, 0, 0x100) == 0);
		CompositeSprite(&out, &s, (640 - s.outW) / 2, (320 - s.outH) / 2);
		UnregisterSprite(&s);
		snprintf(path, sizeof path, "%s/sprite3d_rotation.bmp", dir);
		printf("  writing %s (%dx%d)\n", path, s.outW, s.outH);
		CHECK(WriteBmp24(path, &out));
		Bmp_Free(&out);
	}
}


// ========================================================================
// the blit fast paths of src/gfx/blit.c (macOS/arm64): exactness and speed
// ========================================================================

/* The blend and tint kernels of the 32-bit blits have a NEON path on the
 * ARM machines (four pixels at a time) and keep the scalar code for the
 * tail / every other target.  These checks compare the public entry points
 * with a copy of the scalar definition over odd sizes and levels; the
 * benchmark behind SPRITE3D_BENCH=1 (default off) times the two. */

static uint32_t Rand32(uint32_t* st)
{
	*st = *st * 1664525u + 1013904223u;
	return *st;
}

static void FillRandom(Bmp_t* b, uint32_t seed)
{
	int x, y;
	uint32_t st = seed;
	for(y = 0; y < b->h; y++)
		for(x = 0; x < b->w; x++)
		{
			uint8_t* p = b->pixels + (size_t)y * (size_t)b->pitch + (size_t)x * (size_t)b->bpp;
			uint32_t v = Rand32(&st);
			p[0] = (uint8_t)v;
			p[1] = (uint8_t)(v >> 8);
			p[2] = (uint8_t)(v >> 16);
			p[3] = (uint8_t)(v >> 24);
		}
}

static inline uint8_t RefSat8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255
										 : v);
}

// the scalar definition of Blend32_Plain (src/gfx/blit.c as it was before the NEON path)
static void RefBlend32Plain(Bmp_t* dst, const Bmp_t* src, int level)
{
	int l2 = level >> 1, y, x;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)src->pixels + (size_t)y * (size_t)src->pitch / 4;
		uint32_t* d = (uint32_t*)dst->pixels + (size_t)y * (size_t)dst->pitch / 4;
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x], dv = d[x];
			int i, out = (int)(sv & 0xff000000u);
			for(i = 0; i < 24; i += 8)
			{
				int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
				out |= RefSat8((((dc - sc) * l2) >> 7) + sc) << i;
			}
			d[x] = (uint32_t)out;
		}
	}
}

// the scalar definition of Tint32 (and of Dim32 with a black colour)
static void RefTint32(Bmp_t* dst, const Bmp_t* src, uint32_t colour, int level)
{
	int inv = 0x100 - level, y, x;
	int cb = (int)((colour & 0xff) * (unsigned)level) >> 8;
	int cg = (int)(((colour >> 8) & 0xff) * (unsigned)level) >> 8;
	int cr = (int)(((colour >> 16) & 0xff) * (unsigned)level) >> 8;
	for(y = 0; y < src->h; y++)
	{
		const uint32_t* s = (const uint32_t*)src->pixels + (size_t)y * (size_t)src->pitch / 4;
		uint32_t* d = (uint32_t*)dst->pixels + (size_t)y * (size_t)dst->pitch / 4;
		for(x = 0; x < src->w; x++)
		{
			uint32_t sv = s[x];
			d[x] = (uint32_t)RefSat8((int)(((sv & 0xff) * (unsigned)inv) >> 8) + cb) |
				   (uint32_t)RefSat8((int)((((sv >> 8) & 0xff) * (unsigned)inv) >> 8) + cg) << 8 |
				   (uint32_t)RefSat8((int)((((sv >> 16) & 0xff) * (unsigned)inv) >> 8) + cr) << 16;
		}
	}
}

static int SameView(const Bmp_t* a, const Bmp_t* b)
{
	int y;
	for(y = 0; y < a->h; y++)
		if(memcmp(a->pixels + (size_t)y * (size_t)a->pitch, b->pixels + (size_t)y * (size_t)b->pitch,
			   (size_t)a->w * 4) != 0)
			return 0;
	return 1;
}

static void TestBlitFastPaths(void)
{
	static const int widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 101};
	static const int levels[] = {1, 2, 3, 37, 64, 127, 128, 200, 255};
	static const uint32_t colours[] = {0, 0x00ffffffu, 0x00123456u, 0x00abcdefu};
	int iw, ih, il, ic;
	int bad = 0;
	printf("blit fast path\n");
	for(iw = 0; iw < (int)(sizeof widths / sizeof widths[0]); iw++)
		for(ih = 1; ih <= 3; ih++)
			for(il = 0; il < (int)(sizeof levels / sizeof levels[0]); il++)
			{
				int w = widths[iw], hgt = ih, level = levels[il];
				Bmp_t src, got, want;
				if(!Bmp_Alloc(&src, w, hgt, PM_RGB32) || !Bmp_Alloc(&got, w, hgt, PM_RGB32) ||
					!Bmp_Alloc(&want, w, hgt, PM_RGB32))
				{
					CHECK(0);
					continue;
				}
				FillRandom(&src, 0x1234567u + (uint32_t)w * 977u + (uint32_t)hgt);
				FillRandom(&want, 0x89abcdefu + (uint32_t)level);
				Bmp_CopyRect(&got, &want);
				RefBlend32Plain(&want, &src, level);
				Blit_Blend(&got, &src, level);
				if(!SameView(&got, &want))
				{
					if(bad++ < 6)
						printf("  blend differs: w=%d h=%d level=%d\n", w, hgt, level);
					CHECK(0);
				}
				// effect 0xF0 uses the same kernel
				Bmp_CopyRect(&got, &want); // (want now holds the blend result: rebuild from scratch)
				FillRandom(&want, 0x89abcdefu + (uint32_t)level);
				Bmp_CopyRect(&got, &want);
				RefBlend32Plain(&want, &src, level);
				Blit_Over(&got, &src, level);
				if(!SameView(&got, &want))
				{
					if(bad++ < 6)
						printf("  over differs: w=%d h=%d level=%d\n", w, hgt, level);
					CHECK(0);
				}
				Bmp_Free(&src);
				Bmp_Free(&got);
				Bmp_Free(&want);
			}
	// the tint / dim kernels: the scalar definition of Tint32 covers Dim32
	// with a black colour
	for(iw = 0; iw < (int)(sizeof widths / sizeof widths[0]); iw++)
		for(ic = 0; ic < (int)(sizeof colours / sizeof colours[0]); ic++)
			for(il = 0; il < (int)(sizeof levels / sizeof levels[0]); il++)
			{
				int w = widths[iw], hgt = 1 + (ic & 1), level = levels[il];
				uint32_t colour = colours[ic];
				Bmp_t src, got, want;
				if(!Bmp_Alloc(&src, w, hgt, PM_RGB32) || !Bmp_Alloc(&got, w, hgt, PM_RGB32) ||
					!Bmp_Alloc(&want, w, hgt, PM_RGB32))
				{
					CHECK(0);
					continue;
				}
				FillRandom(&src, 0xdeadbeefu + (uint32_t)w * 31u + (uint32_t)colour);
				FillRandom(&got, 0x13579bdfu + (uint32_t)level);
				Bmp_CopyRect(&want, &got);
				RefTint32(&want, &src, colour, level);
				Blit_Tint(&got, &src, colour, level);
				if(!SameView(&got, &want))
				{
					if(bad++ < 6)
						printf("  tint differs: w=%d level=%d colour=%06x\n", w, level, colour);
					CHECK(0);
				}
				Bmp_Free(&src);
				Bmp_Free(&got);
				Bmp_Free(&want);
			}
	{
		// level 0x100 is the fully transparent / fully tinted end
		Bmp_t src, got, want;
		int level = 0x100;
		uint32_t colour = 0x00123456u;
		CHECK(Bmp_Alloc(&src, 37, 3, PM_RGB32) && Bmp_Alloc(&got, 37, 3, PM_RGB32) &&
			  Bmp_Alloc(&want, 37, 3, PM_RGB32));
		FillRandom(&src, 7);
		FillRandom(&got, 9);
		Bmp_CopyRect(&want, &got);
		RefTint32(&want, &src, colour, level);
		Blit_Tint(&got, &src, colour, level);
		CHECK(SameView(&got, &want));
		Bmp_Free(&src);
		Bmp_Free(&got);
		Bmp_Free(&want);
	}
	printf("  %d differing kernel case(s)\n", bad);
}

// SPRITE3D_BENCH=1: time the kernels on a full-HD picture (default off)
static double NowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void BenchBlit(void)
{
	Bmp_t src, dst;
	const int w = 1920, h = 1080, iters = 20;
	double t0, t1, sBlend, sRef, sTint, tRef;
	int i;
	if(!getenv("SPRITE3D_BENCH"))
		return;
	printf("blit benchmark (1920x1080, %d iterations)\n", iters);
	CHECK(Bmp_Alloc(&src, w, h, PM_RGB32) && Bmp_Alloc(&dst, w, h, PM_RGB32));
	FillRandom(&src, 1);
	FillRandom(&dst, 2);

	t0 = NowMs();
	for(i = 0; i < iters; i++)
		Blit_Blend(&dst, &src, 0x80);
	t1 = NowMs();
	sBlend = t1 - t0;

	Bmp_CopyRect(&dst, &src);
	t0 = NowMs();
	for(i = 0; i < iters; i++)
		RefBlend32Plain(&dst, &src, 0x80);
	t1 = NowMs();
	sRef = t1 - t0;

	t0 = NowMs();
	for(i = 0; i < iters; i++)
		Blit_Tint(&dst, &src, 0x00123456u, 0x40);
	t1 = NowMs();
	sTint = t1 - t0;

	t0 = NowMs();
	for(i = 0; i < iters; i++)
		RefTint32(&dst, &src, 0x00123456u, 0x40);
	t1 = NowMs();
	tRef = t1 - t0;

	printf("  blend: engine %.1f ms  scalar %.1f ms  speedup %.2fx (%.0f MB/s vs %.0f MB/s)\n", sBlend, sRef,
		sRef / sBlend, (double)w * h * 8 * iters / sBlend / 1000.0, (double)w * h * 8 * iters / sRef / 1000.0);
	printf("  tint:  engine %.1f ms  scalar %.1f ms  speedup %.2fx\n", sTint, tRef, tRef / sTint);
	CHECK(sBlend > 0 && sTint > 0);
	Bmp_Free(&src);
	Bmp_Free(&dst);
}

// ========================================================================
// main
// ========================================================================

int main(void)
{
	printf("test_sprite_3d\n");
	Engine_SelectProfile("1.69/472"); // the generation that has "90 5D"
	SetupGfx();

	TestFixtures();
	TestRotZeroMatches5C();
	TestRotZ();
	TestPerspScale();
	TestRotXY();
	TestDegenerate();
	TestVisual();
	TestBlitFastPaths();
	BenchBlit();

	TeardownGfx();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
