/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/blit_neon.c - the ARM NEON paths of the 32-bit blit kernels against
 *                     their scalar definitions (src/gfx/blit.c, declared in
 *                     inc/bgi/gfx/bitmap.h); built as bin/test_blit_neon by
 *                     `make test`
 *
 * On an arm64 build the kernels Blit_AlphaCopy (ARGB32 -> RGB32), Blit_Blend
 * and Blit_Tint run sixteen (or four) pixels at a time with NEON and the
 * scalar loop for the tail.  A fast path is only worth having when it is
 * bit-exact, so every case is run twice - Blit_SetNeon(1) and (0) - and the
 * two results are compared byte for byte; the widths sweep the vector width
 * (1 .. 17, 31 .. 33, 100) so that a tail-only run, a head-only run and a
 * mixed run are all covered.  The alpha copy and the plain blend also have an
 * independent scalar reference here, so a mistake shared by both paths would
 * still be caught.  On a target without NEON the two passes are the same code
 * and the test is a reference check only; it still passes.
 */
#include "bgi/gfx/bitmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFails;
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// a deterministic pixel: alpha from `alphaOf`, colour from the position and salt
static uint32_t MakePixel(int x, int y, int salt, int alpha)
{
	uint32_t b = (uint32_t)(x * 37 + y * 11 + salt * 53) & 0xffu;
	uint32_t g = (uint32_t)(x * 17 + y * 29 + salt * 7) & 0xffu;
	uint32_t r = (uint32_t)(x * 5 + y * 61 + salt * 3) & 0xffu;
	return ((uint32_t)alpha << 24) | (r << 16) | (g << 8) | b;
}

// fill an ARGB32 picture; `alpha` is either one value for the whole picture or -1 for a sweep
static void FillArgb(Bmp_t* b, int salt, int alpha)
{
	int x, y;
	for(y = 0; y < b->h; y++)
	{
		uint32_t* row = (uint32_t*)(b->pixels + (size_t)y * b->pitch);
		for(x = 0; x < b->w; x++)
		{
			int a = alpha;
			if(a < 0)
			{
				static const int sweep[] = {0, 1, 2, 3, 4, 7, 63, 127, 128, 129, 200, 253, 254, 255};
				a = sweep[(x + y * 3 + salt) % (int)(sizeof sweep / sizeof sweep[0])];
			}
			row[x] = MakePixel(x, y, salt, a);
		}
	}
}

// fill an RGB32 picture
static void FillRgb(Bmp_t* b, int salt)
{
	int x, y;
	for(y = 0; y < b->h; y++)
	{
		uint32_t* row = (uint32_t*)(b->pixels + (size_t)y * b->pitch);
		for(x = 0; x < b->w; x++)
			row[x] = MakePixel(x, y, salt, 0) & 0x00ffffffu;
	}
}

// the scalar definition of the ARGB32 -> RGB32 alpha copy (src/gfx/blit.c)
static uint32_t RefAlphaCopy(uint32_t sv, uint32_t dv)
{
	int i;
	uint32_t out = dv & 0xff000000u;
	int a = (int)(sv >> 25);
	if(!(sv & 0xfe000000u))
		return dv;
	for(i = 0; i < 24; i += 8)
	{
		int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
		int v = (((sc - dc) * a) >> 7) + dc;
		out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << i;
	}
	return out;
}

// the scalar definition of the plain blend (Blend32_Plain in src/gfx/blit.c)
static uint32_t RefBlend(uint32_t sv, uint32_t dv, int level)
{
	int i;
	uint32_t out = sv & 0xff000000u;
	int l2 = level >> 1;
	for(i = 0; i < 24; i += 8)
	{
		int sc = (int)((sv >> i) & 0xff), dc = (int)((dv >> i) & 0xff);
		int v = sc + (((dc - sc) * l2) >> 7);
		out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << i;
	}
	return out;
}

// run one kernel over a fresh pair of pictures, NEON on or off, into `dstPixels`
static void RunAlphaCopy(int w, int h, int salt, int alpha, int neon, uint32_t* dstPixels)
{
	Bmp_t s, d;
	Bmp_Alloc(&s, w, h, PM_ARGB32);
	Bmp_Alloc(&d, w, h, PM_RGB32);
	FillArgb(&s, salt, alpha);
	FillRgb(&d, salt + 1);
	Blit_SetNeon(neon);
	Blit_AlphaCopy(&d, &s);
	memcpy(dstPixels, d.pixels, (size_t)w * h * 4);
	Blit_SetNeon(-1);
	Bmp_Free(&s);
	Bmp_Free(&d);
}

static void RunBlend(int w, int h, int salt, int level, int neon, uint32_t* dstPixels)
{
	Bmp_t s, d;
	Bmp_Alloc(&s, w, h, PM_RGB32);
	Bmp_Alloc(&d, w, h, PM_RGB32);
	FillRgb(&s, salt);
	FillRgb(&d, salt + 1);
	Blit_SetNeon(neon);
	Blit_Blend(&d, &s, level);
	memcpy(dstPixels, d.pixels, (size_t)w * h * 4);
	Blit_SetNeon(-1);
	Bmp_Free(&s);
	Bmp_Free(&d);
}

static void RunTint(int w, int h, int salt, uint32_t colour, int level, int neon, uint32_t* dstPixels)
{
	Bmp_t s, d;
	Bmp_Alloc(&s, w, h, PM_RGB32);
	Bmp_Alloc(&d, w, h, PM_RGB32);
	FillRgb(&s, salt);
	FillRgb(&d, salt + 1);
	Blit_SetNeon(neon);
	Blit_Tint(&d, &s, colour, level);
	memcpy(dstPixels, d.pixels, (size_t)w * h * 4);
	Blit_SetNeon(-1);
	Bmp_Free(&s);
	Bmp_Free(&d);
}

static void TestAlphaCopy(void)
{
	static const int widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 100};
	static const int alphas[] = {-1, 0, 1, 2, 3, 64, 127, 128, 254, 255};
	static const int heights[] = {1, 3};
	size_t wi, ai, hi;
	printf("alpha copy\n");
	for(hi = 0; hi < sizeof heights / sizeof heights[0]; hi++)
		for(wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
			for(ai = 0; ai < sizeof alphas / sizeof alphas[0]; ai++)
			{
				int w = widths[wi], h = heights[hi], salt = (int)(wi * 7 + ai);
				size_t n = (size_t)w * (size_t)h;
				uint32_t* fast = (uint32_t*)malloc(n * 4);
				uint32_t* slow = (uint32_t*)malloc(n * 4);
				Bmp_t s, d;
				int x, y;
				if(fast == NULL || slow == NULL)
				{
					free(fast);
					free(slow);
					CHECK(!"out of memory");
					return;
				}
				RunAlphaCopy(w, h, salt, alphas[ai], 1, fast);
				RunAlphaCopy(w, h, salt, alphas[ai], 0, slow);
				CHECK(memcmp(fast, slow, n * 4) == 0);
				// and both against the scalar definition, pixel by pixel
				Bmp_Alloc(&s, w, h, PM_ARGB32);
				Bmp_Alloc(&d, w, h, PM_RGB32);
				FillArgb(&s, salt, alphas[ai]);
				FillRgb(&d, salt + 1);
				for(y = 0; y < h; y++)
				{
					const uint32_t* sr = (const uint32_t*)(s.pixels + (size_t)y * s.pitch);
					const uint32_t* dr = (const uint32_t*)(d.pixels + (size_t)y * d.pitch);
					for(x = 0; x < w; x++)
					{
						uint32_t want = RefAlphaCopy(sr[x], dr[x]);
						CHECK(fast[y * w + x] == want);
						CHECK(slow[y * w + x] == want);
					}
				}
				Bmp_Free(&s);
				Bmp_Free(&d);
				free(fast);
				free(slow);
			}
}

static void TestBlend(void)
{
	static const int widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 33, 100};
	static const int levels[] = {0, 1, 2, 3, 63, 64, 127, 128, 129, 200, 255, 256};
	size_t wi, li;
	printf("blend\n");
	for(wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
		for(li = 0; li < sizeof levels / sizeof levels[0]; li++)
		{
			int w = widths[wi], h = 3, salt = (int)(wi + li);
			size_t n = (size_t)w * (size_t)h;
			uint32_t* fast = (uint32_t*)malloc(n * 4);
			uint32_t* slow = (uint32_t*)malloc(n * 4);
			Bmp_t s, d;
			int x, y;
			if(fast == NULL || slow == NULL)
			{
				free(fast);
				free(slow);
				CHECK(!"out of memory");
				return;
			}
			RunBlend(w, h, salt, levels[li], 1, fast);
			RunBlend(w, h, salt, levels[li], 0, slow);
			CHECK(memcmp(fast, slow, n * 4) == 0);
			Bmp_Alloc(&s, w, h, PM_RGB32);
			Bmp_Alloc(&d, w, h, PM_RGB32);
			FillRgb(&s, salt);
			FillRgb(&d, salt + 1);
			for(y = 0; y < h; y++)
			{
				const uint32_t* sr = (const uint32_t*)(s.pixels + (size_t)y * s.pitch);
				const uint32_t* dr = (const uint32_t*)(d.pixels + (size_t)y * d.pitch);
				for(x = 0; x < w; x++)
				{
					uint32_t want = RefBlend(sr[x], dr[x], levels[li]);
					CHECK(fast[y * w + x] == want);
					CHECK(slow[y * w + x] == want);
				}
			}
			Bmp_Free(&s);
			Bmp_Free(&d);
			free(fast);
			free(slow);
		}
}

static void TestTint(void)
{
	static const int widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 33, 100};
	static const uint32_t colours[] = {0x000000u, 0xffffffu, 0x123456u, 0x80ff10u, 0x010203u};
	static const int levels[] = {0, 1, 2, 63, 127, 128, 200, 255, 256};
	size_t wi, ci, li;
	printf("tint\n");
	for(wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
		for(ci = 0; ci < sizeof colours / sizeof colours[0]; ci++)
			for(li = 0; li < sizeof levels / sizeof levels[0]; li++)
			{
				int w = widths[wi], h = 2, salt = (int)(wi + ci + li);
				size_t n = (size_t)w * (size_t)h;
				uint32_t* fast = (uint32_t*)malloc(n * 4);
				uint32_t* slow = (uint32_t*)malloc(n * 4);
				if(fast == NULL || slow == NULL)
				{
					free(fast);
					free(slow);
					CHECK(!"out of memory");
					return;
				}
				RunTint(w, h, salt, colours[ci], levels[li], 1, fast);
				RunTint(w, h, salt, colours[ci], levels[li], 0, slow);
				CHECK(memcmp(fast, slow, n * 4) == 0);
				free(fast);
				free(slow);
			}
}

int main(void)
{
	printf("blit neon\n");
	TestAlphaCopy();
	TestBlend();
	TestTint();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
