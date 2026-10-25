// tjsc - runs a TJS2 script on macOS arm64
//
// TJS2 is the language the KiriKiri engine and its KAG scenario system are
// written in; this tool is the first desktop target of the port, so that the
// script engine can be exercised (and its output compared) without the
// window, the archive layer or the game data.
//
// Usage: tjsc <script.tjs> [args...]

#include "darwin_platform.h"

#include "SystemIntf.h"
#include "tjs.h"
#include "tjsString.h"
#include "tjsError.h"
#include "tjsNative.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The engine writes its messages through iTJSConsoleOutput; the terminal is
// the console here.  TJS2 hands the text over as UTF-16, so it converts to
// UTF-8 before printing.
class ConsoleOutput : public iTJSConsoleOutput
{
public:
	void ExceptionPrint(const tjs_char* msg) override
	{
		fput_utf8(msg, stderr);
		fputc('\n', stderr);
	}
	void Print(const tjs_char* msg) override
	{
		fput_utf8(msg, stdout);
	}

private:
	static void fput_utf8(const tjs_char* s, FILE* f)
	{
		if(s == nullptr)
			return;
		for(; *s; s++)
		{
			tjs_uint32 c = (tjs_uint32)*s;
			if(c < 0x80)
				fputc((int)c, f);
			else if(c < 0x800)
			{
				fputc((int)(0xC0 | (c >> 6)), f);
				fputc((int)(0x80 | (c & 0x3F)), f);
			}
			else
			{
				fputc((int)(0xE0 | (c >> 12)), f);
				fputc((int)(0x80 | ((c >> 6) & 0x3F)), f);
				fputc((int)(0x80 | (c & 0x3F)), f);
			}
		}
	}
};

// Read the whole file; TJS2 takes the source as UTF-16, so the bytes go
// through the engine's own multibyte converter (UTF-8 on this platform).
static bool ReadScript(const char* path, std::vector<tjs_char>& out)
{
	FILE* f = fopen(path, "rb");
	if(f == nullptr)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::string bytes((size_t)(size > 0 ? size : 0), '\0');
	if(size > 0)
		bytes.resize((size_t)fread(&bytes[0], 1, (size_t)size, f));
	fclose(f);
	out.assign(bytes.size() + 1, 0);
	TJS_mbstowcs(&out[0], bytes.c_str(), out.size());
	return true;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <script.tjs> [args...]\n", argv[0]);
		return 2;
	}
	std::vector<tjs_char> script;
	if(!ReadScript(argv[1], script))
	{
		fprintf(stderr, "tjsc: cannot read %s\n", argv[1]);
		return 1;
	}

	ConsoleOutput console;
	tTJS* tjs = new tTJS();
	tjs->SetConsoleOutput(&console);

	// the scripts print through System (darwin_system.cpp); the engine's own
	// globals lack it because the application layer owns it upstream
	{
		iTJSDispatch2* global = tjs->GetGlobal();
		iTJSDispatch2* sys = TVPCreateNativeClass_System();
		tTJSVariant val(sys, NULL);
		sys->Release();
		global->PropSet(TJS_MEMBERENSURE, TJS_W("System"), NULL, &val, global);
	}
	int status = 0;
	try
	{
		ttstr name = ttstr(argv[1]);
		tjs->ExecScript(&script[0], nullptr, nullptr, name.c_str());
	}
	catch(const eTJS& e)
	{
		// the exception carries the message TJS2 formatted for the user
		const tjs_char* m = e.GetMessage().c_str();
		if(m != nullptr)
		{
			console.Print(m);
			fputc('\n', stderr);
		}
		status = 1;
	}
	catch(...)
	{
		fprintf(stderr, "tjsc: unhandled exception\n");
		status = 1;
	}
	tjs->Shutdown();
	tjs->Release();
	return status;
}
