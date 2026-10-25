/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * ResampleImage_Soft.cpp - TVPResampleImage for the software back end
 *
 * The engine stretches a picture when a layer is scaled or a transition runs
 * (visual/gl/ResampleImage.cpp is the SIMD one, built on cocos2d's allocator).
 * A software back end can only promise nearest neighbour without the lattice
 * machinery, so that is what this does: the caller's rectangle is sampled
 * point for point and written through the destination's scan lines.
 */

#include "tjsCommHead.h"
#include "LayerBitmapIntf.h"
#include "gl/ResampleImage.h"

#include <cstring>

void TVPResampleImage(const tTVPRect& cliprect, iTVPBaseBitmap* dest, const tTVPRect& destrect,
	const iTVPBaseBitmap* src, const tTVPRect& srcrect, tTVPBBStretchType type, tjs_real typeopt,
	tTVPBBBltMethod method, tjs_int opa, bool hda)
{
	(void)type; // nearest neighbour regardless of the requested filter
	(void)typeopt;
	(void)method;
	(void)opa;
	(void)hda;
	if(dest == nullptr || src == nullptr)
		return;
	int dw = destrect.right - destrect.left + 1;
	int dh = destrect.bottom - destrect.top + 1;
	int sw = srcrect.right - srcrect.left + 1;
	int sh = srcrect.bottom - srcrect.top + 1;
	if(dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
		return;
	for(int y = 0; y < dh; y++)
	{
		int dy = destrect.top + y;
		if(dy < cliprect.top || dy > cliprect.bottom)
			continue;
		void* d = dest->GetScanLineForWrite(dy);
		if(d == nullptr)
			continue;
		int sy = srcrect.top + (int)((int64_t)y * sh / dh);
		const void* s = src->GetScanLine(sy);
		if(s == nullptr)
			continue;
		tjs_uint32* dst = (tjs_uint32*)d + destrect.left;
		const tjs_uint32* srcline = (const tjs_uint32*)s;
		for(int x = 0; x < dw; x++)
		{
			int dx = destrect.left + x;
			if(dx < cliprect.left || dx > cliprect.right)
				continue;
			int sx = srcrect.left + (int)((int64_t)x * sw / dw);
			dst[x] = srcline[sx];
		}
	}
}
