/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/darwin_ui.c - unit test of the macOS (SDL2) back end's own user
 *                     interface (src/os/darwin/: dchild.c, dedit.c,
 *                     duifield.c, duikit.c, ddialogs.c, ddbgwin.c);
 *                     built as bin/test_darwin_ui by `make test`
 *
 * Everything runs on SDL's dummy video driver, so no screen is needed:
 * a main window, eight child windows, the hidden text entry over it,
 * the script's dialogs and the file / folder pickers are exercised
 * through the same inc/bgi/os.h calls the engine makes.  The dialogs are
 * answered by BGI_AUTO_DIALOG (the unattended-run switch of the back
 * end), the file dialogs by BGI_AUTO_FILE; the message boxes only print
 * (BGI_MSGBOX_STDERR).
 *
 * The groups of tests, in the order they run:
 *
 *   window    - the main window comes up under the dummy driver
 *   children  - create, show, move, position, DC blits and close of the
 *               eight "B0 1x" pop-ups, and their lifetime events
 *   edit      - the "B0 2x" entry: text set / read, the byte limit cut to
 *               whole characters, typed text (Shift-JIS conversion), the
 *               reject-ASCII switch, Tab handing the focus back
 *   dialogs   - OS_DialogInput1 / Input2 / Profile / Select results,
 *               confirmed (auto OK) and cancelled (auto Cancel)
 *   files     - OS_FileDialog / FileDialogN choosing BGI_AUTO_FILE and
 *               cancelling, OS_BrowseFolder taking its initial folder
 *   debug     - the debugger's window opens, presents and closes
 *   msgbox    - the automatic answer of OS_MessageBox
 *
 * The scratch files live in BGI_TEST_TMP (else /tmp).
 */
#include "bgi/os.h"
// the back end event dispatch: SDL_PushEvent of this SDL2-compat build
// crashes on its ABI conversion, so synthetic input goes straight through
// the same dispatch function the SDL queue feeds (window.c)
#include "../src/os/darwin/dui_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int gFails; // the number of failed checks so far
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

static void CheckStr(const char* what, const char* got, const char* want)
{
	if(got == NULL || strcmp(got, want) != 0)
	{
		printf("  FAIL %s: got \"%s\", want \"%s\"\n", what, got != NULL ? got : "(null)", want);
		gFails++;
	}
}

// ---- the engine's window handlers ---------------------------------------------------------------

static int gPaint, gEditPaint, gChildPaint[8], gChildDestroyed[8], gCloseRequests;
static uint32_t gChildPix[16 * 16];
static int gSurfW = 16, gSurfH = 16;

static void OnPaint(void)
{
	gPaint++;
}

static void OnEditPaint(void)
{
	gEditPaint++;
}

// what the engine's Child_OnPaint does: present the back buffer through the child's DC
static void OnChildPaint(int index, OsDc_t* dc)
{
	OsSurface_t s;
	if(index < 0 || index >= 8)
		return;
	gChildPaint[index]++;
	if(dc == NULL)
		return;
	s.pixels = gChildPix;
	s.pitch = gSurfW * 4;
	s.width = gSurfW;
	s.height = gSurfH;
	s.bpp = 32;
	OS_StretchToWindow(dc, 0, 0, gSurfW, gSurfH, &s, NULL);
}

static void OnChildDestroyed(int index)
{
	if(index >= 0 && index < 8)
		gChildDestroyed[index]++;
}

static void OnCloseRequest(void)
{
	gCloseRequests++;
}

static const OsEventHandlers_t kHandlers = {
	.paint = OnPaint,
	.edit_paint = OnEditPaint,
	.child_paint = OnChildPaint,
	.child_destroyed = OnChildDestroyed,
	.close_request = OnCloseRequest,
};

// ---- small helpers ------------------------------------------------------------------------------

// the native path of an engine path (the back end's OS_PathNative service)
static void NativeOf(const char* enginePath, char* out, size_t n)
{
	OS_PathNative(enginePath, out, n);
}

// 1 when the engine path names the same file system object as `native`
static int SameThing(const char* enginePath, const char* native)
{
	char a[PATH_MAX], b[PATH_MAX], want[PATH_MAX];
	NativeOf(enginePath, a, sizeof a);
	if(realpath(a, b) == NULL || realpath(native, want) == NULL)
		return 0;
	return strcmp(b, want) == 0;
}

static void PushText(const char* utf8)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = SDL_TEXTINPUT;
	e.text.windowID = Dwin_MainWindowId();
	snprintf(e.text.text, sizeof e.text.text, "%s", utf8);
	Dwin_DispatchEvent(&e); // the entry takes it from Dui_HandleEvent, as from the pump
}

static void PushKey(SDL_Keycode sym)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = SDL_KEYDOWN;
	e.key.windowID = Dwin_MainWindowId();
	e.key.keysym.sym = sym;
	Dwin_DispatchEvent(&e);
}

// ---- the groups ---------------------------------------------------------------------------------

static void TestWindow(void)
{
	OsWindowDesc_t d;
	d.title = "OpenBGI darwin UI test";
	d.x = 40;
	d.y = 40;
	d.width = 800;
	d.height = 600;
	OS_SetEventHandlers(&kHandlers);
	CHECK(OS_WindowCreate(&d));
	CHECK(OS_WindowAlive());
	OS_WindowSetPicture(800, 600);
	CHECK(OS_WindowGetDc() != NULL);
	OS_WindowReleaseDc(NULL);
}

static void TestChildren(void)
{
	int i, x, y;
	int other;
	OsChild_t* c[8];
	for(i = 0; i < 8; i++)
	{
		c[i] = OS_ChildCreate(i, "child", 100 + i * 20, 200 + i * 10, 64, 48);
		CHECK(c[i] != NULL);
	}
	// a ninth window and a slot in use are refused
	other = 0;
	CHECK(OS_ChildCreate(8, "bad", 0, 0, 64, 48) == NULL);
	CHECK(OS_ChildCreate(0, "dup", 0, 0, 64, 48) == NULL);
	(void)other;
	for(i = 0; i < 8; i++)
	{
		OsDc_t* dc;
		CHECK(gChildDestroyed[i] == 0);
		OS_ChildShow(c[i], 1);
		CHECK(gChildPaint[i] >= 1); // showing asks the engine to paint
		dc = OS_ChildGetDc(c[i]);
		CHECK(dc != NULL);
		OS_StretchToWindow(dc, 0, 0, 16, 16, NULL, NULL); // NULL surface is refused, not a crash
		OS_ChildReleaseDc(c[i], dc);
		OS_ChildMove(c[i], 300 + i * 11, 100 + i * 7, 80, 60);
		CHECK(gChildPaint[i] >= 2); // a move repaints
		OS_ChildGetPos(c[i], &x, &y);
		CHECK(x == 300 + i * 11 && y == 100 + i * 7);
		OS_ChildSetTitle(c[i], "child moved");
		OS_ChildShow(c[i], 0);
	}
	for(i = 0; i < 8; i++)
	{
		OS_ChildClose(c[i]);
		CHECK(gChildDestroyed[i] == 1);
	}
	// a closed slot comes back
	c[3] = OS_ChildCreate(3, "again", 50, 60, 0x20, 0x20);
	CHECK(c[3] != NULL);
	OS_ChildClose(c[3]);
	CHECK(gChildDestroyed[3] == 2);
}

static void TestEdit(void)
{
	char buf[0x100];
	const char* face = "\x82\x6c\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4e"; // "ＭＳ ゴシック"
	OS_EditMove(10, 20, 200, 40);
	OS_EditSetFont(face, 16, 8);
	OS_EditShow(1);
	OS_EditLimitText(8);
	OS_EditSetText("HelloWorld");
	CHECK(OS_EditGetText(buf, sizeof buf) == 8);
	CheckStr("edit limit", buf, "HelloWor");
	OS_EditSetText("abc");
	CHECK(OS_EditGetText(buf, sizeof buf) == 3);
	CheckStr("edit text", buf, "abc");
	// typed text arrives as UTF-8 and is stored in Shift-JIS
	OS_EditSelectAll();
	OS_EditFocus(1);
	PushText("xy");
	CHECK(OS_EditGetText(buf, sizeof buf) == 2);
	CheckStr("typing replaces the selection", buf, "xy");
	PushKey(SDLK_BACKSPACE);
	CHECK(OS_EditGetText(buf, sizeof buf) == 1);
	CheckStr("backspace", buf, "x");
	// a full-width character (UTF-8 "あ" -> Shift-JIS 82 a0)
	PushText("\xe3\x81\x82");
	CHECK(OS_EditGetText(buf, sizeof buf) == 3);
	CheckStr("full-width typing", buf, "x\x82\xa0");
	// the limit cuts to whole characters: "あいう" with 5 bytes keeps "あい"
	OS_EditLimitText(5);
	OS_EditSetText("\x82\xa0\x82\xa2\x82\xa4");
	CHECK(OS_EditGetText(buf, sizeof buf) == 4);
	CheckStr("limit keeps whole characters", buf, "\x82\xa0\x82\xa2");
	// "B0 29": a single printable ASCII character is swallowed
	OS_EditLimitText(0);
	OS_EditRejectAscii(1);
	OS_EditSetText("");
	PushText("z");
	CHECK(OS_EditGetText(buf, sizeof buf) == 0);
	PushText("\xe3\x81\x82");
	CHECK(OS_EditGetText(buf, sizeof buf) == 2);
	CheckStr("reject-ascii keeps full-width", buf, "\x82\xa0");
	OS_EditRejectAscii(0);
	// Tab hands the focus back and selects everything (the subclass of the original)
	PushKey(SDLK_TAB);
	OS_EditShow(0);
	OS_EditDestroy();
}

static void TestDialogs(void)
{
	char out[0x100], out1[0x100], out2[0x100];
	char surname[0x100], given[0x100], nick[0x100], pronoun[0x100];
	int32_t month = 0, day = 0;

	setenv("BGI_AUTO_DIALOG", "1", 1);
	memset(out, 0, sizeof out);
	CHECK(OS_DialogInput1("Input", "init", 8, 0, out) == 1);
	CheckStr("Input1 confirmed", out, "init");
	memset(out, 0, sizeof out);
	CHECK(OS_DialogInput1("Input", "123", 8, 1, out) == 1); // digits field, initial kept
	CheckStr("Input1 digits confirmed", out, "123");

	memset(out1, 0, sizeof out1);
	memset(out2, 0, sizeof out2);
	CHECK(OS_DialogInput2("Input2", "one", "first", 8, 0, out1, "two", "second", 8, 0, out2, 0) == 1);
	CheckStr("Input2 field 1", out1, "first");
	CheckStr("Input2 field 2", out2, "second");
	memset(out1, 0, sizeof out1);
	memset(out2, 0, sizeof out2);
	CHECK(OS_DialogInput2("Input2", "one", "1", 8, 1, out1, "two", "2", 8, 1, out2, 1) == 1); // compact "B0 87"
	CheckStr("Input2Ex compact field 1", out1, "1");
	CheckStr("Input2Ex compact field 2", out2, "2");

	surname[0] = given[0] = nick[0] = pronoun[0] = 0;
	month = 0;
	day = 0;
	CHECK(OS_DialogProfile(surname, given, nick, pronoun, &month, &day) == 1);
	CheckStr("profile surname", surname, "");
	CHECK(month == 0 && day == 0);

	memset(out, 0, sizeof out);
	CHECK(OS_DialogSelect(out, sizeof out, "Language", "Japanese\nEnglish\n") == 1);
	CheckStr("dialog select takes the first entry", out, "Japanese");

	setenv("BGI_AUTO_DIALOG", "cancel", 1);
	memset(out, 0x55, sizeof out);
	CHECK(OS_DialogInput1("Input", "init", 8, 0, out) == 0);
	CHECK((unsigned char)out[0] == 0x55); // nothing written when cancelled
	memset(out, 0, sizeof out);
	CHECK(OS_DialogSelect(out, sizeof out, "Language", "Japanese\nEnglish\n") == 0);
}

static void TestFiles(void)
{
	char dir[0x400], file[0x400], path[0x400];
	const char* descs[2] = {"BMP", "Text"};
	const char* pats[2] = {"*.bmp", "*.txt"};
	FILE* f;
	int r;
	snprintf(dir, sizeof dir, "%s/openbgi-darwin-ui-XXXXXX", getenv("BGI_TEST_TMP") ? getenv("BGI_TEST_TMP") : "/tmp");
	if(mkdtemp(dir) == NULL)
	{
		printf("  SKIP files: no scratch directory\n");
		return;
	}
	snprintf(file, sizeof file, "%s/picture.bmp", dir);
	f = fopen(file, "wb");
	if(f != NULL)
	{
		fputs("BM", f);
		fclose(f);
	}
	CHECK(f != NULL);
	setenv("BGI_AUTO_DIALOG", "1", 1);
	// BGI_AUTO_FILE answers the picker with that native path
	setenv("BGI_AUTO_FILE", file, 1);
	memset(path, 0, sizeof path);
	CHECK(OS_FileDialog(0, path, sizeof path, "BMP(*.bmp) *.bmp", "bmp", "C:\\", "Open") == 1);
	CHECK(SameThing(path, file));
	memset(path, 0, sizeof path);
	CHECK(OS_FileDialogN(1, path, sizeof path, descs, pats, 2, "", "C:\\", "Save") == 1);
	CHECK(SameThing(path, file));
	// without BGI_AUTO_FILE an unattended picker chooses nothing (and does not hang)
	unsetenv("BGI_AUTO_FILE");
	memset(path, 0, sizeof path);
	CHECK(OS_FileDialog(0, path, sizeof path, "BMP(*.bmp) *.bmp", "bmp", "C:\\", "Open") == 0);
	setenv("BGI_AUTO_DIALOG", "cancel", 1);
	memset(path, 0, sizeof path);
	CHECK(OS_FileDialogN(0, path, sizeof path, descs, pats, 2, "", "C:\\", "Open") == 0);
	// the folder browser of "81 3A" takes the folder it starts in
	setenv("BGI_AUTO_DIALOG", "1", 1);
	memset(path, 0, sizeof path);
	r = OS_BrowseFolder(path, sizeof path, "Select folder", "C:\\tests");
	CHECK(r == 1);
	if(r == 1)
	{
		char cwd[PATH_MAX], want[PATH_MAX];
		CHECK(getcwd(cwd, sizeof cwd) != NULL);
		snprintf(want, sizeof want, "%s/tests", cwd);
		CHECK(SameThing(path, want));
	}
	setenv("BGI_AUTO_DIALOG", "cancel", 1);
	CHECK(OS_BrowseFolder(path, sizeof path, "Select folder", "C:\\") == 0);
	unlink(file);
	rmdir(dir);
}

static void TestDebugWindow(void)
{
	OsSurface_t s;
	uint32_t pix[8 * 8];
	int i;
	CHECK(OS_DebugWindowAlive() == 0);
	CHECK(OS_DebugWindowOpen(320, 200, "OpenBGI debugger") == 1);
	CHECK(OS_DebugWindowAlive() == 1);
	CHECK(OS_DebugWindowOpen(320, 200, "again") == 0); // one at a time
	for(i = 0; i < 8 * 8; i++)
		pix[i] = 0x00112233u;
	s.pixels = pix;
	s.pitch = 8 * 4;
	s.width = 8;
	s.height = 8;
	s.bpp = 32;
	OS_DebugWindowPresent(&s); // larger and smaller surfaces are both clipped
	s.width = 4;
	s.height = 4;
	OS_DebugWindowPresent(&s);
	OS_DebugWindowClose();
	CHECK(OS_DebugWindowAlive() == 0);
}

static void TestMessageBox(void)
{
	setenv("BGI_MSGBOX_STDERR", "1", 1);
	CHECK(OS_MessageBox("hello", "test", OS_MB_OK) == OS_IDOK);
	CHECK(OS_MessageBox("hello", "test", OS_MB_YESNO) == OS_IDYES);
	CHECK(OS_MessageBox("hello", "test", OS_MB_YESNO | OS_MB_DEFBUTTON2) == OS_IDNO);
	CHECK(OS_MessageBox("hello", "test", OS_MB_OKCANCEL | OS_MB_DEFBUTTON2) == OS_IDCANCEL);
}

/* --show-dialogs: a short real-GUI run (no dummy driver) that opens every
 * dialog once, has its window's pixels read back (BGI_DUMP_DIALOG), and
 * cancels it - the visual proof of the SDL windows with nobody at the
 * screen.  Not part of the automatic tests; the normal run below is. */
static void ShowDialogs(void)
{
	char out[0x100], out1[0x100], out2[0x100];
	char surname[0x100], given[0x100], nick[0x100], pronoun[0x100];
	int32_t month = 0, day = 0;
	int r1, r2, r3, r4, r5, r6;
	setenv("BGI_AUTO_DIALOG", "show", 1);
	unsetenv("BGI_AUTO_FILE");
	if(getenv("BGI_DUMP_DIALOG") == NULL)
	{
		setenv("BGI_DUMP_DIALOG", "/tmp/darwin-dialog", 1);
		setenv("BGI_DUMP_DIALOG_ALL", "1", 1);
	}
	surname[0] = given[0] = nick[0] = pronoun[0] = 0;
	r1 = OS_DialogInput1("OpenBGI input", "", 32, 0, out);
	r2 = OS_DialogInput2("OpenBGI two fields", "Name", "", 32, 0, out1, "Text", "", 32, 0, out2, 0);
	r3 = OS_DialogInput2("OpenBGI compact", "One", "", 16, 1, out1, "Two", "", 16, 1, out2, 1);
	r4 = OS_DialogProfile(surname, given, nick, pronoun, &month, &day);
	r5 = OS_DialogSelect(out, sizeof out, "Language", "Japanese\nEnglish\n");
	r6 = OS_FileDialog(0, out, sizeof out, "Images(*.bmp) *.bmp", "bmp", "C:\\", "Open");
	printf("show-dialogs: Input1=%d Input2=%d Input2Ex=%d Profile=%d Select=%d File=%d Browse=%d\n", r1, r2, r3,
		r4, r5, r6, OS_BrowseFolder(out, sizeof out, "Select folder", "C:\\"));
}

int main(int argc, char** argv)
{
	const char* tmp;
	if(argc > 1 && strcmp(argv[1], "--show-dialogs") == 0)
	{
		setvbuf(stdout, NULL, _IONBF, 0);
		if(!OS_Init())
			return 1;
		TestWindow();
		ShowDialogs();
		OS_WindowDestroy();
		OS_Shutdown();
		return gFails != 0;
	}
	setvbuf(stdout, NULL, _IONBF, 0); // the crash reports below are useful line by line
	// the back end's own windows appear on SDL's dummy display: no screen needed
	if(getenv("SDL_VIDEODRIVER") == NULL)
		setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("BGI_MSGBOX_STDERR", "1", 1);
	tmp = getenv("BGI_TEST_TMP");
	if(tmp != NULL && tmp[0] != 0 && chdir(tmp) != 0)
		printf("  (BGI_TEST_TMP %s not usable)\n", tmp);
	printf("darwin_ui: begin\n");
	if(!OS_Init())
	{
		printf("  FAIL OS_Init\n");
		return 1;
	}
	TestWindow();
	printf("  window ok\n");
	TestChildren();
	printf("  children ok\n");
	TestEdit();
	printf("  edit ok\n");
	TestDialogs();
	printf("  dialogs ok\n");
	TestFiles();
	printf("  files ok\n");
	TestDebugWindow();
	printf("  debug window ok\n");
	TestMessageBox();
	printf("  message box ok\n");
	OS_WindowDestroy();
	OS_Shutdown();
	if(gFails != 0)
	{
		printf("darwin_ui: %d failure(s)\n", gFails);
		return 1;
	}
	printf("darwin_ui: all checks passed\n");
	return 0;
}
