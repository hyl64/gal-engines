// krkrtext - draw text onto a bitmap through the engine's own bitmap API
//
// tTVPNativeBaseBitmap::DrawTextSingle is the engine's text drawing: it takes a
// character's coverage from the rasterizer (or the game's prerendered font) and
// composites it with the render manager's colour-map methods.  This tool drives
// that path on a software bitmap and writes the result out.
//
// Usage: krkrtext <out.bmp> [height] [text]

#include "tjsCommHead.h"
#include "tjsString.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"
#include "tvpgl.h"
#include "tvpfontstruc.h"

#include <SDL.h>

#include <cstdio>
#include <unistd.h>

class tTVPBitmap_Soft : public iTVPBaseBitmap
{
public:
	iTVPRenderManager* GetRenderManager() override { return TVPGetRenderManager(); }
};

int main(int argc, char** argv)
{
	const char* out = argc > 1 ? argv[1] : "text.bmp";
	int size = argc > 2 ? atoi(argv[2]) : 40;
	const char* text = argc > 3 ? argv[3] : "あ漢A9 テスト";
	TVPInitTVPGL();

	const tjs_uint w = 480, h = 160;
	tTVPBitmap_Soft bmp;
	bmp.Recreate(w, h, 32);
	bmp.Fill(tTVPRect(0, 0, (tjs_int)w - 1, (tjs_int)h - 1), 0xff182030u);

	tTVPFont font;
	font.Height = size;
	font.Flags = 0;
	font.Angle = 0;
	font.Face = ttstr(""); // the platform's default face
	bmp.SetFont(font);

	// DrawText takes a whole string and dispatches to the multiple-character
	// path; DrawTextSingle is for one glyph
	bmp.DrawText(tTVPRect(0, 0, (tjs_int)w - 1, (tjs_int)h - 1), 20, 20, text,
		0x00f0f0f0u, bmAlpha, 255, true, true);
	bmp.DrawText(tTVPRect(0, 0, (tjs_int)w - 1, (tjs_int)h - 1), 20, 90, text,
		0x00ffcc66u, bmCopy, 255, true, true);

	// how much of the picture changed: text either drew or it did not
	int lit = 0;
	for(tjs_uint y = 0; y < h; y++)
	{
		const tjs_uint32* line = (const tjs_uint32*)bmp.GetScanLine(y);
		for(tjs_uint x = 0; x < w; x++)
			if((line[x] & 0x00ffffffu) != 0x00301820u) // the stored 0xAABBGGRR of the fill
				lit++;
	}
	printf("drawn: %d pixel(s) differ from the background\n", lit);

	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom((void*)bmp.GetScanLine(0),
		(int)w, (int)h, 32, bmp.GetPitchBytes(), SDL_PIXELFORMAT_ABGR8888);
	int rc = surf != nullptr && SDL_SaveBMP(surf, out) == 0 ? 0 : 1;
	if(surf != nullptr)
		SDL_FreeSurface(surf);
	printf(rc == 0 ? "wrote %s\n" : "cannot write %s\n", out);
	fflush(stdout);
	_exit(rc);
}
