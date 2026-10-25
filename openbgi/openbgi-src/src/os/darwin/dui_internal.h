/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dui_internal.h - the macOS (SDL2) back end's own user interface
 *
 * The back end draws its own dialogs, text field, child windows and
 * debugger window from 32-bit software surfaces, exactly like the POSIX
 * back end does (src/os/posix/ui_internal.h is the model); SDL supplies
 * the windows, the renderers and the keyboard / mouse / IME input.  The
 * engine's own window and its event pump stay in window.c, which routes
 * the events of these auxiliary windows to the files here through
 * Dui_HandleEvent().
 *
 * The files:
 *   duikit.c   - a small dialog toolkit (labels, text fields, buttons,
 *                lists) and the auxiliary SDL window helper
 *   duifield.c - the single-line text field (editing + drawing)
 *   ddialogs.c - the script's dialogs, the file dialogs and the folder
 *                browser
 *   dchild.c   - the pop-up child windows of "B0 1x"
 *   dedit.c    - the text entry over the main window of "B0 2x"
 *   ddbgwin.c  - the debugger's window
 */
#ifndef BGI_OS_DARWIN_DUI_INTERNAL_H_
#define BGI_OS_DARWIN_DUI_INTERNAL_H_

#include "darwin_internal.h"
#include "bgi/msg.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- colours and metrics of the classic look (as in the POSIX back end) ------------------------
#define UI_FACE        0xf0f0f0 // 0x00RRGGBB: the dialog background and the buttons
#define UI_TEXT        0x000000
#define UI_FIELD_BG    0xffffff // the text fields
#define UI_SELECT_BG   0x3399ff // the selection highlight and its text
#define UI_SELECT_TEXT 0xffffff
#define UI_SHADOW      0xa0a0a0 // the 3D edges: shadow, dark, light
#define UI_DARK        0x696969
#define UI_LIGHT       0xffffff
#define UI_FRAME       0x7a7a7a // the outline of the default button

#define UI_FONT_H    16 // pixels: the UI font's cell height
#define UI_MARGIN    12 // pixels between the border and the controls
#define UI_BTN_W     80 // push button size in pixels
#define UI_BTN_H     24
#define UI_FIELD_H   22 // text field height in pixels

// ---- window.c: the engine's main window, for the files here -------------------------------------

// the engine's main window / renderer / staging surface; NULL before OS_WindowCreate
SDL_Window* Dwin_MainWindow(void);
SDL_Renderer* Dwin_MainRenderer(void);
SDL_Surface* Dwin_Stage(void); // the picture the engine draws (32-bit ARGB)
int Dwin_Alive(void);          // 1 between OS_WindowCreate and OS_WindowDestroy
Uint32 Dwin_MainWindowId(void); // 0 without a window
// an SDL key code as a Windows virtual-key code (0 when there is none)
int Dwin_TranslateVk(SDL_Keycode k);
// dispatch one SDL event to the engine's handlers (the dialogs' modal loop
// hands it the main window's events so that the game keeps repainting)
void Dwin_DispatchEvent(SDL_Event* e);

/* Called at the top of the main window's event dispatch: 1 when the event
 * belonged to one of the auxiliary windows (or was dropped while a dialog
 * is modal) and the engine must not see it. */
int Dui_HandleEvent(SDL_Event* e);
// 1 while one of the dialogs' modal loops is running
int Dui_ModalDepth(void);
// called from Dwin_Present: draw the text entry over the presented picture
void Dui_PresentEditOverlay(SDL_Renderer* ren);

// the event handlers Dui_HandleEvent routes to (1 = consumed)
int DuiEdit_HandleEvent(SDL_Event* e);  // dedit.c
int DuiChild_HandleEvent(SDL_Event* e); // dchild.c
int DuiDebug_HandleEvent(SDL_Event* e); // ddbgwin.c

// child windows: a blit to a child's DC (0 when `dc` is not one)
int Dui_ChildBlit(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, int srcX, int srcY,
	int srcW, int srcH);

void Dui_EditCreate(void); // the hidden 1 x 1 entry of OS_WindowCreate

// ---- DuiWin: an auxiliary SDL window with a 32-bit software surface ----------------------------

typedef struct DuiWin
{
	SDL_Window* win;    // NULL when it is not open
	SDL_Renderer* ren;  // may be NULL (a display without accelerated rendering)
	SDL_Texture* tex;   // streaming ARGB8888, w x h
	uint32_t* pix;      // w * h, 0x00RRGGBB, the software surface
	uint32_t* upload;   // w * h ARGB for SDL_UpdateTexture
	int w, h;           // client size in pixels
	int open;           // the window exists
} DuiWin_t;

// create the window (hidden when `hidden`) and its surface, filled UI_FACE; 1 ok
int DuiWin_Open(DuiWin_t* d, const char* titleSjis, int x, int y, int w, int h, int hidden);
void DuiWin_SetTitle(DuiWin_t* d, const char* titleSjis);
void DuiWin_Present(DuiWin_t* d); // upload `pix` and show it
void DuiWin_Close(DuiWin_t* d);
Uint32 DuiWin_Id(DuiWin_t* d); // the SDL window id, 0 when closed

// fill / blit into a 32-bit 0x00RRGGBB surface, clipped to w x h
void Dui_FillRect(uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h, uint32_t rgb);
// nearest-neighbour blit of a whole OsSurface into that surface (bpp 16 / 32)
void Dui_BlitSurfaceTo(uint32_t* pix, int bufW, int bufH, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s,
	int srcX, int srcY, int srcW, int srcH, const int* clip);
// one pixel of an OsSurface as 0x00RRGGBB
uint32_t Dui_SurfacePixel(const OsSurface_t* s, int x, int y);

// the "auto-confirm" switch of unattended runs: BGI_AUTO_DIALOG=1 (or "ok")
// chooses the default button, BGI_AUTO_DIALOG=cancel the escape one; 0 when
// the variable is not set (the normal interactive behaviour)
#define DUI_AUTO_NONE   0
#define DUI_AUTO_OK     1
#define DUI_AUTO_CANCEL 2
#define DUI_AUTO_SHOW   3 // draw the dialog, save its readback (BGI_DUMP_DIALOG) and cancel it
int Dui_AutoChoice(void);

// ---- duifield.c: the single-line text field ----------------------------------------------------

#define DUI_FIELD_TEXT 0x400 // bytes (the engine's own buffers are 0x100 / 0x30c)

typedef struct DField
{
	char text[DUI_FIELD_TEXT]; // in the OS text encoding (Shift-JIS normally)
	int limit;                 // bytes, 0 = unlimited (the buffer size still applies)
	int caret;                 // byte offset of the caret in `text`
	int selAll;                // the whole text is selected: typing replaces it
	int noAscii;               // reject single-byte printable characters ("B0 29")
} DField_t;

void DField_SetText(DField_t* f, const char* text); // replace (cut to the limit), caret at the end
void DField_SelectAll(DField_t* f);
/* a key press from an SDL event: the editing keys (BackSpace, Delete, the
 * arrows, Home, End); 1 changed, 0 handled without a change, -1 not a
 * text-entry key (Tab, Enter, Escape, anything that types) */
int DField_Key(DField_t* f, int vk, int ctrl);
// insert the text of an SDL_TEXTINPUT event (UTF-8); 1 when the field changed
int DField_InsertUtf8(DField_t* f, const char* utf8);
// UTF-8 -> the field's storage encoding, filled into out (n bytes)
void DField_FromUtf8(const char* utf8, char* out, size_t n);
// draw the field into a 32-bit buffer (pitch in pixels, bufW x bufH) at (x, y, w, h)
void DField_Draw(const DField_t* f, uint32_t* pix, int pitch, int bufW, int bufH, int x, int y, int w, int h,
	OsFont_t* font, uint32_t rgb, int centred, int focused, int frame);

// ---- duikit.c: the dialog toolkit --------------------------------------------------------------

#define UI_MAX_LABELS  16
#define UI_MAX_FIELDS  8
#define UI_MAX_BUTTONS 8
#define UI_MAX_LISTS   2
#define UI_LIST_ROW_H  (UI_FONT_H + 4)

typedef struct DUiLabel
{
	int x, y;
	char text[0x400]; // the OS text encoding
} DUiLabel_t;

typedef struct DUiField
{
	int x, y, w, h;
	DField_t f;
	int numeric, min, max;      // a number picker: digits only, kept within min .. max
	int digits;                 // a digits-only field
	const char* const* choices; // a choice picker: up / down step through these texts
	int nChoices;
} DUiField_t;

typedef struct DUiList
{
	int x, y, w, h;
	const char* const* items; // the OS text encoding (not copied)
	int count;
	int selected; // -1 for none
	int top;      // the first row shown
} DUiList_t;

typedef struct DUiButton
{
	int x, y, w, h;
	const char* text;
	int id;
} DUiButton_t;

typedef struct DDlg DDlg_t;
struct DDlg
{
	DuiWin_t win;
	int w, h;            // client size
	OsFont_t* font;      // the UI font
	DUiLabel_t labels[UI_MAX_LABELS];
	int nLabels;
	DUiField_t fields[UI_MAX_FIELDS];
	int nFields;
	DUiButton_t buttons[UI_MAX_BUTTONS];
	int nButtons;
	DUiList_t lists[UI_MAX_LISTS];
	int nLists;
	int focus;     // 0 .. nFields-1 a field, then the buttons, then the lists
	int defButton; // index, -1 none: Enter
	int escButton; // index, -1 none: Escape and the close box
	int icon;      // 0 none, else the character drawn in the icon disc
	uint32_t iconRgb;
	int pressed;         // button index under a held mouse button, -1 none
	uint32_t lastClick;  // OS_TicksMs of the last click on a list row (double click)
	int result, done;    // the id of the button that ended the dialog
	// a button handler may refuse to close (returns 0); id is the button's id
	int (*onButton)(DDlg_t* d, int id, void* ctx);
	void* ctx; // handed to onButton / onChange / onListSelect
	void (*onChange)(DDlg_t* d, int field, void* ctx);
	void (*onListSelect)(DDlg_t* d, int list, void* ctx);
	int redraw; // set by a handler that changed labels or fields
};

int DUi_LabelAdd(DDlg_t* d, int x, int y, const char* text);
int DUi_FieldAdd(DDlg_t* d, int x, int y, int w, const char* initial, int limit);
int DUi_ButtonAdd(DDlg_t* d, int x, int y, const char* text, int id);
int DUi_ListAdd(DDlg_t* d, int x, int y, int w, int rows, const char* const* items, int count, int selected);
void DUi_LabelSet(DDlg_t* d, int label, const char* text);
void DUi_FieldSetText(DUiField_t* f, const char* text);
// create the window centred on the screen, run the modal loop; the id of the button that ended it
int DUi_Run(DDlg_t* d, const char* titleSjis);
int DUi_TextWidth(const char* text);

#endif // BGI_OS_DARWIN_DUI_INTERNAL_H_
