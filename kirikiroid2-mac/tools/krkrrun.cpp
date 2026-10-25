// krkrrun - run a TJS2 script with the engine's classes registered
//
// This is the closest thing to the engine's own start-up a tool can do: a TJS2
// instance with the native classes the engine registers (Window, Layer, Bitmap,
// System, Storages, Scripts and the rest), so a script can create a window, put
// layers in it and draw.  What it is for is checking that path without a game:
// everything the script touches is the engine's own object.
//
// Usage: krkrrun <script.tjs>

#include "tjsCommHead.h"
#include "tjs.h"
#include "tjsString.h"
#include "tjsError.h"
#include "tjsNative.h"
#include "tjsVariant.h"
#include "tjsDebug.h"

#include "WindowIntf.h"
#include "MenuItemIntf.h"
#include "TimerIntf.h"
#include "LayerIntf.h"
#include "BitmapIntf.h"
#include "LayerBitmapIntf.h"
#include "LayerManager.h"
#include "SystemIntf.h"
#include "StorageIntf.h"
#include "EventIntf.h"
#include "ScriptMgnIntf.h"
#include "RectItf.h"
#include "FontImpl.h"
#include "DebugIntf.h"
#include "PluginImpl.h"
#include "tvpgl.h"
#include "StorageIntf.h"
#include "DrawDevice.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"

#include <SDL.h>

#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <execinfo.h>
#include <cstdlib>

// who called exit(): the engine's terminate path leads somewhere we cannot see
// otherwise, and the answer is in this stack
static void OnExitTrace()
{
	if(getenv("KRKR_TRACE_EXIT") == nullptr)
		return;
	void* frames[32];
	const int n = backtrace(frames, 32);
	fprintf(stderr, "[exit trace] %d frame(s)\n", n);
	backtrace_symbols_fd(frames, n, 2);
}

// the console the script prints through
class ConsoleOutput : public iTJSConsoleOutput
{
public:
	void ExceptionPrint(const tjs_char* msg) override
	{
		// %ls with the engine's wide chars prints nothing for non-ASCII under
		// the C locale, so every Japanese error vanished; go through UTF-8
		fprintf(stderr, "%s\n", ttstr(msg).AsStdString().c_str());
	}
	void Print(const tjs_char* msg) override
	{
		fprintf(stdout, "%s", ttstr(msg).AsStdString().c_str());
		fflush(stdout);
	}
};

static bool ReadScript(const char* path, std::vector<tjs_char>& out)
{
	FILE* f = fopen(path, "rb");
	if(f == nullptr)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::string bytes((size_t)(size > 0 ? size : 0), '\0');
	if(size > 0)
		bytes.resize((size_t)fread(&bytes[0], 1, (size_t)size, f));
	fclose(f);
	out.assign(bytes.size() + 1, 0);
	TJS_mbstowcs(&out[0], bytes.c_str(), out.size());
	return true;
}

static void Register(iTJSDispatch2* global, const tjs_char* name, iTJSDispatch2* object)
{
	if(object == nullptr)
	{
		fprintf(stderr, "krkrrun: %ls is not available on this back end\n", (const wchar_t*)name);
		return;
	}
	tTJSVariant val(object, NULL);
	object->Release();
	global->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP, name, NULL, &val, global);
}

// what the window composited, written out so the frame can be looked at
static bool SaveComposed(const char* path)
{
	// what the device presented is the frame; the primary layer is only a
	// fallback for a run that never reached Show()
	extern const uint32_t* TVPDrawDeviceDarwinGetFrame(int* width, int* height);
	extern iTVPDrawDevice* TVPGetLastDrawDeviceDarwin();
	int fw = 0, fh = 0;
	if(const uint32_t* frame = TVPDrawDeviceDarwinGetFrame(&fw, &fh))
	{
		if(fw > 0 && fh > 0)
		{
			SDL_Surface* s2 = SDL_CreateRGBSurfaceWithFormatFrom((void*)frame, fw, fh, 32, fw * 4,
				SDL_PIXELFORMAT_ARGB8888);
			const bool ok2 = s2 != nullptr && SDL_SaveBMP(s2, path) == 0;
			if(s2 != nullptr)
				SDL_FreeSurface(s2);
			if(ok2)
			{
				printf("wrote %s (%dx%d, presented frame)\n", path, fw, fh);
				return true;
			}
		}
	}
	iTVPDrawDevice* device = TVPGetLastDrawDeviceDarwin();
	if(device == nullptr)
	{
		fprintf(stderr, "krkrrun: no draw device was constructed\n");
		return false;
	}
	tTJSNI_BaseLayer* primary = device->GetPrimaryLayer();
	if(primary == nullptr)
	{
		fprintf(stderr, "krkrrun: the device has no primary layer\n");
		return false;
	}
	tTVPBaseTexture* screen = primary->GetMainImage();
	const int w = (int)screen->GetWidth();
	const int h = (int)screen->GetHeight();
	if(w <= 0 || h <= 0)
	{
		fprintf(stderr, "krkrrun: the primary layer is %dx%d\n", w, h);
		return false;
	}
	SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom((void*)screen->GetScanLine(0),
		w, h, 32, screen->GetPitchBytes(), SDL_PIXELFORMAT_ABGR8888);
	const bool ok = surf != nullptr && SDL_SaveBMP(surf, path) == 0;
	if(surf != nullptr)
		SDL_FreeSurface(surf);
	printf(ok ? "wrote %s (%dx%d)\n" : "cannot write %s\n", path, w, h);
	return ok;
}

int main(int argc, char** argv)
{
	atexit(OnExitTrace);
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <script.tjs>\n", argv[0]);
		return 2;
	}
	std::vector<tjs_char> script;
	const std::string argEarly(argv[1]);
	const bool archiveEarly = argEarly.size() > 4
		&& argEarly.compare(argEarly.size() - 4, 4, ".xp3") == 0;
	if(!archiveEarly && !ReadScript(argv[1], script))
	{
		fprintf(stderr, "krkrrun: cannot read %s\n", argv[1]);
		return 1;
	}

	// the pixel pipeline's function table: without it the first blitter call
	// jumps through a null pointer, which is how the layer's first fill died
	TVPInitTVPGL();

	// The engine's own start-up makes the game's folder searchable, so a script's
	// relative storage name ("system/Initialize.tjs") resolves.  KAG depends on it.
	{
		std::string root = "file://";
		char cwd[1024];
		if(getcwd(cwd, sizeof cwd) != nullptr)
			root += cwd;
		root += "/";
		// KAG's own scripts live in system/ and name each other without the
		// prefix, so that folder goes in first (the first match wins)
		TVPAddAutoPath(ttstr((root + "system/").c_str()));
		TVPAddAutoPath(ttstr(root.c_str()));

		TVPSetCurrentDirectory(ttstr(root.c_str()));
	}

	ConsoleOutput console;
	// Scripts.execStorage and friends reach the engine through this global; the
	// application's start-up assigns it, and a tool creates the engine itself
	extern tTJS* TVPScriptEngine;
	tTJS* tjs = new tTJS();
	TVPScriptEngine = tjs;
	// KRKR_TJS_TRACE=1 keeps the TJS call stack so a script exception prints
	// where in the game's scripts it happened, not just "immediate event"
	if(getenv("KRKR_TJS_TRACE") != nullptr)
	{
		TJS::TJSAddRefStackTracer();
	}
	// the engine's own TJS-level definitions (gcsAuto and the rest); KAG needs them
	extern void TVPRunEngineInitScript(tTJS*);
	TVPRunEngineInitScript(tjs);
	tjs->SetConsoleOutput(&console);
	iTJSDispatch2* global = tjs->GetGlobal();

	// the classes the engine registers at start-up, as far as this back end has
	// them; each factory answers nothing where the platform half is missing
	Register(global, TJS_W("System"), TVPCreateNativeClass_System());
	Register(global, TJS_W("Window"), TVPCreateNativeClass_Window());
	Register(global, TJS_W("Layer"), TVPCreateNativeClass_Layer());
	Register(global, TJS_W("Bitmap"), TVPCreateNativeClass_Bitmap());
	Register(global, TJS_W("Storages"), TVPCreateNativeClass_Storages());
	Register(global, TJS_W("Scripts"), TVPCreateNativeClass_Scripts());
	Register(global, TJS_W("Rect"), TVPCreateNativeClass_Rect());
	Register(global, TJS_W("Font"), TVPCreateNativeClass_Font());
	Register(global, TJS_W("Debug"), TVPCreateNativeClass_Debug());
	Register(global, TJS_W("Plugins"), TVPCreateNativeClass_Plugins());
	Register(global, TJS_W("MenuItem"), TVPCreateNativeClass_MenuItem());
	Register(global, TJS_W("Timer"), TVPCreateNativeClass_Timer());
	Register(global, TJS_W("AsyncTrigger"), TVPCreateNativeClass_AsyncTrigger());
	extern tTJSNativeClass* TVPCreateNativeClass_VideoOverlay();
	Register(global, TJS_W("VideoOverlay"), TVPCreateNativeClass_VideoOverlay());
	extern tTJSNativeClass* TVPCreateNativeClass_WaveSoundBuffer();
	Register(global, TJS_W("WaveSoundBuffer"), TVPCreateNativeClass_WaveSoundBuffer());
	// KAG's parser: the class that reads .ks scenarios
	extern iTJSDispatch2* TVPCreateNativeClass_KAGParser();
	if(iTJSDispatch2* parser = TVPCreateNativeClass_KAGParser())
	{
		tTJSVariant val(parser, nullptr);
		parser->Release();
		global->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP, TJS_W("KAGParser"), nullptr, &val, global);
	}

	int status = 0;
	try
	{
		// KiriKiri's own start-up sets these before a game's first script runs;
		// KAG's scripts test kirikiriz to pick their code paths
		const tjs_char prelude[] =
			TJS_W("global.kirikiriz = true;")
			TJS_W("global.kirikiri2 = true;");
		tjs->ExecScript(prelude, nullptr, nullptr, TJS_W("(prelude)"));

		// A game may ship as an archive: make it the storage root and run the
		// project's startup.tjs from inside it, the way the engine's start-up does.
		static const char kXp3Tail[] = ".xp3";
		const std::string arg(argv[1]);
		const bool isArchive = arg.size() > 4 && arg.compare(arg.size() - 4, 4, kXp3Tail) == 0;
		if(isArchive)
		{
			char real[PATH_MAX];
			if(realpath(arg.c_str(), real) == nullptr)
			{
				fprintf(stderr, "krkrrun: cannot resolve %s\n", arg.c_str());
				return 1;
			}
			// the trailing ">" is how a storage name says "inside this archive"
			std::string root = std::string("file://") + real + ">";
			TVPSetCurrentDirectory(ttstr(root.c_str()));
			TVPAddAutoPath(ttstr(root.c_str()));
			const tjs_char bootstrap[] =
				TJS_W("if(!Storages.isExistentStorage(\"startup.tjs\"))")
				TJS_W(" throw new Exception(\"no startup.tjs in the archive\");")
				TJS_W("Scripts.execStorage(\"startup.tjs\");");
			tjs->ExecScript(bootstrap, nullptr, nullptr, TJS_W("(archive)"));
		}
		else
		{
			ttstr name = ttstr(argv[1]);
			tjs->ExecScript(&script[0], nullptr, nullptr, name.c_str());
		}

		// an optional run loop: a framework that waits for timers and events needs
		// one, or it never gets past its first wait
		if(const char* seconds = getenv("KRKR_LOOP_SECONDS"))
		{
			extern void TVPDarwinTickTimers();
			extern iTVPDrawDevice* TVPGetLastDrawDeviceDarwin();
			const double limit = atof(seconds) * 1000.0;
			const Uint64 start = SDL_GetTicks64();
			while((double)(SDL_GetTicks64() - start) < limit)
			{
				TVPDarwinTickTimers();
				// sound fades step on the frame, so their completion events
				// are posted from the same thread everything else runs on
				extern void TVPDarwinTickSoundFades();
				TVPDarwinTickSoundFades();
				// the framework's own events (AsyncTrigger and friends) are queued
				// and dispatched here
				extern void TVPInvokeEvents();
				TVPInvokeEvents();
				if(iTVPDrawDevice* device = TVPGetLastDrawDeviceDarwin())
				{
					device->Update();
					device->Show();
				}
				SDL_Event e;
				while(SDL_PollEvent(&e))
				{
					if(e.type == SDL_QUIT)
						break;
				}

				/* KRKR_AUTO_CLICK="x,y,from_ms": click a few times from that
				 * moment on.  A headless host has no mouse, and a title screen sits
				 * waiting for one. */
				if(const char* click = getenv("KRKR_AUTO_CLICK"))
				{
					static int clicks;
					const long from = atol(strchr(click, ',') ? strrchr(click, ',') + 1 : "0");
					const long long now = (long long)(SDL_GetTicks64() - start);
					if(clicks < 30 && now >= from + (long long)clicks * 3000)
					{
						int x = 0, y = 0;
						sscanf(click, "%d,%d", &x, &y);
						if(getenv("KRKR_DEBUG_INPUT"))
						{
							fprintf(stderr, "[input] click %d at %lld ms\n", clicks + 1, now);
							fflush(stderr);
						}
						if(iTVPDrawDevice* device = TVPGetLastDrawDeviceDarwin())
						{
							/* a click that lands while the scene changes can name a layer
							 * that has just been thrown away; the run should outlive it */
							try
							{
								// real window order: down, up, then the
								// synthesized click; a button acts on mouse-up
								/* KAG ignores a click that lands without pointer motion
								 * (its anti-misclick guard), so move first */
								device->OnMouseMove(x + 1, y + 1, 0);
								device->OnMouseMove(x, y, 0);
								device->OnMouseDown(x, y, mbLeft, 0);
								device->OnMouseUp(x, y, mbLeft, 0);
								device->OnClick(x, y);
							}
							catch(...)
							{
								if(getenv("KRKR_DEBUG_INPUT"))
								{
									fprintf(stderr, "[input] click hit a stale layer\n");
									fflush(stderr);
								}
							}
						}
						clicks++;
					}
				}
				/* KRKR_AUTO_CLICK2 works the same way with its own point, so a run can
				 * press the title's button and then walk through the story. */
				if(const char* click2 = getenv("KRKR_AUTO_CLICK2"))
				{
					static int clicks2;
					const long from2 = atol(strrchr(click2, ',') ? strrchr(click2, ',') + 1 : "0");
					const long long now2 = (long long)(SDL_GetTicks64() - start);
					if(clicks2 < 30 && now2 >= from2 + (long long)clicks2 * 2000)
					{
						int x = 0, y = 0;
						sscanf(click2, "%d,%d", &x, &y);
						if(getenv("KRKR_DEBUG_INPUT"))
						{
							fprintf(stderr, "[input] click2 %d at %lld ms\n", clicks2 + 1, now2);
							fflush(stderr);
						}
						if(iTVPDrawDevice* device = TVPGetLastDrawDeviceDarwin())
						{
							/* a click that lands while the scene changes can name a layer
							 * that has just been thrown away; the run should outlive it */
							try
							{
								// real window order: down, up, then the
								// synthesized click; a button acts on mouse-up
								/* KAG ignores a click that lands without pointer motion
								 * (its anti-misclick guard), so move first */
								device->OnMouseMove(x + 1, y + 1, 0);
								device->OnMouseMove(x, y, 0);
								device->OnMouseDown(x, y, mbLeft, 0);
								device->OnMouseUp(x, y, mbLeft, 0);
								device->OnClick(x, y);
							}
							catch(...)
							{
								if(getenv("KRKR_DEBUG_INPUT"))
								{
									fprintf(stderr, "[input] click hit a stale layer\n");
									fflush(stderr);
								}
							}
						}
						clicks2++;
					}
				}
				SDL_Delay(16);
			}
		}
		// an optional frame, taken after the script has run
		if(argc >= 4 && strcmp(argv[2], "--screenshot") == 0)
		{
			fflush(stdout);
			SaveComposed(argv[3]);
		}
	}
	catch(const eTJS& e)
	{
		fprintf(stderr, "krkrrun: %s\n", e.GetMessage().AsStdString().c_str());
		status = 1;
	}
	catch(...)
	{
		fprintf(stderr, "[m] caught unknown\n");
		fprintf(stderr, "krkrrun: unhandled exception\n");
		status = 1;
	}
	fflush(stdout);
	fflush(stderr);
	_exit(status);
}
