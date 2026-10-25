// darwin_host.cpp - the host layer the archive code asks of its platform
//
// base/win32/StorageImpl.cpp plays this part on Windows and the Android
// layer on Android: the storage factory, the paths helpers, the exit hooks
// and the version figures.  The desktop port needs the archive side of it
// (open an XP3 and read its files), so this file implements exactly that,
// on POSIX, without the Windows file-media machinery:
//
//   TVPCreateStream            - "file://./data.xp3" and plain paths -> a stream
//   TVPGetCachedArchiveHandle  - the shared handle an archive's streams read from
//   TVPOpenArchive             - sniff the signature, build the right reader
//   the paths / temp / remove helpers and the exit hooks

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsVariant.h"
#include "tjsNative.h"

#include "StorageIntf.h"
#include "XP3Archive.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "Application.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

// ---------------------------------------------------------------------------
// a stream over a file
// ---------------------------------------------------------------------------

// tTJSBinaryStream is the engine's stream interface; this is the file one the
// storage factory hands out.
class tTVPFileStream : public tTJSBinaryStream
{
	FILE* f;
	tjs_uint64 size;

public:
	tTVPFileStream(FILE* file, tjs_uint64 fileSize) : f(file), size(fileSize) {}

	~tTVPFileStream() override
	{
		if(f != nullptr)
			fclose(f);
	}

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) override
	{
		int w = whence == TJS_BS_SEEK_SET ? SEEK_SET : (whence == TJS_BS_SEEK_CUR ? SEEK_CUR : SEEK_END);
		if(fseek(f, (long)offset, w) != 0)
			return (tjs_uint64)-1;
		long pos = ftell(f);
		return pos < 0 ? (tjs_uint64)-1 : (tjs_uint64)pos;
	}

	tjs_uint TJS_INTF_METHOD Read(void* buffer, tjs_uint read_size) override
	{
		return (tjs_uint)fread(buffer, 1, read_size, f);
	}

	tjs_uint TJS_INTF_METHOD Write(const void* buffer, tjs_uint write_size) override
	{
		return (tjs_uint)fwrite(buffer, 1, write_size, f);
	}

	tjs_uint64 TJS_INTF_METHOD GetSize() override
	{
		return size;
	}
};

// ---------------------------------------------------------------------------
// the "file" storage media
// ---------------------------------------------------------------------------

// The engine reaches every file through a storage media: "file://" is the
// one that names the local file system, and the storage code (which the
// engine already has) asks it for existence, streams and directory listings.
// Upstream implements this class per platform (base/win32/StorageImpl.cpp
// against the Win32 API); this is the POSIX one.
class tTVPFileMedia : public iTVPStorageMedia
{
	tjs_int RefCount;

	static void ToNarrow(const ttstr& name, std::string& out)
	{
		tTJSNarrowStringHolder narrow(name.c_str());
		out = (const char*)narrow;
	}

public:
	tTVPFileMedia() : RefCount(1) {}

	void TJS_INTF_METHOD AddRef() override { RefCount++; }

	void TJS_INTF_METHOD Release() override
	{
		if(--RefCount == 0)
			delete this;
	}

	void TJS_INTF_METHOD GetName(ttstr& name) override
	{
		name = TJS_W("file");
	}

	void TJS_INTF_METHOD NormalizeDomainName(ttstr& name) override
	{
		// the local file system has one domain and it names itself nothing
		name.Clear();
	}

	// The engine caches what it looks up by the normalized name, and it
	// expects the media to decide what "the same name" means.  macOS's
	// default volume is case-insensitive like the Windows one, so the path
	// case-folds, exactly as the Windows back end does.
	void TJS_INTF_METHOD NormalizePathName(ttstr& name) override
	{
		tjs_char* p = name.Independ();
		for(; *p; p++)
		{
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += TJS_W('a') - TJS_W('A');
			else if(*p == TJS_W('\\'))
				*p = TJS_W('/');
		}
	}

	bool TJS_INTF_METHOD CheckExistentStorage(const ttstr& name) override
	{
		std::string path;
		ToNarrow(name, path);
		struct stat st;
		return stat(path.c_str(), &st) == 0;
	}

	// "\" (the archive delimiter) is handled above this layer, so `name` is a
	// plain path here
	tTJSBinaryStream* TJS_INTF_METHOD Open(const ttstr& name, tjs_uint32 flags) override
	{
		std::string path;
		ToNarrow(name, path);
		bool write = (flags & TJS_BS_ACCESS_MASK) == TJS_BS_WRITE;
		FILE* f = fopen(path.c_str(), write ? "wb" : "rb");
		if(f == nullptr)
			return nullptr;
		tjs_uint64 size = 0;
		if(!write)
		{
			fseek(f, 0, SEEK_END);
			long end = ftell(f);
			size = end > 0 ? (tjs_uint64)end : 0;
			fseek(f, 0, SEEK_SET);
		}
		return new tTVPFileStream(f, size);
	}

	void TJS_INTF_METHOD GetListAt(const ttstr& name, iTVPStorageLister* lister) override
	{
		std::string dirPath;
		ToNarrow(name, dirPath);
		DIR* d = opendir(dirPath.c_str());
		if(d == nullptr)
			return;
		struct dirent* e;
		while((e = readdir(d)) != nullptr)
		{
			if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			std::string child = dirPath + "/" + e->d_name;
			struct stat st;
			if(stat(child.c_str(), &st) != 0)
				continue;
			ttstr entry(e->d_name);
			if(S_ISDIR(st.st_mode))
				entry += TJS_W("/");
			lister->Add(entry);
		}
		closedir(d);
	}

	// the media's names are the local file system's names already
	void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr& name) override
	{
		(void)name;
	}
};

iTVPStorageMedia* TVPCreateFileMedia()
{
	return new tTVPFileMedia();
}

// The Storages class itself is the engine's (base/StorageIntf.cpp); only its
// factory lives in the platform layer upstream, so this is that one line.
tTJSNativeClass* TVPCreateNativeClass_Storages()
{
	return new tTJSNC_Storages();
}

// ---------------------------------------------------------------------------
// the archive factory
// ---------------------------------------------------------------------------

tTVPArchive* TVPOpenArchive(const ttstr& name, bool normalizeFileName)
{
	try
	{
		tTJSBinaryStream* st = TVPCreateStream(name);
		if(st == nullptr)
			return nullptr;
		// the XP3 reader keeps the stream it is given and reports whether the
		// signature matched by throwing when it did not
		try
		{
			tTVPArchive* arc = tTVPXP3Archive::Create(name, st, normalizeFileName);
			if(arc != nullptr)
				return arc;
		}
		catch(...)
		{
		}
		delete st;
	}
	catch(...)
	{
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

// The storage names use '/' on POSIX; the callers build them with the
// separator the scripts wrote, which is '\\' on Windows.
void TVPPreNormalizeStorageName(ttstr& name)
{
	if(name.IsEmpty())
		return;
	tjs_char* p = const_cast<tjs_char*>(name.c_str());
	for(; *p; p++)
		if(*p == TJS_W('\\'))
			*p = TJS_W('/');
}

bool TVPRemoveFile(const ttstr& name)
{
	tTJSNarrowStringHolder narrow(name.c_str());
	return remove((const char*)narrow) == 0;
}

static bool RemoveFolderRecursive(const std::string& path)
{
	DIR* d = opendir(path.c_str());
	if(d == nullptr)
		return rmdir(path.c_str()) == 0;
	struct dirent* e;
	bool ok = true;
	while((e = readdir(d)) != nullptr)
	{
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		std::string child = path + "/" + e->d_name;
		struct stat st;
		if(stat(child.c_str(), &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
			ok = RemoveFolderRecursive(child) && ok;
		else
			ok = (unlink(child.c_str()) == 0) && ok;
	}
	closedir(d);
	return ok && rmdir(path.c_str()) == 0;
}

bool TVPRemoveFolder(const ttstr& name)
{
	tTJSNarrowStringHolder narrow(name.c_str());
	return RemoveFolderRecursive((const char*)narrow);
}

// CreateDirectory with the parents, as the engine expects of it
static bool CreateFoldersImpl(const std::string& path)
{
	if(path.empty())
		return true;
	struct stat st;
	if(stat(path.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode);
	size_t slash = path.find_last_of('/');
	if(slash != std::string::npos && slash > 0)
	{
		if(!CreateFoldersImpl(path.substr(0, slash)))
			return false;
	}
	return mkdir(path.c_str(), 0777) == 0 || stat(path.c_str(), &st) == 0;
}

bool TVPCreateFolders(const ttstr& folder)
{
	if(folder.IsEmpty())
		return true;
	tTJSNarrowStringHolder narrow(folder.c_str());
	std::string path = (const char*)narrow;
	// the callers may end the name with a separator
	while(path.size() > 1 && path.back() == '/')
		path.pop_back();
	return CreateFoldersImpl(path);
}

// The engine's data directory: the game's own folder, as the original runs
// from inside it.
void TVPEnsureDataPathDirectory()
{
	const char* dir = getenv("KRKR_DATA_PATH");
	if(dir == nullptr)
		return;
	struct stat st;
	if(stat(dir, &st) != 0)
		mkdir(dir, 0777);
}

ttstr TVPGetTemporaryName()
{
	// mkstemp gives a name that is ours alone; the engine only needs the name
	char templ[] = "/tmp/krkr2-XXXXXX";
	int fd = mkstemp(templ);
	if(fd >= 0)
		close(fd);
	return ttstr(templ);
}

// ---------------------------------------------------------------------------
// the application layer's odds and ends
// ---------------------------------------------------------------------------

void TVPGetVersion()
{
	TVPVersionMajor = 2;
	TVPVersionMinor = 32;
	TVPVersionRelease = 0;
	TVPVersionBuild = 0;
}

// The original reads its switches from the command line; the engine itself
// has none of them, so every query reports "not given".
bool TVPGetCommandLine(const tjs_char* name, tTJSVariant* value)
{
	(void)name;
	(void)value;
	return false;
}

ttstr TVPReadAboutStringFromResource()
{
	return ttstr();
}

// The exit handlers run in priority order when the engine shuts down; atexit
// cannot order them, so they are kept here and run by hand.
namespace
{
	struct ExitHandler
	{
		tjs_int priority;
		void (*handler)();
	};
	std::vector<ExitHandler> gExitHandlers;
	bool gExitHandlersInstalled = false;

	bool HigherPriority(const ExitHandler& a, const ExitHandler& b)
	{
		return a.priority > b.priority;
	}

	void RunExitHandlers()
	{
		std::vector<ExitHandler> handlers = gExitHandlers;
		gExitHandlers.clear();
		std::sort(handlers.begin(), handlers.end(), HigherPriority);
		for(auto& h : handlers)
			if(h.handler != nullptr)
				h.handler();
	}
}

void TVPAddAtExitHandler(tjs_int pri, void (*handler)())
{
	if(!gExitHandlersInstalled)
	{
		atexit(RunExitHandlers);
		gExitHandlersInstalled = true;
	}
	gExitHandlers.push_back({pri, handler});
}

// the engine's own application object lives in environ/Application.cpp

// ---------------------------------------------------------------------------
// idle hooks
// ---------------------------------------------------------------------------

// The engine lets parts of itself ask to be called back while it idles (the
// compressed software textures refresh themselves that way).  A tool that
// renders a frame and exits has no idle loop to run them from, so they are
// recorded and simply never fire; the window layer will run them once there is
// a loop.
#include "EventIntf.h"

#include <vector>

static std::vector<tTVPContinuousEventCallbackIntf*> gContinuousHooks;
