/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_persp.c - the perspective blitter of the 3-D projected sprite
 *                ("90 5D", 1.588 on); interface in inc/bgi/gfx/sprite.h
 *
 * "90 5C" places a picture with a plane transform (rotate + scale) and a
 * perspective factor taken from its depth alone, which stays an affine map
 * (blit_xform.c).  "90 5D" adds rotations about the X and Y axes: the
 * picture plane is turned in 3-D, its four projected corners no longer form
 * a parallelogram and the inverse map is a homography.  The Z rotation of
 * "90 5D" commutes with the projection (the projection denominator depends
 * only on z), so it is exactly the plane rotation of "90 5C" and is added to
 * the angle in src/gfx/sprite.c instead of reaching this file.
 *
 * The model (angles 16.16 degrees; the picture's y axis points down, +z
 * points away from the viewer, the origin is the sprite's origin):
 *
 *   (u, v)                  a picture pixel relative to the origin, in
 *                           pixels, already multiplied by the animated
 *                           scale and zoom
 *   after rotX and rotY:    x2 =  u*cos(rotY) + v*sin(rotX)*sin(rotY)
 *                           y2 =  v*cos(rotX)
 *                           z  = -u*sin(rotY) + v*sin(rotX)*cos(rotY)
 *   perspective:            k  = projDist / (projDist + fz + z)
 *                               (1 when projDist is 0, as ProjectScale)
 *   plane rotation:         dx = cos(angle)*k*x2 + sin(angle)*k*y2
 *                           dy = -sin(angle)*k*x2 + cos(angle)*k*y2
 *
 * with `angle` the plane rotation of "90 5C" (+ rotZ).  This is a 3 x 3
 * homography from (u, v, 1) to (dx*w, dy*w, w); the blitter inverts it once
 * and walks the destination rectangle with two divisions per pixel.  The
 * samples are taken nearest or with the same 4-bit bilinear weights as the
 * affine blitters, and the destination blend / level rules are those of
 * Blit_Xform / Blit_XformCopy so that the sprite's mask, effect and level
 * paths of mode 5 work unchanged.
 *
 * A picture with a corner on or behind the eye (projDist + fz + z <= 0), or
 * a matrix that is singular (a picture turned exactly edge-on), has no
 * bounded projection and draws nothing; Sprite_SetProjected3D reports the
 * resulting empty box instead, like the "90 5C" 2 x 2 rule.
 */
#include <math.h>
#include <string.h>

#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/sprite.h"

#define DEG16_TO_RAD 2.663161090079238e-07 // 2*pi / (360 << 16)
#define INV_65536    1.52587890625e-05     // 1 / 65536

#define ROW(b, y)      ((b)->pixels + (size_t)(y) * (size_t)(b)->pitch)
#define PIX32(b, x, y) (*(const uint32_t*)(ROW(b, y) + (size_t)(x) * 4))

// pack a lane to a byte with unsigned saturation (packuswb)
static inline uint8_t Sat8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255
										 : v);
}

// ========================================================================
// the homography
// ========================================================================

/* The forward map, homogeneous: (u, v, 1) -> (a u + b v + c, d u + e v + f,
 * g u + h v + i), i.e. dx = Xh / Wh, dy = Yh / Wh, and its inverse, which
 * maps a destination offset (dx, dy) from the origin to the picture offset
 * (u, v).  All in doubles: the transform of "90 5C" is likewise set up in
 * doubles in blit_xform.c. */
typedef struct Persp
{
	double a, b, c;
	double d, e, f;
	double g, h, i;
	double inv[9]; // destination (dx, dy, 1) -> picture (u', v', w'), u = u'/w'
	int valid;
} Persp_t;

// project a picture offset (u, v), in pixels relative to the origin; 0 when the point is on or behind the eye
static int Persp_Forward(const Persp_t* p, double u, double v, double* dx, double* dy)
{
	double w = p->g * u + p->h * v + p->i;
	if(!(w > 1e-6))
		return 0;
	*dx = (p->a * u + p->b * v + p->c) / w;
	*dy = (p->d * u + p->e * v + p->f) / w;
	return 1;
}

/* Set up the homography and its inverse for the picture of w x h pixels
 * whose origin is at (cx16, cy16) (16.16), the plane rotation `angle` and
 * the two 3-D rotations, the depth fz16 (16.16) and the projection distance
 * `projDist` (pixels, 0: no depth scaling).  `sx` / `sy` (16.16) are the
 * animated scale and zoom of the picture plane - the depth of the plane is
 * scaled with it, so the factors are folded into the matrix columns. */
static void Persp_Setup(Persp_t* p, int w, int h, int32_t cx16, int32_t cy16, int32_t angle,
	int32_t rotX, int32_t rotY, int32_t fz16, int projDist, int32_t sx, int32_t sy)
{
	double A = (double)angle * DEG16_TO_RAD;
	double X = (double)rotX * DEG16_TO_RAD;
	double Y = (double)rotY * DEG16_TO_RAD;
	double cA = cos(A), sA = sin(A);
	double cX = cos(X), sX = sin(X);
	double cY = cos(Y), sY = sin(Y);
	double sxP = (double)(uint32_t)sx * INV_65536;
	double syP = (double)(uint32_t)sy * INV_65536;
	double pd = (double)projDist;
	double f = (double)fz16 * INV_65536;
	double det, u0, u1, v0, v1, w00, w01, w10, w11;
	double cx = (double)cx16 * INV_65536, cy = (double)cy16 * INV_65536;
	int i;

	memset(p, 0, sizeof *p);
	if(sx == 0 || sy == 0)
		return;
	if(pd == 0.0)
	{
		// no depth scaling: the projection is the plane rotation alone
		p->a = cA * cY;
		p->b = cA * sX * sY + sA * cX;
		p->d = -sA * cY;
		p->e = -sA * sX * sY + cA * cX;
		p->g = 0.0;
		p->h = 0.0;
		p->i = 1.0;
	}
	else
	{
		p->a = pd * cA * cY;
		p->b = pd * (cA * sX * sY + sA * cX);
		p->d = -pd * sA * cY;
		p->e = pd * (-sA * sX * sY + cA * cX);
		p->g = -sY;
		p->h = sX * cY;
		p->i = pd + f;
	}
	// the picture plane is scaled before the rotation: scale the u and v columns
	p->a *= sxP;
	p->d *= sxP;
	p->g *= sxP;
	p->b *= syP;
	p->e *= syP;
	p->h *= syP;

	// a corner on or behind the eye: the projected picture is not bounded
	u0 = -cx;
	u1 = (double)w - cx;
	v0 = -cy;
	v1 = (double)h - cy;
	w00 = p->g * u0 + p->h * v0 + p->i;
	w01 = p->g * u0 + p->h * v1 + p->i;
	w10 = p->g * u1 + p->h * v0 + p->i;
	w11 = p->g * u1 + p->h * v1 + p->i;
	if(!(w00 > 1e-6 && w01 > 1e-6 && w10 > 1e-6 && w11 > 1e-6))
		return;

	det = p->a * (p->e * p->i - p->f * p->h) - p->b * (p->d * p->i - p->f * p->g) +
		  p->c * (p->d * p->h - p->e * p->g);
	if(!(fabs(det) > 1e-9) || !isfinite(det))
		return;
	p->inv[0] = (p->e * p->i - p->f * p->h) / det;
	p->inv[1] = (p->c * p->h - p->b * p->i) / det;
	p->inv[2] = (p->b * p->f - p->c * p->e) / det;
	p->inv[3] = (p->f * p->g - p->d * p->i) / det;
	p->inv[4] = (p->a * p->i - p->c * p->g) / det;
	p->inv[5] = (p->c * p->d - p->a * p->f) / det;
	p->inv[6] = (p->d * p->h - p->e * p->g) / det;
	p->inv[7] = (p->b * p->g - p->a * p->h) / det;
	p->inv[8] = (p->a * p->e - p->b * p->d) / det;
	for(i = 0; i < 9; i++)
		if(!isfinite(p->inv[i]))
			return;
	p->valid = 1;
}

/* The axis-aligned box of the projected picture: its width and height, and
 * `anchor`, where the origin lands inside the box (so the box is drawn at
 * position - anchor).  `sx` / `sy` are the plane scale (16.16), `subX` /
 * `subY` the 16.16 fraction of the projected position that shifts the
 * drawing right and down (the box grows by up to a pixel, as in
 * Sprite_BoundsProjected).  0 when the projection is empty or unbounded. */
int Blit_PerspBounds(int32_t* outW, int32_t* outH, int32_t anchor[2], int w, int h, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t rotX, int32_t rotY, int32_t sx, int32_t sy, int32_t fz16, int projDist,
	int32_t subX, int32_t subY)
{
	Persp_t p;
	double cx = (double)cx16 * INV_65536, cy = (double)cy16 * INV_65536;
	double fracX = (double)(uint32_t)subX * INV_65536;
	double fracY = (double)(uint32_t)subY * INV_65536;
	double u[4], v[4], dx[4], dy[4];
	double minX, maxX, minY, maxY;
	int32_t x0, x1, y0, y1;
	int i;

	*outW = *outH = 0;
	anchor[0] = anchor[1] = 0;
	Persp_Setup(&p, w, h, cx16, cy16, angle, rotX, rotY, fz16, projDist, sx, sy);
	if(!p.valid)
		return 0;

	u[0] = -cx;
	v[0] = -cy;
	u[1] = (double)w - cx;
	v[1] = -cy;
	u[2] = -cx;
	v[2] = (double)h - cy;
	u[3] = (double)w - cx;
	v[3] = (double)h - cy;
	for(i = 0; i < 4; i++)
		if(!Persp_Forward(&p, u[i], v[i], &dx[i], &dy[i]))
			return 0;

	minX = maxX = dx[0];
	minY = maxY = dy[0];
	for(i = 1; i < 4; i++)
	{
		if(dx[i] < minX)
			minX = dx[i];
		if(dx[i] > maxX)
			maxX = dx[i];
		if(dy[i] < minY)
			minY = dy[i];
		if(dy[i] > maxY)
			maxY = dy[i];
	}
	if(!(minX > -1e6 && maxX < 1e6 && minY > -1e6 && maxY < 1e6))
		return 0;

	// the 0.0001 keeps a corner exactly on a pixel boundary from adding a
	// pixel; the sub-pixel remainder of the position can add one at the far
	// side (the leading side is covered by the floor of the minimum)
	x0 = (int32_t)floor(minX);
	x1 = (int32_t)floor(maxX - 0.0001 + fracX);
	y0 = (int32_t)floor(minY);
	y1 = (int32_t)floor(maxY - 0.0001 + fracY);
	*outW = x1 - x0 + 1;
	*outH = y1 - y0 + 1;
	anchor[0] = -x0;
	anchor[1] = -y0;
	return 1;
}

// ========================================================================
// sampling and the two blitters
// ========================================================================

/* 4-bit bilinear sample of a 32-bit picture at the 16.16 position (px, py);
 * `lanes` receives B, G, R, A.  The neighbourhood and the edge rules are
 * those of Xform_Sample in blit_xform.c: a part of -2 and below, or at or
 * beyond the far edge, is black / transparent; -1 and the last index blend
 * with black. */
static void Persp_Sample(const Bmp_t* src, int32_t px, int32_t py, int lanes[4])
{
	int x = px >> 16, y = py >> 16;
	int fx = (px >> 12) & 15, fy = (py >> 12) & 15;
	int w = src->w, h = src->h, i;
	uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;

	lanes[0] = lanes[1] = lanes[2] = lanes[3] = 0;
	if(y <= -2 || x <= -2 || y >= h)
		return;
	if(y >= 0)
	{
		if(x >= w)
			return;
		if(x >= 0)
			p00 = PIX32(src, x, y);
		if(x + 1 < w)
			p01 = PIX32(src, x + 1, y);
	}
	if(y + 1 < h)
	{
		if(fy == 0)
		{ // one row is enough
			for(i = 0; i < 4; i++)
			{
				int a = (int)((p00 >> (8 * i)) & 0xff), b = (int)((p01 >> (8 * i)) & 0xff);
				lanes[i] = b + (((a - b) * (16 - fx)) >> 4);
			}
			return;
		}
		if(x >= w)
			return;
		if(x >= 0)
			p10 = PIX32(src, x, y + 1);
		if(x + 1 < w)
			p11 = PIX32(src, x + 1, y + 1);
	}
	for(i = 0; i < 4; i++)
	{
		int a = (int)((p00 >> (8 * i)) & 0xff), b = (int)((p01 >> (8 * i)) & 0xff);
		int c = (int)((p10 >> (8 * i)) & 0xff), d = (int)((p11 >> (8 * i)) & 0xff);
		int r0 = b + (((a - b) * (16 - fx)) >> 4);
		int r1 = d + (((c - d) * (16 - fx)) >> 4);
		lanes[i] = r1 + (((r0 - r1) * (16 - fy)) >> 4);
	}
}

// nearest sample; 0 when outside (unsigned compare, so negatives fall out)
static inline int Persp_Nearest(const Bmp_t* src, int32_t px, int32_t py, uint32_t* out)
{
	int x = px >> 16, y = py >> 16;
	if((uint32_t)y >= (uint32_t)src->h || (uint32_t)x >= (uint32_t)src->w)
	{
		*out = 0;
		return 0;
	}
	*out = PIX32(src, x, y);
	return 1;
}

// the blend step of the level blends: d + ((s - d) * inv) >> 8 on B, G, R; the top byte keeps d's
static inline uint32_t BlendInv(uint32_t dv, const int s[4], int inv)
{
	uint32_t out = dv & 0xff000000u;
	int i;
	for(i = 0; i < 3; i++)
	{
		int dc = (int)((dv >> (8 * i)) & 0xff);
		out |= (uint32_t)Sat8(dc + (((s[i] - dc) * inv) >> 8)) << (8 * i);
	}
	return out;
}

// the picture position (16.16) of a destination pixel from its homogeneous
// numerators (the row accumulator plus the column accumulator) and denominator; 0 when behind the eye
static inline int Persp_Texel(double uNum, double vNum, double wDen, double cx, double cy, int32_t* px,
	int32_t* py)
{
	double u, v;
	if(!(fabs(wDen) > 1e-9))
		return 0;
	u = uNum / wDen;
	v = vNum / wDen;
	*px = BGI_Ftol((cx + u) * 65536.0);
	*py = BGI_Ftol((cy + v) * 65536.0);
	return 1;
}

/* The shared destination walk of the two blitters: for every destination
 * pixel, the picture position (16.16) of the homography.  `body` is a macro
 * or code block below instead of a callback so the inner loop stays flat. */
#define PERSP_ROWS(dst) \
	for(j = 0; j < (dst)->h; j++, rowU[0] += p.inv[1], rowV[0] += p.inv[4], rowW[0] += p.inv[7])
#define PERSP_COLS(dst, i) \
	for(colU[0] = 0.0, colV[0] = 0.0, colW[0] = 0.0, i = 0; i < (dst)->w; i++, colU[0] += p.inv[0], colV[0] += p.inv[3], colW[0] += p.inv[6])

/* Draw `src` through the 3-D projection onto an RGB32 destination, blended
 * by level like Blit_Xform: an RGB32 source is written outright at level 0
 * (outside pixels black) and blended by `level` above; an ARGB32 source is
 * blended by its alpha and the level.  `x16`, `y16` is the destination
 * coordinate (16.16, within dst) where the picture origin lands; (cx16,
 * cy16) is the origin inside the picture, all in the coordinates of
 * Blit_Xform.  `sx`, `sy`: plane scale (16.16), `fz16` the sprite depth.
 * 0 ok, 0x13 a zero plane scale; an empty projection draws nothing. */
int Blit_Persp(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t rotX, int32_t rotY, int32_t sx, int32_t sy, int32_t fz16, int projDist,
	int level, int smooth)
{
	Persp_t p;
	double rowU[1], rowV[1], rowW[1], colU[1], colV[1], colW[1];
	double cx = (double)cx16 * INV_65536, cy = (double)cy16 * INV_65536;
	int invLevel;
	int i, j;

	if(sx == 0 || sy == 0)
		return 0x13;
	if(dst->mode != PM_RGB32)
		return 0;
	if((uint32_t)level >= 0x100u) // fully transparent: nothing
		return 0;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	Persp_Setup(&p, src->w, src->h, cx16, cy16, angle, rotX, rotY, fz16, projDist, sx, sy);
	if(!p.valid)
		return 0;

	invLevel = 0x100 - level;
	// the inverse map takes (destination - x16, destination - y16); the
	// destination walk starts at (0, 0), so the start is evaluated at -x16
	rowU[0] = p.inv[2] - p.inv[0] * ((double)x16 * INV_65536) - p.inv[1] * ((double)y16 * INV_65536);
	rowV[0] = p.inv[5] - p.inv[3] * ((double)x16 * INV_65536) - p.inv[4] * ((double)y16 * INV_65536);
	rowW[0] = p.inv[8] - p.inv[6] * ((double)x16 * INV_65536) - p.inv[7] * ((double)y16 * INV_65536);

	if(src->mode == PM_RGB32 && level == 0)
	{
		// a plain (unblended) write of the transformed picture
		PERSP_ROWS(dst)
		{
			uint32_t* d = (uint32_t*)ROW(dst, j);
			PERSP_COLS(dst, i)
			{
				int32_t px, py;
				if(!Persp_Texel(rowU[0] + colU[0], rowV[0] + colV[0], rowW[0] + colW[0], cx, cy, &px, &py))
				{
					d[i] = 0;
					continue;
				}
				if(smooth)
				{
					int s[4];
					Persp_Sample(src, px, py, s);
					d[i] = (uint32_t)Sat8(s[0]) | (uint32_t)Sat8(s[1]) << 8 | (uint32_t)Sat8(s[2]) << 16 | (uint32_t)Sat8(s[3]) << 24;
				}
				else
					Persp_Nearest(src, px, py, &d[i]);
			}
		}
	}
	else if(src->mode == PM_RGB32)
	{
		PERSP_ROWS(dst)
		{
			uint32_t* d = (uint32_t*)ROW(dst, j);
			PERSP_COLS(dst, i)
			{
				int s[4];
				int32_t px, py;
				if(!Persp_Texel(rowU[0] + colU[0], rowV[0] + colV[0], rowW[0] + colW[0], cx, cy, &px, &py))
					continue;
				if(smooth)
					Persp_Sample(src, px, py, s);
				else
				{
					uint32_t sv;
					Persp_Nearest(src, px, py, &sv);
					s[0] = (int)(sv & 0xff);
					s[1] = (int)((sv >> 8) & 0xff);
					s[2] = (int)((sv >> 16) & 0xff);
				}
				d[i] = BlendInv(d[i], s, invLevel);
			}
		}
	}
	else
	{
		// ARGB32: alpha blend with weight ((a >> 1) * inv) >> 8, alpha
		// below 2 (after filtering) leaves the destination alone
		for(j = 0; j < dst->h; j++, rowU[0] += p.inv[1], rowV[0] += p.inv[4], rowW[0] += p.inv[7])
		{
			uint32_t* d = (uint32_t*)ROW(dst, j);
			int k3;
			PERSP_COLS(dst, i)
			{
				int s[4], a7, w;
				uint32_t dv, out;
				int32_t px, py;
				if(!Persp_Texel(rowU[0] + colU[0], rowV[0] + colV[0], rowW[0] + colW[0], cx, cy, &px, &py))
					continue;
				if(smooth)
				{
					Persp_Sample(src, px, py, s);
					a7 = Sat8(s[3]) >> 1; // packuswb, then alpha >> 1
				}
				else
				{
					uint32_t sv;
					if(!Persp_Nearest(src, px, py, &sv))
						continue;
					s[0] = (int)(sv & 0xff);
					s[1] = (int)((sv >> 8) & 0xff);
					s[2] = (int)((sv >> 16) & 0xff);
					a7 = (int)(sv >> 25);
				}
				if(!a7)
					continue;
				w = (a7 * invLevel) >> 8;
				dv = d[i];
				out = dv & 0xff000000u;
				for(k3 = 0; k3 < 3; k3++)
				{
					int dc = (int)((dv >> (8 * k3)) & 0xff);
					out |= (uint32_t)Sat8(dc + (((s[k3] - dc) * w) >> 7)) << (8 * k3);
				}
				d[i] = out;
			}
		}
	}
	return 0;
}

/* Write `src` through the 3-D projection into dst, which must be of the
 * source's mode (RGB32 or ARGB32); every destination pixel is replaced (the
 * sample, black / transparent outside the picture, with its colour scaled by
 * (0x100 - level) / 256 and its alpha kept).  A level of 0x100 or more
 * clears dst; an empty projection clears it as well.  0 ok, 0x13 a zero
 * plane scale or an empty projection; nothing is done for other modes. */
int Blit_PerspCopy(Bmp_t* dst, int32_t x16, int32_t y16, const Bmp_t* src, int32_t cx16, int32_t cy16,
	int32_t angle, int32_t rotX, int32_t rotY, int32_t sx, int32_t sy, int32_t fz16, int projDist,
	int level, int smooth)
{
	Persp_t p;
	double rowU[1], rowV[1], rowW[1], colU[1], colV[1], colW[1];
	double cx = (double)cx16 * INV_65536, cy = (double)cy16 * INV_65536;
	int invLevel = 0x100 - level;
	int i, j;

	if(sx == 0 || sy == 0)
		return 0x13;
	if(dst->mode != src->mode)
		return 0;
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0;
	Persp_Setup(&p, src->w, src->h, cx16, cy16, angle, rotX, rotY, fz16, projDist, sx, sy);
	if(!p.valid)
	{
		Bmp_Clear(dst, NULL);
		return 0x13;
	}
	if((uint32_t)level >= 0x100u)
	{
		Bmp_Clear(dst, NULL);
		return 0;
	}

	// the inverse map takes (destination - x16, destination - y16); the
	// destination walk starts at (0, 0), so the start is evaluated at -x16
	rowU[0] = p.inv[2] - p.inv[0] * ((double)x16 * INV_65536) - p.inv[1] * ((double)y16 * INV_65536);
	rowV[0] = p.inv[5] - p.inv[3] * ((double)x16 * INV_65536) - p.inv[4] * ((double)y16 * INV_65536);
	rowW[0] = p.inv[8] - p.inv[6] * ((double)x16 * INV_65536) - p.inv[7] * ((double)y16 * INV_65536);

	for(j = 0; j < dst->h; j++, rowU[0] += p.inv[1], rowV[0] += p.inv[4], rowW[0] += p.inv[7])
	{
		uint32_t* d = (uint32_t*)ROW(dst, j);
		PERSP_COLS(dst, i)
		{
			int s[4];
			int32_t px, py;
			if(!Persp_Texel(rowU[0] + colU[0], rowV[0] + colV[0], rowW[0] + colW[0], cx, cy, &px, &py))
			{
				d[i] = 0;
				continue;
			}
			if(smooth)
				Persp_Sample(src, px, py, s);
			else
			{
				uint32_t sv;
				Persp_Nearest(src, px, py, &sv);
				s[0] = (int)(sv & 0xff);
				s[1] = (int)((sv >> 8) & 0xff);
				s[2] = (int)((sv >> 16) & 0xff);
				s[3] = (int)(sv >> 24);
			}
			if(level)
			{
				s[0] = (s[0] * invLevel) >> 8;
				s[1] = (s[1] * invLevel) >> 8;
				s[2] = (s[2] * invLevel) >> 8;
			}
			d[i] = (uint32_t)Sat8(s[0]) | (uint32_t)Sat8(s[1]) << 8 | (uint32_t)Sat8(s[2]) << 16 | (uint32_t)Sat8(s[3]) << 24;
		}
	}
	return 0;
}
