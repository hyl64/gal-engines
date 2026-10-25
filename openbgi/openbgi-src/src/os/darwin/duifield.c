/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * duifield.c - the single-line text field of the macOS (SDL2) back end's
 *              own windows (dui_internal.h)
 *
 * The text entry over the picture (dedit.c) and the dialogs (ddialogs.c)
 * share it.  It is the POSIX back end's field widget (src/os/posix/field.c)
 * with the SDL input model: the editing keys arrive as Windows
 * virtual-key codes from the event pump and the typed text as SDL_TEXTINPUT
 * events (UTF-8, converted to the OS text encoding here).  The selection
 * is all or nothing, as the original's EM_SETSEL 0, -1 after Tab / Enter
 * leaves it.
 */
#include "dui_internal.h"

// byte length of the character starting at s[i]
static int CharLen(const char* s, int i)
{
	if(gOsTextUtf8)
	{
		int n = OsCommon_Utf8SeqLen((const uint8_t*)s + i);
		return n ? n : 1;
	}
	return OsCommon_SjisIsLead((uint8_t)s[i]) && s[i + 1] ? 2 : 1;
}

// the start of the character before offset i
static int PrevChar(const char* s, int i)
{
	int p = 0, q = 0;
	while(q < i)
	{
		p = q;
		q += CharLen(s, q);
	}
	return p;
}

// cut the text to the limit and to whole characters
static void TruncateWhole(DField_t* f)
{
	int i;
	if(f->limit > 0 && (int)strlen(f->text) > f->limit)
		f->text[f->limit] = 0;
	// a truncated two-byte character leaves its lead byte behind: drop it
	for(i = 0; f->text[i];)
	{
		if(OsCommon_SjisIsLead((uint8_t)f->text[i]) && !f->text[i + 1])
		{
			f->text[i] = 0;
			break;
		}
		if(gOsTextUtf8 && OsCommon_Utf8SeqLen((const uint8_t*)f->text + i) == 0)
		{
			f->text[i] = 0;
			break;
		}
		i += CharLen(f->text, i);
	}
}

void DField_SetText(DField_t* f, const char* text)
{
	snprintf(f->text, sizeof f->text, "%s", text ? text : "");
	TruncateWhole(f);
	f->caret = (int)strlen(f->text);
	f->selAll = 0;
}

void DField_SelectAll(DField_t* f)
{
	f->selAll = f->text[0] != 0;
	f->caret = (int)strlen(f->text);
}

/* insert `n` bytes of text at the caret, replacing the selection; what
 * does not fit the limit or the buffer is dropped by whole characters */
static void FieldInsert(DField_t* f, const char* bytes, int n)
{
	int len, room;
	if(f->selAll)
	{
		f->text[0] = 0;
		f->caret = 0;
		f->selAll = 0;
	}
	len = (int)strlen(f->text);
	room = (int)sizeof f->text - 1 - len;
	if(f->limit > 0 && f->limit - len < room)
		room = f->limit - len;
	if(n > room)
	{
		int k = 0;
		while(k < n && k + CharLen(bytes, k) <= room)
			k += CharLen(bytes, k);
		n = k;
	}
	if(n <= 0)
		return;
	memmove(f->text + f->caret + n, f->text + f->caret, (size_t)(len - f->caret + 1));
	memcpy(f->text + f->caret, bytes, (size_t)n);
	f->caret += n;
}

// the text of an SDL_TEXTINPUT event as the field stores it
void DField_FromUtf8(const char* utf8, char* out, size_t n)
{
	if(gOsTextUtf8)
		snprintf(out, n, "%s", utf8);
	else
		OS_Utf8ToSjis(utf8, out, n);
}

int DField_InsertUtf8(DField_t* f, const char* utf8)
{
	char bytes[DUI_FIELD_TEXT];
	int len;
	if(utf8 == NULL || *utf8 == 0)
		return 0;
	DField_FromUtf8(utf8, bytes, sizeof bytes);
	len = (int)strlen(bytes);
	if(len <= 0)
		return 0;
	// WM_CHAR 0x20 .. 0x7f swallowed by the subclass of 1.69/472 on ("B0 29")
	if(f->noAscii && len == 1 && (uint8_t)bytes[0] >= 0x20 && (uint8_t)bytes[0] < 0x80)
		return 0;
	FieldInsert(f, bytes, len);
	return 1;
}

/* an editing key press (the keys that type arrive as SDL_TEXTINPUT
 * instead): 1 changed, 0 handled without a change, -1 not a text-entry
 * key (the caller decides about Tab, Enter, Escape and the shortcuts) */
int DField_Key(DField_t* f, int vk, int ctrl)
{
	switch(vk)
	{
		case 0x08: // BackSpace
			if(f->selAll)
			{
				f->text[0] = 0;
				f->caret = 0;
				f->selAll = 0;
				return 1;
			}
			if(f->caret > 0)
			{
				int p = PrevChar(f->text, f->caret);
				memmove(f->text + p, f->text + f->caret, strlen(f->text + f->caret) + 1);
				f->caret = p;
				return 1;
			}
			return 0;
		case 0x2e: // Delete
			if(f->selAll)
			{
				f->text[0] = 0;
				f->caret = 0;
				f->selAll = 0;
				return 1;
			}
			if(f->text[f->caret])
			{
				int n = CharLen(f->text, f->caret);
				memmove(f->text + f->caret, f->text + f->caret + n, strlen(f->text + f->caret + n) + 1);
				return 1;
			}
			return 0;
		case 0x25: // Left
			if(f->selAll)
				f->selAll = 0;
			else if(f->caret > 0)
				f->caret = PrevChar(f->text, f->caret);
			return 0;
		case 0x27: // Right
			if(f->selAll)
				f->selAll = 0;
			else if(f->text[f->caret])
				f->caret += CharLen(f->text, f->caret);
			return 0;
		case 0x24: // Home
			f->selAll = 0;
			f->caret = 0;
			return 0;
		case 0x23: // End
			f->selAll = 0;
			f->caret = (int)strlen(f->text);
			return 0;
		case 'A':
			if(ctrl)
			{
				DField_SelectAll(f);
				return 0;
			}
			return -1;
		case 0x09: // Tab, Enter, Escape: the owner decides
		case 0x0d:
		case 0x1b:
			return -1;
		default:
			return -1;
	}
}

// fill a rectangle of a 32-bit buffer (pitch in pixels), clipped to bufW x bufH
void Dui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb)
{
	int xx, yy;
	for(yy = y; yy < y + h; yy++)
	{
		if(yy < 0 || yy >= bufH)
			continue;
		for(xx = x; xx < x + w; xx++)
			if(xx >= 0 && xx < bufW)
				pix[(size_t)yy * pitch + xx] = rgb;
	}
}

/* draw the field: the optional sunken box, the text centred or
 * left-aligned behind 2 pixels of padding, the selection highlight when
 * the whole text is selected, and a one-pixel caret in the text colour */
void DField_Draw(const DField_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	OsFont_t* font, uint32_t rgb, int centred, int focused, int frame)
{
	int fontH = OsPosix_FontHeight(font);
	int textW = OsPosix_FontTextWidth(font, f->text, -1);
	int tx, ty = y + (h - fontH) / 2;
	int inner = frame ? 3 : 0; // the box's border width
	if(frame)
	{
		Dui_FillRect(pix, pitch, bufW, bufH, x, y, w, h, UI_FIELD_BG);
		Dui_FillRect(pix, pitch, bufW, bufH, x, y, w, 1, UI_SHADOW);
		Dui_FillRect(pix, pitch, bufW, bufH, x, y, 1, h, UI_SHADOW);
		Dui_FillRect(pix, pitch, bufW, bufH, x + 1, y + 1, w - 2, 1, UI_DARK);
		Dui_FillRect(pix, pitch, bufW, bufH, x + 1, y + 1, 1, h - 2, UI_DARK);
		Dui_FillRect(pix, pitch, bufW, bufH, x, y + h - 1, w, 1, UI_LIGHT);
		Dui_FillRect(pix, pitch, bufW, bufH, x + w - 1, y, 1, h, UI_LIGHT);
	}
	tx = centred ? x + (w - textW) / 2 : x + inner + 2;
	// keep the caret in view when the text is wider than the field
	if(!centred && textW > w - 2 * inner - 4)
	{
		int caretX = OsPosix_FontTextWidth(font, f->text, f->caret);
		if(caretX > w - 2 * inner - 6)
			tx -= caretX - (w - 2 * inner - 6);
	}
	if(focused && f->selAll && f->text[0])
	{
		Dui_FillRect(pix, pitch, bufW, bufH, tx, ty, textW, fontH, UI_SELECT_BG);
		OsPosix_FontDraw(font, pix, pitch, bufW, bufH, tx, ty, f->text, UI_SELECT_TEXT);
	}
	else
		OsPosix_FontDraw(font, pix, pitch, bufW, bufH, tx, ty, f->text, rgb);
	if(focused && !f->selAll)
	{
		int cx = tx + OsPosix_FontTextWidth(font, f->text, f->caret);
		Dui_FillRect(pix, pitch, bufW, bufH, cx, ty, 1, fontH, rgb);
	}
}
