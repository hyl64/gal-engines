// krkrscript - run a TJS2 script with the engine's storage layer attached
//
// The engine's scripts reach the game's files through the TVP classes, not
// through C++: Storages is the object that answers "does this storage exist"
// and that turns a name into a path, and an archive name with a '>' in it
// ("data.xp3>script/startup.tjs") goes through the XP3 reader.  This tool
// registers Storages next to the System object and runs a script, so the
// script layer and the archive layer can be exercised together without the
// window.
//
// Usage: krkrscript <script.tjs>

#include "darwin_platform.h"

#include "SystemIntf.h"
#include "tjs.h"
#include "tjsString.h"
#include "tjsError.h"
#include "tjsNative.h"
#include "tjsVariant.h"

#include "StorageIntf.h"
#include "XP3Archive.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>

// the console the script prints through (System.inform et al.)
class ConsoleOutput : public iTJSConsoleOutput
{
public:
	void ExceptionPrint(const tjs_char* msg) override
	{
		KrkrErr(msg);
		fputc('\n', stderr);
	}
	void Print(const tjs_char* msg) override
	{
		KrkrOut(msg);
	}
};

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

static void RegisterClass(iTJSDispatch2* global, const tjs_char* name, iTJSDispatch2* cls)
{
	tTJSVariant val(cls, NULL);
	cls->Release();
	global->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP, name, NULL, &val, global);
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <script.tjs>\n", argv[0]);
		return 2;
	}
	std::vector<tjs_char> script;
	if(!ReadScript(argv[1], script))
	{
		fprintf(stderr, "krkrscript: cannot read %s\n", argv[1]);
		return 1;
	}

	ConsoleOutput console;
	tTJS* tjs = new tTJS();
	tjs->SetConsoleOutput(&console);
	iTJSDispatch2* global = tjs->GetGlobal();

	// System is this host's (darwin_system.cpp); Storages is the engine's own
	// (base/StorageIntf.cpp), which reaches the file media and the archives
	RegisterClass(global, TJS_W("System"), TVPCreateNativeClass_System());
	RegisterClass(global, TJS_W("Storages"), TVPCreateNativeClass_Storages());

	// The engine adds its own folder to the search path at start-up; a host
	// does the same, so that "file://./data.xp3" is the folder the tool runs in.
	{
		char cwd[1024];
		if(getcwd(cwd, sizeof cwd) != NULL)
		{
			std::string base("file://");
			base += cwd;
			base += "/";
			TVPAddAutoPath(ttstr(base.c_str()));
		}
	}

	int status = 0;
	try
	{
		ttstr name = ttstr(argv[1]);
		tjs->ExecScript(&script[0], nullptr, nullptr, name.c_str());
	}
	catch(const eTJS& e)
	{
		const tjs_char* m = e.GetMessage().c_str();
		if(m != nullptr)
		{
			KrkrErr(m);
			fputc('\n', stderr);
		}
		status = 1;
	}
	catch(...)
	{
		fprintf(stderr, "krkrscript: unhandled exception\n");
		status = 1;
	}
	tjs->Shutdown();
	tjs->Release();
	fflush(stdout);
	fflush(stderr);
	// the engine's storage statics are torn down by the C runtime after this;
	// their critical sections are gone by then in a tool this short-lived
	_exit(status);
}
