/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - what the POSIX back end knows about the machine and other
 *             processes (inc/bgi/os.h): the processor, memory, user and
 *             computer names the installer records, the Windows version
 *             the engine is told, the performance counter, and the
 *             process and shell services (running a program, opening a
 *             document); the wallpaper and shortcut services report
 *             failure
 */
#include "darwin_internal.h"

#include <mach/mach.h>
#include <sys/sysctl.h>

// ---- machine -----------------------------------------------------------------------------------

/* The physical memory in bytes and what the VM statistics call free plus
 * inactive pages; macOS has no sysinfo and no swap figures worth reporting.
 * The page count comes from host_statistics64, the size from hw.memsize. */
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

// GlobalMemoryStatus, each figure cut to 32 bits; the virtual space is a 32-bit process's 2 GB
void OS_MemoryStatus(OsMemStatus_t* m)
{
	uint64_t total, avail;
	memset(m, 0, sizeof *m);
	MemInfo(&total, &avail);
	m->totalPhys = (uint32_t)BGI_MIN(total, 0xffffffffu);
	m->availPhys = (uint32_t)BGI_MIN(avail, 0xffffffffu);
	// no page file of its own: the swap the system keeps is not a per-process figure
	m->totalPageFile = m->totalPhys;
	m->availPageFile = m->availPhys;
	m->totalVirtual = 0x7ffe0000u; // the 2 GB user space of a 32-bit process
	m->availVirtual = m->totalVirtual;
	m->loadPercent = m->totalPhys ? (uint32_t)(100 - (uint64_t)m->availPhys * 100 / m->totalPhys) : 0;
}

// the host name
int OS_ComputerName(char* buf, size_t n)
{
	return gethostname(buf, n) == 0;
}

// $USER, else the password entry of the real user id, else "user"
int OS_UserName(char* buf, size_t n)
{
	const char* u = getenv("USER");
	if(!u)
	{
		struct passwd* pw = getpwuid(getuid());
		u = pw ? pw->pw_name : "user";
	}
	snprintf(buf, n, "%s", u);
	return 1;
}

// CPUID through the compiler's intrinsic on x86; the leaf's range (basic / extended) is checked against the highest supported
/* On an x86 Mac the instruction answers for itself.  On Apple silicon there
 * is none, and the engine needs one: it refuses to start without CMOV and MMX
 * ("CMOV + MMX are required to run at all", src/sys/cpu.c) and picks its
 * blitters from SSE / SSE2, and the games' own boot programs check the vendor
 * and the feature word as well and stop with "The CPU installed in this
 * hardware is not supported".  The answers below describe a baseline x86-64 -
 * the machine the original was built for - so those checks pass; nothing here
 * executes x86 code, it only describes one. */
#if !defined(BGI_X86)
static const uint32_t kFeatureEdx = 0x078BFBFFu; /* FPU VME DE PSE TSC MSR PAE MCE
	CX8 APIC SEP MTRR PGE MCA CMOV PAT PSE36 CLFSH MMX FXSR SSE SSE2 */
static const uint32_t kFeatureEcx = 0x00000001u; /* SSE3 */
static const uint32_t kExtendedMax = 0x80000008u;

static void PutBrand(uint32_t leaf, uint32_t out[4])
{
	/* "Intel(R) Core(TM)2 CPU        @ 2.40GHz", 16 bytes per leaf, the
	 * last one padded with NULs (leaf 0x80000004 carries the tail) */
	static const char brand[48] =
		"Intel(R) Core(TM)2 CPU         @ 2.40GHz";
	int base = (int)(leaf - 0x80000002u) * 16;
	for(int i = 0; i < 4; i++)
	{
		int at = base + i * 4;
		out[i] = (uint32_t)(uint8_t)brand[at]
			| (uint32_t)(uint8_t)brand[at + 1] << 8
			| (uint32_t)(uint8_t)brand[at + 2] << 16
			| (uint32_t)(uint8_t)brand[at + 3] << 24;
	}
}
#endif

int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
	if(getenv("BGI_DEBUG_CPUID") != NULL)
		fprintf(stderr, "[cpuid] leaf=%08x\n", leaf);
#if defined(BGI_X86)
	unsigned a, b, c, d;
	unsigned max = __get_cpuid_max(leaf & 0x80000000u, NULL);
	if(max == 0 || leaf > max)
		return 0;
	__cpuid(leaf, a, b, c, d);
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
	return 1;
#else
	switch(leaf)
	{
	case 0:
		out[0] = 0x0Du; /* highest basic leaf */
		/* the vendor comes back as EBX, EDX, ECX = "Genu", "ineI", "ntel" */
		out[1] = 0x756E6547u; /* "Genu" in EBX */
		out[2] = 0x6C65746Eu; /* "ntel" in ECX */
		out[3] = 0x49656E69u; /* "ineI" in EDX */
		return 1;
	case 1:
		out[0] = 0x000006FBu; /* family 6, model 15, stepping 11 */
		out[1] = 0x00000800u; /* brand index 0, CLFLUSH 8 * 8 bytes */
		out[2] = kFeatureEcx;
		out[3] = kFeatureEdx;
		return 1;
	case 2:
		/* the cache descriptors: 8 KB L1 I, 8 KB L1 D, 2 MB L2 */
		out[0] = 0x0102u | 0x01u << 16;
		out[1] = 0x00000000u;
		out[2] = 0x00000000u;
		out[3] = 0x00000000u;
		return 1;
	case 0x80000001u:
		/* the extended feature word: long mode, SYSCALL, NX, 1 GB pages */
		out[0] = 0;
		out[1] = 0;
		out[2] = 0x00000001u; /* LAHF/SAHF */
		out[3] = 0x20100800u;
		return 1;
	case 0x80000000u:
		out[0] = kExtendedMax;
		out[1] = out[2] = out[3] = 0;
		return 1;
	case 0x80000002u:
	case 0x80000003u:
	case 0x80000004u:
		PutBrand(leaf, out);
		return 1;
	case 0x80000005u:
	case 0x80000006u:
		/* L1 and L2 as the extended leaves report them */
		if(leaf == 0x80000005u)
		{
			out[0] = 0x00000000u;
			out[1] = 0x00000000u;
			out[2] = 0x00000000u;
			out[3] = 0x00000000u;
		}
		else
		{
			out[0] = 0;
			out[1] = 0;
			out[2] = 0x00020040u; /* 64-way, 2 MB */
			out[3] = 0;
		}
		return 1;
	case 0x80000008u:
		out[0] = 0x00003024u; /* 36-bit physical, 36-bit linear */
		out[1] = out[2] = out[3] = 0;
		return 1;
	default:
		return 0;
	}
#endif
}

// RDTSC after a serialising CPUID, as the original's CPU speed measurement does; 0 off x86
uint64_t OS_ReadTsc(void)
{
#if defined(BGI_X86)
	unsigned a, b, c, d;
	__cpuid(0, a, b, c, d); // the original serialises with CPUID first
	return __rdtsc();
#else
	return 0;
#endif
}

// the monotonic clock in nanoseconds stands in for the performance counter
int OS_PerfCounterNs(int64_t* out)
{
	struct timespec ts;
	if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	*out = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
	return 1;
}

// CPUID.1:EAX (family, model, stepping), 0 without CPUID
uint32_t OS_CpuSignature(void)
{
	uint32_t r[4];
	return OS_Cpuid(1, r) ? r[0] : 0;
}

// Windows XP (NT 5.1) is reported, so that the engine takes its NT code paths
int OS_Version(int* platform, int* major, int* minor)
{
	*platform = 2; // VER_PLATFORM_WIN32_NT: the engine takes the NT paths (IOCTLs, registry)
	*major = 5;
	*minor = 1;
	return 1;
}

/* the LANGID of the locale in LC_ALL / LC_MESSAGES / LANG: the primary
 * language from the two-letter code (the ones the games ask about, else
 * English), the sublanguage 1 (the default) */
int OS_UiLanguage(void)
{
	static const struct
	{
		const char* code;
		int primary;
	} kLangs[] = {{"ja", 0x11}, {"en", 0x09}, {"zh", 0x04}, {"ko", 0x12}, {"de", 0x07}, {"fr", 0x0c}, {"es", 0x0a},
		{"it", 0x10}, {"ru", 0x19}, {"pt", 0x16}};
	const char* env = getenv("LC_ALL");
	size_t i;
	if(!env || !*env)
		env = getenv("LC_MESSAGES");
	if(!env || !*env)
		env = getenv("LANG");
	if(env)
		for(i = 0; i < sizeof kLangs / sizeof kLangs[0]; i++)
			if(strncmp(env, kLangs[i].code, 2) == 0 && (env[2] == 0 || env[2] == '_' || env[2] == '.' || env[2] == '@'))
				return 0x400 | kLangs[i].primary;
	return 0x409;
}

// the same as XP Service Pack 3, build 2600
void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	int platform, major, minor;
	OS_Version(&platform, &major, &minor);
	out[0] = (uint32_t)major;
	out[1] = (uint32_t)minor;
	out[2] = 2600; // the build of the version OS_Version reports (XP SP3)
	out[3] = (uint32_t)platform;
	if(n)
		snprintf(csd, n, "Service Pack 3");
}

// GlobalMemoryStatusEx: the physical memory in full
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	MemInfo(totalPhys, availPhys);
	(void)0;
}

// "SDL2 display" with a zero driver version
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	// no Direct3D here: SDL2's renderer is the "adapter"
	if(n)
		snprintf(desc, n, "SDL2 display");
	version[0] = version[1] = version[2] = version[3] = 0;
	return 1;
}

// ---- processes ---------------------------------------------------------------------------------

struct OsProcess
{
	pid_t pid;        // the child
	int done, status; // reaped by OS_ProcessWait, and its wait status
};

// the command line (Shift-JIS, converted) through "sh -c"; NULL when fork fails (a failed exec ends the child with 127)
OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	OsProcess_t* p = (OsProcess_t*)calloc(1, sizeof *p);
	char utf[0x1000];
	OsPosix_SjisToUtf8(cmdLine, utf, sizeof utf);
	p->pid = fork();
	if(p->pid == 0)
	{
		execl("/bin/sh", "sh", "-c", utf, (char*)NULL);
		_exit(127);
	}
	if(p->pid < 0)
	{
		free(p);
		return NULL;
	}
	return p;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	BGI_UNUSED(p); // WaitForInputIdle has no equivalent; the process is running
}

// poll waitpid every 10 ms until the child has ended (1) or `ms` have passed (0); a waitpid error counts as ended
int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	uint32_t end = OS_TicksMs() + ms;
	if(p->done)
		return 1;
	for(;;)
	{
		int r = waitpid(p->pid, &p->status, WNOHANG);
		if(r == p->pid || r < 0)
		{
			p->done = 1;
			return 1;
		}
		if((int32_t)(OS_TicksMs() - end) >= 0)
			return 0;
		OS_SleepMs(10);
	}
}

// free the handle; a child that was never waited for stays a zombie until the engine exits
void OS_ProcessClose(OsProcess_t* p)
{
	free(p);
}

// ShellExecute "open": xdg-open on the converted path, detached; `dir` is ignored
int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	char cmd[0x1000], f[PATH_MAX];
	OsProcess_t* p;
	BGI_UNUSED(dir);
	OsPosix_Path(file, f, sizeof f);
	snprintf(cmd, sizeof cmd, "xdg-open '%s' %s >/dev/null 2>&1", f, params ? params : "");
	p = OS_ProcessStart(cmd);
	if(!p)
		return 0;
	OS_ProcessClose(p);
	return 1;
}

// "B0 F0": no portable desktop wallpaper service; failure
int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	BGI_UNUSED(path);
	BGI_UNUSED(stretch);
	BGI_UNUSED(tile);
	return 0; // no portable desktop wallpaper service
}

// no shortcuts on this platform; the scripts take the failure
int OS_CreateShortcut(const char* linkPath, const char* target)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	return 0;
}

int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0; // no shortcuts on this platform; the scripts take the failure
}
