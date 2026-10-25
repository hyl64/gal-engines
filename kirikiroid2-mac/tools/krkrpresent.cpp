// krkrpresent - put an engine texture on a real window
//
// The engine draws into iTVPTexture2D objects: the software render manager makes
// them, the layers composite into them, and the back end's job is to get one of
// them onto the screen.  On Windows that is a BitBlt through GDI, on Android a
// GL texture upload.  Here it is an SDL texture and a renderer, and this tool is
// that last hop on its own: decode a picture from an XP3 into an engine texture
// and present it, with a frame counter and a screenshot so the hop can be seen
// to work.
//
// Usage: krkrpresent <archive.xp3> <name inside> [frames] [out.bmp]

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsError.h"
#include "StorageIntf.h"
#include "RenderManager.h"
#include "GraphicsLoaderIntf.h"

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

// the decoder fills the engine texture line by line
struct Target
{
	iTVPTexture2D* texture;
	int width;
	int height;
};

static int SizeCallback(void* data, tjs_uint w, tjs_uint h, tTVPGraphicPixelFormat fmt)
{
	(void)fmt;
	Target* t = (Target*)data;
	t->width = (int)w;
	t->height = (int)h;
	// the engine's own software texture, from the engine's own render manager
	t->texture = TVPGetRenderManager()->CreateTexture2D(nullptr, 0, w, h, TVPTextureFormat::RGBA);
	return t->texture != nullptr ? t->texture->GetPitch() : 0;
}

static void* ScanLineCallback(void* data, tjs_int y)
{
	Target* t = (Target*)data;
	if(y < 0 || t->texture == nullptr)
		return nullptr;
	return t->texture->GetScanLineForWrite((tjs_uint)y);
}

int main(int argc, char** argv)
{
	if(argc < 3)
	{
		fprintf(stderr, "usage: %s <archive.xp3> <name inside> [frames] [out.bmp]\n", argv[0]);
		return 2;
	}
	const char* archive = argv[1];
	const char* inner = argv[2];
	int frames = argc > 3 ? atoi(argv[3]) : 120;
	const char* out = argc > 4 ? argv[4] : nullptr;

	std::string name = "file://" + std::string(archive) + ">" + inner;
	tTJSBinaryStream* st = TVPCreateStream(ttstr(name.c_str()));
	if(st == nullptr)
	{
		fprintf(stderr, "krkrpresent: cannot open %s\n", name.c_str());
		return 1;
	}
	Target target = {nullptr, 0, 0};
	std::string path(inner);
	size_t dot = path.find_last_of('.');
	std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
	for(char& c : ext)
		c = (char)tolower((unsigned char)c);
	try
	{
		if(ext == "png")
			TVPLoadPNG(nullptr, &target, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else if(ext == "tlg" || ext == "tlg5" || ext == "tlg6")
			TVPLoadTLG(nullptr, &target, SizeCallback, ScanLineCallback, nullptr, st, 0, glmNormal);
		else
		{
			fprintf(stderr, "krkrpresent: no decoder for '.%s'\n", ext.c_str());
			delete st;
			return 1;
		}
	}
	catch(const eTJS&)
	{
		delete st;
		return 1;
	}
	delete st;
	if(target.texture == nullptr)
	{
		fprintf(stderr, "krkrpresent: the decoder produced no texture\n");
		return 1;
	}
	printf("texture: %dx%d from %s\n", target.width, target.height, name.c_str());

	if(SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fprintf(stderr, "krkrpresent: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* win = SDL_CreateWindow("krkrpresent", SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, target.width, target.height, SDL_WINDOW_SHOWN);
	if(win == nullptr)
	{
		fprintf(stderr, "krkrpresent: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	if(ren == nullptr)
		ren = SDL_CreateRenderer(win, -1, 0);
	SDL_Texture* sdltex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
		SDL_TEXTUREACCESS_STREAMING, target.width, target.height);
	if(sdltex == nullptr)
	{
		fprintf(stderr, "krkrpresent: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	// the hop: engine texture scan lines into the SDL texture, then present
	int shown = 0;
	bool running = true;
	for(int frame = 0; frame < frames && running; frame++)
	{
		SDL_UpdateTexture(sdltex, nullptr, target.texture->GetScanLineForRead(0),
			target.texture->GetPitch());
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, sdltex, nullptr, nullptr);
		SDL_RenderPresent(ren);
		shown++;
		SDL_Event e;
		while(SDL_PollEvent(&e))
			if(e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
				running = false;
		SDL_Delay(16); // ~60 fps
	}
	printf("presented %d frame(s) on '%s'\n", shown, SDL_GetVideoDriver(0) ? SDL_GetCurrentVideoDriver() : "?");
	if(out != nullptr)
	{
		// what the window showed: the engine texture again, straight out
		SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(
			(void*)target.texture->GetScanLineForRead(0), target.width, target.height,
			32, target.texture->GetPitch(), SDL_PIXELFORMAT_ABGR8888);
		if(shot != nullptr)
		{
			printf(SDL_SaveBMP(shot, out) == 0 ? "wrote %s\n" : "cannot write %s\n", out);
			SDL_FreeSurface(shot);
		}
	}
	SDL_DestroyTexture(sdltex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	fflush(stdout);
	_exit(0);
}
