// krkrfont - rasterise glyphs with the engine's FreeType font stack
//
// visual/FreeType.cpp, FreeTypeFontRasterizer.cpp, FontSystem.cpp and
// PrerenderedFont.cpp are the platform-independent half of the text pipeline:
// they turn a tTVPFont and a character into a tTVPCharacterData (a coverage or
// colour bitmap plus its metrics), and the bitmap layer blits it.  The Windows
// back end draws text through GDI instead, so this is the path the port wants.
//
// This tool rasterises a few characters and writes them as a strip, so the
// glyphs can be looked at, and prints the metrics the blitter uses.
//
// Usage: krkrfont <out.bmp> [height] [characters]

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tvpfontstruc.h"
#include "CharacterData.h"
#include "FreeTypeFontRasterizer.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	const char* out = argc > 1 ? argv[1] : "font.bmp";
	int height = argc > 2 ? atoi(argv[2]) : 40;
	// a kana, a kanji, a Latin letter and a digit: the mix a galgame shows
	const char* text = argc > 3 ? argv[3] : "あ漢A9";

	tTVPFont font;
	font.Height = height;
	font.Flags = 0;
	font.Angle = 0;
	font.Face = ttstr(""); // the platform's default face

	FreeTypeFontRasterizer* ras = new FreeTypeFontRasterizer();
	ras->AddRef();
	ras->ApplyFont(font);
	tjs_int ascent = ras->GetAscentHeight();
	printf("ascent: %d, height: %d\n", (int)ascent, height);

	// lay the glyphs out in a row, the way a text line would
	struct Cell
	{
		tTVPCharacterData* data;
		int adv;
	};
	std::vector<Cell> cells;
	ttstr str(text);
	int totalW = 0;
	for(tjs_int i = 0; i < str.GetLen(); i++)
	{
		tjs_int w = 0, h = 0;
		tTVPFontAndCharacterData fc;
		fc.Font = font;
		fc.FontHash = 0;
		fc.Character = str.c_str()[i];
		fc.BlurLevel = 0;
		fc.BlurWidth = 0;
		fc.Antialiased = true;
		fc.Blured = false;
		fc.Hinting = true;
		Cell c;
		c.data = ras->GetBitmap(fc, 0, 0);
		ras->GetTextExtent(str.c_str()[i], w, h);
		c.adv = w;
		printf("  U+%04X extent %dx%d blackbox %ux%u pitch %d origin (%d,%d) aa=%d full=%d\n",
			(unsigned)str.c_str()[i], (int)w, (int)h,
			c.data ? c.data->BlackBoxX : 0, c.data ? c.data->BlackBoxY : 0,
			c.data ? (int)c.data->Pitch : 0,
			c.data ? (int)c.data->OriginX : 0, c.data ? (int)c.data->OriginY : 0,
			c.data ? (int)c.data->Antialiased : 0, c.data ? (int)c.data->FullColored : 0);
		totalW += w > 0 ? w : height / 2;
		cells.push_back(c);
	}

	int lineH = height + ascent + 8;
	std::vector<unsigned> pixels((size_t)totalW * lineH, 0xff000000u);
	int penX = 0;
	for(Cell& c : cells)
	{
		if(c.data != nullptr && c.data->BlackBoxX > 0 && c.data->BlackBoxY > 0)
		{
			const unsigned char* src = c.data->GetData();
			int sx = penX + c.data->OriginX;
			int sy = ascent + c.data->OriginY;
			for(tjs_uint y = 0; y < c.data->BlackBoxY; y++)
			{
				int dy = sy + (int)y;
				if(dy < 0 || dy >= lineH)
					continue;
				for(tjs_uint x = 0; x < c.data->BlackBoxX; x++)
				{
					int dx = sx + (int)x;
					if(dx < 0 || dx >= totalW)
						continue;
					// a normal glyph is 8-bit coverage (one byte per pixel,
					// its pitch equal to its width); a colour glyph is four
					// bytes, BGRA in memory
					unsigned a;
					if(c.data->FullColored)
					{
						const unsigned char* p = src + (size_t)y * c.data->Pitch + (size_t)x * 4;
						unsigned v = (unsigned)p[3] << 24 | (unsigned)p[2] << 16
							| (unsigned)p[1] << 8 | (unsigned)p[0];
						pixels[(size_t)dy * totalW + dx] = v;
						continue;
					}
					a = src[(size_t)y * c.data->Pitch + (size_t)x];
					// composite white by the coverage over the black background,
					// so the result shows what the bitmap layer would blit
					unsigned v = 0xff000000u | (a << 16) | (a << 8) | a;
					pixels[(size_t)dy * totalW + dx] = v;
				}
			}
		}
		penX += c.adv > 0 ? c.adv : height / 2;
	}

	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), totalW, lineH,
		32, totalW * 4, SDL_PIXELFORMAT_ABGR8888);
	if(surf == nullptr)
	{
		fprintf(stderr, "krkrfont: %s\n", SDL_GetError());
		return 1;
	}
	int rc = SDL_SaveBMP(surf, out) == 0 ? 0 : 1;
	SDL_FreeSurface(surf);
	printf(rc == 0 ? "wrote %s (%dx%d)\n" : "cannot write %s\n", out, totalW, lineH);
	fflush(stdout);
	// the engine's statics are torn down after this; a one-shot tool does not
	// need to risk that order, and its output is already flushed
	_exit(rc);
}
