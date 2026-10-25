/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the macOS (SDL2) main window, rendering and event pump
 *
 * The engine keeps one main window, draws into plain-memory surfaces and
 * hands them over between OS_WindowGetDc and OS_WindowReleaseDc; the
 * blits compose into a staging surface the size of the picture and the
 * release presents it (see darwin_internal.h).  Events are translated
 * from SDL into the OsEventHandlers callbacks, one message per
 * OS_PumpMessages call: keys as Windows virtual-key codes, mouse
 * positions in picture coordinates once the picture is scaled into the
 * client area.  The close box and Cmd+Q arrive as SDL_QUIT and are
 * routed through close_request, so the engine decides what closing means.
 */

#include "darwin_internal.h"
#include "dui_internal.h"

#include <SDL.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

OsEventHandlers_t gDwinHandlers;
int gDwinQuitPosted;
int gDwinPictureW = 800;
int gDwinPictureH = 600;

// the SDL objects behind the single main window; window/renderer/texture
// are created in OS_WindowCreate and released in OS_WindowDestroy
static SDL_Window* gWin;
static SDL_Renderer* gRen;
static SDL_Texture* gTex;

// the picture the engine addresses: blits compose here, at picture size,
// and Dwin_Present scales it into the client area (aspect kept, centred)
static SDL_Surface* gStage;

/* A whole-frame blit (the engine's present of its composed canvas) is
 * uploaded straight from the source into the texture instead of being copied
 * into the staging surface first and from there into the texture: one full
 * picture copy per frame less.  The staging surface then holds an older
 * picture, which is fine for the window - it is only read again by the two
 * diagnostics that dump it (BGI_DUMP_STAGE, BGI_SCREENSHOT), and those switch
 * the copy back on (WantStage()).  BGI_FAST_PRESENT=0 disables the shortcut
 * for an A/B measurement. */
static int gTexFresh; // the texture already holds the picture Dwin_Present shows
static int gStageWanted = -1; // -1 unknown: does a diagnostic read the staging surface?

// the token OS_WindowGetDc hands out; the DC is opaque to the engine
static int gDcToken;

static int gAlive;
static int gShown;
static int gFullscreen;
static int gClientW = 800;
static int gClientH = 600;
static int gCursorShown = 1;
static int gDragAccept;
static const OsWindowDesc_t* gDesc;

// ---- what the back end's own UI files need (dui_internal.h) ------------------------------------

SDL_Window* Dwin_MainWindow(void)
{
	return gWin;
}

SDL_Renderer* Dwin_MainRenderer(void)
{
	return gRen;
}

SDL_Surface* Dwin_Stage(void)
{
	return gStage;
}

int Dwin_Alive(void)
{
	return gAlive;
}

Uint32 Dwin_MainWindowId(void)
{
	return gWin != NULL ? SDL_GetWindowID(gWin) : 0;
}

// the virtual-key down state OS_KeyState and OS_KeyboardState report; it is
// kept here because the pump is the only place that sees the keys go by
static uint8_t gKeyDown[256];

// ---------------------------------------------------------------------------
// Key translation
// ---------------------------------------------------------------------------

/* Translate an SDL keycode into a Windows virtual-key code; the engine's
 * input layer is written against the VK_* values ("keys are Windows
 * virtual-key codes", bgi/os.h).  Keys without a VK counterpart return 0
 * and are dropped. */
int Dwin_TranslateVk(SDL_Keycode k)
{
	switch(k)
	{
	case SDLK_BACKSPACE: return 0x08;
	case SDLK_TAB: return 0x09;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: return 0x0D;
	case SDLK_ESCAPE: return 0x1B;
	case SDLK_SPACE: return 0x20;
	case SDLK_PAGEUP: return 0x21;
	case SDLK_PAGEDOWN: return 0x22;
	case SDLK_END: return 0x23;
	case SDLK_HOME: return 0x24;
	case SDLK_LEFT: return 0x25;
	case SDLK_UP: return 0x26;
	case SDLK_RIGHT: return 0x27;
	case SDLK_DOWN: return 0x28;
	case SDLK_INSERT: return 0x2D;
	case SDLK_DELETE: return 0x2E;
	case SDLK_PRINTSCREEN: return 0x2C;
	case SDLK_LSHIFT:
	case SDLK_RSHIFT: return 0x10;
	case SDLK_LCTRL:
	case SDLK_RCTRL: return 0x11;
	case SDLK_LALT:
	case SDLK_RALT: return 0x12;
	case SDLK_CAPSLOCK: return 0x14;
	case SDLK_F1: return 0x70;
	case SDLK_F2: return 0x71;
	case SDLK_F3: return 0x72;
	case SDLK_F4: return 0x73;
	case SDLK_F5: return 0x74;
	case SDLK_F6: return 0x75;
	case SDLK_F7: return 0x76;
	case SDLK_F8: return 0x77;
	case SDLK_F9: return 0x78;
	case SDLK_F10: return 0x79;
	case SDLK_F11: return 0x7A;
	case SDLK_F12: return 0x7B;
	default:
		// letters and digits: the VK codes are their ASCII capitals
		if(k >= SDLK_a && k <= SDLK_z)
			return k - SDLK_a + 0x41;
		if(k >= SDLK_0 && k <= SDLK_9)
			return k;
		if(k >= SDLK_KP_1 && k <= SDLK_KP_9)
			return k - SDLK_KP_1 + 0x61;
		if(k == SDLK_KP_0)
			return 0x60;
		return 0;
	}
}

// ---------------------------------------------------------------------------
// Init and shutdown
// ---------------------------------------------------------------------------

// OS_Init lives in process.c with the directories; it calls these two so the
// SDL subsystems come up beside them
int Dwin_Init(void)
{
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
	{
		SDL_Log("SDL_Init: %s", SDL_GetError());
		return 0;
	}
	// HALFTONE-stretch stand-in: the renderer scales the picture linearly
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	return 1;
}

void Dwin_Shutdown(void)
{
	if(gStage != NULL)
	{
		SDL_FreeSurface(gStage);
		gStage = NULL;
	}
	if(gTex != NULL)
	{
		SDL_DestroyTexture(gTex);
		gTex = NULL;
	}
	if(gRen != NULL)
	{
		SDL_DestroyRenderer(gRen);
		gRen = NULL;
	}
	if(gWin != NULL)
	{
		SDL_DestroyWindow(gWin);
		gWin = NULL;
	}
	SDL_Quit();
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

int OS_WindowCreate(const OsWindowDesc_t* d)
{
	// the engine raises `created` from this call, so the handlers must be
	// in place; remember the desc for the show / title calls
	gDesc = d;
	int flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if(gFullscreen)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	gWin = SDL_CreateWindow(d->title, d->x, d->y, d->width, d->height, flags);
	if(gWin == NULL)
	{
		if(getenv("BGI_DEBUG_OS")) fprintf(stderr, "[os] SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 0;
	}
	gRen = SDL_CreateRenderer(gWin, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if(gRen == NULL)
		gRen = SDL_CreateRenderer(gWin, -1, 0); // software fallback
	gClientW = d->width;
	gClientH = d->height;
	gAlive = 1;
	Dui_EditCreate(); // the hidden 1 x 1 text entry of "B0 2x"
	if(gDwinHandlers.created != NULL)
		gDwinHandlers.created();
	return 1;
}

void OS_WindowDestroy(void)
{
	OS_EditDestroy();
	gAlive = 0;
	if(gDwinHandlers.destroyed != NULL)
		gDwinHandlers.destroyed();
	if(gTex != NULL)
	{
		SDL_DestroyTexture(gTex);
		gTex = NULL;
	}
	if(gRen != NULL)
	{
		SDL_DestroyRenderer(gRen);
		gRen = NULL;
	}
	if(gWin != NULL)
	{
		SDL_DestroyWindow(gWin);
		gWin = NULL;
	}
}

int OS_WindowAlive(void)
{
	return gAlive;
}

void OS_WindowShow(int show)
{
	if(gWin == NULL)
		return;
	if(show)
	{
		SDL_ShowWindow(gWin);
		SDL_RaiseWindow(gWin);
		if(!gShown)
		{
			gShown = 1;
			// ShowWindow + UpdateWindow triggers a WM_PAINT on Win32; the
			// first present shows the black picture before the engine runs
			Dwin_Present();
		}
	}
	else
		SDL_HideWindow(gWin);
}

void OS_WindowSetTitle(const char* t)
{
	if(gWin == NULL)
		return;
	// NULL clears the game's part of the title (OsCommon_MainTitle stays)
	SDL_SetWindowTitle(gWin, t != NULL ? t : "OpenBGI");
}

void OS_WindowMove(int x, int y)
{
	if(gWin != NULL)
		SDL_SetWindowPosition(gWin, x, y);
}

void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	if(gWin == NULL)
	{
		*x = *y = *w = *h = 0;
		return;
	}
	int lx, ly, cw, ch;
	SDL_GetWindowPosition(gWin, &lx, &ly);
	SDL_GetWindowSize(gWin, &cw, &ch);
	// outer rectangle: the client area plus the window frame SDL reports
	int left, top, right, bottom;
	SDL_GetWindowBordersSize(gWin, &top, &left, &bottom, &right);
	*x = lx - left;
	*y = ly - top;
	*w = cw + left + right;
	*h = ch + top + bottom;
}

int OS_WindowMinimized(void)
{
	return gWin != NULL && (SDL_GetWindowFlags(gWin) & SDL_WINDOW_MINIMIZED) != 0;
}

int OS_WindowActive(void)
{
	return gWin != NULL && (SDL_GetWindowFlags(gWin) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void OS_WindowMinimize(void)
{
	if(gWin != NULL)
		SDL_MinimizeWindow(gWin);
}

void OS_WindowClose(void)
{
	// like posting WM_CLOSE: close_request from the next pump
	Dwin_RequestClose();
}

void OS_ScreenSize(int* w, int* h)
{
	SDL_DisplayMode m;
	if(SDL_GetDesktopDisplayMode(gWin != NULL ? SDL_GetWindowDisplayIndex(gWin) : 0, &m) == 0)
	{
		*w = m.w;
		*h = m.h;
	}
	else
	{
		*w = 800;
		*h = 600;
	}
}

// the engine does not manage the frame on SDL: the window manager owns it
void OS_FrameMetrics(int* fw, int* fh)
{
	*fw = 0;
	*fh = 0;
}

int OS_FrameTop(void)
{
	return 0;
}

void OS_ImeStatus(int open)
{
	(void)open; // the IME status window is an Windows-only affordance
}

// ---------------------------------------------------------------------------
// Display modes
// ---------------------------------------------------------------------------

int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	(void)bpp; // SDL renders at the desktop depth; the engine's surfaces carry their own bpp
	gFullscreen = fullscreen;
	if(gWin == NULL)
		return 1; // remembered for OS_WindowCreate
	if(fullscreen)
	{
		// a borderless window over a display mode: SDL scales the desktop
		// mode instead of switching it, which is what modern macs allow
		if(w > 0 && h > 0)
			SDL_SetWindowSize(gWin, w, h);
		return SDL_SetWindowFullscreen(gWin, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
	}
	return 1;
}

void OS_DisplayRestore(void)
{
	if(gWin != NULL)
		SDL_SetWindowFullscreen(gWin, 0);
	gFullscreen = 0;
}

void OS_WindowSetFullscreen(int modeW, int modeH)
{
	(void)modeW;
	(void)modeH;
	if(gWin != NULL)
	{
		SDL_SetWindowFullscreen(gWin, SDL_WINDOW_FULLSCREEN_DESKTOP);
		gFullscreen = 1;
		SDL_SetRenderDrawColor(gRen, 0, 0, 0, 255);
		SDL_RenderClear(gRen);
	}
}

void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	if(gWin == NULL)
		return;
	SDL_SetWindowFullscreen(gWin, 0);
	gFullscreen = 0;
	SDL_SetWindowPosition(gWin, x, y);
	SDL_SetWindowSize(gWin, clientW, clientH);
	gClientW = clientW;
	gClientH = clientH;
}

void OS_WindowSetPicture(int w, int h)
{
	gDwinPictureW = w;
	gDwinPictureH = h;
	if(gWin == NULL)
		return;
	if(gStage != NULL)
		SDL_FreeSurface(gStage);
	gStage = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(gStage, NULL, 0xFF000000);
	if(gTex != NULL)
		SDL_DestroyTexture(gTex);
	gTex = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
	// the picture letterboxed into the client area, aspect kept: the
	// renderer's logical size does exactly that, and the mouse maps back
	// through SDL_ConvertEventToRenderCoordinates
	SDL_RenderSetLogicalSize(gRen, w, h);
}

// ---------------------------------------------------------------------------
// Device context: blits into the staging surface, present on release
// ---------------------------------------------------------------------------

OsDc_t* OS_WindowGetDc(void)
{
	return gAlive ? (OsDc_t*)&gDcToken : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	(void)dc;
	// "the engine takes one for a present and releases it right after":
	// the release is the end of a present
	Dwin_Present();
}

void Dwin_Present(void); /* the SDL present, defined below */
static int WantStage(void); /* does a diagnostic read the staging surface? (defined below) */

void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h,
	const OsSurface_t* src, int srcX, int srcY)
{
	if(src == NULL)
		return;
	if(dc != NULL && Dui_ChildBlit(dc, dstX, dstY, w, h, src, srcX, srcY, w, h))
		return;
	if(gStage == NULL)
		return;
	// clip to the picture; the engine stays in picture coordinates
	if(dstX < 0)
	{
		srcX -= dstX;
		w += dstX;
		dstX = 0;
	}
	if(dstY < 0)
	{
		srcY -= dstY;
		h += dstY;
		dstY = 0;
	}
	if(dstX + w > gStage->w)
		w = gStage->w - dstX;
	if(dstY + h > gStage->h)
		h = gStage->h - dstY;
	if(w <= 0 || h <= 0)
		return;
	/* the whole canvas, as the engine's present of it: upload it straight
	 * from the source (the clip above changed nothing when the blit is whole) */
	if(w == gStage->w && h == gStage->h && dstX == 0 && dstY == 0 && srcX == 0 && srcY == 0 &&
		src->bpp == 32 && src->width >= w && src->height >= h && !WantStage())
	{
		if(gTex != NULL && SDL_UpdateTexture(gTex, NULL, src->pixels, src->pitch) == 0)
		{
			gTexFresh = 1;
			Dwin_Present(); /* a blit into the window surface is a frame here */
			return;
		}
	}
	SDL_LockSurface(gStage);
	uint8_t* dstBase = (uint8_t*)gStage->pixels + dstY * gStage->pitch + dstX * 4;
	const uint8_t* srcBase = (const uint8_t*)src->pixels + (size_t)srcY * src->pitch;
	if(src->bpp == 32)
	{
		// 0x00RRGGBB reads as the same ARGB8888 little-endian pixel
		for(int y = 0; y < h; y++)
			memcpy(dstBase + (size_t)y * gStage->pitch,
				srcBase + (size_t)y * src->pitch + (size_t)srcX * 4, (size_t)w * 4);
	}
	else
	{
		// 16 bpp RGB555 unpacks into 32 bpp, opaque
		for(int y = 0; y < h; y++)
		{
			const uint16_t* s = (const uint16_t*)(srcBase + (size_t)y * src->pitch) + srcX;
			uint32_t* d = (uint32_t*)(dstBase + (size_t)y * gStage->pitch);
			for(int x = 0; x < w; x++)
			{
				uint32_t v = s[x];
				uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
				r = r << 3 | r >> 2;
				g = g << 3 | g >> 2;
				b = b << 3 | b >> 2;
				d[x] = 0xFF000000 | r << 16 | g << 8 | b;
			}
		}
	}
	SDL_UnlockSurface(gStage);
	Dwin_Present(); /* a blit into the window surface is a frame here */
}

void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src,
	const int* clip)
{
	if(src == NULL)
		return;
	if(dc != NULL && Dui_ChildBlit(dc, dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height))
		return;
	if(gStage == NULL)
		return;
	// the whole surface stretches into the destination rectangle; `clip`
	// ({l, t, r, b}, r/b exclusive) cuts the output in window coordinates
	if(clip != NULL)
	{
		int cl = clip[0], ct = clip[1], cr = clip[2], cb = clip[3];
		if(dstX < cl)
		{
			dstW -= cl - dstX;
			dstX = cl;
		}
		if(dstY < ct)
		{
			dstH -= ct - dstY;
			dstY = ct;
		}
		if(dstX + dstW > cr)
			dstW = cr - dstX;
		if(dstY + dstH > cb)
			dstH = cb - dstY;
	}
	if(dstW <= 0 || dstH <= 0)
		return;
	SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormatFrom(src->pixels, src->width, src->height,
		src->bpp, src->pitch,
		src->bpp == 32 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB555);
	if(tmp == NULL)
		return;
	// SDL_BlitScaled scales nearest-neighbour; the renderer's linear hint
	// takes over whenever the picture itself is scaled for the screen
	SDL_Rect dst = {dstX, dstY, dstW, dstH};
	SDL_BlitScaled(tmp, NULL, gStage, &dst);
	SDL_FreeSurface(tmp);
	Dwin_Present(); /* a blit into the window surface is a frame here */
}

/* 1 when a diagnostic reads the staging surface: the blits then keep it
 * up to date (BGI_DUMP_STAGE dumps it, BGI_SCREENSHOT copies it) */
static int WantStage(void)
{
	if(gStageWanted < 0)
	{
		const char* fast = getenv("BGI_FAST_PRESENT");
		const char* stage = getenv("BGI_DUMP_STAGE");
		const char* shot = getenv("BGI_SCREENSHOT");
		gStageWanted = fast != NULL && fast[0] == '0';
		gStageWanted = gStageWanted || (stage != NULL && stage[0] != 0) || (shot != NULL && shot[0] != 0);
	}
	return gStageWanted;
}

void OS_WindowFillBlack(void)
{
	if(gStage != NULL)
		SDL_FillRect(gStage, NULL, 0xFF000000);
	if(gRen != NULL)
	{
		SDL_SetRenderDrawColor(gRen, 0, 0, 0, 255);
		SDL_RenderClear(gRen);
		SDL_RenderPresent(gRen);
	}
}

void Dwin_Present(void)
{
	if(gRen == NULL || gTex == NULL || gStage == NULL)
		return;
	if(gTexFresh)
		gTexFresh = 0; // the blit uploaded the picture itself
	else
		SDL_UpdateTexture(gTex, NULL, gStage->pixels, gStage->pitch);
	SDL_RenderClear(gRen);
	SDL_RenderCopy(gRen, gTex, NULL, NULL);
	Dui_PresentEditOverlay(gRen); // the text entry of "B0 2x", when it is shown

	/* BGI_DUMP_RENDERED=<path>: read back what the renderer holds, which is the
	 * picture the present path produced - the proof that a window would show the
	 * game, without needing a screen to photograph */
	{
		const char* wantR = getenv("BGI_DUMP_RENDERED");
		static int doneR;
		if(wantR != NULL && wantR[0] != 0 && !doneR)
		{
			static uint32_t firstR;
			static int framesR;
			if(framesR == 0)
				firstR = OS_TicksMs();
			framesR++;
			const char* at = getenv("BGI_DUMP_RENDERED_MS");
			const uint32_t wantMs = at ? (uint32_t)atoi(at) : 8000u;
			if(OS_TicksMs() - firstR >= wantMs)
			{
				int rw = 0, rh = 0;
				SDL_GetRendererOutputSize(gRen, &rw, &rh);
				if(rw > 0 && rh > 0)
				{
					SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, rw, rh, 32,
						SDL_PIXELFORMAT_ARGB8888);
					if(out != NULL)
					{
						if(SDL_RenderReadPixels(gRen, NULL, SDL_PIXELFORMAT_ARGB8888,
							out->pixels, out->pitch) == 0 && SDL_SaveBMP(out, wantR) == 0)
							fprintf(stderr, "bgi: renderer read back to %s (%dx%d)\n", wantR, rw, rh);
						SDL_FreeSurface(out);
					}
				}
				doneR = 1;
			}
		}
	}

	SDL_RenderPresent(gRen);

	/* BGI_SCREENSHOT: copy the presented pixels and write them out.  The copy
	 * matters: the engine may draw into the surface again while a file is being
	 * written, and saving straight from it was a crash waiting for the right
	 * moment. */
	{
		static int frames;
		static int done;
		if(getenv("BGI_DEBUG_BOOT"))
		{
			fprintf(stderr, "[present] frame %d\n", frames);
			fflush(stderr);
		}
		const char* want = getenv("BGI_SCREENSHOT");
		if(want != NULL && want[0] != 0 && !done)
		{
			/* BGI_SCREENSHOT_MS: when to take it, counted from the first present;
			 * a title needs time and a key before it has anything to show */
			static uint32_t first;
			if(frames == 0)
				first = OS_TicksMs();
			frames++;
			{
				const char* at = getenv("BGI_SCREENSHOT_MS");
				const uint32_t want_ms = at ? (uint32_t)atoi(at) : 3000u;
				if(OS_TicksMs() - first >= want_ms)
				{
				const int pitch = gStage->pitch;
				const int h = gStage->h;
				uint8_t* copy = (uint8_t*)malloc((size_t)pitch * h);
				if(copy != NULL)
				{
					memcpy(copy, gStage->pixels, (size_t)pitch * h);
					SDL_Surface* out = SDL_CreateRGBSurfaceWithFormatFrom(copy, gStage->w, h, 32,
						pitch, SDL_PIXELFORMAT_ARGB8888);
					if(out != NULL)
					{
						if(SDL_SaveBMP(out, want) == 0)
							fprintf(stderr, "bgi: wrote %s (%dx%d, frame %d)\n", want,
								(int)gStage->w, h, frames);
						SDL_FreeSurface(out);
					}
					free(copy);
				}
				done = 1;
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gDwinHandlers = *h;
}

void Dwin_RequestClose(void)
{
	// the engine decides: close_request first, quit only without one
	if(gDwinHandlers.close_request != NULL)
		gDwinHandlers.close_request();
	else
		gDwinQuitPosted = 1;
}

void OS_PostQuit(void)
{
	gDwinQuitPosted = 1;
}

void OS_DragAccept(int accept)
{
	gDragAccept = accept;
}

/* Map a client-area point into picture coordinates; the picture is shown
 * centred in the client area with its aspect kept, so this is the inverse
 * of the renderer's letterbox. */
void Dwin_ClientToPicture(int* x, int* y)
{
	if(gDwinPictureW <= 0 || gDwinPictureH <= 0 || gClientW <= 0 || gClientH <= 0)
		return;
	double scale = (double)gClientW / gDwinPictureW;
	double sh = (double)gClientH / gDwinPictureH;
	if(sh < scale)
		scale = sh;
	int drawW = (int)(gDwinPictureW * scale + 0.5);
	int drawH = (int)(gDwinPictureH * scale + 0.5);
	int offX = (gClientW - drawW) / 2;
	int offY = (gClientH - drawH) / 2;
	*x = (int)((*x - offX) / scale);
	*y = (int)((*y - offY) / scale);
}

/* Dispatch one SDL event to the engine's handlers.  Window events raise
 * the WM_*-shaped callbacks; mouse positions are converted into picture
 * coordinates first. */
static void DispatchEvent(SDL_Event* e)
{
	OsEventHandlers_t* h = &gDwinHandlers;
	if(Dui_HandleEvent(e))
		return;
	switch(e->type)
	{
	case SDL_QUIT:
		Dwin_RequestClose();
		return;
	case SDL_WINDOWEVENT:
		switch(e->window.event)
		{
		case SDL_WINDOWEVENT_EXPOSED:
			if(h->paint != NULL)
				h->paint();
			return;
		case SDL_WINDOWEVENT_MINIMIZED:
			if(h->sized != NULL)
				h->sized(1);
			return;
		case SDL_WINDOWEVENT_RESTORED:
		case SDL_WINDOWEVENT_SIZE_CHANGED:
			if(gWin != NULL)
				SDL_GetWindowSize(gWin, &gClientW, &gClientH);
			if(h->sized != NULL)
				h->sized(0);
			return;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			if(h->activate != NULL)
				h->activate(1);
			return;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			if(h->activate != NULL)
				h->activate(0);
			return;
		case SDL_WINDOWEVENT_CLOSE:
			Dwin_RequestClose();
			return;
		}
		return;
	case SDL_KEYDOWN:
	{
		int vk = Dwin_TranslateVk(e->key.keysym.sym);
		if(vk == 0)
			return;
		gKeyDown[vk & 0xFF] = 1;
		// Alt+key and F10 are system keys on Windows (WM_SYSKEYDOWN)
		if((e->key.keysym.mod & KMOD_ALT) != 0 || vk == 0x79)
		{
			if(h->syskey != NULL)
				h->syskey(vk);
			return;
		}
		if(h->key_down != NULL)
			h->key_down(vk, e->key.repeat);
		return;
	}
	case SDL_KEYUP:
	{
		int vk = Dwin_TranslateVk(e->key.keysym.sym);
		if(vk == 0)
			return;
		gKeyDown[vk & 0xFF] = 0;
		if(h->key_up != NULL)
			h->key_up(vk);
		return;
	}
	case SDL_MOUSEMOTION:
		if(h->mouse_move != NULL)
		{
			int x = e->motion.x, y = e->motion.y;
			Dwin_ClientToPicture(&x, &y);
			h->mouse_move(x, y);
		}
		return;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	{
		if(h->double_click != NULL && e->type == SDL_MOUSEBUTTONDOWN && e->button.clicks == 2)
			h->double_click(e->button.button - 1);
		if(h->mouse_button != NULL)
		{
			int x = e->button.x, y = e->button.y;
			Dwin_ClientToPicture(&x, &y);
			h->mouse_button(e->button.button - 1, e->type == SDL_MOUSEBUTTONDOWN, x, y);
		}
		return;
	}
	case SDL_MOUSEWHEEL:
		if(h->mouse_wheel != NULL)
			h->mouse_wheel(e->wheel.y * 120); // WHEEL_DELTA units, + = away
		return;
	case SDL_DROPFILE:
		if(gDragAccept && h->drop_file != NULL && e->drop.file != NULL)
		{
			h->drop_file(e->drop.file);
			SDL_free(e->drop.file);
		}
		else if(e->drop.file != NULL)
			SDL_free(e->drop.file);
		return;
	}
}

// one event through the engine's handlers (the dialogs' modal loops call this)
void Dwin_DispatchEvent(SDL_Event* e)
{
	DispatchEvent(e);
}

int OS_PumpMessages(void)
{
	// a posted quit is delivered once, from a later pump (WM_QUIT)
	if(gDwinQuitPosted)
	{
		gDwinQuitPosted = 0;
		if(gDwinHandlers.quit != NULL)
			gDwinHandlers.quit();
		return 1;
	}
	SDL_Event e;
	if(!SDL_PollEvent(&e))
		return 0;
	DispatchEvent(&e);
	return 1;
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

void OS_CursorSetShape(int shape)
{
	(void)shape; // the back end keeps the system arrow for now
}

void OS_CursorShow(int show)
{
	gCursorShown = show;
	SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

void OS_CursorGetPos(int* x, int* y)
{
	// screen coordinates, as GetCursorPos gives them
	SDL_GetGlobalMouseState(x, y);
}

void OS_CursorSetPos(int x, int y)
{
	SDL_WarpMouseGlobal(x, y);
}

void OS_ScreenToClient(int* x, int* y)
{
	if(gWin == NULL)
		return;
	int wx, wy;
	SDL_GetWindowPosition(gWin, &wx, &wy);
	int left, top;
	SDL_GetWindowBordersSize(gWin, &top, &left, NULL, NULL);
	*x -= wx + left;
	*y -= wy + top;
	int cw, ch;
	SDL_GetWindowSize(gWin, &cw, &ch);
	gClientW = cw;
	gClientH = ch;
	// the engine keeps working in picture coordinates
	Dwin_ClientToPicture(x, y);
}

void OS_ClientToScreen(int* x, int* y)
{
	// the inverse of OS_ScreenToClient: picture coordinates back to the screen
	if(gWin == NULL)
		return;
	int wx, wy, left, top, cw, ch;
	SDL_GetWindowPosition(gWin, &wx, &wy);
	SDL_GetWindowBordersSize(gWin, &top, &left, NULL, NULL);
	SDL_GetWindowSize(gWin, &cw, &ch);
	if(gRen != NULL)
		SDL_RenderSetLogicalSize(gRen, gDwinPictureW, gDwinPictureH);
	// undo the letterbox: picture point -> client point -> screen point
	if(gDwinPictureW > 0 && gDwinPictureH > 0 && cw > 0 && ch > 0)
	{
		double scale = (double)cw / gDwinPictureW;
		double sh = (double)ch / gDwinPictureH;
		if(sh < scale)
			scale = sh;
		int drawW = (int)(gDwinPictureW * scale + 0.5);
		int drawH = (int)(gDwinPictureH * scale + 0.5);
		*x = (int)(*x * scale) + (cw - drawW) / 2;
		*y = (int)(*y * scale) + (ch - drawH) / 2;
	}
	*x += wx + left;
	*y += wy + top;
}

// the mouse is not swapped on macOS: the system setting has no counterpart
int OS_MouseButtonsSwapped(void)
{
	return 0;
}

// GetAsyncKeyState & 0x8000: the high bit when the key is down
int OS_KeyState(int vk)
{
	if(vk < 0 || vk > 255)
		return 0;
	/* BGI_AUTO_KEY_MS=<n>: press Enter once, n milliseconds in, for a fifth of a
	 * second - a diagnostic that lets a title screen that waits for a key move on
	 * when the machine has nobody at the keyboard */
	{
		static uint32_t started;
		static int armed = -1;
		if(armed < 0)
		{
			const char* at = getenv("BGI_AUTO_KEY_MS");
			armed = at ? atoi(at) : 0;
			started = OS_TicksMs();
		}
		if(armed > 0 && vk == 0x0D)
		{
			uint32_t now = OS_TicksMs();
			if(now - started >= (uint32_t)armed && now - started < (uint32_t)armed + 200)
				return (int)0x80000000;
		}
	}
	return gKeyDown[vk] ? (int)0x80000000 : 0;
}

// GetKeyboardState: bit 7 marks a down key, the rest of the byte is unused
void OS_KeyboardState(uint8_t out[256])
{
	memset(out, 0, 256);
	for(int i = 0; i < 256; i++)
		if(gKeyDown[i])
			out[i] = 0x80;
}

// SetClipboardData(CF_TEXT): the engine's text is Shift-JIS, the clipboard
// holds UTF-8
int OS_ClipboardSetText(const char* text)
{
	char utf[0x2000];
	if(text == NULL)
		return 0;
	OsPosix_SjisToUtf8(text, utf, sizeof utf);
	return SDL_SetClipboardText(utf) == 0;
}

// os_posix.h: the "80 5A" close instruction and signal handlers land here
void OsPosix_RequestClose(void)
{
	Dwin_RequestClose();
}


/* BGI_DUMP_STAGE=<path>: write the drawing surface straight out, without going
 * through a present.  A game that draws but never presents is otherwise
 * invisible on a machine with no screen: this reads the surface the engine
 * composes into, so "is anything on it" can be answered directly. */
void Dwin_DumpStage(const char* path)
{
	if(gStage == NULL || path == NULL)
		return;
	const int w = gStage->w;
	const int h = gStage->h;
	const int pitch = gStage->pitch;
	if(w <= 0 || h <= 0)
		return;

	const int rowBytes = w * 3;
	const int pad = (4 - (rowBytes % 4)) % 4;
	const uint32_t dataSize = (uint32_t)(rowBytes + pad) * (uint32_t)h;
	const uint32_t fileSize = 54 + dataSize;

	FILE* f = fopen(path, "wb");
	if(f == NULL)
		return;
	uint8_t header[54] = {0};
	header[0] = 'B'; header[1] = 'M';
	memcpy(header + 2, &fileSize, 4);
	uint32_t offset = 54;
	memcpy(header + 10, &offset, 4);
	uint32_t infoSize = 40;
	memcpy(header + 14, &infoSize, 4);
	memcpy(header + 18, &w, 4);
	memcpy(header + 22, &h, 4);
	uint16_t planes = 1, bpp = 24;
	memcpy(header + 26, &planes, 2);
	memcpy(header + 28, &bpp, 2);
	memcpy(header + 34, &dataSize, 4);
	fwrite(header, 1, sizeof header, f);

	uint8_t* row = (uint8_t*)calloc(1, (size_t)rowBytes + pad);
	if(row == NULL)
	{
		fclose(f);
		return;
	}
	for(int y = h - 1; y >= 0; y--)
	{
		const uint8_t* src = (const uint8_t*)gStage->pixels + (size_t)pitch * y;
		for(int x = 0; x < w; x++)
		{
			// the surface is 32-bit with the alpha in the top byte
			row[x * 3 + 0] = src[x * 4 + 0];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 2];
		}
		fwrite(row, 1, (size_t)rowBytes + pad, f);
	}
	free(row);
	fclose(f);
	fprintf(stderr, "bgi: dumped the surface to %s (%dx%d)\n", path, w, h);
	fflush(stderr);
}
