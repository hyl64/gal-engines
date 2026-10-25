// pfsbench - in-process measurements of the Artemis container layer
//
// The command line tools answer "how long does the whole job take"; this answers
// "where did the time go", by linking libartemis.a directly and timing the
// library calls the engine actually makes.  Nothing here is part of the engine:
// it is a bench harness, and it lives in bench/artemis/ so the three people
// editing artemis/src/ never have to know it exists.
//
//   pfsbench open   <archive> <iters>      Open(): mmap + index parse + phase detection
//   pfsbench rand   <archive> <n> [--mode page|content] [--seed S]
//                                          random single-entry access
//   pfsbench seq    <archive> [--out devnull|dir:<path>]
//                                          whole-archive extract via ExtractTo
//   pfsbench one    <archive> <member> <iters>
//                                          Content() of one member, repeatedly (warm)
//   pfsbench types  <archive> [--fix]      what `pfstool list --types` costs, and what a
//                                          bounded 384-byte sniff *would* cost
//   pfsbench memref <bytes>                memcpy / XOR ceilings, for scale
//   pfsbench alloc  <n> <bytes>            per-call buffer cost, for ExtractTo
//   pfsbench hold   <archive> <seconds>    open and idle, so vmmap can look at it
//   pfsbench find   <archive> <member> <n>  name lookup: hit on the way, miss all the way
//
// Everything is printed as tab-separated records so the shell summary can read
// it:
//
//   META <key> <value>
//   PCT  <metric> <p50> <p95> <min> <max> <n> <unit>
//   STAT <name> <value> <unit>
//
// Times are milliseconds unless the unit says otherwise; throughputs are MiB/s
// over the bytes the call actually touched.

#include "../../../artemis/src/pfs.h"
#include "../../../artemis/src/sniff.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	using Clock = std::chrono::steady_clock;

	double Millis(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	double Percentile(const std::vector<double>& sorted, double p)
	{
		if(sorted.empty())
			return 0.0;
		// nearest rank, so p95 of 20 samples is a number that was really measured
		size_t rank = (size_t)std::ceil(p / 100.0 * (double)sorted.size());
		if(rank == 0)
			rank = 1;
		if(rank > sorted.size())
			rank = sorted.size();
		return sorted[rank - 1];
	}

	void Emit(const std::string& metric, std::vector<double> samples, const char* unit)
	{
		if(samples.empty())
			return;
		std::sort(samples.begin(), samples.end());
		double sum = 0;
		for(double v : samples)
			sum += v;
		printf("PCT\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%zu\t%s\n", metric.c_str(),
			Percentile(samples, 50), Percentile(samples, 95), samples.front(),
			samples.back(), samples.size(), unit);
		fflush(stdout);
	}

	void Stat(const std::string& name, double value, const char* unit)
	{
		printf("STAT\t%s\t%.4f\t%s\n", name.c_str(), value, unit);
		fflush(stdout);
	}

	void Meta(const std::string& key, const std::string& value)
	{
		printf("META\t%s\t%s\n", key.c_str(), value.c_str());
		fflush(stdout);
	}

	// what Activity Monitor calls "memory": resident and dirtied by this process
	size_t PhysFootprint()
	{
		task_vm_info_data_t info;
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		if(task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
			return 0;
		return (size_t)info.phys_footprint;
	}

	size_t PeakRss()
	{
		struct rusage usage;
		getrusage(RUSAGE_SELF, &usage);
		return (size_t)usage.ru_maxrss; // bytes on macOS
	}

	// How much of the mapping is actually resident: the difference between
	// mmap-on-demand and "the archive got read" shows up here and nowhere else,
	// because a mapped file's pages are resident-but-clean and barely move
	// phys_footprint.
	size_t ResidentPages(const void* base, size_t length)
	{
		if(base == nullptr || length == 0)
			return 0;
		const size_t page = (size_t)sysconf(_SC_PAGESIZE);
		const size_t whole = length / page; // mincore wants addresses inside the map
		if(whole == 0)
			return 0;
		std::vector<char> vec(whole);
		if(mincore((void*)base, whole * page, vec.data()) != 0)
			return 0;
		size_t resident = 0;
		for(char c : vec)
			if((c & 1) != 0)
				resident++;
		return resident;
	}

	void ReportBacking(const artemis::Archive& a)
	{
		const size_t page = (size_t)sysconf(_SC_PAGESIZE);
		const size_t resident = ResidentPages(a.bytes(), a.length());
		Meta("streaming", a.streaming() ? "yes" : "no");
		Meta("archive_bytes", std::to_string(a.length()));
		Meta("entries", std::to_string(a.entries.size()));
		Meta("index_bytes", std::to_string(a.indexSize));
		Meta("resident_pages", std::to_string(resident));
		Meta("resident_bytes", std::to_string(resident * page));
		Meta("mapping_bytes", std::to_string(a.length()));
		Stat("footprint_after_open", (double)PhysFootprint() / 1048576.0, "MiB");
	}

	// ------------------------------------------------------------------ open

	int Open(const std::string& path, int iters)
	{
		std::vector<double> first, warm;
		size_t entries = 0;
		for(int i = 0; i < iters; i++)
		{
			auto t0 = Clock::now();
			artemis::Archive a = artemis::Open(path);
			auto t1 = Clock::now();
			entries = a.entries.size();
			(i == 0 ? first : warm).push_back(Millis(t0, t1));
		}
		artemis::Archive held = artemis::Open(path);
		ReportBacking(held);
		Meta("open_iters", std::to_string(iters));
		Stat("entries_seen", (double)entries, "count");
		Emit("open_first_ms", first, "ms");
		Emit("open_warm_ms", warm, "ms");
		// the bytes an open has to touch: the index, and that is (nearly) all
		Stat("open_index_mib", (double)held.indexSize / 1048576.0, "MiB");
		return 0;
	}

	// ------------------------------------------------------------------ random

	int Random(const std::string& path, int n, const std::string& mode, uint64_t seed)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		std::mt19937_64 rng(seed);
		std::uniform_int_distribution<size_t> pick(0, a.entries.size() - 1);

		std::vector<double> samples;
		double bytes = 0;
		size_t checksum = 0;
		const uint8_t* base = a.bytes();
		for(int i = 0; i < n; i++)
		{
			const artemis::Entry& e = a.entries[pick(rng)];
			if(mode == "page")
			{
				// first touch of the entry's first byte: what a random read costs
				// before any of the engine's own work happens
				auto t0 = Clock::now();
				checksum += ((const volatile uint8_t*)base)[e.offset];
				auto t1 = Clock::now();
				samples.push_back(Millis(t0, t1));
				bytes += 1;
			}
			else
			{
				// the finest granularity the library offers: Content() is the whole
				// entry, decrypted.  For a 126 KiB mean member that is 126 KiB of
				// work per "read", and that is exactly the number a loading spinner
				// is made of.
				auto t0 = Clock::now();
				std::vector<uint8_t> content = artemis::Content(a, e);
				auto t1 = Clock::now();
				samples.push_back(Millis(t0, t1));
				bytes += (double)content.size();
				checksum += content.empty() ? 0 : content[0] + content[content.size() - 1];
			}
		}
		char unitName[64];
		snprintf(unitName, sizeof(unitName), "rand_%s_ms", mode.c_str());
		Emit(unitName, samples, "ms");
		if(mode != "page")
		{
			double total = 0;
			for(double v : samples)
				total += v;
			Stat("rand_content_total_ms", total, "ms");
			Stat("rand_content_mibs", bytes / 1048576.0 / (total / 1000.0), "MiB/s");
			Stat("rand_content_mean_kib", bytes / (double)n / 1024.0, "KiB");
		}
		Meta("rand_n", std::to_string(n));
		Meta("rand_seed", std::to_string(seed));
		Meta("rand_checksum", std::to_string(checksum));
		return 0;
	}

	// --------------------------------------------------------------------- seq

	static void MakeDirsFor(const std::string& path)
	{
		for(size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
			mkdir(path.substr(0, p).c_str(), 0755);
	}

	int Seq(const std::string& path, const std::string& out)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		const bool toDisk = out.rfind("dir:", 0) == 0;
		const std::string dir = toDisk ? out.substr(4) : "";
		if(toDisk && !dir.empty())
		{
			mkdir(dir.c_str(), 0755);
		}

		double bytes = 0;
		size_t written = 0;
		auto t0 = Clock::now();
		FILE* sink = nullptr;
		if(toDisk)
			sink = nullptr; // a file per entry, opened below
		else
			sink = fopen("/dev/null", "wb");
		if(!toDisk && sink == nullptr)
			return 2;
		for(const artemis::Entry& e : a.entries)
		{
			FILE* f = sink;
			if(toDisk)
			{
				std::string dst = dir + "/" + artemis::NormalizeName(e.name);
				MakeDirsFor(dst);
				f = fopen(dst.c_str(), "wb");
				if(f == nullptr)
					continue;
			}
			if(artemis::ExtractTo(a, e, f))
			{
				bytes += (double)e.size;
				written++;
			}
			if(toDisk)
				fclose(f);
		}
		if(sink != nullptr)
			fclose(sink);
		auto t1 = Clock::now();
		const double ms = Millis(t0, t1);
		Stat("seq_total_ms", ms, "ms");
		Stat("seq_mibs", bytes / 1048576.0 / (ms / 1000.0), "MiB/s");
		Stat("seq_bytes", bytes, "B");
		Meta("seq_files", std::to_string(written));
		Meta("seq_out", toDisk ? "dir" : "devnull");
		Stat("seq_peak_rss_mib", (double)PeakRss() / 1048576.0, "MiB");
		return 0;
	}

	// --------------------------------------------------------------------- one

	// Loading one asset is the engine's hot path: a background, an event tile, a
	// script.  Warm the member's pages first, so what is left is the library's own
	// cost - memcpy plus the XOR layer - and not the file system.
	int One(const std::string& path, const std::string& member, int iters)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		const artemis::Entry* e = artemis::Find(a, member);
		if(e == nullptr)
		{
			fprintf(stderr, "pfsbench: no member matching '%s'\n", member.c_str());
			return 1;
		}
		Meta("one_member", e->name);
		Stat("one_entry_kib", (double)e->size / 1024.0, "KiB");
		Stat("one_offset_mib", (double)e->offset / 1048576.0, "MiB");
		if(e->size > 0)
		{
			std::vector<uint8_t> warm = artemis::Content(a, *e); // untimed: page it in
			if(!warm.empty())
				Meta("one_first_byte", std::to_string(warm[0]));
		}
		std::vector<double> samples;
		double bytes = 0;
		for(int i = 0; i < iters; i++)
		{
			auto t0 = Clock::now();
			std::vector<uint8_t> content = artemis::Content(a, *e);
			auto t1 = Clock::now();
			samples.push_back(Millis(t0, t1));
			bytes += (double)content.size();
		}
		double total = 0;
		for(double v : samples)
			total += v;
		Emit("one_ms", samples, "ms");
		if(total > 0)
			Stat("one_mibs", bytes / 1048576.0 / (total / 1000.0), "MiB/s");
		Meta("one_iters", std::to_string(iters));
		Stat("one_peak_rss_mib", (double)PeakRss() / 1048576.0, "MiB");
		return 0;
	}

	// ------------------------------------------------------------------- types

	// `pfstool list --types` wants 512 bytes of a member to sniff it.  Today it
	// gets them from Content(), which decrypts and allocates the whole member;
	// this measures both that and the bounded alternative, so the difference is
	// a number rather than an opinion.
	int Types(const std::string& path, bool fix)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		double bytes = 0;
		size_t seen = 0;
		auto t0 = Clock::now();
		for(const artemis::Entry& e : a.entries)
		{
			if(e.size == 0)
				continue;
			std::string kind;
			if(!fix)
			{
				std::vector<uint8_t> sample = artemis::Content(a, e);
				if(sample.size() > 512)
					sample.resize(512);
				kind = artemis::SniffBytes(sample.data(), sample.size()).kind;
				bytes += (double)e.size; // what Content() really touched
			}
			else
			{
				const size_t n = e.size < 384 ? e.size : 384;
				std::vector<uint8_t> sample(a.bytes() + e.offset, a.bytes() + e.offset + n);
				if(a.encrypted() && a.key.size() == 20)
				{
					size_t k = a.phase == artemis::KeyPhase::ArchiveOffset
						? (size_t)(e.offset % 20) : 0;
					for(size_t i = 0; i < n; i++)
					{
						sample[i] ^= a.key[k];
						if(++k == 20)
							k = 0;
					}
				}
				kind = artemis::SniffBytes(sample.data(), sample.size()).kind;
				bytes += (double)n;
			}
			seen += kind.empty() ? 0 : 1;
		}
		auto t1 = Clock::now();
		const double ms = Millis(t0, t1);
		Stat(fix ? "types_fixed_ms" : "types_content_ms", ms, "ms");
		Stat(fix ? "types_fixed_mibs" : "types_content_mibs",
			bytes / 1048576.0 / (ms / 1000.0), "MiB/s");
		Stat(fix ? "types_fixed_bytes_mib" : "types_content_bytes_mib", bytes / 1048576.0, "MiB");
		Meta("types_sniffed", std::to_string(seen));
		Stat(fix ? "types_fixed_peak_rss_mib" : "types_content_peak_rss_mib",
			(double)PeakRss() / 1048576.0, "MiB");
		return 0;
	}

	// ------------------------------------------------------------------ memref

	// The XOR layer's ceiling: what this machine can do with the same bytes, no
	// I/O, no allocator.  ApplyKey is a byte-at-a-time loop with a 20-byte
	// wraparound, so the gap between xor1 and xor20x8 is the headroom it has.
	int Memref(size_t bytes)
	{
		std::vector<uint8_t> src(bytes, 0x5a), dst(bytes, 0);
		uint8_t key[20];
		for(int i = 0; i < 20; i++)
			key[i] = (uint8_t)(i * 37 + 11);

		// three passes, best kept: this machine runs other people's builds, and a
		// preempted pass would read as a slower CPU than it is
		auto timeIt = [&](const char* name, auto&& fn) {
			fn(); // warm, untimed
			double best = 1e30;
			for(int pass = 0; pass < 3; pass++)
			{
				auto t0 = Clock::now();
				fn();
				auto t1 = Clock::now();
				best = std::min(best, Millis(t0, t1));
			}
			Stat(name, (double)bytes / 1048576.0 / (best / 1000.0), "MiB/s");
		};

		timeIt("memref_memcpy_mibs", [&] { memcpy(dst.data(), src.data(), bytes); });
		timeIt("memref_xor1_mibs", [&] { // the shape of ApplyKey today
			size_t phase = 0;
			for(size_t i = 0; i < bytes; i++)
			{
				dst[i] = src[i] ^ key[phase];
				if(++phase == 20)
					phase = 0;
			}
		});
		timeIt("memref_xor4_mibs", [&] { // 32-bit words, no wraparound inside a word
			uint32_t k4;
			const uint8_t* s = src.data();
			uint8_t* d = dst.data();
			size_t i = 0;
			size_t phase = 0;
			for(; i + 4 <= bytes; i += 4)
			{
				memcpy(&k4, key + phase, 4);
				uint32_t w;
				memcpy(&w, s + i, 4);
				w ^= k4;
				memcpy(d + i, &w, 4);
				phase = (phase + 4) % 20;
			}
			for(; i < bytes; i++)
				d[i] = s[i] ^ key[phase % 20];
		});
		timeIt("memref_xor20_mibs", [&] { // 20-byte blocks: 8+8+4, the natural period
			const uint8_t* s = src.data();
			uint8_t* d = dst.data();
			uint64_t k0, k1;
			uint32_t k2;
			memcpy(&k0, key, 8);
			memcpy(&k1, key + 8, 8);
			memcpy(&k2, key + 16, 4);
			size_t i = 0;
			for(; i + 20 <= bytes; i += 20)
			{
				uint64_t a0, a1;
				uint32_t a2;
				memcpy(&a0, s + i, 8);
				memcpy(&a1, s + i + 8, 8);
				memcpy(&a2, s + i + 16, 4);
				a0 ^= k0;
				a1 ^= k1;
				a2 ^= k2;
				memcpy(d + i, &a0, 8);
				memcpy(d + i + 8, &a1, 8);
				memcpy(d + i + 16, &a2, 4);
			}
			for(; i < bytes; i++)
				d[i] = s[i] ^ key[(i) % 20];
		});
		Stat("memref_bytes_mib", (double)bytes / 1048576.0, "MiB");
		return 0;
	}

	// ------------------------------------------------------------------ alloc

	// ExtractTo builds a 256 KiB std::vector for every entry it writes, and a
	// vector of that size is value-initialised: an archive of 1682 members pays
	// 1682 allocations and 1682 memsets of 256 KiB, to copy members that average
	// 103 KiB.  This measures what that costs, and what reusing one buffer costs.
	int Alloc(size_t n, size_t bytes)
	{
		volatile size_t sink = 0;
		auto t0 = Clock::now();
		for(size_t i = 0; i < n; i++)
		{
			std::vector<uint8_t> buffer(bytes); // value-initialised, like the real one
			sink += buffer[0] + buffer[bytes - 1];
		}
		auto t1 = Clock::now();
		const double perCall = Millis(t0, t1);

		auto t2 = Clock::now();
		{
			std::vector<uint8_t> buffer(bytes);
			for(size_t i = 0; i < n; i++)
				sink += buffer[0] + buffer[bytes - 1];
		}
		auto t3 = Clock::now();
		const double reused = Millis(t2, t3);

		Meta("alloc_calls", std::to_string(n));
		Meta("alloc_bytes", std::to_string(bytes));
		Meta("alloc_sink", std::to_string((size_t)sink));
		Stat("alloc_per_call_ms", perCall, "ms");
		Stat("alloc_reused_ms", reused, "ms");
		Stat("alloc_per_call_us", perCall * 1000.0 / (double)n, "us");
		Stat("alloc_memset_mibs", (double)(n * bytes) / 1048576.0 / (perCall / 1000.0), "MiB/s");
		return 0;
	}

	// ------------------------------------------------------------------- hold

	// An archive held open and idle, so `vmmap -summary <pid>` can be pointed at
	// it: the OS's own account of what mapping a 202 MB archive did to this
	// process.  Everything else in this harness is too fast to sample.
	int Hold(const std::string& path, int seconds)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		printf("HOLDING\t%d\t%s\n", (int)getpid(), path.c_str());
		fflush(stdout);
		for(int i = 0; i < seconds * 10; i++)
			usleep(100000);
		return 0;
	}

	// -------------------------------------------------------------------- find

	// Find() walks the entry vector and calls NormalizeName() - which allocates a
	// std::string - on every name it compares.  A hit returns early, so it looks
	// cheap; a miss pays for all of them.  Asset loaders miss a lot (does this
	// character have a variant? does this scene have an SE?), so both numbers
	// matter.
	int FindBench(const std::string& path, const std::string& member, int iters)
	{
		artemis::Archive a = artemis::Open(path);
		ReportBacking(a);
		Meta("find_member", member);
		Meta("find_entries", std::to_string(a.entries.size()));
		const artemis::Entry* hit = artemis::Find(a, member);
		Meta("find_hit", hit != nullptr ? "yes" : "no");

		std::vector<double> hits, misses;
		for(int i = 0; i < iters; i++)
		{
			auto t0 = Clock::now();
			artemis::Find(a, member);
			auto t1 = Clock::now();
			hits.push_back(Millis(t0, t1) * 1000.0);
		}
		for(int i = 0; i < iters; i++)
		{
			auto t0 = Clock::now();
			artemis::Find(a, "no\\such\\member\\at\\all.png"); // walks every entry
			auto t1 = Clock::now();
			misses.push_back(Millis(t0, t1) * 1000.0);
		}
		Emit("find_hit_us", hits, "us");
		Emit("find_miss_us", misses, "us");
		return 0;
	}

	int Usage()
	{
		fprintf(stderr,
			"usage: pfsbench open   <archive> <iters>\n"
			"       pfsbench rand   <archive> <n> [--mode page|content] [--seed S]\n"
			"       pfsbench seq    <archive> [--out devnull|dir:<path>]\n"
			"       pfsbench types  <archive> [--fix]\n"
			"       pfsbench memref <bytes>\n"
			"       pfsbench alloc  <n> <bytes>\n"
			"       pfsbench hold   <archive> <seconds>\n"
			"       pfsbench find   <archive> <member> <n>\n");
		return 2;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
		return Usage();
	const std::string cmd = argv[1];
	try
	{
		if(cmd == "open" && argc >= 4)
			return Open(argv[2], atoi(argv[3]));
		if(cmd == "rand" && argc >= 4)
		{
			int n = atoi(argv[3]);
			std::string mode = "content";
			uint64_t seed = 20251025;
			for(int i = 4; i < argc; i++)
			{
				const std::string a = argv[i];
				if(a == "--mode" && i + 1 < argc)
					mode = argv[++i];
				else if(a == "--seed" && i + 1 < argc)
					seed = strtoull(argv[++i], nullptr, 10);
			}
			return Random(argv[2], n, mode, seed);
		}
		if(cmd == "seq" && argc >= 3)
		{
			std::string out = "devnull";
			for(int i = 3; i < argc; i++)
			{
				const std::string a = argv[i];
				if(a == "--out" && i + 1 < argc)
					out = argv[++i];
			}
			return Seq(argv[2], out);
		}
		if(cmd == "one" && argc >= 5)
			return One(argv[2], argv[3], atoi(argv[4]));
		if(cmd == "types" && argc >= 3)
		{
			bool fix = false;
			for(int i = 3; i < argc; i++)
				if(std::string(argv[i]) == "--fix")
					fix = true;
			return Types(argv[2], fix);
		}
		if(cmd == "memref" && argc >= 3)
			return Memref((size_t)strtoull(argv[2], nullptr, 10));
		if(cmd == "find" && argc >= 5)
			return FindBench(argv[2], argv[3], atoi(argv[4]));
		if(cmd == "hold" && argc >= 4)
			return Hold(argv[2], atoi(argv[3]));
		if(cmd == "alloc" && argc >= 4)
			return Alloc((size_t)strtoull(argv[2], nullptr, 10),
				(size_t)strtoull(argv[3], nullptr, 10));
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "pfsbench: %s\n", e.what());
		return 1;
	}
	return Usage();
}
