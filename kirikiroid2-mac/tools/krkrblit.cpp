// krkrblit - check the engine's software blitters on this machine
//
// visual/tvpgl.cpp holds the whole pixel pipeline as function pointers, which
// TVPInitTVPGL() points at the generic C implementations; every layer, sprite
// and transition of a KiriKiri game ends up in these.  They have to work before
// anything can be drawn, so this tool runs the ones a picture goes through and
// checks the results against what blending must do:
//
//   * a fully opaque source replaces the destination
//   * a fully transparent source changes nothing
//   * an additive blend never darkens a channel
//
// Usage: krkrblit          (prints one line per check, exit 1 on a failure)

#include "tjsCommHead.h"
#include "tvpgl.h"

#include <cstdio>
#include <cstring>

extern void TVPGL_C_Init();

static int gFailures;

// The blitters compute in fixed point ((s - d) * alpha >> 8), so a result can
// sit one step below the arithmetic value; every check allows that.
static bool Near(unsigned a, unsigned b, int tolerance)
{
	for(int shift = 0; shift <= 24; shift += 8)
	{
		int ca = (int)((a >> shift) & 0xff);
		int cb = (int)((b >> shift) & 0xff);
		if(ca > cb ? ca - cb > tolerance : cb - ca > tolerance)
			return false;
	}
	return true;
}

static void Check(const char* what, unsigned got, unsigned want, int tolerance = 1)
{
	if(Near(got, want, tolerance))
	{
		printf("  ok    %-46s 0x%08x\n", what, got);
		return;
	}
	printf("  FAIL  %-46s 0x%08x != 0x%08x\n", what, got, want);
	gFailures++;
}

int main()
{
	// the dispatch table and the lookup tables the C blitters read
	TVPInitTVPGL();

	const unsigned opaqueSrc = 0xff2040c0u; // alpha 0xff
	const unsigned clearSrc = 0x002040c0u;  // alpha 0x00
	const unsigned destColour = 0xff808080u;

	// TVPAlphaBlend leaves the destination's alpha alone (the "no alpha"
	// destination variant); TVPAlphaBlend_d blends it as well
	unsigned dest = destColour;
	TVPAlphaBlend_d(&dest, &opaqueSrc, 1);
	Check("opaque source replaces the destination", dest, opaqueSrc);

	dest = destColour;
	TVPAlphaBlend_d(&dest, &clearSrc, 1);
	Check("transparent source leaves the destination", dest, destColour, 0);

	// half alpha: the result must sit between the two, per channel
	unsigned half = 0x80808080u; // alpha 0x80, colour 0x808080
	unsigned over = 0xff000000u; // opaque black
	unsigned out = over;
	TVPAlphaBlend_d(&out, &half, 1);
	unsigned r = (out >> 16) & 0xff;
	if(r > 0 && r < 0x80)
		printf("  ok    %-46s r=0x%02x\n", "half alpha lands between the colours", r);
	else
	{
		printf("  FAIL  %-46s r=0x%02x\n", "half alpha lands between the colours", r);
		gFailures++;
	}

	// additive: brighter than the destination on every channel
	unsigned addDest = 0xff101010u;
	unsigned addSrc = 0xff202020u;
	TVPAddBlend(&addDest, &addSrc, 1);
	Check("additive blend brightens", addDest, 0xff303030u);

	// the 32 to 24 bit conversion the BMP and TLG writers use
	{
		unsigned src32 = 0xff8000ffu;
		unsigned char out24[3] = {0, 0, 0};
		TVPConvert32BitTo24Bit(out24, (const unsigned char*)&src32, 1);
		printf("  %-52s %02x %02x %02x\n", "32 -> 24 bit conversion", out24[0], out24[1], out24[2]);
	}

	printf("%s: %d check(s) failed\n", gFailures ? "FAILED" : "all passed", gFailures);
	return gFailures ? 1 : 0;
}
