// krkrscene - compose a scene the way the engine would: background, window, text
//
// Three pieces of the engine meet here, each of them the engine's own:
// TVPLoadPNG decodes the background out of an XP3 through the storage layer,
// the FreeType font stack rasterises the text, and tvpgl blends both onto the
// picture.  The layers and the window manager that would normally arrange this
// are not built yet, so the arrangement is this tool's - but nothing about the
// pixels is: decoding, glyph shapes and blending all come from the engine.
//
// Usage: krkrscene <archive.xp3> <background> <out.bmp> "text" [x] [y] [size]

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsError.h"
#include "StorageIntf.h"
#include "GraphicsLoaderIntf.h"
#include "tvpfontstruc.h"
#include "CharacterData.h"
#include "FreeTypeFontRasterizer.h"
#include "tvpgl.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

// ---------------------------------------------------------------------------
// the picture: the engine's tjs_uint32 is 0xAABBGGRR (ABGR8888 to SDL)
// ---------------------------------------------------------------------------

struct Picture
{
	std::vector<unsigned> pixels;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

static int SizeCallback(void* data, tjs_uint w, tjs_uint h, tTVPGraphicPixelFormat fmt)
{
	(void)fmt;
	Picture* pic = (Picture*)data;
	pic->width = (int)w;
	pic->height = (int)h;
	pic->pitch = (int)w * 4;
	pic->pixels.assign((size_t)w * h, 0);
	return pic->pitch;
}

static void* ScanLineCallback(void* data, tjs_int y)
{
	Picture* pic = (Picture*)data;
	if(y < 0 || y >= pic->height)
		return nullptr;
	return (void*)(pic->pixels.data() + (size_t)y * pic->width);
}

static bool LoadBackground(const std::string& archive, const std::string& inner, Picture& pic)
{
	std::string name = "file://" + archive + ">" + inner;
	tTJSBinaryStream* st = TVPCreateStream(ttstr(name.c_str()));
	if(st == nullptr)
	{
		fprintf(stderr, "krkrscene: cannot open %s\n", name.c_str());
		return false;
	}
	size_t dot = inner.find_last_of('.');
	std::string ext = dot == std::string::npos ? "" : inner.substr(dot + 1);
	try
	{
		if(ext == "png")
			TVPLoadPNG(nullptr, &pic, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else if(ext == "tlg" || ext == "tlg5" || ext == "tlg6")
			TVPLoadTLG(nullptr, &pic, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else
		{
			fprintf(stderr, "krkrscene: no decoder for '.%s'\n", ext.c_str());
			delete st;
			return false;
		}
	}
	catch(const eTJS&)
	{
		delete st;
		return false;
	}
	delete st;
	return pic.width > 0 && pic.height > 0;
}

// ---------------------------------------------------------------------------
// blending, with the engine's own blitters
// ---------------------------------------------------------------------------

// Fill a rectangle with `colour` at `opa`, through TVPAlphaBlend_d: one row of
// the colour is blended at a time, so the arithmetic is the engine's.
static void FillRect(Picture& pic, int x, int y, int w, int h, unsigned colour, int opa)
{
	if(w <= 0 || h <= 0)
		return;
	std::vector<unsigned> row((size_t)w, colour);
	for(int dy = y; dy < y + h; dy++)
	{
		if(dy < 0 || dy >= pic.height)
			continue;
		for(int dx = x; dx < x + w; dx += (int)row.size())
		{
			int len = x + w - dx;
			if(len > (int)row.size())
				len = (int)row.size();
			if(dx < 0)
			{
				row.assign((size_t)len, colour);
				TVPAlphaBlend_d(pic.pixels.data() + (size_t)dy * pic.width, row.data(), 1);
				continue;
			}
			if(dx + len > pic.width)
				len = pic.width - dx;
			if(len <= 0)
				break;
			for(int i = 0; i < len; i++)
				row[(size_t)i] = (colour & 0x00ffffffu) | ((unsigned)opa << 24);
			TVPAlphaBlend_d(pic.pixels.data() + (size_t)dy * pic.width + dx, row.data(), len);
		}
	}
}

// Draw one glyph's coverage onto the picture as `colour`, through
// TVPAlphaBlend_d; the coverage becomes the alpha of a one-colour image.
static void BlendGlyph(Picture& pic, const tTVPCharacterData* data, int x, int y, unsigned colour)
{
	if(data == nullptr || data->BlackBoxX == 0 || data->BlackBoxY == 0)
		return;
	std::vector<unsigned> line(data->BlackBoxX);
	for(tjs_uint dy = 0; dy < data->BlackBoxY; dy++)
	{
		int py = y + (int)dy;
		if(py < 0 || py >= pic.height)
			continue;
		const unsigned char* cov = data->GetData() + (size_t)dy * data->Pitch;
		for(tjs_uint dx = 0; dx < data->BlackBoxX; dx++)
			line[dx] = (colour & 0x00ffffffu) | ((unsigned)cov[dx] << 24);
		int px = x;
		int len = (int)data->BlackBoxX;
		if(px < 0)
		{
			line.erase(line.begin(), line.begin() - px);
			len += px;
			px = 0;
		}
		if(px + len > pic.width)
			len = pic.width - px;
		if(len > 0)
			TVPAlphaBlend_d(pic.pixels.data() + (size_t)py * pic.width + px, line.data(), len);
	}
}

int main(int argc, char** argv)
{
	if(argc < 5)
	{
		fprintf(stderr,
			"usage: %s <archive.xp3> <background> <out.bmp> \"text\" [x] [y] [size]\n", argv[0]);
		return 2;
	}
	const char* archive = argv[1];
	const char* bgname = argv[2];
	const char* out = argv[3];
	const char* text = argv[4];
	int x = argc > 5 ? atoi(argv[5]) : 80;
	int y = argc > 6 ? atoi(argv[6]) : 0;
	int size = argc > 7 ? atoi(argv[7]) : 32;

	Picture pic;
	if(!LoadBackground(archive, bgname, pic))
	{
		fprintf(stderr, "krkrscene: cannot decode the background\n");
		return 1;
	}
	printf("background: %dx%d from %s>%s\n", pic.width, pic.height, archive, bgname);

	TVPInitTVPGL();

	// the message window: the dark band a galgame puts its text in
	int winH = size * 4;
	int winY = pic.height - winH - size;
	if(winY < 0)
		winY = 0;
	FillRect(pic, 0, winY, pic.width, winH, 0x00101828u, 200);
	FillRect(pic, size / 2, winY, pic.width - size, 2, 0x00c0d0e0u, 180);

	// the text, through the engine's font stack
	tTVPFont font;
	font.Height = size;
	font.Flags = 0;
	font.Angle = 0;
	font.Face = ttstr("");
	FreeTypeFontRasterizer* ras = new FreeTypeFontRasterizer();
	ras->AddRef();
	ras->ApplyFont(font);
	tjs_int ascent = ras->GetAscentHeight();

	if(y == 0)
		y = winY + (winH - size) / 2;
	ttstr str(text);
	int penX = x;
	int lineY = y;
	for(tjs_int i = 0; i < str.GetLen(); i++)
	{
		tjs_char ch = str.c_str()[i];
		if(ch == TJS_W('\n'))
		{
			penX = x;
			lineY += size * 3 / 2;
			continue;
		}
		tjs_int w = 0, h = 0;
		ras->GetTextExtent(ch, w, h);
		tTVPFontAndCharacterData fc;
		fc.Font = font;
		fc.FontHash = 0;
		fc.Character = ch;
		fc.BlurLevel = 0;
		fc.BlurWidth = 0;
		fc.Antialiased = true;
		fc.Blured = false;
		fc.Hinting = true;
		tTVPCharacterData* data = ras->GetBitmap(fc, 0, 0);
		// the glyph's origin is relative to the ascent line
		BlendGlyph(pic, data, penX + data->OriginX, lineY + ascent + data->OriginY,
			0x00f0f4ffu);
		penX += w > 0 ? w : size / 2;
	}
	printf("text: %s at (%d,%d), %dpx\n", text, x, y, size);

	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pic.pixels.data(),
		pic.width, pic.height, 32, pic.pitch, SDL_PIXELFORMAT_ABGR8888);
	if(surf == nullptr)
	{
		fprintf(stderr, "krkrscene: %s\n", SDL_GetError());
		return 1;
	}
	int rc = SDL_SaveBMP(surf, out) == 0 ? 0 : 1;
	SDL_FreeSurface(surf);
	printf(rc == 0 ? "wrote %s\n" : "cannot write %s\n", out);
	fflush(stdout);
	_exit(rc);
}
