/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ddbgwin.c - the debugger's own window of the macOS (SDL2) back end
 *             (OS_DebugWindowOpen and friends, inc/bgi/os.h): a second
 *             top-level window placed beside the game's that the debugger
 *             (src/dbg) draws its panes into and whose input it gets
 *             through the debug_* handlers
 */
#include "dui_internal.h"

static DuiWin_t gDbg;    // win.win is NULL while there is none
static int gDbgW, gDbgH; // its client size in pixels

/* open the window (w x h, fixed size, shown at once) next to the main
 * window - to its right when the screen has room, else to its left, else
 * at (40, 40); 0 without a display or when it exists already */
int OS_DebugWindowOpen(int w, int h, const char* title)
{
	SDL_Rect bounds;
	SDL_Window* main = Dwin_MainWindow();
	int x = 40, y = 40;
	if(!Dwin_Alive() || gDbg.open || w <= 0 || h <= 0)
		return 0;
	if(main != NULL)
	{
		int mx = 0, my = 0, mw = 0, mh = 0;
		SDL_GetWindowPosition(main, &mx, &my);
		SDL_GetWindowSize(main, &mw, &mh);
		if(SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(main), &bounds) == 0)
		{
			if(mx + mw + 8 + w <= bounds.x + bounds.w)
			{
				x = mx + mw + 8;
				y = my;
			}
			else if(mx - 8 - w >= bounds.x)
			{
				x = mx - 8 - w;
				y = my;
			}
			else
			{
				x = bounds.x + 40;
				y = bounds.y + 40;
			}
		}
	}
	if(!DuiWin_Open(&gDbg, title, x, y, w, h, 0))
		return 0;
	Dui_FillRect(gDbg.pix, w, w, h, 0, 0, w, h, 0x000000);
	DuiWin_Present(&gDbg);
	gDbgW = w;
	gDbgH = h;
	return 1;
}

void OS_DebugWindowClose(void)
{
	if(!gDbg.open)
		return;
	DuiWin_Close(&gDbg);
	gDbgW = gDbgH = 0;
}

int OS_DebugWindowAlive(void)
{
	return gDbg.open;
}

// the surface at 1:1 from the window's top-left corner, cut to the window
void OS_DebugWindowPresent(const OsSurface_t* s)
{
	int x, y, w, h;
	if(!gDbg.open || s == NULL || s->pixels == NULL)
		return;
	w = s->width < gDbgW ? s->width : gDbgW;
	h = s->height < gDbgH ? s->height : gDbgH;
	if(w <= 0 || h <= 0)
		return;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			gDbg.pix[(size_t)y * gDbg.w + x] = Dui_SurfacePixel(s, x, y);
	DuiWin_Present(&gDbg);
}

/* the events of the debugger's window: keys with their characters, the
 * mouse, the close box.  The debugger's input is ASCII: the character of
 * a press is the printable Latin-1 code point (0 for any other key). */
int DuiDebug_HandleEvent(SDL_Event* e)
{
	Uint32 id;
	if(!gDbg.open)
		return 0;
	id = DuiWin_Id(&gDbg);
	if(id == 0)
		return 0;
	switch(e->type)
	{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		{
			int vk, ch = 0;
			if(e->key.windowID != id)
				return 0;
			vk = Dwin_TranslateVk(e->key.keysym.sym);
			if(e->type == SDL_KEYDOWN && e->key.keysym.sym >= 0x20 && e->key.keysym.sym < 0x7f)
				ch = (int)e->key.keysym.sym;
			if(gDwinHandlers.debug_key != NULL && (vk != 0 || ch != 0))
				gDwinHandlers.debug_key(vk, e->type == SDL_KEYDOWN, ch);
			return 1;
		}
		case SDL_TEXTINPUT:
			if(e->text.windowID != id)
				return 0;
			return 1; // the key press already carried the character
		case SDL_MOUSEMOTION:
			if(e->motion.windowID != id)
				return 0;
			if(gDwinHandlers.debug_mouse != NULL)
				gDwinHandlers.debug_mouse(e->motion.x, e->motion.y, -1, 0);
			return 1;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			int b;
			if(e->button.windowID != id)
				return 0;
			b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
				: e->button.button == SDL_BUTTON_MIDDLE                               ? 2
																					  : -1;
			if(b >= 0 && gDwinHandlers.debug_mouse != NULL)
				gDwinHandlers.debug_mouse(e->button.x, e->button.y, b, e->type == SDL_MOUSEBUTTONDOWN);
			return 1;
		}
		case SDL_MOUSEWHEEL:
			if(e->wheel.windowID != id)
				return 0;
			if(gDwinHandlers.debug_wheel != NULL)
				gDwinHandlers.debug_wheel(e->wheel.y * 120);
			return 1;
		case SDL_WINDOWEVENT:
			if(e->window.windowID != id)
				return 0;
			if(e->window.event == SDL_WINDOWEVENT_CLOSE && gDwinHandlers.debug_close != NULL)
				gDwinHandlers.debug_close();
			return 1;
		default:
			return 0;
	}
}
