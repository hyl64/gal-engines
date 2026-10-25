/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dedit.c - the text entry of the macOS (SDL2) back end ("B0 2x" of the
 *           scripts) (inc/bgi/os.h)
 *
 * The original's EDIT control sits over the picture and draws its text
 * centred with a transparent background on top of whatever the engine
 * presented under it.  This back end draws the field itself, like the
 * POSIX one: the engine's picture lives in the staging surface of
 * window.c, so a present copies the entry's rectangle out of that,
 * draws the text with the field widget of duifield.c and hands the
 * composite to the renderer as a second texture at the entry's position.
 * The keys reach it through DuiEdit_HandleEvent from the main event pump
 * while it has the focus; the typed text is SDL_TEXTINPUT (UTF-8),
 * stored in the OS text encoding.
 */
#include "dui_internal.h"

static struct
{
	int created;
	int visible, focus;  // shown over the picture / has the keyboard
	int x, y, w, h;      // the control's rectangle in picture pixels
	DField_t field;      // the text and its editing state
	OsFont_t* font;      // the font of OS_EditSetFont, NULL before one was set
	int fontSize;        // its size as requested
	char face[0x80];     // its face name (Shift-JIS)
	uint32_t rgb;        // the text colour, 0x00RRGGBB
	SDL_Texture* tex;    // the overlay texture of the last present
	int texW, texH;      // its size
	uint32_t* pix;       // the capture + text, w * h
	int pixW, pixH;
	uint32_t* upload;    // ARGB copy for the texture
} gEdit;

// the entry as the original creates it with the window: hidden, 1 x 1, empty
void Dui_EditCreate(void)
{
	if(gEdit.font != NULL)
		OS_FontClose(gEdit.font);
	if(gEdit.tex != NULL)
		SDL_DestroyTexture(gEdit.tex);
	free(gEdit.pix);
	free(gEdit.upload);
	memset(&gEdit, 0, sizeof gEdit);
	gEdit.created = 1;
	gEdit.w = gEdit.h = 1;
	gEdit.rgb = 0;
	DField_SetText(&gEdit.field, "");
}

// make the overlay buffers hold w x h pixels
static int EnsureOverlay(int w, int h)
{
	if(gEdit.pix != NULL && gEdit.pixW == w && gEdit.pixH == h)
		return 1;
	free(gEdit.pix);
	free(gEdit.upload);
	if(gEdit.tex != NULL)
	{
		SDL_DestroyTexture(gEdit.tex);
		gEdit.tex = NULL;
	}
	gEdit.pix = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
	gEdit.upload = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
	gEdit.pixW = w;
	gEdit.pixH = h;
	gEdit.texW = gEdit.texH = 0;
	return gEdit.pix != NULL && gEdit.upload != NULL;
}

/* repaint the entry: copy the picture under it out of the staging
 * surface, draw the text over that, upload and put it over the presented
 * frame (called from Dwin_Present) */
void Dui_PresentEditOverlay(SDL_Renderer* ren)
{
	SDL_Surface* stage;
	int w, h, row, col;
	if(!gEdit.created || !gEdit.visible || ren == NULL)
		return;
	w = gEdit.w;
	h = gEdit.h;
	if(w <= 0 || h <= 0 || !EnsureOverlay(w, h))
		return;
	stage = Dwin_Stage();
	memset(gEdit.pix, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
	if(stage != NULL)
		SDL_LockSurface(stage);
	for(row = 0; row < h; row++)
	{
		int sy = gEdit.y + row;
		if(stage == NULL || sy < 0 || sy >= stage->h)
			continue;
		for(col = 0; col < w; col++)
		{
			int sx = gEdit.x + col;
			if(sx >= 0 && sx < stage->w)
				gEdit.pix[(size_t)row * w + col] = *(uint32_t*)((uint8_t*)stage->pixels + (size_t)sy * stage->pitch +
					(size_t)sx * 4) & 0xffffffu;
		}
	}
	if(stage != NULL)
		SDL_UnlockSurface(stage);
	DField_Draw(&gEdit.field, gEdit.pix, w, w, h, 0, 0, w, h, gEdit.font, gEdit.rgb, 1, gEdit.focus, 0);
	// the texture is recreated when the size changed
	if(gEdit.tex == NULL || gEdit.texW != w || gEdit.texH != h)
	{
		if(gEdit.tex != NULL)
			SDL_DestroyTexture(gEdit.tex);
		gEdit.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		if(gEdit.tex != NULL)
			SDL_SetTextureBlendMode(gEdit.tex, SDL_BLENDMODE_NONE);
		gEdit.texW = w;
		gEdit.texH = h;
	}
	if(gEdit.tex == NULL)
		return;
	{
		size_t n = (size_t)w * (size_t)h, i;
		for(i = 0; i < n; i++)
			gEdit.upload[i] = gEdit.pix[i] | 0xff000000u;
	}
	if(SDL_UpdateTexture(gEdit.tex, NULL, gEdit.upload, w * 4) == 0)
	{
		SDL_Rect dst;
		dst.x = gEdit.x;
		dst.y = gEdit.y;
		dst.w = w;
		dst.h = h;
		SDL_RenderCopy(ren, gEdit.tex, NULL, &dst);
	}
}

// the engine's picture under the control, then present the composite
static void EditRefresh(void)
{
	if(!gEdit.created)
		return;
	if(gEdit.visible && gDwinHandlers.edit_paint != NULL && Dwin_Alive())
		gDwinHandlers.edit_paint();
	Dwin_Present();
}

void OS_EditShow(int show)
{
	if(!gEdit.created)
		Dui_EditCreate();
	gEdit.visible = show != 0;
	if(!show)
		gEdit.focus = 0;
	EditRefresh();
}

// MoveWindow in client (picture) coordinates (never smaller than 1 x 1), then a repaint
void OS_EditMove(int x, int y, int w, int h)
{
	gEdit.x = x;
	gEdit.y = y;
	gEdit.w = w > 0 ? w : 1;
	gEdit.h = h > 0 ? h : 1;
	EditRefresh();
}

// replace the font: the same face and size the original's CreateFontA asks for, then a repaint
void OS_EditSetFont(const char* face, int height, int width)
{
	if(!gEdit.created)
		Dui_EditCreate();
	if(gEdit.font != NULL)
		OS_FontClose(gEdit.font);
	snprintf(gEdit.face, sizeof gEdit.face, "%s", face != NULL ? face : "");
	gEdit.fontSize = height;
	gEdit.font = OS_FontOpen(face, height, width, 0, 0);
	EditRefresh();
}

// EM_LIMITTEXT: the limit in bytes; 0 or one beyond the buffer means unlimited
void OS_EditLimitText(int maxChars)
{
	gEdit.field.limit = maxChars > 0 && maxChars < (int)sizeof gEdit.field.text ? maxChars : 0;
}

void OS_EditSetText(const char* t)
{
	if(!gEdit.created)
		Dui_EditCreate();
	DField_SetText(&gEdit.field, t);
	EditRefresh();
}

void OS_EditSelectAll(void)
{
	DField_SelectAll(&gEdit.field);
	EditRefresh();
}

// SetFocus(the control / the main window): the entry takes the keys while visible
void OS_EditFocus(int toEdit)
{
	gEdit.focus = toEdit != 0 && gEdit.visible;
	if(gEdit.focus)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
	EditRefresh();
}

// GetWindowText: the text into buf (n bytes, NUL-terminated, cut when too long); the length copied
int OS_EditGetText(char* buf, size_t n)
{
	size_t len;
	if(buf == NULL || n == 0)
		return 0;
	len = strlen(gEdit.field.text);
	if(len >= n)
		len = n - 1;
	memcpy(buf, gEdit.field.text, len);
	buf[len] = 0;
	return (int)len;
}

void OS_EditSetColour(uint32_t rgb)
{
	gEdit.rgb = rgb & 0xffffffu;
	EditRefresh();
}

void OS_EditRefresh(void)
{
	EditRefresh();
}

// "B0 29": single-byte printable characters are swallowed
void OS_EditRejectAscii(int reject)
{
	gEdit.field.noAscii = reject;
}

// "B0 23": the IME open state is not tracked here; reported as closed (as the POSIX back end does)
int OS_EditImeOpen(void)
{
	return 0;
}

// the control goes with the window: release the font and the overlay, hide, drop the focus
void OS_EditDestroy(void)
{
	if(gEdit.font != NULL)
		OS_FontClose(gEdit.font);
	gEdit.font = NULL;
	if(gEdit.tex != NULL)
		SDL_DestroyTexture(gEdit.tex);
	gEdit.tex = NULL;
	free(gEdit.pix);
	free(gEdit.upload);
	gEdit.pix = NULL;
	gEdit.upload = NULL;
	gEdit.pixW = gEdit.pixH = 0;
	gEdit.visible = 0;
	gEdit.focus = 0;
	gEdit.created = 0;
}

/* a key press or a text input while the entry has the focus (the main
 * event pump calls this first): Tab and Enter select the text and hand
 * the focus back, everything else edits the field; 1 when consumed */
int DuiEdit_HandleEvent(SDL_Event* e)
{
	int vk, r;
	if(!gEdit.created || !gEdit.visible || !gEdit.focus)
		return 0;
	if(e->type == SDL_TEXTINPUT)
	{
		if(DField_InsertUtf8(&gEdit.field, e->text.text))
			EditRefresh();
		return 1;
	}
	if(e->type != SDL_KEYDOWN)
		return 0;
	vk = Dwin_TranslateVk(e->key.keysym.sym);
	if(vk == 0x09 || vk == 0x0d)
	{ // Tab / Enter: select everything and hand the focus back
		DField_SelectAll(&gEdit.field);
		OS_EditFocus(0);
		return 1;
	}
	if(vk == 0)
		return 1; // a key that types nothing: the control consumes it anyway
	r = DField_Key(&gEdit.field, vk, (e->key.keysym.mod & KMOD_CTRL) != 0);
	if(r > 0)
		EditRefresh();
	return 1;
}
