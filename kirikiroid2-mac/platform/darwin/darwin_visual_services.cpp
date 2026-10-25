/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * darwin_visual_services.cpp - the visual services this back end does not have
 *
 * The engine registers an image decoder per format and asks the platform for a
 * cursor; the graphics loader (visual/GraphicsLoaderIntf.cpp) names all of them
 * at link time.  The desktop back end decodes what KiriKiri games actually
 * carry - PNG and TLG, in visual/LoadPNG.cpp and visual/LoadTLG.cpp - so the
 * other formats answer as unsupported instead of pretending: a game that needs
 * JPEG, JXR, WEBP, BPG or PVR loses those pictures and says so, and nothing
 * else changes.
 *
 * The cursor is the same story: games set a cursor shape by name, and a back
 * end with no cursor theme leaves the system arrow alone.
 */

#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsError.h"

#include <functional>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <string>

#include <SDL.h>
#include "GraphicsLoaderIntf.h"
#include "MsgIntf.h"
#include "ClipboardIntf.h"
#include "MenuItemIntf.h"
#include "WindowIntf.h"
#include "VideoOvlIntf.h"
#include "SysInitIntf.h"

#include <set>
#include <cstdlib>

// ---------------------------------------------------------------------------
// the formats this back end does not decode
// ---------------------------------------------------------------------------

static void UnsupportedFormat(const char* name)
{
	if(getenv("KRKR_DEBUG_IMAGE") != nullptr)
	{
		// who asked for a format this back end does not have
		void* frames[24];
		const int n = backtrace(frames, 24);
		fprintf(stderr, "[image] no decoder for %s, asked from:\n", name);
		backtrace_symbols_fd(frames, n, 2);
		fflush(stderr);
	}
	TVPThrowExceptionMessage(TVPCannotOpenStorage,
		ttstr(TJS_W("the macOS back end has no decoder for ")) + ttstr(name));
}

void TVPLoadJXR(void*, void*, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback,
	tTVPMetaInfoPushCallback, tTJSBinaryStream*, tjs_int, tTVPGraphicLoadMode)
{
	UnsupportedFormat("JPEG XR");
}

void TVPLoadWEBP(void*, void*, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback,
	tTVPMetaInfoPushCallback, tTJSBinaryStream*, tjs_int, tTVPGraphicLoadMode)
{
	UnsupportedFormat("WebP");
}

void TVPLoadBPG(void*, void*, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback,
	tTVPMetaInfoPushCallback, tTJSBinaryStream*, tjs_int, tTVPGraphicLoadMode)
{
	UnsupportedFormat("BPG");
}

iTVPTexture2D* TVPLoadPVRv3(tTJSBinaryStream* s,
	const std::function<void(const ttstr&, const tTJSVariant&)>& meta)
{
	/* The engine reaches this when a picture is to become a GPU texture; the
	 * Windows build's version does that for every format.  A software back end
	 * keeps its pictures in bitmaps, so answering nothing makes TVPLoadGraphic
	 * fall back to the bitmap loader, which handles every format the handler
	 * table knows. */
	(void)s;
	(void)meta;
	return nullptr;
}

void TVPLoadPVRv3(void*, void*, tTVPGraphicSizeCallback, tTVPGraphicScanLineCallback,
	tTVPMetaInfoPushCallback, tTJSBinaryStream*, tjs_int, tTVPGraphicLoadMode)
{
	UnsupportedFormat("PVR");
}

void TVPLoadHeaderBPG(void*, tTJSBinaryStream*, iTJSDispatch2**)
{
	UnsupportedFormat("BPG");
}

void TVPSaveAsJXR(void*, tTJSBinaryStream*, const iTVPBaseBitmap*, const ttstr&, iTJSDispatch2*)
{
	UnsupportedFormat("JPEG XR");
}

void TVPSaveAsTLG(void*, tTJSBinaryStream*, const iTVPBaseBitmap*, const ttstr&, iTJSDispatch2*)
{
	UnsupportedFormat("TLG");
}

void TVPLoadHeaderJXR(void*, tTJSBinaryStream*, iTJSDispatch2**)
{
	UnsupportedFormat("JPEG XR");
}

void TVPLoadHeaderWEBP(void*, tTJSBinaryStream* src, iTJSDispatch2**)
{
	// this is a probe, not a load: it must stay quiet about a picture that is not
	// its own, or it hides every format registered after it
	uint8_t magic[4] = {0};
	const tjs_uint64 at = src->GetPosition();
	const bool got = src->Read(magic, 4) == 4;
	src->SetPosition(at);
	if(!got || memcmp(magic, "RIFF", 4) != 0)
		return;
	UnsupportedFormat("WebP");
}

bool TVPAcceptSaveAsJXR(void*, const ttstr&, iTJSDispatch2**)
{
	return false;
}



bool TVPAcceptSaveAsTLG(void*, const ttstr&, iTJSDispatch2**)
{
	return false; // no TLG writer in this back end
}

void TVPLoadHeaderPVRv3(void*, tTJSBinaryStream* src, iTJSDispatch2**)
{
	// this is a probe, not a load: it must stay quiet about a picture that is not
	// its own, or it hides every format registered after it
	uint8_t magic[4] = {0};
	const tjs_uint64 at = src->GetPosition();
	const bool got = src->Read(magic, 4) == 4;
	src->SetPosition(at);
	if(!got || memcmp(magic, "PVR\\3", 4) != 0)
		return;
	UnsupportedFormat("PVR");
}

// ---------------------------------------------------------------------------
// the application's odds and ends
// ---------------------------------------------------------------------------

// the engine asks where it runs from (the original lives in the game folder)
ttstr TVPGetAppPath()
{
	char cwd[1024];
	if(getcwd(cwd, sizeof cwd) == nullptr)
		return ttstr("file://./");
	std::string path("file://");
	path += cwd;
	path += "/";
	return ttstr(path.c_str());
}

// no window is up in a tool, and no asynchronous work is running to stop
void TVPTerminateSync(int code)
{
	// The engine's error path (TVPShowScriptException) and System.exit() both
	// end here and expect the process to actually stop.  A silent no-op turned
	// every script exception into a zombie that kept looping with events
	// disabled, which is why a broken run never told us it was broken.
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}


void TVPDumpHWException()
{
}

void TVPOpenPatchLibUrl()
{
}

// the clipboard is the system's; SDL owns the pasteboard here
bool TVPClipboardGetText(ttstr& text)
{
	char* clip = SDL_GetClipboardText();
	if(clip == nullptr)
		return false;
	text = ttstr(clip);
	SDL_free(clip);
	return true;
}

void TVPClipboardSetText(const ttstr& text)
{
	tTJSNarrowStringHolder narrow(text.c_str());
	SDL_SetClipboardText((const char*)narrow);
}

// ---------------------------------------------------------------------------
// the last odds and ends the runtime asks for
// ---------------------------------------------------------------------------

// the engine's shutdown flag and the "quit when no window opened" switch
bool TVPSystemUninitCalled = false;
bool TVPTerminateOnNoWindowStartup = false;



// the clipboard holds text here, so whatever the engine asks for is there
bool TVPClipboardHasFormat(tTVPClipboardFormat format)
{
	(void)format;
	return true;
}




// games name a cursor; a back end without a cursor theme keeps the system one,
// so the name is recorded and no shape is set
static tjs_int gCursorShape = 0;


// ---------------------------------------------------------------------------
// what the Windows system layer would supply
// ---------------------------------------------------------------------------
//
// Some of the engine's files were written against the Windows back end's system
// layer (base/win32/SysInitImpl.cpp, environ/win32/SystemControl.cpp,
// base/win32/StorageImpl.cpp) and name a few globals and helpers from it.  The
// desktop host does not have that layer yet; these are the pieces it names,
// answered the way a host with no plug-ins, no popup menu and no SIMD tables
// would answer them.

ttstr TVPDataPath;          // where the game's data lives; the hosts sets it
ttstr TVPNativeProjectDir;  // the project folder of the original tool

void TVPGL_ASM_Init()
{
	// the SIMD blitters are never initialised here: TVPInitTVPGL points the
	// function pointers at the C implementations
}

// TVPJPEGLoadPrecision is defined by visual/LoadJPEG.cpp, now in the build

void TVPDetectCPU()
{
}

void TVPShowPopMenu(tTJSNI_MenuItem* item)
{
	(void)item; // no popup menus without a window to pop them up in
}



// list a folder's entries, the way the engine's own storage layer does: the
// callback gets the name and whether it is a folder
void TVPListDir(const std::string& folder, std::function<void(const std::string&, int)> cb)
{
	DIR* d = opendir(folder.c_str());
	if(d == nullptr)
		return;
	struct dirent* e;
	while((e = readdir(d)) != nullptr)
	{
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		struct stat st;
		std::string path = folder + "/" + e->d_name;
		int isDir = (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
		cb(e->d_name, isDir);
	}
	closedir(d);
}

// ---------------------------------------------------------------------------
// the system control, the plug-in registry and the window list
// ---------------------------------------------------------------------------
//
// environ/win32/SystemControl.cpp is the Windows application's control object:
// it owns the event enable state, runs the idle callbacks and keeps the window
// list.  A host that renders frames itself needs the names it exports, decided
// the way a host with no plug-ins, no gamepad and no window list would decide
// them.

#include "SystemControl.h"

// The Windows application owns one of these and the engine reaches for it from
// event code (TVPGetSystemEventDisabledState goes straight through it), so the
// desktop hosts a real instance rather than a null pointer.
static tTVPSystemControl gSystemControl;
tTVPSystemControl* TVPSystemControl = &gSystemControl;
bool TVPSystemControlAlive = true;

// the engine's set of plug-in names; nothing loads any here
std::set<ttstr> TVPRegisteredPlugins;

void TVPSetCommandLine(const tjs_char* name, const ttstr& value)
{
	(void)name;
	(void)value; // the original parses command line switches; a host has none
}

void TVPLoadInternalPlugins()
{
}

void TVPLoadInternalPlugin(const ttstr& name)
{
	(void)name;
}

// no child windows: a layer is drawn into the same screen
iWindowLayer* TVPCreateAndAddWindow(tTJSNI_Window* w)
{
	(void)w;
	return nullptr;
}

bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent)
{
	(void)keycode;
	(void)getcurrent;
	return false; // no gamepad is connected
}

// ---------------------------------------------------------------------------
// the last of the application's surface
// ---------------------------------------------------------------------------

// menu items, video overlays and image functions are script-side objects whose
// platform half is the window; this back end offers none of them yet, so their
// factories answer nothing rather than handing back a broken object
iTJSDispatch2* TVPCreateMenuItemObject(iTJSDispatch2* window)
{
	(void)window;
	return nullptr;
}

// TVPCreateNativeClass_VideoOverlay lives in visual/darwin/VideoOverlay_Darwin.cpp

// nothing to detach: no overlay window exists on this back end
void tTJSNI_VideoOverlay::DetachVideoOverlay()
{
}

void tTJSNI_VideoOverlay::ResetOverlayParams()
{
}

void tTJSNI_VideoOverlay::SetRectangleToVideoOverlay()
{
}

tTJSNativeClass* TVPCreateNativeClass_ImageFunction()
{
	return nullptr;
}

bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent)
{
	(void)keycode;
	(void)getcurrent;
	return false; // no input is being polled outside the window's events
}


void TVPCauseAtInstallExtensionClass(iTJSDispatch2* global)
{
	(void)global;
}

tjs_int TVPGetCommandLineArgumentGeneration()
{
	return 0; // the command line never changes here
}

// ---------------------------------------------------------------------------
// the screen's metrics and the application's path
// ---------------------------------------------------------------------------

#include "Application.h"
#include "TVPScreen.h"
#include "WaveIntf.h"
#include "tvpgl.h"


// there is one display; its usable area is the SDL desktop mode's size
static void DesktopSize(int& w, int& h)
{
	w = 0;
	h = 0;
	if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
		return;
	SDL_DisplayMode mode;
	if(SDL_GetDesktopDisplayMode(0, &mode) == 0)
	{
		w = mode.w;
		h = mode.h;
	}
}

int tTVPScreen::GetWidth()
{
	int w = 0, h = 0;
	DesktopSize(w, h);
	return w;
}

int tTVPScreen::GetHeight()
{
	int w = 0, h = 0;
	DesktopSize(w, h);
	return h;
}

int tTVPScreen::GetDesktopLeft()
{
	return 0;
}

int tTVPScreen::GetDesktopTop()
{
	return 0;
}

int tTVPScreen::GetDesktopWidth()
{
	int w = 0, h = 0;
	DesktopSize(w, h);
	return w;
}

int tTVPScreen::GetDesktopHeight()
{
	int w = 0, h = 0;
	DesktopSize(w, h);
	return h;
}

// ---------------------------------------------------------------------------
// sound buffers: this back end has no sound device yet
// ---------------------------------------------------------------------------

tTJSNativeClass* TVPCreateNativeClass_CDDASoundBuffer()
{
	return nullptr;
}

tTJSNativeClass* TVPCreateNativeClass_MIDISoundBuffer()
{
	return nullptr;
}

// TVPCreateNativeClass_WaveSoundBuffer lives in visual/darwin/SoundBuffer_Darwin.cpp

tTJSNativeClass* TVPCreateNativeClass_BitmapLayerTreeOwner()
{
	return nullptr;
}


// TVPCreateNativeClass_MenuItem lives in visual/darwin/MenuItem_Darwin.cpp: the
// class is more than a stub because a framework derives from it at start-up

// ---------------------------------------------------------------------------
// the application's globals
// ---------------------------------------------------------------------------

bool TVPTerminated = false;
int TVPTerminateCode = 0;
ttstr TVPProjectDir;

ttstr TVPGetOSName()
{
	return ttstr("macOS");
}

void TVPCheckAbout()
{
}

// ---------------------------------------------------------------------------
// the application's start-up hooks
// ---------------------------------------------------------------------------

void TVPConsoleLog(const ttstr& mes, bool important)
{
	tTJSNarrowStringHolder narrow(mes.c_str());
	fprintf(important ? stderr : stdout, "%s\n", (const char*)narrow);
}

void TVPInitFontNames()
{
}

// the engine's system start-up: with the software render manager and the
// FreeType rasterizers already in place there is nothing else to raise
void TVPSystemInit()
{
	TVPInitTVPGL();
}

void TVPSystemUninit()
{
	TVPSystemUninitCalled = true;
}

void TVPLockSoundMixer()
{
}

void TVPUnlockSoundMixer()
{
}

ttstr TVPGetPlatformName()
{
	return ttstr("macOS");
}

void TVPExecuteUserConfig()
{
}

void TVPCheckPrintDataPath()
{
}

// the engine's base systems: the storage media, the script engine and the
// graphics tables are already up in this host, so this only has to agree
void TVPInitializeBaseSystems()
{
	TVPInitTVPGL();
}

void TVPResetVolumeToAllSoundBuffer()
{
}


/* Queued events (timers, AsyncTrigger, asynchronous sound/video callbacks) are
 * dispatched here.  Layer mouse callbacks use TVP_EPT_IMMEDIATE and do not pass
 * through this queue.  Avoid recursive queue delivery when a handler posts a new
 * event; the main loop will call this entry point again. */
static int gDelivering;
void tTVPSystemControl::InvokeEvents()
{
	extern void TVPDeliverAllEvents();
	if(gDelivering > 0)
		return;
	gDelivering++;
	if(getenv("KRKR_DEBUG_EVENTS"))
	{
		fprintf(stderr, "[events] delivering\n");
		fflush(stderr);
	}
	TVPDeliverAllEvents();
	gDelivering--;
	// A handler may have posted another event while delivery was guarded.  Clear
	// the one-shot latch only after unwinding, so the next host-loop pass can run;
	// calling TVPEventReceived inside delivery would recursively deliver events.
	extern void TVPEventReceived();
	TVPEventReceived();
}

// the system control's idle bookkeeping: without an idle loop none of it runs
void tTVPSystemControl::BeginContinuousEvent()
{
}

void tTVPSystemControl::EndContinuousEvent()
{
}

// TVPDeliverAllEvents calls TVPEventReceived before dispatch; that clears
// TVPEventInvoked and then calls this notification.  Calling TVPEventReceived
// here would recurse even when no other delivery is active.
void tTVPSystemControl::NotifyEventDelivered()
{
}

// The system control object itself: the Windows one starts timers, a watcher
// thread and a message queue.  A host that drives its own frames needs the
// object to exist and its two idle entry points to be quiet, so this is the
// smallest one that satisfies the class.
tTVPSystemControl::tTVPSystemControl()
{
	/* EventEnable decides whether TVPPostEvent delivers anything at all, and the
	 * Windows constructor is where it gets its value.  Left uninitialised here it
	 * is whatever the memory held, which is how every event - every layer's
	 * onClick, KAG's window triggers - came to be dropped. */
	EventEnable = true;
	ContinuousEventCalling = false;
	AutoShowConsoleOnError = false;
	LastCompactedTick = 0;
	LastCloseClickedTick = 0;
	LastShowModalWindowSentTick = 0;
	LastRehashedTick = 0;
}

void tTVPSystemControl::CallDeliverAllEventsOnIdle()
{
}

void tTVPSystemControl::SystemWatchTimerTimer()
{
}

// whether closing the last window ends the engine
bool TVPTerminateOnWindowClose = true;

// the engine's asynchronous termination: a host that owns its loop just records
// the request, and Terminate() is where it would act on it
void TVPTerminateAsync(int code)
{
	TVPTerminateCode = code;
	TVPTerminated = true;
}

int TVPGetPadCount()
{
	return 0; // no pad is connected
}

// the clipboard object and PCM conversion: the script-side clipboard is the
// system's (TVPClipboardGet/SetText above), and the mixer's format conversion
// has no caller until sound exists
tTJSNativeClass* TVPCreateNativeClass_Clipboard()
{
	return nullptr;
}

void TVPConvertPCMToFloat(float* dest, const void* src, const tTVPWaveFormat& format, int count)
{
	(void)src;
	(void)format;
	if(dest != nullptr)
		for(int i = 0; i < count; i++)
			dest[i] = 0.0f;
}

// ---------------------------------------------------------------------------
// the allocation wrappers and the one avutil helper the application names
// ---------------------------------------------------------------------------
//
// The Android build links with --wrap=malloc so that it can account for the
// engine's allocations; the application layer calls the wrapped names directly.
// Here they are the system calls, which is what the wrapper would forward to.
extern "C" void* __real_malloc(size_t size);
extern "C" void __real_free(void* p);
extern "C" void* __real_realloc(void* p, size_t size);

extern "C" void* __real_malloc(size_t size)
{
	return malloc(size);
}

extern "C" void __real_free(void* p)
{
	free(p);
}

extern "C" void* __real_realloc(void* p, size_t size)
{
	return realloc(p, size);
}

// ffmpeg's path helper, used where the engine splits a file name: the last
// separator and what precedes it
extern "C" char* av_dirname(char* path)
{
	if(path == nullptr)
		return nullptr;
	char* last = nullptr;
	for(char* p = path; *p; p++)
		if(*p == '/' || *p == '\\')
			last = p;
	if(last == nullptr)
		return path;
	*last = '\0';
	return path;
}

// the application's main window closed: a host without windows has nothing to
// release, and the engine's own shutdown path runs anyway
void TVPMainWindowClosed()
{
}
