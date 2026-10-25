// Platform.cpp - the macOS implementation of the platform interface
//
// environ/Platform.h is what the engine asks of the host that is not a
// stream or a window: memory figures, message boxes, the application's
// directories, file helpers, the IME, the log.  Upstream implements it once
// per platform (environ/linux/Platform.cpp covers three of the calls,
// environ/win32/ and the Android layer the rest); this is the macOS one.
//
// Everything here is POSIX except the memory figures, which come from
// sysctl and the VM statistics, and the message boxes, which have no
// terminal counterpart until the window layer lands: they print to stderr
// and take the default button, which is exactly what an unattended run
// wants.

#include "Platform.h"
#include "tjsConfig.h" // tTJSNarrowStringHolder
#include "tjsCommHead.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <mach/mach.h>

// macOS keeps struct stat's timestamps in st_*timespec and defines st_*time as
// macros for them; those macros would rewrite the field names of tTVP_stat,
// so they go away for this file
#if defined(__APPLE__)
#undef st_atime
#undef st_mtime
#undef st_ctime
#endif

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

static void MemInfo(uint64_t* total, uint64_t* avail)
{
	int mib[2] = {CTL_HW, HW_MEMSIZE};
	uint64_t mem = 0;
	size_t len = sizeof mem;
	vm_size_t page = 0;
	vm_statistics64_data_t vm;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	mach_port_t host = mach_host_self();
	*total = *avail = 0;
	if(sysctl(mib, 2, &mem, &len, NULL, 0) == 0)
		*total = mem;
	if(host_page_size(host, &page) == KERN_SUCCESS
		&& host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS)
		*avail = ((uint64_t)vm.free_count + (uint64_t)vm.inactive_count) * (uint64_t)page;
	mach_port_deallocate(mach_task_self(), host);
}

// the figures the memory dialog shows, in kB
void TVPGetMemoryInfo(TVPMemoryInfo& m)
{
	uint64_t total, avail;
	MemInfo(&total, &avail);
	m.MemTotal = (unsigned long)(total / 1024);
	m.MemFree = (unsigned long)(avail / 1024);
	// macOS keeps swap in the dynamic pager rather than a per-process file
	m.SwapTotal = 0;
	m.SwapFree = 0;
	m.VirtualTotal = (unsigned long)(total / 1024);
	m.VirtualUsed = m.MemTotal - m.MemFree;
}

tjs_int TVPGetSystemFreeMemory()
{
	uint64_t total, avail;
	MemInfo(&total, &avail);
	(void)total;
	return (tjs_int)(avail / (1024 * 1024));
}

tjs_int TVPGetSelfUsedMemory()
{
	struct rusage ru;
	if(getrusage(RUSAGE_SELF, &ru) != 0)
		return 0;
	return (tjs_int)(ru.ru_maxrss / 1024); // macOS reports bytes, not kilobytes
}

// ---------------------------------------------------------------------------
// message boxes
// ---------------------------------------------------------------------------



int TVPShowSimpleInputBox(ttstr& text, const ttstr& caption, const ttstr& prompt, const std::vector<ttstr>& vecButtons)
{
	(void)text;
	(void)vecButtons;
	return TVPShowSimpleMessageBox(prompt, caption);
}

// ---------------------------------------------------------------------------
// directories and files
// ---------------------------------------------------------------------------

std::vector<std::string> TVPGetDriverPath()
{
	std::vector<std::string> v;
	v.emplace_back("/");
	return v;
}

// where the game keeps what it writes: the application support directory
std::vector<std::string> TVPGetAppStoragePath()
{
	std::vector<std::string> v;
	const char* home = getenv("HOME");
	v.emplace_back(std::string(home != nullptr ? home : ".") + "/Library/Application Support/Kirikiri2");
	return v;
}

bool TVPCheckStartupPath(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string TVPGetPackageVersionString()
{
	return "1.0.0.0";
}

void TVPExitApplication(int code)
{
	(void)code;
	exit(code);
}

// the per-user preference file of the engine
const std::string& TVPGetInternalPreferencePath()
{
	static std::string path;
	if(path.empty())
	{
		std::vector<std::string> dirs = TVPGetAppStoragePath();
		path = dirs[0] + "/preference.json";
	}
	return path;
}

bool TVPDeleteFile(const std::string& filename)
{
	return unlink(filename.c_str()) == 0;
}

bool TVPRenameFile(const std::string& from, const std::string& to)
{
	return rename(from.c_str(), to.c_str()) == 0;
}

bool TVPCopyFile(const std::string& from, const std::string& to)
{
	FILE* in = fopen(from.c_str(), "rb");
	if(in == nullptr)
		return false;
	FILE* out = fopen(to.c_str(), "wb");
	if(out == nullptr)
	{
		fclose(in);
		return false;
	}
	std::vector<char> buf(1 << 16);
	bool ok = true;
	for(;;)
	{
		size_t got = fread(&buf[0], 1, buf.size(), in);
		if(got > 0 && fwrite(&buf[0], 1, got, out) != got)
		{
			ok = false;
			break;
		}
		if(got < buf.size())
			break;
	}
	fclose(in);
	fclose(out);
	return ok;
}

// ---------------------------------------------------------------------------
// the IME, the scheduler and the log
// ---------------------------------------------------------------------------

void TVPShowIME(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

void TVPHideIME()
{
}

void TVPRelinquishCPU()
{
	// give the other threads of the process a turn
	usleep(1);
}

void TVPPrintLog(const char* str)
{
	if(str != nullptr)
		fprintf(stderr, "%s", str);
}

void TVPFetchSDCardPermission()
{
}

// ---------------------------------------------------------------------------
// stat and utime
// ---------------------------------------------------------------------------

bool TVP_stat(const tjs_char* name, tTVP_stat& s)
{
	ttstr path(name);
	return TVP_stat((const char*)ttstr(path).c_str(), s);
}

bool TVP_stat(const char* name, tTVP_stat& s)
{
	struct stat st;
	if(stat(name, &st) != 0)
		return false;
	s.st_mode = (uint16_t)st.st_mode;
	s.st_size = (uint64_t)st.st_size;
#if defined(__APPLE__)
	s.st_atime = (uint64_t)st.st_atimespec.tv_sec;
	s.st_mtime = (uint64_t)st.st_mtimespec.tv_sec;
	s.st_ctime = (uint64_t)st.st_ctimespec.tv_sec;
#else
	s.st_atime = (uint64_t)st.st_atime;
	s.st_mtime = (uint64_t)st.st_mtime;
	s.st_ctime = (uint64_t)st.st_ctime;
#endif
	return true;
}

void TVP_utime(const char* name, time_t modtime)
{
	utime(name, nullptr);
	struct utimbuf ut;
	ut.actime = modtime;
	ut.modtime = modtime;
	utime(name, &ut);
}

void TVPSendToOtherApp(const std::string& filename)
{
	// "open" is the desktop's way of handing a file to its application
	std::string cmd = "open \"" + filename + "\"";
	if(system(cmd.c_str()) == -1)
		fprintf(stderr, "TVPSendToOtherApp: cannot run open\n");
}

std::string TVPGetCurrentLanguage()
{
	const char* lang = getenv("LANG");
	if(lang != nullptr && strncmp(lang, "ja", 2) == 0)
		return "ja";
	if(lang != nullptr && strncmp(lang, "zh", 2) == 0)
		return "zh_CN";
	return "en_us";
}

// the C-style message box the engine's boot path uses; with no window it goes to
// stderr and reports the first button, which is what an unattended run wants
extern "C" int TVPShowSimpleMessageBox(const char* text, const char* caption, unsigned int nButton,
	const char** btnText)
{
	if(caption != nullptr && *caption)
		fprintf(stderr, "[%s] ", caption);
	if(text != nullptr)
		fprintf(stderr, "%s\n", text);
	(void)nButton;
	(void)btnText;
	return 0;
}

// the three-button overload the engine's error paths use; with no window it goes
// to stderr and reports the first button
int TVPShowSimpleMessageBox(const ttstr& text, const ttstr& caption,
	const std::vector<ttstr>& vecButtons)
{
	tTJSNarrowStringHolder narrowText(text.c_str());
	tTJSNarrowStringHolder narrowCaption(caption.c_str());
	if(!caption.IsEmpty())
		fprintf(stderr, "[%s] ", (const char*)narrowCaption);
	fprintf(stderr, "%s\n", (const char*)narrowText);
	(void)vecButtons;
	return 0;
}
