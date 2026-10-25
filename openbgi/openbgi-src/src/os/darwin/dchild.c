/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dchild.c - the pop-up child windows of the macOS (SDL2) back end
 *            ("B0 1x" of the scripts: small top-level windows the engine
 *            draws into like the main one) (inc/bgi/os.h)
 *
 * Each of the eight slots is a transient SDL top-level window of fixed
 * size placed at the outer origin the engine asks for (the window manager
 * adds its own frame, so the engine's OS_FrameMetrics are zero here).
 * The engine keeps a back buffer per window and hands it over through
 * OS_ChildGetDc / OS_StretchToWindow, which is where a child's DC is
 * recognised; its `child_paint` event asks the engine to present the
 * buffer again (on show, on a move and when the window is uncovered).
 * The close box is refused, as the original's window procedure does.
 */
#include "dui_internal.h"

#define DUI_CHILD_MAX 8
#define CHILD_DC_MAGIC 0x43686c64u // "Chld"

typedef struct DuiChildDc
{
	uint32_t magic;
	int index;
} DuiChildDc_t;

typedef struct DuiChild
{
	int used, shown, closing;
	int index;
	int x, y, w, h; // the outer rectangle (the client area: the window manager frames it)
	DuiWin_t win;
	DuiChildDc_t dc;
} DuiChild_t;

static DuiChild_t gChildren[DUI_CHILD_MAX];

// the slot a DC token names, NULL when it is not one of ours
static DuiChild_t* ChildByDc(OsDc_t* dc)
{
	DuiChildDc_t* t = (DuiChildDc_t*)dc;
	DuiChild_t* c;
	if(t == NULL || t->magic != CHILD_DC_MAGIC || t->index < 0 || t->index >= DUI_CHILD_MAX)
		return NULL;
	c = &gChildren[t->index];
	if(!c->used || &c->dc != t)
		return NULL;
	return c;
}

// ask the engine to present its back buffer of that slot now
static void ChildPaint(DuiChild_t* c)
{
	if(gDwinHandlers.child_paint != NULL)
		gDwinHandlers.child_paint(c->index, &c->dc);
}

OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	DuiChild_t* c;
	if(!Dwin_Alive() || index < 0 || index >= DUI_CHILD_MAX || gChildren[index].used)
		return NULL;
	if(outerW < 1)
		outerW = 1;
	if(outerH < 1)
		outerH = 1;
	c = &gChildren[index];
	memset(c, 0, sizeof *c);
	if(!DuiWin_Open(&c->win, title, x, y, outerW, outerH, 1))
		return NULL;
	// the child starts black, as its back buffer does
	Dui_FillRect(c->win.pix, c->win.w, c->win.w, c->win.h, 0, 0, c->win.w, c->win.h, 0x000000);
	c->used = 1;
	c->index = index;
	c->x = x;
	c->y = y;
	c->w = outerW;
	c->h = outerH;
	c->dc.magic = CHILD_DC_MAGIC;
	c->dc.index = index;
	SDL_SetWindowMinimumSize(c->win.win, outerW, outerH);
	SDL_SetWindowMaximumSize(c->win.win, outerW, outerH);
	return (OsChild_t*)c;
}

// destroy the window and tell the engine (child_destroyed, as WM_DESTROY would)
static void ChildDestroy(DuiChild_t* c)
{
	int index = c->index;
	if(!c->used)
		return;
	c->used = 0;
	c->closing = 1;
	DuiWin_Close(&c->win);
	if(gDwinHandlers.child_destroyed != NULL)
		gDwinHandlers.child_destroyed(index);
	memset(c, 0, sizeof *c);
}

// the engine's own close (SendMessage(WM_CLOSE)): the only way a child window goes
void OS_ChildClose(OsChild_t* c)
{
	if(c != NULL && ((DuiChild_t*)c)->used)
		ChildDestroy((DuiChild_t*)c);
}

void OS_ChildShow(OsChild_t* c, int show)
{
	DuiChild_t* w = (DuiChild_t*)c;
	if(w == NULL || !w->used)
		return;
	if(show)
	{
		SDL_ShowWindow(w->win.win);
		SDL_RaiseWindow(w->win.win);
		w->shown = 1;
		ChildPaint(w); // SW_SHOWNA + the first paint
	}
	else
	{
		SDL_HideWindow(w->win.win);
		w->shown = 0;
	}
}

void OS_ChildSetTitle(OsChild_t* c, const char* title)
{
	DuiChild_t* w = (DuiChild_t*)c;
	if(w != NULL && w->used)
		DuiWin_SetTitle(&w->win, title);
}

// MoveWindow(.., repaint): the new outer rectangle, then a repaint
void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH)
{
	DuiChild_t* w = (DuiChild_t*)c;
	if(w == NULL || !w->used)
		return;
	if(outerW < 1)
		outerW = 1;
	if(outerH < 1)
		outerH = 1;
	w->x = x;
	w->y = y;
	w->w = outerW;
	w->h = outerH;
	SDL_SetWindowPosition(w->win.win, x, y);
	SDL_SetWindowSize(w->win.win, outerW, outerH);
	SDL_SetWindowMinimumSize(w->win.win, outerW, outerH);
	SDL_SetWindowMaximumSize(w->win.win, outerW, outerH);
	ChildPaint(w);
}

// GetWindowRect's origin: the outer rectangle's origin in screen coordinates
void OS_ChildGetPos(OsChild_t* c, int* x, int* y)
{
	DuiChild_t* w = (DuiChild_t*)c;
	int wx = 0, wy = 0;
	*x = *y = 0;
	if(w == NULL || !w->used)
		return;
	SDL_GetWindowPosition(w->win.win, &wx, &wy);
	// the window manager's frame is unknown to SDL here; the client origin
	// stands in for the outer one (OS_FrameMetrics is 0 on this back end)
	*x = wx;
	*y = wy;
}

// the slot's DC token names the window for Dui_ChildBlit; nothing is acquired
OsDc_t* OS_ChildGetDc(OsChild_t* c)
{
	DuiChild_t* w = (DuiChild_t*)c;
	return w != NULL && w->used ? (OsDc_t*)&w->dc : NULL;
}

void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc)
{
	(void)c;
	(void)dc;
}

// a blit into a child window: draw into its surface and present it
int Dui_ChildBlit(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, int srcX, int srcY,
	int srcW, int srcH)
{
	DuiChild_t* c = ChildByDc(dc);
	if(c == NULL)
		return 0;
	Dui_BlitSurfaceTo(c->win.pix, c->win.w, c->win.h, dstX, dstY, dstW, dstH, src, srcX, srcY, srcW, srcH, NULL);
	DuiWin_Present(&c->win);
	return 1;
}

/* the pump's look at the child windows' events (window.c routes every SDL
 * event here first): a paint presents the engine's buffer again, the close
 * box does nothing, the mouse is not the engine's; key presses fall
 * through to the main window's handlers, which is what the original's
 * child window procedure does */
int DuiChild_HandleEvent(SDL_Event* e)
{
	Uint32 id;
	int i;
	switch(e->type)
	{
		case SDL_WINDOWEVENT:
			for(i = 0; i < DUI_CHILD_MAX; i++)
			{
				if(!gChildren[i].used)
					continue;
				id = DuiWin_Id(&gChildren[i].win);
				if(id == 0 || e->window.windowID != id)
					continue;
				if(e->window.event == SDL_WINDOWEVENT_EXPOSED)
					ChildPaint(&gChildren[i]);
				// SDL_WINDOWEVENT_CLOSE: refused, the engine decides when a child goes
				return 1;
			}
			return 0;
		case SDL_MOUSEMOTION:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEWHEEL:
			for(i = 0; i < DUI_CHILD_MAX; i++)
			{
				if(!gChildren[i].used)
					continue;
				id = DuiWin_Id(&gChildren[i].win);
				if(id != 0 && (e->motion.windowID == id))
					return 1; // a child's mouse events never reach the game
			}
			return 0;
		default:
			return 0;
	}
}
