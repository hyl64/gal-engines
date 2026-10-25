// darwin_platform.h - the small surface the desktop host shares with the
// engine: UTF-16 text out, and the console class the scripts print through.
//
// The engine speaks UTF-16 (tjs_char is char16_t) and its scripts print with
// System.inform / System.println, which the application layer provides
// upstream (environ/cocos2d for Android, environ/Application.cpp for
// Windows).  This is that piece for macOS.

#ifndef KRKR_DARWIN_PLATFORM_H
#define KRKR_DARWIN_PLATFORM_H

#include "tjsCommHead.h"

// Write the text as UTF-8 to stdout / stderr.
void KrkrOut(const tjs_char* s);
void KrkrErr(const tjs_char* s);

namespace TJS
{
	class tTJSNativeClass;
}

// The "System" object: inform, print, println and the platform name, so that
// a script can report what it does without a window.
iTJSDispatch2* KrkrCreateSystemClass();

#endif // KRKR_DARWIN_PLATFORM_H
