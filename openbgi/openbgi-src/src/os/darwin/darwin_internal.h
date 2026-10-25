/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * darwin_internal.h - the common includes and the shared state of the macOS
 *                     (SDL2) back end
 *
 * macOS is a POSIX system, so the files of src/os/posix that only touch the
 * system (sjis.c, paths.c, process.c, files.c, time.c, thread.c, machine.c,
 * registry.c, font.c) carry over unchanged; they keep the OsPosix_* names of
 * bgi/os_posix.h, which is what "the POSIX layer" means for both back ends.
 * Only the window, the display and the audio device are platform-specific
 * and live in window.c and audio.c; the back end's own user interface
 * (child windows, the text entry, the edit field, the script's dialogs,
 * the file and folder pickers, the debugger's window) lives in the
 * dui_internal.h family (duikit.c, duifield.c, ddialogs.c, dchild.c,
 * dedit.c, ddbgwin.c).  stubs.c keeps only what is still missing (the
 * launcher's game chooser).
 *
 * The three directories below are set once by OS_Init (process.c) and read
 * by everything else: the game directory is the root of the one drive "C:"
 * this back end presents, the configuration directory holds the registry
 * file and the instance lock files.
 */
#ifndef BGI_OS_DARWIN_INTERNAL_H_
#define BGI_OS_DARWIN_INTERNAL_H_

#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_posix.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <iconv.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

// the CPUID / RDTSC intrinsics of machine.c exist on x86 only; every mac
// this back end runs on is arm64, where machine.c answers 0, which is what
// the engine's feature probe expects from a CPU without them
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#define BGI_X86 1
#endif

// set by OS_Init (process.c)
extern char gPosixExeDir[PATH_MAX];    // the game directory with a trailing '/': the one drive this layer presents
extern char gPosixConfigDir[PATH_MAX]; // ~/Library/Application Support/OpenBGI
extern char gPosixRegFile[PATH_MAX];   // the registry emulation's file (registry.c)

void OsPosix_SjisInit(void);     // open the CP932 converters (sjis.c)
void OsPosix_SjisShutdown(void); // close them

// The event handlers installed with OS_SetEventHandlers (window.c).
// A copy, as os.h prescribes; entries may be NULL and are checked.
extern OsEventHandlers_t gDwinHandlers;

// Set by OS_PostQuit and by a close request that finds no `close_request`
// handler; delivered as `quit` from a later pump (WM_QUIT).
extern int gDwinQuitPosted;

// The picture size, as set by OS_WindowSetPicture (window.c).
extern int gDwinPictureW;
extern int gDwinPictureH;

// bring up / shut down the SDL subsystems (window.c); 1 when SDL is ready
int Dwin_Init(void);
void Dwin_Shutdown(void);

/* Present the staging surface now: upload, scale into the client area
 * (aspect kept, centred) and SDL_RenderPresent.  Called from
 * OS_WindowReleaseDc and after the window is shown or exposed. */
void Dwin_Present(void);

/* Map client-area pixel coordinates into picture coordinates; the mouse
 * handlers and OS_ScreenToClient report picture coordinates once the
 * picture is scaled into the client area (see OS_WindowSetPicture in
 * bgi/os.h). */
void Dwin_ClientToPicture(int* x, int* y);

/* The Apple menu quit item, the close box and Cmd+Q arrive as SDL_QUIT:
 * route them through close_request so the engine decides (WM_CLOSE). */
void Dwin_RequestClose(void);

#endif // BGI_OS_DARWIN_INTERNAL_H_
