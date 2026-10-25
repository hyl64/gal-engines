// krkrview - decode a picture out of an XP3 archive with the engine's decoder
//
// TVPLoadPNG / TVPLoadTLG take a stream and hand the picture back line by line
// through callbacks, which is the path a game takes without any window object
// in the way.  The stream comes from the storage layer, so the name may be
// "file:///abs/game.xp3>title.png".  The result goes into a .bmp (and onto the
// screen when there is one), so what was decoded can be checked.
//
// Usage: krkrview <archive.xp3> <name inside> <out.bmp>

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsError.h"
#include "StorageIntf.h"
#include "GraphicsLoaderIntf.h"

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

// The engine's tjs_uint32 pixels are 0xAABBGGRR: alpha in the high byte and
// then blue, green, red down to the low one.  In memory (little endian) that
// is R, G, B, A, which is SDL_PIXELFORMAT_ABGR8888 - not ARGB8888, which was
// the obvious guess and the wrong one.
struct Picture
{
	std::vector<unsigned> pixels; // 0xAABBGGRR
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

static std::string LowerExt(const std::string& path)
{
	size_t dot = path.find_last_of('.');
	if(dot == std::string::npos)
		return std::string();
	std::string ext = path.substr(dot + 1);
	for(char& c : ext)
		c = (char)tolower((unsigned char)c);
	return ext;
}

int main(int argc, char** argv)
{
	if(argc < 4)
	{
		fprintf(stderr, "usage: %s <archive.xp3> <name inside> <out.bmp>\n", argv[0]);
		return 2;
	}
	std::string archive(argv[1]);
	std::string inner(argv[2]);
	const char* out = argv[3];

	std::string name = "file://" + archive + ">" + inner;
	tTJSBinaryStream* st = TVPCreateStream(ttstr(name.c_str()));
	if(st == nullptr)
	{
		fprintf(stderr, "krkrview: cannot open %s\n", name.c_str());
		return 1;
	}
	std::string ext = LowerExt(inner);
	Picture pic;
	try
	{
		if(ext == "png")
			TVPLoadPNG(nullptr, &pic, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else if(ext == "tlg" || ext == "tlg5" || ext == "tlg6")
			TVPLoadTLG(nullptr, &pic, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else
		{
			fprintf(stderr, "krkrview: no decoder for '.%s' here (png, tlg)\n", ext.c_str());
			delete st;
			return 1;
		}
	}
	catch(const eTJS& e)
	{
		fprintf(stderr, "krkrview: decode failed\n");
		delete st;
		return 1;
	}
	delete st;
	if(pic.width <= 0 || pic.height <= 0)
	{
		fprintf(stderr, "krkrview: decoder returned no picture\n");
		return 1;
	}
	printf("decoded %dx%d from %s\n", pic.width, pic.height, name.c_str());

	if(SDL_Init(SDL_INIT_VIDEO) == 0)
	{
		SDL_Window* win = SDL_CreateWindow("krkrview", SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED, pic.width, pic.height,
			getenv("SDL_VIDEODRIVER") ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
		if(win != nullptr)
		{
			SDL_Renderer* ren = SDL_CreateRenderer(win, -1, 0);
			SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
				SDL_TEXTUREACCESS_STREAMING, pic.width, pic.height);
			SDL_UpdateTexture(tex, nullptr, pic.pixels.data(), pic.pitch);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, nullptr, nullptr);
			SDL_RenderPresent(ren);
			SDL_DestroyTexture(tex);
			SDL_DestroyRenderer(ren);
			SDL_DestroyWindow(win);
		}
		SDL_Quit();
	}

	// a raw dump of the decoder's bytes, so the pixel layout can be checked
	// without a container format in the way
	{
		std::string rawPath = std::string(out) + ".raw";
		FILE* rf = fopen(rawPath.c_str(), "wb");
		if(rf != nullptr)
		{
			fwrite(pic.pixels.data(), 4, pic.pixels.size(), rf);
			fclose(rf);
		}
	}

	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pic.pixels.data(),
		pic.width, pic.height, 32, pic.pitch, SDL_PIXELFORMAT_ABGR8888);
	if(surf == nullptr)
	{
		fprintf(stderr, "krkrview: %s\n", SDL_GetError());
		return 1;
	}
	int rc = SDL_SaveBMP(surf, out) == 0 ? 0 : 1;
	SDL_FreeSurface(surf);
	printf(rc == 0 ? "wrote %s\n" : "cannot write %s\n", out);
	fflush(stdout);
	// the engine's storage statics are torn down after this; a one-shot tool
	// does not need to risk the order, and its output is already flushed
	_exit(rc);
}
