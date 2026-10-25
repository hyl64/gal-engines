// krkrpack - list and extract the files of an XP3 archive
//
// XP3 is the archive format of every KiriKiri game (data.xp3, patch.xp3, ...):
// a header, a per-file index of compressed segments, and the segments
// themselves.  The engine's own reader (src/core/base/XP3Archive.cpp) does the
// decoding, so this tool is the archive layer's desktop front end: it opens an
// archive through the same tTVPXP3Archive the engine uses and enumerates it.
//
// Usage:
//   krkrpack list    <archive.xp3>
//   krkrpack extract <archive.xp3> <directory>

#include "StorageIntf.h"
#include "tjsError.h"
#include "XP3Archive.h"
#include "BinaryStream.h"
#include "tjsString.h"
#include "tjsCommHead.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

// Convert the engine's UTF-16 text to UTF-8 for the terminal.
static std::string ToUtf8(const ttstr& s)
{
	std::string out;
	const tjs_char* p = s.c_str();
	for(; p != nullptr && *p; p++)
	{
		tjs_uint32 c = (tjs_uint32)*p;
		if(c < 0x80)
			out += (char)c;
		else if(c < 0x800)
		{
			out += (char)(0xC0 | (c >> 6));
			out += (char)(0x80 | (c & 0x3F));
		}
		else
		{
			out += (char)(0xE0 | (c >> 12));
			out += (char)(0x80 | ((c >> 6) & 0x3F));
			out += (char)(0x80 | (c & 0x3F));
		}
	}
	return out;
}

// The engine reaches files through its storage interface, and every storage
// name carries its media: a local file is "file://<path>".  A plain path from
// the command line gets the prefix, an absolute one then reads
// "file:///tmp/game/data.xp3".
static ttstr ToStorageName(const char* path)
{
	std::string name("file://");
	name += path;
	return ttstr(name.c_str());
}

static tTVPArchive* OpenArchive(const char* path)
{
	ttstr name = ToStorageName(path);
	if(!TVPIsXP3Archive(name))
	{
		fprintf(stderr, "krkrpack: %s is not an XP3 archive\n", path);
		return nullptr;
	}
	return TVPOpenArchive(name, false);
}

static int ListArchive(tTVPArchive* arc)
{
	tjs_uint n = arc->GetCount();
	printf("%u file(s) in %s\n", (unsigned)n, ToUtf8(arc->ArchiveName).c_str());
	tjs_uint64 total = 0;
	for(tjs_uint i = 0; i < n; i++)
	{
		ttstr name = arc->GetName(i);
		tTJSBinaryStream* s = arc->CreateStreamByIndex(i);
		tjs_uint64 size = s != nullptr ? s->GetSize() : 0;
		delete s;
		total += size;
		printf("%10llu  %s\n", (unsigned long long)size, ToUtf8(name).c_str());
	}
	printf("--- %u file(s), %llu byte(s) uncompressed\n", (unsigned)n, (unsigned long long)total);
	return 0;
}

static int ExtractArchive(tTVPArchive* arc, const char* outDir)
{
	tjs_uint n = arc->GetCount();
	tjs_uint files = 0;
	tjs_uint64 written = 0;
	for(tjs_uint i = 0; i < n; i++)
	{
		std::string name = ToUtf8(arc->GetName(i));
		if(!name.empty() && name.back() == '/')
			continue; // directory entry
		std::string out = std::string(outDir) + "/" + name;
		// make the parent directories
		for(size_t p = out.find('/', std::string(outDir).size() + 1); p != std::string::npos; p = out.find('/', p + 1))
			mkdir(out.substr(0, p).c_str(), 0755);
		tTJSBinaryStream* s = arc->CreateStreamByIndex(i);
		if(s == nullptr)
		{
			fprintf(stderr, "krkrpack: cannot open %s\n", name.c_str());
			continue;
		}
		FILE* f = fopen(out.c_str(), "wb");
		if(f == nullptr)
		{
			fprintf(stderr, "krkrpack: cannot write %s\n", out.c_str());
			delete s;
			continue;
		}
		std::vector<tjs_uint8> buf(1 << 16);
		tjs_uint64 size = 0;
		for(;;)
		{
			tjs_uint got = s->Read(&buf[0], (tjs_uint)buf.size());
			if(got == 0)
				break;
			fwrite(&buf[0], 1, got, f);
			size += got;
		}
		fclose(f);
		delete s;
		files++;
		written += size;
		printf("%10llu  %s\n", (unsigned long long)size, name.c_str());
	}
	printf("--- %u file(s), %llu byte(s) written to %s\n", (unsigned)files, (unsigned long long)written, outDir);
	return 0;
}

static int Run(int argc, char** argv)
{
	if(argc < 3)
	{
		fprintf(stderr,
			"usage: %s list    <archive.xp3>\n"
			"       %s extract <archive.xp3> <directory>\n",
			argv[0], argv[0]);
		return 2;
	}
	std::string cmd(argv[1]);
	if(cmd == "extract" && argc < 4)
	{
		fprintf(stderr, "krkrpack: extract needs an output directory\n");
		return 2;
	}
	tTVPArchive* arc = nullptr;
	try
	{
		arc = OpenArchive(argv[2]);
	}
	catch(const eTJS& e)
	{
		tTJSNarrowStringHolder narrow(e.GetMessage().c_str());
		fprintf(stderr, "krkrpack: %s\n", (const char*)narrow);
		return 1;
	}
	if(arc == nullptr)
		return 1;
	int status;
	if(cmd == "list")
		status = ListArchive(arc);
	else if(cmd == "extract")
		status = ExtractArchive(arc, argv[3]);
	else
	{
		fprintf(stderr, "krkrpack: unknown command %s\n", cmd.c_str());
		status = 2;
	}
	arc->Release();
	return status;
}

// The engine reports errors by throwing; the tool prints them and exits
// non-zero rather than letting the exception reach the C runtime.
int main(int argc, char** argv)
{
	int status = 1;
	try
	{
		status = Run(argc, argv);
	}
	catch(const eTJS& e)
	{
		tTJSNarrowStringHolder narrow(e.GetMessage().c_str());
		fprintf(stderr, "krkrpack: %s\n", (const char*)narrow);
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "krkrpack: %s\n", e.what());
	}
	catch(...)
	{
		fprintf(stderr, "krkrpack: unknown error\n");
	}
	fflush(stdout);
	fflush(stderr);
	// The engine keeps its storage statics and its archive cache in globals
	// whose destructors run the engine's critical sections; those are torn
	// down in an order that a one-shot tool does not need to risk, so the
	// process ends here with its output already flushed.
	_exit(status);
}
