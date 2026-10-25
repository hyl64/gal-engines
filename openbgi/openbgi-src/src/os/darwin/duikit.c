/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * duikit.c - the dialog toolkit of the macOS (SDL2) back end, and the
 *            auxiliary-window helper the child windows, the dialogs and
 *            the debugger's window share (dui_internal.h)
 *
 * Without a toolkit the back end draws its dialogs itself: a dialog is an
 * SDL window painted from a 32-bit software surface in the classic grey
 * Windows look, run as a modal loop that keeps dispatching the engine's
 * own window events (so the game repaints) but drops their input.  The
 * keyboard works as in a Windows dialog: Tab cycles the fields and
 * buttons, Enter activates the focused or the default button, Escape the
 * cancel button, Space a focused button, the arrows move between buttons
 * or step a number picker.  The typed text arrives as SDL_TEXTINPUT
 * (UTF-8) and is stored in the OS text encoding (Shift-JIS normally).
 */
#include "dui_internal.h"

// ---- the auxiliary window ------------------------------------------------------------------------

int DuiWin_Open(DuiWin_t* d, const char* titleSjis, int x, int y, int w, int h, int hidden)
{
	char title[0x400];
	Uint32 flags;
	memset(d, 0, sizeof *d);
	if(SDL_WasInit(SDL_INIT_VIDEO) == 0 || w <= 0 || h <= 0)
		return 0;
	OsPosix_SjisToUtf8(titleSjis != NULL ? titleSjis : "OpenBGI", title, sizeof title);
	flags = hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
	d->win = SDL_CreateWindow(title, x, y, w, h, flags);
	if(d->win == NULL)
		return 0;
	/* BGI_DUMP_DIALOG reads the window back, which a headless session's
	 * accelerated renderer does not support: the diagnostic uses SDL's
	 * software renderer (the same picture, a CPU surface) */
	if(getenv("BGI_DUMP_DIALOG") != NULL)
		d->ren = SDL_CreateRenderer(d->win, -1, SDL_RENDERER_SOFTWARE);
	if(d->ren == NULL)
		d->ren = SDL_CreateRenderer(d->win, -1, SDL_RENDERER_ACCELERATED);
	if(d->ren == NULL)
		d->ren = SDL_CreateRenderer(d->win, -1, 0);
	if(d->ren != NULL)
	{
		d->tex = SDL_CreateTexture(d->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		if(d->tex != NULL)
			SDL_SetTextureBlendMode(d->tex, SDL_BLENDMODE_NONE);
	}
	d->pix = (uint32_t*)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
	d->upload = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
	if(d->pix == NULL || d->upload == NULL)
	{
		DuiWin_Close(d);
		return 0;
	}
	d->w = w;
	d->h = h;
	d->open = 1;
	Dui_FillRect(d->pix, w, w, h, 0, 0, w, h, UI_FACE);
	if(!hidden)
	{
		SDL_ShowWindow(d->win);
		SDL_RaiseWindow(d->win);
	}
	return 1;
}

void DuiWin_SetTitle(DuiWin_t* d, const char* titleSjis)
{
	char title[0x400];
	if(d == NULL || d->win == NULL)
		return;
	OsPosix_SjisToUtf8(titleSjis != NULL ? titleSjis : "", title, sizeof title);
	SDL_SetWindowTitle(d->win, title);
}

// upload the software surface and show it (alpha set to opaque: the surface is 0x00RRGGBB)
void DuiWin_Present(DuiWin_t* d)
{
	size_t n, i;
	if(d == NULL || !d->open || d->pix == NULL)
		return;
	if(d->ren != NULL && d->tex != NULL && d->upload != NULL)
	{
		n = (size_t)d->w * (size_t)d->h;
		for(i = 0; i < n; i++)
			d->upload[i] = d->pix[i] | 0xff000000u;
		if(SDL_UpdateTexture(d->tex, NULL, d->upload, d->w * 4) == 0)
		{
			SDL_RenderClear(d->ren);
			SDL_RenderCopy(d->ren, d->tex, NULL, NULL);
			SDL_RenderPresent(d->ren);
			return;
		}
	}
	if(d->win != NULL)
	{ // no renderer: draw through the window surface
		SDL_Surface* ws = SDL_GetWindowSurface(d->win);
		if(ws != NULL && d->upload != NULL)
		{
			SDL_Surface* src;
			n = (size_t)d->w * (size_t)d->h;
			for(i = 0; i < n; i++)
				d->upload[i] = d->pix[i] | 0xff000000u;
			src = SDL_CreateRGBSurfaceFrom(d->upload, d->w, d->h, 32, d->w * 4, 0x00ff0000, 0x0000ff00,
				0x000000ff, 0xff000000);
			if(src != NULL)
			{
				SDL_BlitSurface(src, NULL, ws, NULL);
				SDL_FreeSurface(src);
			}
			SDL_UpdateWindowSurface(d->win);
		}
	}
}

void DuiWin_Close(DuiWin_t* d)
{
	if(d == NULL)
		return;
	if(d->tex != NULL)
		SDL_DestroyTexture(d->tex);
	if(d->ren != NULL)
		SDL_DestroyRenderer(d->ren);
	if(d->win != NULL)
		SDL_DestroyWindow(d->win);
	free(d->pix);
	free(d->upload);
	memset(d, 0, sizeof *d);
}

Uint32 DuiWin_Id(DuiWin_t* d)
{
	return d != NULL && d->win != NULL ? SDL_GetWindowID(d->win) : 0;
}

// ---- pixels ----------------------------------------------------------------------------------------

uint32_t Dui_SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* row = (const uint8_t*)s->pixels + (size_t)y * s->pitch;
	if(s->bpp == 32)
		return *(const uint32_t*)(row + (size_t)x * 4) & 0xffffffu;
	{
		uint32_t v = *(const uint16_t*)(row + (size_t)x * 2);
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
}

// nearest-neighbour blit of a whole OsSurface into a 0x00RRGGBB surface, clipped
void Dui_BlitSurfaceTo(uint32_t* pix, int bufW, int bufH, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s,
	int srcX, int srcY, int srcW, int srcH, const int* clip)
{
	int x, y;
	if(s == NULL || s->pixels == NULL || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
		return;
	for(y = 0; y < dstH; y++)
	{
		int dy = dstY + y;
		int sy = srcY + y * srcH / dstH;
		if(dy < 0 || dy >= bufH || sy < 0 || sy >= s->height)
			continue;
		if(clip != NULL && (dy < clip[1] || dy >= clip[3]))
			continue;
		for(x = 0; x < dstW; x++)
		{
			int dx = dstX + x;
			int sx = srcX + x * srcW / dstW;
			if(dx < 0 || dx >= bufW || sx < 0 || sx >= s->width)
				continue;
			if(clip != NULL && (dx < clip[0] || dx >= clip[2]))
				continue;
			pix[(size_t)dy * bufW + dx] = Dui_SurfacePixel(s, sx, sy);
		}
	}
}

// ---- the unattended-run switch --------------------------------------------------------------------

int Dui_AutoChoice(void)
{
	const char* v = getenv("BGI_AUTO_DIALOG");
	if(v == NULL || v[0] == 0 || strcmp(v, "0") == 0)
		return DUI_AUTO_NONE;
	if(strcasecmp(v, "cancel") == 0 || strcasecmp(v, "no") == 0 || strcmp(v, "2") == 0)
		return DUI_AUTO_CANCEL;
	if(strcasecmp(v, "show") == 0 || strcmp(v, "3") == 0)
		return DUI_AUTO_SHOW;
	return DUI_AUTO_OK;
}

// ---- the dialog toolkit --------------------------------------------------------------------------

int DUi_LabelAdd(DDlg_t* d, int x, int y, const char* text)
{
	DUiLabel_t* l;
	if(d->nLabels >= UI_MAX_LABELS)
		return -1;
	l = &d->labels[d->nLabels++];
	l->x = x;
	l->y = y;
	snprintf(l->text, sizeof l->text, "%s", text != NULL ? text : "");
	return d->nLabels - 1;
}

int DUi_FieldAdd(DDlg_t* d, int x, int y, int w, const char* initial, int limit)
{
	DUiField_t* f;
	if(d->nFields >= UI_MAX_FIELDS)
		return -1;
	f = &d->fields[d->nFields++];
	memset(f, 0, sizeof *f);
	f->x = x;
	f->y = y;
	f->w = w;
	f->h = UI_FIELD_H;
	f->f.limit = limit > 0 && limit < (int)sizeof f->f.text ? limit : 0;
	DField_SetText(&f->f, initial != NULL ? initial : "");
	return d->nFields - 1;
}

int DUi_ButtonAdd(DDlg_t* d, int x, int y, const char* text, int id)
{
	DUiButton_t* b;
	if(d->nButtons >= UI_MAX_BUTTONS)
		return -1;
	b = &d->buttons[d->nButtons++];
	b->x = x;
	b->y = y;
	b->w = UI_BTN_W;
	b->h = UI_BTN_H;
	b->text = text;
	b->id = id;
	return d->nButtons - 1;
}

int DUi_ListAdd(DDlg_t* d, int x, int y, int w, int rows, const char* const* items, int count, int selected)
{
	DUiList_t* l;
	if(d->nLists >= UI_MAX_LISTS)
		return -1;
	l = &d->lists[d->nLists++];
	l->x = x;
	l->y = y;
	l->w = w;
	l->h = rows * UI_LIST_ROW_H + 4;
	l->items = items;
	l->count = count;
	l->selected = selected < count ? selected : -1;
	l->top = 0;
	if(l->selected >= rows)
		l->top = l->selected - rows + 1;
	return d->nLists - 1;
}

void DUi_LabelSet(DDlg_t* d, int label, const char* text)
{
	if(label >= 0 && label < d->nLabels)
		snprintf(d->labels[label].text, sizeof d->labels[label].text, "%s", text != NULL ? text : "");
}

// replace a field's text, caret at the end, nothing selected
void DUi_FieldSetText(DUiField_t* f, const char* text)
{
	DField_SetText(&f->f, text);
}

// the rows a list shows at once
static int ListRows(const DUiList_t* l)
{
	return (l->h - 4) / UI_LIST_ROW_H;
}

static void ListScrollToSelection(DUiList_t* l)
{
	int rows = ListRows(l);
	if(l->selected < 0)
		return;
	if(l->selected < l->top)
		l->top = l->selected;
	if(l->selected >= l->top + rows)
		l->top = l->selected - rows + 1;
}

// the sunken white box of a field or a list
static void SunkenBox(DDlg_t* d, int x, int y, int w, int h)
{
	uint32_t* p = d->win.pix;
	Dui_FillRect(p, d->w, d->w, d->h, x, y, w, h, UI_FIELD_BG);
	Dui_FillRect(p, d->w, d->w, d->h, x, y, w, 1, UI_SHADOW);
	Dui_FillRect(p, d->w, d->w, d->h, x, y, 1, h, UI_SHADOW);
	Dui_FillRect(p, d->w, d->w, d->h, x + 1, y + 1, w - 2, 1, UI_DARK);
	Dui_FillRect(p, d->w, d->w, d->h, x + 1, y + 1, 1, h - 2, UI_DARK);
	Dui_FillRect(p, d->w, d->w, d->h, x, y + h - 1, w, 1, UI_LIGHT);
	Dui_FillRect(p, d->w, d->w, d->h, x + w - 1, y, 1, h, UI_LIGHT);
}

static void ListDraw(DDlg_t* d, int index)
{
	DUiList_t* l = &d->lists[index];
	int rows = ListRows(l), i, focused = d->focus == d->nFields + d->nButtons + index;
	uint32_t* p = d->win.pix;
	SunkenBox(d, l->x, l->y, l->w, l->h);
	for(i = 0; i < rows && l->top + i < l->count; i++)
	{
		int row = l->top + i, y = l->y + 2 + i * UI_LIST_ROW_H;
		int sel = row == l->selected;
		if(sel)
			Dui_FillRect(p, d->w, d->w, d->h, l->x + 2, y, l->w - 4, UI_LIST_ROW_H, focused ? UI_SELECT_BG : UI_SHADOW);
		if(l->items[row] != NULL)
			OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, l->x + 6, y + 2, l->items[row], sel ? UI_SELECT_TEXT : UI_TEXT);
	}
	if(l->count > rows)
	{
		int trackH = l->h - 4, barH = trackH * rows / l->count, barY = trackH * l->top / l->count;
		if(barH < 8)
			barH = 8;
		Dui_FillRect(p, d->w, d->w, d->h, l->x + l->w - 6, l->y + 2, 4, trackH, UI_FACE);
		Dui_FillRect(p, d->w, d->w, d->h, l->x + l->w - 6, l->y + 2 + barY, 4, barH, UI_DARK);
	}
}

// paint the whole dialog into its surface and show it
static void DlgDraw(DDlg_t* d)
{
	int i;
	uint32_t* p = d->win.pix;
	Dui_FillRect(p, d->w, d->w, d->h, 0, 0, d->w, d->h, UI_FACE);
	if(d->icon)
	{
		int cx = UI_MARGIN + 16, cy = UI_MARGIN + 16, x, y;
		char letter[2] = {(char)d->icon, 0};
		int lw;
		for(y = -16; y < 16; y++)
			for(x = -16; x < 16; x++)
				if(x * x + y * y <= 15 * 15)
					p[(size_t)(cy + y) * d->w + cx + x] = d->iconRgb;
		lw = OsPosix_FontTextWidth(d->font, letter, -1);
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, cx - lw / 2, cy - UI_FONT_H / 2, letter, 0xffffff);
	}
	for(i = 0; i < d->nLabels; i++)
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, d->labels[i].x, d->labels[i].y, d->labels[i].text, UI_TEXT);
	for(i = 0; i < d->nFields; i++)
	{
		DUiField_t* f = &d->fields[i];
		DField_Draw(&f->f, p, d->w, d->w, d->h, f->x, f->y, f->w, f->h, d->font, UI_TEXT, 0, d->focus == i, 1);
	}
	for(i = 0; i < d->nLists; i++)
		ListDraw(d, i);
	for(i = 0; i < d->nButtons; i++)
	{
		DUiButton_t* b = &d->buttons[i];
		int down = d->pressed == i, tw;
		int focused = d->focus == d->nFields + i;
		Dui_FillRect(p, d->w, d->w, d->h, b->x, b->y, b->w, b->h, UI_FACE);
		Dui_FillRect(p, d->w, d->w, d->h, b->x, b->y, b->w, 1, down ? UI_DARK : UI_LIGHT);
		Dui_FillRect(p, d->w, d->w, d->h, b->x, b->y, 1, b->h, down ? UI_DARK : UI_LIGHT);
		Dui_FillRect(p, d->w, d->w, d->h, b->x, b->y + b->h - 1, b->w, 1, down ? UI_LIGHT : UI_DARK);
		Dui_FillRect(p, d->w, d->w, d->h, b->x + b->w - 1, b->y, 1, b->h, down ? UI_LIGHT : UI_DARK);
		Dui_FillRect(p, d->w, d->w, d->h, b->x + 1, b->y + b->h - 2, b->w - 2, 1, down ? UI_FACE : UI_SHADOW);
		Dui_FillRect(p, d->w, d->w, d->h, b->x + b->w - 2, b->y + 1, 1, b->h - 2, down ? UI_FACE : UI_SHADOW);
		if(i == d->defButton || focused)
		{
			Dui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y - 1, b->w + 2, 1, UI_FRAME);
			Dui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y + b->h, b->w + 2, 1, UI_FRAME);
			Dui_FillRect(p, d->w, d->w, d->h, b->x - 1, b->y - 1, 1, b->h + 2, UI_FRAME);
			Dui_FillRect(p, d->w, d->w, d->h, b->x + b->w, b->y - 1, 1, b->h + 2, UI_FRAME);
		}
		tw = OsPosix_FontTextWidth(d->font, b->text, -1);
		OsPosix_FontDraw(d->font, p, d->w, d->w, d->h, b->x + (b->w - tw) / 2 + down,
			b->y + (b->h - UI_FONT_H) / 2 + down, b->text, UI_TEXT);
	}
	DuiWin_Present(&d->win);
}

static int HitButton(DDlg_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nButtons; i++)
	{
		DUiButton_t* b = &d->buttons[i];
		if(x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

static int HitField(DDlg_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nFields; i++)
	{
		DUiField_t* f = &d->fields[i];
		if(x >= f->x && x < f->x + f->w && y >= f->y && y < f->y + f->h)
			return i;
	}
	return -1;
}

static int HitList(DDlg_t* d, int x, int y)
{
	int i;
	for(i = 0; i < d->nLists; i++)
	{
		DUiList_t* l = &d->lists[i];
		if(x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h)
			return i;
	}
	return -1;
}

// a button was pressed: end the dialog with its id unless the onButton handler refuses
static void Activate(DDlg_t* d, int button)
{
	if(button < 0 || button >= d->nButtons)
		return;
	if(d->onButton != NULL && !d->onButton(d, d->buttons[button].id, d->ctx))
	{
		d->redraw = 1;
		return;
	}
	d->result = d->buttons[button].id;
	d->done = 1;
}

// select a row of a list (clamped; -1 clears) and tell the owner
static void ListSelect(DDlg_t* d, int index, int row)
{
	DUiList_t* l = &d->lists[index];
	if(l->count == 0)
		row = -1;
	else if(row >= l->count)
		row = l->count - 1;
	if(row < -1)
		row = -1;
	if(row == l->selected)
		return;
	l->selected = row;
	ListScrollToSelection(l);
	if(d->onListSelect != NULL)
		d->onListSelect(d, index, d->ctx);
	d->redraw = 1;
}

// a numeric picker only holds digits inside its range
static void ClampNumeric(DUiField_t* f)
{
	int v, i, o = 0;
	char digits[16];
	if(!f->numeric)
		return;
	for(i = 0; f->f.text[i] && o < 15; i++)
		if(f->f.text[i] >= '0' && f->f.text[i] <= '9')
			digits[o++] = f->f.text[i];
	digits[o] = 0;
	v = atoi(digits);
	if(o == 0)
		return;
	if(v < f->min)
		v = f->min;
	if(v > f->max)
		v = f->max;
	snprintf(f->f.text, sizeof f->f.text, "%d", v);
	f->f.caret = (int)strlen(f->f.text);
}

// whether an edit of a digits-only field stays within what typing may add
static int DigitsKept(const char* before, const char* after)
{
	int i, otherB = 0, otherA = 0, minusB = 0, minusA = 0;
	for(i = 0; before[i]; i++)
		before[i] == '-' ? minusB++ : (before[i] < '0' || before[i] > '9') ? otherB++ : 0;
	for(i = 0; after[i]; i++)
		after[i] == '-' ? minusA++ : (after[i] < '0' || after[i] > '9') ? otherA++ : 0;
	return otherA <= otherB && (minusA <= minusB || before[0] == 0);
}

// the typed text of a focused field
static void DlgText(DDlg_t* d, const char* utf8)
{
	DUiField_t* f;
	char before[sizeof d->fields[0].f.text];
	int changed;
	if(d->focus < 0 || d->focus >= d->nFields)
		return;
	f = &d->fields[d->focus];
	snprintf(before, sizeof before, "%s", f->f.text);
	changed = DField_InsertUtf8(&f->f, utf8);
	if(changed && f->digits && !DigitsKept(before, f->f.text))
	{
		DField_SetText(&f->f, before);
		f->f.caret = (int)strlen(f->f.text);
		changed = 0;
	}
	if(changed)
	{
		ClampNumeric(f);
		if(d->onChange != NULL)
			d->onChange(d, d->focus, d->ctx);
	}
}

// the dialog's SDL events; the modal loop in DUi_Run calls this for its own window's events
static void DlgEvent(DDlg_t* d, SDL_Event* e)
{
	switch(e->type)
	{
		case SDL_WINDOWEVENT:
			switch(e->window.event)
			{
				case SDL_WINDOWEVENT_EXPOSED:
					DlgDraw(d);
					break;
				case SDL_WINDOWEVENT_CLOSE:
					if(d->escButton >= 0)
						Activate(d, d->escButton);
					break;
				default:
					break;
			}
			return;
		case SDL_TEXTINPUT:
			DlgText(d, e->text.text);
			return;
		case SDL_KEYDOWN:
		{
			int vk = Dwin_TranslateVk(e->key.keysym.sym);
			int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
			int shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
			if(vk == 0)
				return;
			if(vk == 0x1b)
			{
				if(d->escButton >= 0)
					Activate(d, d->escButton);
				return;
			}
			if(vk == 0x0d)
			{
				if(d->focus >= d->nFields && d->focus < d->nFields + d->nButtons)
					Activate(d, d->focus - d->nFields);
				else if(d->defButton >= 0)
					Activate(d, d->defButton);
				return;
			}
			if(vk == 0x09)
			{
				int n = d->nFields + d->nButtons + d->nLists;
				if(n > 0)
				{
					if(shift)
						d->focus = (d->focus + n - 1) % n;
					else
						d->focus = (d->focus + 1) % n;
					if(d->focus < d->nFields)
						DField_SelectAll(&d->fields[d->focus].f);
				}
				DlgDraw(d);
				return;
			}
			if(d->focus >= d->nFields + d->nButtons)
			{ // a list has the focus
				int index = d->focus - d->nFields - d->nButtons;
				DUiList_t* l = &d->lists[index];
				int rows = ListRows(l), row = l->selected;
				if(vk == 0x26)
					row = row > 0 ? row - 1 : 0;
				else if(vk == 0x28)
					row = row + 1;
				else if(vk == 0x21)
					row = row > rows ? row - rows : 0;
				else if(vk == 0x22)
					row = row + rows;
				else if(vk == 0x24)
					row = 0;
				else if(vk == 0x23)
					row = l->count - 1;
				else
					return;
				ListSelect(d, index, row);
				DlgDraw(d);
				return;
			}
			if(d->focus >= d->nFields)
			{ // a button has the focus: Space presses it, the arrows move between them
				if(vk == 0x20)
					Activate(d, d->focus - d->nFields);
				else if(vk == 0x25 || vk == 0x27)
				{
					int n = d->nButtons, b = d->focus - d->nFields;
					if(n <= 0)
						return;
					b = vk == 0x25 ? (b + n - 1) % n : (b + 1) % n;
					d->focus = d->nFields + b;
					DlgDraw(d);
				}
				return;
			}
			{ // a field has the focus
				DUiField_t* f = &d->fields[d->focus];
				int r;
				if(f->numeric && (vk == 0x26 || vk == 0x28))
				{
					int v = atoi(f->f.text) + (vk == 0x26 ? 1 : -1);
					if(v < f->min)
						v = f->max;
					if(v > f->max)
						v = f->min;
					snprintf(f->f.text, sizeof f->f.text, "%d", v);
					f->f.caret = (int)strlen(f->f.text);
					f->f.selAll = 0;
					if(d->onChange != NULL)
						d->onChange(d, d->focus, d->ctx);
					DlgDraw(d);
					return;
				}
				if(f->nChoices > 0 && (vk == 0x26 || vk == 0x28))
				{
					int cur = -1, k;
					for(k = 0; k < f->nChoices; k++)
						if(strcmp(f->choices[k], f->f.text) == 0)
							cur = k;
					if(cur < 0)
						cur = vk == 0x26 ? f->nChoices : -1;
					cur = vk == 0x26 ? (cur + f->nChoices - 1) % f->nChoices : (cur + 1) % f->nChoices;
					DField_SetText(&f->f, f->choices[cur]);
					if(d->onChange != NULL)
						d->onChange(d, d->focus, d->ctx);
					DlgDraw(d);
					return;
				}
				{
					r = DField_Key(&f->f, vk, ctrl);
					if(r > 0)
					{
						ClampNumeric(f);
						if(d->onChange != NULL)
							d->onChange(d, d->focus, d->ctx);
					}
				}
				if(r >= 0)
					DlgDraw(d);
				return;
			}
		}
		case SDL_MOUSEBUTTONDOWN:
		{
			int x = e->button.x, y = e->button.y;
			int b, f, li;
			if(e->button.button != SDL_BUTTON_LEFT)
				return;
			b = HitButton(d, x, y);
			f = HitField(d, x, y);
			li = HitList(d, x, y);
			if(b >= 0)
			{
				d->pressed = b;
				d->focus = d->nFields + b;
				DlgDraw(d);
				return;
			}
			if(f >= 0)
			{
				d->focus = f;
				d->fields[f].f.selAll = 0;
				d->fields[f].f.caret = (int)strlen(d->fields[f].f.text);
				DlgDraw(d);
				return;
			}
			if(li >= 0)
			{
				DUiList_t* l = &d->lists[li];
				int row = l->top + (y - l->y - 2) / UI_LIST_ROW_H;
				int same = row == l->selected && row < l->count;
				d->focus = d->nFields + d->nButtons + li;
				if(row < l->count)
					ListSelect(d, li, row);
				// a double click on the row activates the default button
				if(e->button.clicks >= 2 && same && d->defButton >= 0)
				{
					DlgDraw(d);
					Activate(d, d->defButton);
					return;
				}
				DlgDraw(d);
			}
			return;
		}
		case SDL_MOUSEBUTTONUP:
			if(e->button.button == SDL_BUTTON_LEFT && d->pressed >= 0)
			{
				int b = HitButton(d, e->button.x, e->button.y);
				int was = d->pressed;
				d->pressed = -1;
				DlgDraw(d);
				if(b == was)
					Activate(d, b);
				if(d->redraw && !d->done)
				{
					d->redraw = 0;
					DlgDraw(d);
				}
			}
			return;
		case SDL_MOUSEWHEEL:
		{
			int mx = 0, my = 0, li;
			SDL_GetMouseState(&mx, &my);
			li = HitList(d, mx, my);
			if(li >= 0 && e->wheel.y != 0)
			{
				DUiList_t* l = &d->lists[li];
				int rows = ListRows(l), maxTop = l->count > rows ? l->count - rows : 0;
				l->top += e->wheel.y > 0 ? -3 : 3;
				if(l->top > maxTop)
					l->top = maxTop;
				if(l->top < 0)
					l->top = 0;
				DlgDraw(d);
			}
			return;
		}
		default:
			return;
	}
}

int DUi_TextWidth(const char* text)
{
	OsFont_t* f = OsPosix_FontUi(UI_FONT_H);
	int w = OsPosix_FontTextWidth(f, text != NULL ? text : "", -1);
	if(f != NULL)
		OS_FontClose(f);
	return w;
}

// the number of modal dialog loops up (dui_internal.h)
static int gDuiModalDepth;

int Dui_ModalDepth(void)
{
	return gDuiModalDepth;
}

// does the event belong to the dialog's own window?
static int IsDlgEvent(DDlg_t* d, SDL_Event* e)
{
	Uint32 id = DuiWin_Id(&d->win);
	if(id == 0)
		return 0;
	switch(e->type)
	{
		case SDL_WINDOWEVENT: return e->window.windowID == id;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_TEXTINPUT: return e->key.windowID == id;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: return e->button.windowID == id;
		case SDL_MOUSEMOTION: return e->motion.windowID == id;
		case SDL_MOUSEWHEEL: return e->wheel.windowID == id;
		default: return 0;
	}
}

// the event classes the engine must not see while a dialog is modal
static int IsEngineInput(SDL_Event* e)
{
	switch(e->type)
	{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_TEXTINPUT:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEMOTION:
		case SDL_MOUSEWHEEL:
		case SDL_QUIT:
			return 1;
		case SDL_WINDOWEVENT:
			return e->window.event == SDL_WINDOWEVENT_CLOSE;
		default:
			return 0;
	}
}

/* the main event pump's first look at an event (window.c calls this):
 * the debugger's window, the text entry, the child windows, then the
 * input a modal dialog suppresses; 1 = consumed */
int Dui_HandleEvent(SDL_Event* e)
{
	if(DuiDebug_HandleEvent(e))
		return 1;
	if(DuiEdit_HandleEvent(e))
		return 1;
	if(DuiChild_HandleEvent(e))
		return 1;
	if(gDuiModalDepth > 0 && IsEngineInput(e))
		return 1;
	return 0;
}

/* BGI_DUMP_DIALOG=<path>: save what the dialog's window shows (the
 * renderer's own pixels) - the visual evidence of a real GUI run with
 * nobody at the screen.  Every dialog goes to <path>-<n>.bmp when
 * BGI_DUMP_DIALOG_ALL is set, else only the first to <path> itself. */
static void DlgDump(DDlg_t* d)
{
	const char* path = getenv("BGI_DUMP_DIALOG");
	static int count;
	char file[0x400];
	SDL_Surface* out;
	int w = 0, h = 0;
	if(path == NULL || path[0] == 0 || d->win.ren == NULL)
		return;
	if(count > 0 && getenv("BGI_DUMP_DIALOG_ALL") == NULL)
		return;
	if(getenv("BGI_DUMP_DIALOG_ALL") != NULL)
		snprintf(file, sizeof file, "%s-%d.bmp", path, count);
	else
		snprintf(file, sizeof file, "%s", path);
	SDL_GetRendererOutputSize(d->win.ren, &w, &h);
	if(w <= 0 || h <= 0)
		return;
	out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if(out == NULL)
		return;
	if(SDL_RenderReadPixels(d->win.ren, NULL, SDL_PIXELFORMAT_ARGB8888, out->pixels, out->pitch) == 0 &&
		SDL_SaveBMP(out, file) == 0)
	{
		fprintf(stderr, "bgi: dialog read back to %s (%dx%d)\n", file, w, h);
		fflush(stderr);
		count++;
	}
	SDL_FreeSurface(out);
}

/* create the window centred on the screen and run the modal loop; the id
 * of the button that ended it.  Without a font the dialog counts as
 * cancelled (the cancel button's id, or 0). */
int DUi_Run(DDlg_t* d, const char* titleSjis)
{
	int sw = 800, sh = 600, x, y, choice;
	SDL_DisplayMode m;
	if(SDL_GetDesktopDisplayMode(0, &m) == 0)
	{
		sw = m.w;
		sh = m.h;
	}
	x = (sw - d->w) / 2;
	y = (sh - d->h) / 2;
	if(!DuiWin_Open(&d->win, titleSjis, x, y, d->w, d->h, 0))
		return d->escButton >= 0 ? d->buttons[d->escButton].id : 0;
	d->font = OsPosix_FontUi(UI_FONT_H);
	if(d->font == NULL)
	{
		DuiWin_Close(&d->win);
		return d->escButton >= 0 ? d->buttons[d->escButton].id : 0;
	}
	d->pressed = -1;
	d->done = 0;
	d->result = 0;
	if(d->focus < 0 || d->focus >= d->nFields + d->nButtons + d->nLists)
		d->focus = 0;
	gDuiModalDepth++;
	DlgDraw(d);
	DlgDump(d);
	if(d->nFields > 0)
		SDL_StartTextInput();
	// an unattended run (the tests, the boot probe): press the default / cancel button at once
	choice = Dui_AutoChoice();
	if(choice == DUI_AUTO_NONE)
	{
		/* a display without a user (SDL's dummy driver, a test run) cannot
		 * answer the dialog: cancel it instead of waiting forever */
		const char* drv = SDL_GetCurrentVideoDriver();
		if(drv == NULL || strcasecmp(drv, "dummy") == 0)
			choice = DUI_AUTO_CANCEL;
	}
	if(choice != DUI_AUTO_NONE)
	{
		char utf[0x300];
		OsPosix_SjisToUtf8(titleSjis != NULL ? titleSjis : "", utf, sizeof utf);
		fprintf(stderr, "[auto] dialog \"%s\": %s\n", utf,
			choice == DUI_AUTO_OK ? "OK" : choice == DUI_AUTO_CANCEL ? "cancel" : "show, then cancel");
		fflush(stderr);
	}
	if(choice == DUI_AUTO_OK)
	{
		if(d->defButton >= 0)
			Activate(d, d->defButton);
		else if(d->nButtons > 0)
			Activate(d, d->nButtons - 1);
	}
	else if(choice == DUI_AUTO_CANCEL || choice == DUI_AUTO_SHOW)
	{
		if(d->escButton >= 0)
			Activate(d, d->escButton);
		else
		{
			d->result = 0;
			d->done = 1;
		}
	}
	while(!d->done)
	{
		SDL_Event e;
		if(!SDL_WaitEventTimeout(&e, 30))
			continue;
		if(IsDlgEvent(d, &e))
		{
			DlgEvent(d, &e);
			if(d->redraw && !d->done)
			{
				d->redraw = 0;
				DlgDraw(d);
			}
			continue;
		}
		Dwin_DispatchEvent(&e); // the engine's windows keep repainting; their input is dropped
	}
	if(d->nFields > 0)
		SDL_StopTextInput();
	if(choice != DUI_AUTO_NONE)
	{
		char utf[0x300];
		OsPosix_SjisToUtf8(titleSjis != NULL ? titleSjis : "", utf, sizeof utf);
		fprintf(stderr, "[auto] dialog \"%s\" returned %d\n", utf, d->result);
		fflush(stderr);
	}
	gDuiModalDepth--;
	if(d->font != NULL)
		OS_FontClose(d->font);
	d->font = NULL;
	DuiWin_Close(&d->win);
	return d->result;
}
