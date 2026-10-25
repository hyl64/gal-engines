// pngbench - decode real PNGs through the engine's own path, and count the cost
//
// `src/pngio.cpp` is where an event image stops being bytes and becomes pixels:
// straight RGBA, whatever the file held.  There is no command line tool that
// decodes a plain .png from a .pfs (iettool decodes .iet descriptors, which is
// the layer above), so a bench harness is the only way to time this layer at
// all.  It links libartemis.a, so it runs the same code the engine runs.
//
//   pngbench <dir-or-list> [--list <file>] [--repeat N] [--io file|memory]
//
//   --list <file>   one path per line; without it, every *.png directly in the
//                   argument directory
//   --repeat N      decode everything N times and report the best/worst pass
//   --write WxH     instead of decoding: encode a W x H RGBA buffer the way
//                   agame does when it writes a frame, and time artemis::WritePng
//                   and artemis::CountColors separately - the two pieces of
//                   "capture a frame" that are not rendering
//   --roundtrip <png>  decode a real frame, then time encoding *its own pixels*
//                   back out: the honest per-frame capture cost, at the frame's
//                   real entropy instead of a synthetic gradient
//   --io file       ReadPng(path)          - file open + read + inflate
//      --io memory   ReadPng(bytes)        - the archive path: already in RAM
//
// Records are the same tab-separated shape pfsbench prints:
//   PCT <metric> <p50> <p95> <min> <max> <n> <unit>
//   STAT <name> <value> <unit>
//   META <key> <value>

#include "../../../artemis/src/pngio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace
{
	using Clock = std::chrono::steady_clock;

	double Millis(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
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

	void Emit(const std::string& metric, std::vector<double> samples, const char* unit)
	{
		if(samples.empty())
			return;
		std::sort(samples.begin(), samples.end());
		auto at = [&](double p) {
			size_t rank = (size_t)std::ceil(p / 100.0 * (double)samples.size());
			if(rank == 0)
				rank = 1;
			if(rank > samples.size())
				rank = samples.size();
			return samples[rank - 1];
		};
		printf("PCT\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%zu\t%s\n", metric.c_str(), at(50), at(95),
			samples.front(), samples.back(), samples.size(), unit);
		fflush(stdout);
	}

	bool ReadWhole(const std::string& path, std::vector<uint8_t>& out)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if(f == nullptr)
			return false;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		out.resize((size_t)(size > 0 ? size : 0));
		const size_t got = size > 0 ? fread(out.data(), 1, out.size(), f) : 0;
		fclose(f);
		out.resize(got);
		return true;
	}

	std::vector<std::string> ListDir(const std::string& dir)
	{
		std::vector<std::string> out;
		DIR* d = opendir(dir.c_str());
		if(d == nullptr)
			return out;
		struct dirent* e;
		while((e = readdir(d)) != nullptr)
		{
			const std::string name = e->d_name;
			if(name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
				out.push_back(dir + "/" + name);
		}
		closedir(d);
		std::sort(out.begin(), out.end());
		return out;
	}
}

namespace
{
	// agame captures a frame by encoding the composited RGBA buffer to PNG and
	// (in its diagnostic build) counting colours.  Neither is rendering, and both
	// sit in the per-frame budget until they are moved out.
	int WriteMode(int width, int height, int repeat, const std::string& outPath)
	{
		const size_t pixels = (size_t)width * (size_t)height;
		std::vector<uint8_t> rgba(pixels * 4);
		for(int y = 0; y < height; y++)
		{
			for(int x = 0; x < width; x++)
			{
				uint8_t* p = &rgba[((size_t)y * width + x) * 4];
				p[0] = (uint8_t)(x * 3 + y);
				p[1] = (uint8_t)(y * 5);
				p[2] = (uint8_t)(x ^ y);
				p[3] = 255;
			}
		}

		double bestWrite = 1e30, bestCount = 1e30;
		double bytes = 0;
		std::string error;
		for(int i = 0; i < repeat; i++)
		{
			auto t0 = Clock::now();
			if(!artemis::WritePng(outPath, width, height, rgba.data(), error))
			{
				fprintf(stderr, "pngbench: write failed: %s\n", error.c_str());
				return 1;
			}
			auto t1 = Clock::now();
			bestWrite = std::min(bestWrite, Millis(t0, t1));
		}
		for(int i = 0; i < repeat; i++)
		{
			auto t0 = Clock::now();
			const size_t colors = artemis::CountColors(rgba.data(), pixels);
			auto t1 = Clock::now();
			(void)colors;
			bestCount = std::min(bestCount, Millis(t0, t1));
		}

		struct stat st;
		if(stat(outPath.c_str(), &st) == 0)
			bytes = (double)st.st_size;
		Meta("frame", std::to_string(width) + "x" + std::to_string(height));
		Meta("repeat", std::to_string(repeat));
		Meta("png_bytes", std::to_string((size_t)bytes));
		Stat("capture_writepng_ms", bestWrite, "ms");
		Stat("capture_countcolors_ms", bestCount, "ms");
		Stat("capture_mpx_per_s", (double)pixels / 1e6 / (bestWrite / 1000.0), "Mpx/s");
		return 0;
	}
}

namespace
{
	// A synthetic gradient compresses better than a photograph, so it encodes
	// faster than a real frame.  This loads a real frame from the game and times
	// writing its own pixels back out.
	int Roundtrip(const std::string& pngPath, int repeat)
	{
		artemis::Image image;
		std::string error;
		auto t0 = Clock::now();
		if(!artemis::ReadPng(pngPath, image, error))
		{
			fprintf(stderr, "pngbench: %s: %s\n", pngPath.c_str(), error.c_str());
			return 1;
		}
		auto t1 = Clock::now();
		const double decodeMs = Millis(t0, t1);

		double bestWrite = 1e30, bestCount = 1e30;
		const std::string out = "/tmp/pngbench-roundtrip.png";
		for(int i = 0; i < repeat; i++)
		{
			auto a = Clock::now();
			if(!artemis::WritePng(out, image.width, image.height, image.rgba.data(), error))
				return 1;
			auto b = Clock::now();
			bestWrite = std::min(bestWrite, Millis(a, b));
		}
		for(int i = 0; i < repeat; i++)
		{
			auto a = Clock::now();
			const size_t colors = artemis::CountColors(image.rgba.data(), image.pixels());
			auto b = Clock::now();
			(void)colors;
			bestCount = std::min(bestCount, Millis(a, b));
		}

		struct stat st;
		double outBytes = 0;
		if(stat(out.c_str(), &st) == 0)
			outBytes = (double)st.st_size;
		Meta("frame", std::to_string(image.width) + "x" + std::to_string(image.height));
		Meta("frame_png_bytes_in", std::to_string((size_t)([&] {
			struct stat in;
			return stat(pngPath.c_str(), &in) == 0 ? in.st_size : 0;
		})()));
		Meta("frame_png_bytes_out", std::to_string((size_t)outBytes));
		Stat("capture_decode_ms", decodeMs, "ms");
		Stat("capture_writepng_ms", bestWrite, "ms");
		Stat("capture_countcolors_ms", bestCount, "ms");
		Stat("capture_mpx_per_s", (double)image.pixels() / 1e6 / (bestWrite / 1000.0), "Mpx/s");
		return 0;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: pngbench <dir> [--list <file>] [--repeat N] [--io file|memory]\n");
		return 2;
	}
	std::string listFile, io = "file", writeSize, roundtrip;
	int repeat = 1;
	std::vector<std::string> files;
	for(int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if(a == "--list" && i + 1 < argc)
			listFile = argv[++i];
		else if(a == "--repeat" && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else if(a == "--io" && i + 1 < argc)
			io = argv[++i];
		else if(a == "--write" && i + 1 < argc)
			writeSize = argv[++i];
		else if(a == "--roundtrip" && i + 1 < argc)
			roundtrip = argv[++i];
		else
			files.push_back(a);
	}
	if(!roundtrip.empty())
		return Roundtrip(roundtrip, repeat < 1 ? 1 : repeat);

	if(!writeSize.empty())
	{
		int w = 0, h = 0;
		if(sscanf(writeSize.c_str(), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
		{
			fprintf(stderr, "pngbench: --write wants WxH, got '%s'\n", writeSize.c_str());
			return 2;
		}
		const std::string outPath = files.empty() ? "/tmp/pngbench-frame.png" : files[0];
		return WriteMode(w, h, repeat < 1 ? 1 : repeat, outPath);
	}

	std::vector<std::string> paths;
	if(!listFile.empty())
	{
		FILE* f = fopen(listFile.c_str(), "r");
		if(f == nullptr)
		{
			fprintf(stderr, "pngbench: cannot read %s\n", listFile.c_str());
			return 1;
		}
		char line[4096];
		while(fgets(line, sizeof(line), f) != nullptr)
		{
			std::string s = line;
			while(!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			if(!s.empty())
				paths.push_back(s);
		}
		fclose(f);
	}
	else
	{
		for(const std::string& dir : files)
		{
			std::vector<std::string> found = ListDir(dir);
			paths.insert(paths.end(), found.begin(), found.end());
		}
	}
	if(paths.empty())
	{
		fprintf(stderr, "pngbench: no .png files to decode\n");
		return 1;
	}

	const bool fromMemory = io == "memory";
	size_t ok = 0, failed = 0, pixels = 0;
	double diskBytes = 0;
	std::vector<double> perImage, perPass;

	for(int pass = 0; pass < repeat; pass++)
	{
		auto passStart = Clock::now();
		for(const std::string& path : paths)
		{
			std::vector<uint8_t> blob;
			artemis::Image image;
			std::string error;
			auto t0 = Clock::now();
			bool good = false;
			if(fromMemory)
			{
				if(ReadWhole(path, blob))
					good = artemis::ReadPng(blob.data(), blob.size(), image, error);
			}
			else
			{
				good = artemis::ReadPng(path, image, error);
			}
			auto t1 = Clock::now();
			if(good)
			{
				ok++;
				pixels += image.pixels();
				if(fromMemory)
					diskBytes += (double)blob.size();
				else
				{
					struct stat st;
					diskBytes += stat(path.c_str(), &st) == 0 ? (double)st.st_size : 0.0;
				}
				if(pass == 0)
					perImage.push_back(Millis(t0, t1));
			}
			else
			{
				failed++;
				if(pass == 0 && failed <= 3)
					fprintf(stderr, "pngbench: %s: %s\n", path.c_str(), error.c_str());
			}
		}
		auto passEnd = Clock::now();
		perPass.push_back(Millis(passStart, passEnd));
	}

	double best = perPass.empty() ? 0 : *std::min_element(perPass.begin(), perPass.end());
	double totalPixels = (double)pixels / (double)repeat;
	Meta("io", io);
	Meta("repeat", std::to_string(repeat));
	Meta("images", std::to_string(ok / (size_t)repeat));
	Meta("failed", std::to_string(failed));
	Stat("png_pass_best_ms", best, "ms");
	Stat("png_images_per_s", (double)(ok / (size_t)repeat) / (best / 1000.0), "img/s");
	Stat("png_mib_per_s", diskBytes / 1048576.0 / (best / 1000.0), "MiB/s");
	Stat("png_mpx_per_s", totalPixels / 1e6 / (best / 1000.0), "Mpx/s");
	Stat("png_pixels_total", totalPixels, "px");
	Emit("png_per_image_ms", perImage, "ms");
	Emit("png_pass_ms", perPass, "ms");
	return failed == 0 ? 0 : 1;
}
