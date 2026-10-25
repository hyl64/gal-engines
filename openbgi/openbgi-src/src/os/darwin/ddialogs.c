/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ddialogs.c - the script's dialogs and the shell's file / folder pickers
 *              of the macOS (SDL2) back end (inc/bgi/os.h, dui_internal.h)
 *
 * The original's dialog resources 0x71 (one text field), 0x72 / 0x7E (two
 * labelled fields), 0x77 (the player's name and birthday), 0x79 (a list
 * of choices, "B0 8C") and SHBrowseForFolder / GetOpenFileName are built
 * on the self-drawn dialog toolkit of duikit.c, exactly like the POSIX
 * back end's dialogs.c: no native controls, so the same look works
 * everywhere.  The Windows dialog results go back through the buffers of
 * os.h; the file and folder dialogs speak engine paths (Shift-JIS,
 * "C:\..." for the game directory) on their way in and out.
 *
 * BGI_AUTO_DIALOG=1 (or "ok") / "cancel" presses the default / cancel
 * button of the dialogs without a user - a switch for unattended runs
 * (the unit tests, the boot probe), off by default.  BGI_AUTO_FILE=<path>
 * answers the file dialogs with that (native) path.
 */
#include "dui_internal.h"

#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DLG_FIELD_W 320 // pixels: the text fields of the input dialogs

/* The dialogs are modal windows over the game's window: without one there
 * is nothing to be modal over, and the contract of inc/bgi/os.h says they
 * report "cancelled" (a script or a unit test may call them at any time;
 * the engine's own tests do). */
static int DialogReady(void)
{
	return Dwin_Alive();
}

// ---- engine paths <-> native paths ---------------------------------------------------------------

static void StripTrailingSlash(char* p)
{
	size_t l = strlen(p);
	while(l > 1 && p[l - 1] == '/')
		p[--l] = 0;
}

/* an absolute native path, with a symlink-free spelling when the file
 * exists (a save dialog's new file gets its parent resolved) */
static void MakeAbsolute(const char* path, char* out, size_t n)
{
	char full[PATH_MAX];
	char real[PATH_MAX];
	if(path == NULL || path[0] == 0)
	{
		if(getcwd(full, sizeof full) == NULL)
			snprintf(full, sizeof full, "/");
	}
	else if(path[0] == '/')
		snprintf(full, sizeof full, "%s", path);
	else
	{
		char cwd[PATH_MAX];
		if(getcwd(cwd, sizeof cwd) == NULL)
			snprintf(cwd, sizeof cwd, "/");
		snprintf(full, sizeof full, "%s/%s", cwd, path);
	}
	if(realpath(full, real) != NULL)
		snprintf(out, n, "%s", real);
	else
	{
		char parent[PATH_MAX], base[0x200];
		char* slash = strrchr(full, '/');
		if(slash != NULL && slash != full && (size_t)(slash - full) < sizeof parent)
		{
			size_t pl = (size_t)(slash - full);
			snprintf(base, sizeof base, "%s", slash + 1);
			memcpy(parent, full, pl);
			parent[pl] = 0;
			if(realpath(parent, real) != NULL)
				snprintf(out, n, "%s/%s", real, base);
			else
				snprintf(out, n, "%s", full);
		}
		else
			snprintf(out, n, "%s", full);
	}
	if(out[0] == 0)
		snprintf(out, n, "/");
	StripTrailingSlash(out);
}

// the game directory the one drive "C:" maps to, without a trailing '/'
static void GameDir(char* out, size_t n)
{
	snprintf(out, n, "%s", gPosixExeDir);
	StripTrailingSlash(out);
}

/* a native path as the engine spells paths: "C:\relative\to\file" when it
 * is under the game directory, else the absolute POSIX path with '\\'
 * separators (a leading '\\' is the file system root for OsPosix_Path) */
static void EnginePathFromNative(const char* native, char* out, size_t n)
{
	char full[PATH_MAX], game[PATH_MAX], utf8[PATH_MAX * 2];
	size_t i, gl;
	MakeAbsolute(native, full, sizeof full);
	GameDir(game, sizeof game);
	gl = strlen(game);
	if(gl > 0 && strncmp(full, game, gl) == 0 && (full[gl] == '/' || full[gl] == 0))
	{
		const char* rel = full + gl;
		if(*rel == '/')
			rel++;
		snprintf(utf8, sizeof utf8, "C:\\%s", rel);
	}
	else
		snprintf(utf8, sizeof utf8, "%s", full);
	for(i = 0; utf8[i] != 0; i++)
		if(utf8[i] == '/')
			utf8[i] = '\\';
	OS_Utf8ToSjis(utf8, out, n);
}

// ---- dialog 0x71: a caption, one field, OK / cancel ----------------------------------------------

int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	DDlg_t d;
	int r, y = UI_MARGIN;
	if(!DialogReady())
		return 0;
	memset(&d, 0, sizeof d);
	d.w = DLG_FIELD_W + 2 * UI_MARGIN;
	DUi_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial, maxLen);
	d.fields[0].f.selAll = 0; // the original leaves the caret at the end (EM_SETSEL len, len)
	d.fields[0].digits = digits;
	y += UI_FIELD_H + UI_MARGIN;
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = 1;
	d.focus = 0;
	r = DUi_Run(&d, title != NULL ? title : MSG_DLG_COMMENT_INPUT);
	if(r == 1 && out != NULL)
		snprintf(out, 0x100, "%s", d.fields[0].f.text);
	return r == 1;
}

/* dialog 0x72: two labelled fields, OK / cancel; `compact` (0x7E, "B0 87")
 * puts the fields side by side */
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	DDlg_t d;
	int r, y = UI_MARGIN;
	if(!DialogReady())
		return 0;
	memset(&d, 0, sizeof d);
	if(compact)
	{
		const int fw = DLG_FIELD_W / 2 - 8;
		d.w = DLG_FIELD_W + 2 * UI_MARGIN;
		DUi_LabelAdd(&d, UI_MARGIN, y, label1 != NULL ? label1 : MSG_DLG_STRING1);
		DUi_LabelAdd(&d, UI_MARGIN + fw + 16, y, label2 != NULL ? label2 : MSG_DLG_STRING2);
		y += UI_FONT_H + 4;
		DUi_FieldAdd(&d, UI_MARGIN, y, fw, initial1, maxLen1);
		DUi_FieldAdd(&d, UI_MARGIN + fw + 16, y, fw, initial2, maxLen2);
		y += UI_FIELD_H + UI_MARGIN;
	}
	else
	{
		d.w = DLG_FIELD_W + 2 * UI_MARGIN;
		DUi_LabelAdd(&d, UI_MARGIN, y, label1 != NULL ? label1 : MSG_DLG_STRING1);
		y += UI_FONT_H + 4;
		DUi_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial1, maxLen1);
		y += UI_FIELD_H + 8;
		DUi_LabelAdd(&d, UI_MARGIN, y, label2 != NULL ? label2 : MSG_DLG_STRING2);
		y += UI_FONT_H + 4;
		DUi_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial2, maxLen2);
		y += UI_FIELD_H + UI_MARGIN;
	}
	d.fields[0].digits = digits1;
	d.fields[1].digits = digits2;
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = 1;
	// the focus goes to the first field without an initial text (a NULL pointer)
	d.focus = initial1 == NULL ? 0 : initial2 == NULL ? 1 : 0;
	r = DUi_Run(&d, title != NULL ? title : MSG_DLG_DATA_INPUT);
	if(r == 1)
	{
		if(out1 != NULL)
			snprintf(out1, 0x100, "%s", d.fields[0].f.text);
		if(out2 != NULL)
			snprintf(out2, 0x100, "%s", d.fields[1].f.text);
	}
	return r == 1;
}

// ---- dialog 0x77: the player's name and birthday ------------------------------------------------

static const int kDaysOfMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // February always 29

typedef struct ProfileCtx
{
	int month, day; // field indexes of the two pickers
} ProfileCtx_t;

// every character of a name field must be double-byte (full width); an empty field passes
static int AllDoubleByte(const char* s)
{
	while(*s)
	{
		if(!OsCommon_SjisIsLead((uint8_t)*s) || !s[1])
			return 0;
		s += 2;
	}
	return 1;
}

// the OK button: a name field with a single-byte character is rejected and the dialog stays open
static int ProfileOk(DDlg_t* d, int id, void* ctx)
{
	static const char* const kNames[4] = {MSG_DLG_SURNAME, MSG_DLG_GIVEN_NAME, MSG_DLG_NICKNAME, MSG_DLG_PRONOUN};
	int i;
	(void)ctx;
	if(id != 1)
		return 1;
	for(i = 0; i < 4; i++)
	{
		if(!AllDoubleByte(d->fields[i].f.text))
		{
			char msg[0x200];
			snprintf(msg, sizeof msg, MSG_DLG_HALFWIDTH, kNames[i]);
			OS_MessageBox(msg, MSG_DLG_INPUT_ERROR, OS_MB_ICONHAND);
			return 0;
		}
	}
	return 1;
}

// the month picker changed: the day range follows
static void ProfileChange(DDlg_t* d, int field, void* ctx)
{
	ProfileCtx_t* pc = (ProfileCtx_t*)ctx;
	if(field == pc->month)
	{
		int m = atoi(d->fields[pc->month].f.text);
		DUiField_t* day = &d->fields[pc->day];
		if(m >= 1 && m <= 12)
		{
			day->max = kDaysOfMonth[m - 1];
			if(atoi(day->f.text) > day->max)
			{
				snprintf(day->f.text, sizeof day->f.text, "1");
				day->f.caret = 1;
			}
		}
	}
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	DDlg_t d;
	ProfileCtx_t pc;
	int y = UI_MARGIN, labelW, fieldX, r;
	char num[16];
	if(!DialogReady())
		return 0;
	memset(&d, 0, sizeof d);
	labelW = DUi_TextWidth(MSG_DLG_NICKNAME) + 16; // the nickname label sets the width of the label column
	fieldX = UI_MARGIN + labelW;
	d.w = fieldX + 200 + UI_MARGIN;

	DUi_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_SURNAME);
	DUi_FieldAdd(&d, fieldX, y, 90, surname, 10);
	DUi_LabelAdd(&d, fieldX + 100, y + 3, MSG_DLG_GIVEN_NAME);
	DUi_FieldAdd(&d, fieldX + 100 + DUi_TextWidth(MSG_DLG_GIVEN_NAME) + 8, y,
		90 - DUi_TextWidth(MSG_DLG_GIVEN_NAME) + 2, givenName, 10);
	y += UI_FIELD_H + 8;
	DUi_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_NICKNAME);
	DUi_FieldAdd(&d, fieldX, y, 200, nickname, 10);
	y += UI_FIELD_H + 8;
	DUi_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_PRONOUN);
	DUi_FieldAdd(&d, fieldX, y, 200, pronoun, 10);
	y += UI_FIELD_H + 8;
	DUi_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_BIRTHDAY);
	snprintf(num, sizeof num, "%d", (int)(month != NULL ? *month : 0) + 1); // the engine holds 0-based selections
	pc.month = DUi_FieldAdd(&d, fieldX, y, 44, num, 2);
	d.fields[pc.month].numeric = 1;
	d.fields[pc.month].min = 1;
	d.fields[pc.month].max = 12;
	DUi_LabelAdd(&d, fieldX + 50, y + 3, MSG_DLG_MONTH);
	snprintf(num, sizeof num, "%d", (int)(day != NULL ? *day : 0) + 1);
	pc.day = DUi_FieldAdd(&d, fieldX + 50 + DUi_TextWidth(MSG_DLG_MONTH) + 12, y, 44, num, 2);
	d.fields[pc.day].numeric = 1;
	d.fields[pc.day].min = 1;
	d.fields[pc.day].max = 31; // 1..31 until the month changes
	DUi_LabelAdd(&d, fieldX + 50 + DUi_TextWidth(MSG_DLG_MONTH) + 12 + 50, y + 3, MSG_DLG_DAY);
	y += UI_FIELD_H + UI_MARGIN;
	DUi_ButtonAdd(&d, (d.w - UI_BTN_W) / 2, y, "OK", 1);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = -1;    // OK only: the dialog cannot be cancelled
	d.focus = d.nFields; // SetFocus(GetDlgItem(hwnd, IDOK))
	d.onButton = ProfileOk;
	d.onChange = ProfileChange;
	d.ctx = &pc;
	r = DUi_Run(&d, MSG_DLG_PROFILE_TITLE);
	if(r != 1)
		return 0;
	if(surname != NULL)
		snprintf(surname, 0x100, "%s", d.fields[0].f.text);
	if(givenName != NULL)
		snprintf(givenName, 0x100, "%s", d.fields[1].f.text);
	if(nickname != NULL)
		snprintf(nickname, 0x100, "%s", d.fields[2].f.text);
	if(pronoun != NULL)
		snprintf(pronoun, 0x100, "%s", d.fields[3].f.text);
	{
		int m = atoi(d.fields[pc.month].f.text), dd = atoi(d.fields[pc.day].f.text);
		if(m < 1)
			m = 1;
		if(dd < 1)
			dd = 1;
		if(month != NULL)
			*month = m - 1;
		if(day != NULL)
			*day = dd - 1;
	}
	return 1;
}

// ---- dialog 0x79 ("B0 8C"): a list of choices ---------------------------------------------------

#define SELECT_MAX 32

int OS_DialogSelect(char* out, size_t n, const char* caption, const char* items)
{
	DDlg_t d;
	char entries[SELECT_MAX][0x200];
	const char* rows[SELECT_MAX];
	char line[0x200];
	int count = 0, i, o, r, y = UI_MARGIN, width = 360, tw;
	if(!DialogReady() || items == NULL || items[0] == 0)
		return 0;
	// split the text into one entry per line (CR / LF)
	for(i = 0; items[i] != 0 && count < SELECT_MAX;)
	{
		o = 0;
		while(items[i] != 0 && items[i] != '\n' && items[i] != '\r' && o < (int)sizeof line - 1)
			line[o++] = items[i++];
		line[o] = 0;
		while(items[i] == '\n' || items[i] == '\r')
			i++;
		if(o == 0)
			continue;
		snprintf(entries[count], sizeof entries[count], "%s", line);
		rows[count] = entries[count];
		tw = DUi_TextWidth(line);
		if(tw + 2 * UI_MARGIN > width)
			width = tw + 2 * UI_MARGIN;
		count++;
	}
	if(count == 0)
		return 0;
	if(width > 640)
		width = 640;
	memset(&d, 0, sizeof d);
	d.w = width + 2 * UI_MARGIN;
	d.h = 0;
	DUi_ListAdd(&d, UI_MARGIN, y, width, count < 12 ? count : 12, rows, count, 0);
	y += d.lists[0].h + UI_MARGIN;
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = 1;
	d.focus = d.nLists > 0 ? d.nFields + d.nButtons + 0 : 0;
	r = DUi_Run(&d, caption != NULL && caption[0] != 0 ? caption : MSG_DLG_INPUT2_TITLE);
	if(r == 1 && d.lists[0].selected >= 0 && d.lists[0].selected < count && out != NULL && n > 0)
		snprintf(out, n, "%s", entries[d.lists[0].selected]);
	return r == 1;
}

// ---- the file dialogs ("80 3B", "81 38") --------------------------------------------------------

#define FD_MAX      512
#define FD_ROWS     13
#define FD_W        640
#define FD_MAX_FLT  8

typedef struct FileCtx
{
	int save;
	char dir[PATH_MAX];            // the directory shown (native, no trailing '/', "/" at the root)
	char names[FD_MAX][0x100];     // its entries, native (UTF-8)
	char shown[FD_MAX][0x100];     // the same for the list, in the OS text encoding
	int isdir[FD_MAX];
	const char* rows[FD_MAX];
	int count;
	char pats[FD_MAX_FLT][0x100];   // the patterns of each filter (';' separated)
	char descs[FD_MAX_FLT][0x200];  // and their descriptions (display text)
	int nfilters, filter;
	char filterText[0x300];         // "Filter: <description>"
	char chosen[PATH_MAX];          // the native path the dialog accepted
	int dirLabel, list, nameField, filterButton;
} FileCtx_t;

static int CompareNames(const void* a, const void* b)
{
	return strcasecmp((const char*)a, (const char*)b);
}

// 1 when the name matches one of the ';' separated patterns of the filter
static int MatchesFilter(const FileCtx_t* c, const char* name)
{
	char buf[0x100];
	char* p, *semi;
	snprintf(buf, sizeof buf, "%s", c->nfilters > 0 ? c->pats[c->filter] : "*");
	p = buf;
	while(p != NULL && *p)
	{
		semi = strchr(p, ';');
		if(semi != NULL)
			*semi = 0;
		if(OsCommon_WildcardMatch(p, name))
			return 1;
		p = semi != NULL ? semi + 1 : NULL;
	}
	return 0;
}

// build "<dir>/<name>"
static void JoinPath(const char* dir, const char* name, char* out, size_t n)
{
	if(strcmp(dir, "/") == 0)
		snprintf(out, n, "/%s", name);
	else
		snprintf(out, n, "%s/%s", dir, name);
}

// list the directory: ".." first (unless at the root), then its directories, then the matching files
static void FileFill(DDlg_t* d, FileCtx_t* c)
{
	DIR* dp;
	struct dirent* e;
	int i, j, start = 0;
	c->count = 0;
	if(strcmp(c->dir, "/") != 0)
	{
		snprintf(c->names[0], sizeof c->names[0], "..");
		c->isdir[0] = 1;
		c->count = start = 1;
	}
	dp = opendir(c->dir);
	if(dp != NULL)
	{
		while((e = readdir(dp)) != NULL && c->count < FD_MAX)
		{
			char full[PATH_MAX * 2];
			struct stat st;
			int isdir;
			if(e->d_name[0] == '.' || strcmp(e->d_name, "..") == 0)
				continue;
			JoinPath(c->dir, e->d_name, full, sizeof full);
			if(stat(full, &st) != 0)
				continue;
			isdir = S_ISDIR(st.st_mode);
			if(!isdir && !MatchesFilter(c, e->d_name))
				continue;
			snprintf(c->names[c->count], sizeof c->names[c->count], "%s", e->d_name);
			c->isdir[c->count] = isdir;
			c->count++;
		}
		closedir(dp);
	}
	// directories first, then files; each group alphabetically (".." stays first)
	for(i = start; i < c->count; i++)
		for(j = i + 1; j < c->count; j++)
		{
			int swap = 0;
			if(c->isdir[i] != c->isdir[j])
				swap = c->isdir[j] && !c->isdir[i];
			else if(CompareNames(c->names[i], c->names[j]) > 0)
				swap = 1;
			if(swap)
			{
				char tmp[0x100];
				int ti = c->isdir[i];
				snprintf(tmp, sizeof tmp, "%s", c->names[i]);
				snprintf(c->names[i], sizeof c->names[i], "%s", c->names[j]);
				snprintf(c->names[j], sizeof c->names[j], "%s", tmp);
				c->isdir[i] = c->isdir[j];
				c->isdir[j] = ti;
			}
		}
	for(i = 0; i < c->count; i++)
	{
		char shown[0x100];
		OS_Utf8ToSjis(c->names[i], shown, sizeof shown);
		if(c->isdir[i])
			snprintf(c->shown[i], sizeof c->shown[i], "%s/", shown);
		else
			snprintf(c->shown[i], sizeof c->shown[i], "%s", shown);
		c->rows[i] = c->shown[i];
	}
	if(d != NULL)
	{
		char shownDir[0x400];
		DUiList_t* l = &d->lists[c->list];
		l->count = c->count;
		l->selected = c->count > 1 ? 1 : -1;
		l->top = 0;
		OS_Utf8ToSjis(c->dir, shownDir, sizeof shownDir);
		DUi_LabelSet(d, c->dirLabel, shownDir);
		snprintf(c->filterText, sizeof c->filterText, "Filter: %s",
			c->nfilters > 0 ? c->descs[c->filter] : "*.*");
		if(c->filterButton >= 0)
			d->buttons[c->filterButton].text = c->filterText;
		d->redraw = 1;
	}
}

// enter the selected row (a directory) or ".."; the dialog stays open
static void FileEnter(DDlg_t* d, FileCtx_t* c)
{
	int sel = d->lists[c->list].selected;
	char next[PATH_MAX];
	if(sel < 0 || sel >= c->count || !c->isdir[sel])
		return;
	if(strcmp(c->names[sel], "..") == 0)
	{
		char* slash;
		snprintf(next, sizeof next, "%s", c->dir);
		slash = strrchr(next, '/');
		if(slash == NULL)
			return;
		if(slash == next)
			slash[1] = 0;
		else
			*slash = 0;
	}
	else
		JoinPath(c->dir, c->names[sel], next, sizeof next);
	snprintf(c->dir, sizeof c->dir, "%s", next);
	FileFill(d, c);
}

// the native path currently named by the field or the selection ("" for none)
static void FileChosenName(DDlg_t* d, FileCtx_t* c, char* out, size_t n)
{
	int sel;
	out[0] = 0;
	if(c->nameField >= 0 && d->fields[c->nameField].f.text[0] != 0)
	{
		OS_SjisToUtf8(d->fields[c->nameField].f.text, out, n);
		return;
	}
	sel = d->lists[c->list].selected;
	if(sel >= 0 && sel < c->count && !c->isdir[sel])
		snprintf(out, n, "%s", c->names[sel]);
}

// add the default extension when the name has none
static void AddDefaultExt(const char* name, const char* ext, char* out, size_t n)
{
	const char* base = strrchr(name, '/');
	const char* dot;
	base = base != NULL ? base + 1 : name;
	dot = strrchr(base, '.');
	if(ext != NULL && ext[0] != 0 && (dot == NULL || dot == base))
		snprintf(out, n, "%s.%s", name, ext);
	else
		snprintf(out, n, "%s", name);
}

static void FileFilterCycle(DDlg_t* d, FileCtx_t* c);

static int FileButton(DDlg_t* d, int id, void* ctx)
{
	FileCtx_t* c = (FileCtx_t*)ctx;
	if(id == 3)
	{ // the filter button: the next filter, the dialog stays open
		FileFilterCycle(d, c);
		return 0;
	}
	if(id == 0)
		return 1; // Cancel
	if(id == 2)
	{ // Up
		char next[PATH_MAX];
		char* slash;
		snprintf(next, sizeof next, "%s", c->dir);
		slash = strrchr(next, '/');
		if(slash == NULL)
			return 0;
		if(slash == next)
			slash[1] = 0;
		else
			*slash = 0;
		snprintf(c->dir, sizeof c->dir, "%s", next);
		FileFill(d, c);
		return 0;
	}
	// Open / Save: a typed name may be a directory to enter, else it is the choice
	{
		char name[PATH_MAX], full[PATH_MAX * 2];
		struct stat st;
		int sel;
		FileChosenName(d, c, name, sizeof name);
		if(name[0] == 0)
		{ // nothing typed: a selected directory is entered, anything else keeps the dialog open
			sel = d->lists[c->list].selected;
			if(sel >= 0 && sel < c->count && c->isdir[sel])
				FileEnter(d, c);
			return 0;
		}
		JoinPath(c->dir, name, full, sizeof full);
		if(stat(full, &st) == 0 && S_ISDIR(st.st_mode))
		{
			snprintf(c->dir, sizeof c->dir, "%s", full);
			FileFill(d, c);
			return 0;
		}
		snprintf(c->chosen, sizeof c->chosen, "%s", full);
		return 1;
	}
}

static void FileFilterCycle(DDlg_t* d, FileCtx_t* c)
{
	if(c->nfilters <= 1)
		return;
	c->filter = (c->filter + 1) % c->nfilters;
	FileFill(d, c);
	d->redraw = 1;
}

// the dialog on the toolkit; `initialDir` native, `initialName` native or empty
static int RunFileDialog(FileCtx_t* c, const char* title, const char* initialDir, const char* initialName,
	const char* defExt)
{
	DDlg_t d;
	char name[0x200];
	int y = UI_MARGIN, r, fieldX;
	memset(&d, 0, sizeof d);
	snprintf(c->dir, sizeof c->dir, "%s", initialDir);
	FileFill(NULL, c);
	d.w = FD_W + 2 * UI_MARGIN;
	c->dirLabel = DUi_LabelAdd(&d, UI_MARGIN, y, "");
	y += UI_FONT_H + 6;
	c->list = DUi_ListAdd(&d, UI_MARGIN, y, FD_W, FD_ROWS, c->rows, c->count, -1);
	y += d.lists[c->list].h + 8;
	fieldX = UI_MARGIN + DUi_TextWidth("File name:") + 8;
	DUi_LabelAdd(&d, UI_MARGIN, y + 3, "File name:");
	c->nameField = DUi_FieldAdd(&d, fieldX, y, d.w - fieldX - UI_MARGIN - 12, initialName != NULL ? initialName : "", 0);
	y += UI_FIELD_H + 8;
	c->filterButton = DUi_ButtonAdd(&d, UI_MARGIN, y, "Filter:", 3);
	d.buttons[c->filterButton].w = 220;
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, c->save ? "Save" : "Open", 1);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	y += UI_BTN_H + UI_MARGIN;
	DUi_ButtonAdd(&d, UI_MARGIN + 220 + 8, y - UI_BTN_H - UI_MARGIN, "Up", 2);
	d.h = y;
	d.defButton = 1; // the Open / Save button
	d.escButton = 2; // Cancel
	d.focus = d.nFields + d.nButtons + c->list;
	d.onButton = FileButton;
	d.ctx = c;
	FileFill(&d, c);
	(void)name;
	r = DUi_Run(&d, title != NULL && title[0] != 0 ? title : "Open");
	if(r != 1)
		return 0;
	if(c->save)
	{
		char withExt[PATH_MAX * 2];
		AddDefaultExt(c->chosen, defExt, withExt, sizeof withExt);
		snprintf(c->chosen, sizeof c->chosen, "%s", withExt);
	}
	return 1;
}

// the file name part of an engine path, native
static void InitialNameNative(const char* enginePath, char* out, size_t n)
{
	char native[PATH_MAX];
	const char* part;
	out[0] = 0;
	if(enginePath == NULL || enginePath[0] == 0)
		return;
	OsPosix_Path(enginePath, native, sizeof native);
	part = OsCommon_FilePart(native);
	snprintf(out, n, "%s", part);
}

// the directory an engine path points into, native; the game directory when empty or unknown
static void InitialDirNative(const char* engineDir, char* out, size_t n)
{
	char native[PATH_MAX];
	struct stat st;
	if(engineDir != NULL && engineDir[0] != 0)
	{
		OsPosix_Path(engineDir, native, sizeof native);
		if(stat(native, &st) == 0 && S_ISDIR(st.st_mode))
		{
			MakeAbsolute(native, out, n);
			return;
		}
	}
	GameDir(native, sizeof native);
	MakeAbsolute(native, out, n);
}

/* one "<description> <pattern>" pair, separated by its last space (the
 * engine's "80 3B" filter is built that way) */
static void ParseFilter(const char* filter, char* desc, size_t dn, char* pat, size_t pn)
{
	char buf[0x400];
	char* space;
	snprintf(buf, sizeof buf, "%s", filter != NULL ? filter : "");
	space = strrchr(buf, ' ');
	if(space != NULL)
	{
		*space = 0;
		snprintf(desc, dn, "%s", buf);
		snprintf(pat, pn, "%s", space + 1);
	}
	else
	{
		snprintf(desc, dn, "%s", buf);
		snprintf(pat, pn, "*.*");
	}
}

int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir,
	const char* title)
{
	FileCtx_t* c;
	char desc[0x200], pat[0x100], native[PATH_MAX], name[0x200];
	int ok;
	const char* autoFile = getenv("BGI_AUTO_FILE");
	const char* autoChoice = getenv("BGI_AUTO_DIALOG");
	if(!DialogReady())
		return 0;
	if(autoFile != NULL && autoFile[0] != 0)
	{
		EnginePathFromNative(autoFile, path, n);
		return 1;
	}
	if(autoChoice != NULL && Dui_AutoChoice() == DUI_AUTO_CANCEL)
		return 0;
	if(autoChoice != NULL && Dui_AutoChoice() == DUI_AUTO_OK)
		return 0; // no BGI_AUTO_FILE: an unattended run chooses nothing
	c = (FileCtx_t*)calloc(1, sizeof *c);
	if(c == NULL)
		return 0;
	c->save = save;
	c->filterButton = -1;
	c->nameField = -1;
	c->nfilters = 1;
	ParseFilter(filter, desc, sizeof desc, pat, sizeof pat);
	snprintf(c->descs[0], sizeof c->descs[0], "%s", desc);
	snprintf(c->pats[0], sizeof c->pats[0], "%s", pat);
	InitialDirNative(initialDir, native, sizeof native);
	InitialNameNative(path, name, sizeof name);
	ok = RunFileDialog(c, title, native, name, defExt);
	if(ok)
		EnginePathFromNative(c->chosen, path, n);
	free(c);
	return ok;
}

int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	FileCtx_t* c;
	char native[PATH_MAX], name[0x200];
	int i, ok;
	const char* autoFile = getenv("BGI_AUTO_FILE");
	const char* autoChoice = getenv("BGI_AUTO_DIALOG");
	if(!DialogReady() || count < 1)
		return 0;
	if(autoFile != NULL && autoFile[0] != 0)
	{
		EnginePathFromNative(autoFile, path, n);
		return 1;
	}
	if(autoChoice != NULL && Dui_AutoChoice() == DUI_AUTO_CANCEL)
		return 0;
	if(autoChoice != NULL && Dui_AutoChoice() == DUI_AUTO_OK)
		return 0; // no BGI_AUTO_FILE: an unattended run chooses nothing
	c = (FileCtx_t*)calloc(1, sizeof *c);
	if(c == NULL)
		return 0;
	c->save = save;
	c->filterButton = -1;
	c->nameField = -1;
	c->nfilters = count < FD_MAX_FLT ? count : FD_MAX_FLT;
	for(i = 0; i < c->nfilters; i++)
	{
		snprintf(c->descs[i], sizeof c->descs[i], "%s", descs[i] != NULL ? descs[i] : "");
		snprintf(c->pats[i], sizeof c->pats[i], "%s", pats[i] != NULL ? pats[i] : "*.*");
	}
	InitialDirNative(initialDir, native, sizeof native);
	InitialNameNative(path, name, sizeof name);
	ok = RunFileDialog(c, title, native, name, defExt);
	if(ok)
		EnginePathFromNative(c->chosen, path, n);
	free(c);
	return ok;
}

// ---- the folder browser ("81 3A", SHBrowseForFolder) --------------------------------------------

#define BROWSE_MAX  512
#define BROWSE_ROWS 12
#define BROWSE_W    420

typedef struct Browse
{
	char dir[PATH_MAX];            // the directory shown (native, no trailing '/', "/" at the root)
	char names[BROWSE_MAX][0x100]; // its subdirectories, native
	char shown[BROWSE_MAX][0x100];
	const char* rows[BROWSE_MAX];
	int count;
	int pathLabel, list;
} Browse_t;

static void BrowseFill(DDlg_t* d, Browse_t* b)
{
	DIR* dp;
	struct dirent* e;
	int i, start = 0;
	b->count = 0;
	if(strcmp(b->dir, "/") != 0)
	{
		snprintf(b->names[0], sizeof b->names[0], "..");
		b->count = start = 1;
	}
	dp = opendir(b->dir);
	if(dp != NULL)
	{
		while((e = readdir(dp)) != NULL && b->count < BROWSE_MAX)
		{
			char full[PATH_MAX * 2];
			struct stat st;
			if(e->d_name[0] == '.')
				continue;
			JoinPath(b->dir, e->d_name, full, sizeof full);
			if(stat(full, &st) == 0 && S_ISDIR(st.st_mode))
				snprintf(b->names[b->count++], sizeof b->names[0], "%s", e->d_name);
		}
		closedir(dp);
		qsort(b->names[start], (size_t)(b->count - start), sizeof b->names[0], CompareNames);
	}
	for(i = 0; i < b->count; i++)
	{
		OS_Utf8ToSjis(b->names[i], b->shown[i], sizeof b->shown[i]);
		b->rows[i] = b->shown[i];
	}
	if(d != NULL)
	{
		char shownDir[0x400];
		DUiList_t* l = &d->lists[b->list];
		l->count = b->count;
		l->selected = b->count > 1 ? 1 : -1;
		l->top = 0;
		OS_Utf8ToSjis(b->dir, shownDir, sizeof shownDir);
		DUi_LabelSet(d, b->pathLabel, shownDir);
		d->focus = d->nFields + d->nButtons + b->list;
	}
}

static void BrowseEnter(DDlg_t* d, Browse_t* b)
{
	int sel = d->lists[b->list].selected;
	char next[PATH_MAX];
	if(sel < 0 || sel >= b->count)
		return;
	if(strcmp(b->names[sel], "..") == 0)
	{
		char* slash;
		snprintf(next, sizeof next, "%s", b->dir);
		slash = strrchr(next, '/');
		if(slash == NULL)
			return;
		if(slash == next)
			slash[1] = 0;
		else
			*slash = 0;
	}
	else
		JoinPath(b->dir, b->names[sel], next, sizeof next);
	snprintf(b->dir, sizeof b->dir, "%s", next);
	BrowseFill(d, b);
}

// 1 = Select ends the dialog; 2 = Open enters the selected folder
static int BrowseButton(DDlg_t* d, int id, void* ctx)
{
	if(id == 2)
	{
		BrowseEnter(d, (Browse_t*)ctx);
		return 0;
	}
	return 1;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	Browse_t* b;
	DDlg_t d;
	struct stat st;
	char native[PATH_MAX];
	int r, y = UI_MARGIN;
	if(!DialogReady() || Dui_AutoChoice() == DUI_AUTO_CANCEL)
		return 0;
	b = (Browse_t*)calloc(1, sizeof *b);
	if(b == NULL)
		return 0;
	if(initial != NULL && initial[0] != 0)
		OsPosix_Path(initial, native, sizeof native);
	else
		snprintf(native, sizeof native, "%s", gPosixExeDir);
	if(stat(native, &st) == 0 && S_ISDIR(st.st_mode))
		MakeAbsolute(native, b->dir, sizeof b->dir);
	else
		MakeAbsolute(gPosixExeDir, b->dir, sizeof b->dir);
	if(Dui_AutoChoice() == DUI_AUTO_OK) // an unattended run takes the directory it starts in
	{
		EnginePathFromNative(b->dir, path, n);
		free(b);
		return 1;
	}
	memset(&d, 0, sizeof d);
	d.w = BROWSE_W + 2 * UI_MARGIN;
	b->pathLabel = DUi_LabelAdd(&d, UI_MARGIN, y, "");
	y += UI_FONT_H + 6;
	b->list = DUi_ListAdd(&d, UI_MARGIN, y, BROWSE_W, BROWSE_ROWS, b->rows, 0, -1);
	y += d.lists[b->list].h + UI_MARGIN;
	DUi_ButtonAdd(&d, UI_MARGIN, y, MSG_BROWSE_OPEN, 2);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, MSG_BROWSE_SELECT, 1);
	DUi_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, MSG_BROWSE_CANCEL, 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0; // Enter and a double click open the folder
	d.escButton = 2;
	d.onButton = BrowseButton;
	d.ctx = b;
	BrowseFill(&d, b);
	r = DUi_Run(&d, title != NULL && title[0] != 0 ? title : MSG_BROWSE_TITLE);
	if(r == 1)
		EnginePathFromNative(b->dir, path, n);
	free(b);
	return r == 1;
}
