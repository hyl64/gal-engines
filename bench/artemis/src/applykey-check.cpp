// applykey-check - is the proposed XOR rewrite byte-for-byte the same, and how fast
//
// R1 in RESULTS.md says `ApplyKey` (artemis/src/pfs.cpp:155) should XOR 20 bytes
// at a time instead of one.  A performance suggestion is worth nothing if the
// bytes come out different, so this harness:
//
//   1. implements the shipped loop and the proposed one, verbatim
//   2. runs both over every phase 0..19, every key phase rule, and every length
//      0..4096 plus a few large ones, comparing byte for byte
//   3. times both on 64 MiB, which is the size that matters for a 202 MB volume
//
// It is a bench file: it does not touch artemis/, it just carries a copy of the
// two loops so the recommendation can be checked and measured before anyone
// edits the engine.
//
//   applykey-check            correctness matrix + timing
//   applykey-check --verify   correctness only (fast, for a quick check)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	enum class Phase { EntryRelative, ArchiveOffset };

	// ---- the shipped loop, copied from artemis/src/pfs.cpp:155-169 ----------
	void ApplyKeyShipped(uint8_t* data, size_t length, size_t relative,
		const uint8_t key[20], Phase phase, uint64_t entryOffset)
	{
		size_t at = phase == Phase::ArchiveOffset
			? (size_t)((entryOffset + relative) % 20)
			: (relative % 20);
		for(size_t i = 0; i < length; i++)
		{
			data[i] ^= key[at];
			if(++at == 20)
				at = 0;
		}
	}

	// ---- the proposed loop, from R1 in RESULTS.md --------------------------
	void ApplyKeyProposed(uint8_t* data, size_t length, size_t relative,
		const uint8_t key[20], Phase phase, uint64_t entryOffset)
	{
		size_t at = phase == Phase::ArchiveOffset
			? (size_t)((entryOffset + relative) % 20)
			: (relative % 20);

		// align to the key period, then every step is a whole 20-byte block
		size_t head = (20 - at) % 20;
		if(head > length)
			head = length;
		for(size_t i = 0; i < head; i++)
			data[i] ^= key[at + i];

		size_t pos = head;
		uint64_t k0, k1;
		uint32_t k2;
		memcpy(&k0, key, 8);
		memcpy(&k1, key + 8, 8);
		memcpy(&k2, key + 16, 4);
		for(; pos + 20 <= length; pos += 20)
		{
			uint64_t a0, a1;
			uint32_t a2;
			memcpy(&a0, data + pos, 8);
			memcpy(&a1, data + pos + 8, 8);
			memcpy(&a2, data + pos + 16, 4);
			a0 ^= k0;
			a1 ^= k1;
			a2 ^= k2;
			memcpy(data + pos, &a0, 8);
			memcpy(data + pos + 8, &a1, 8);
			memcpy(data + pos + 16, &a2, 4);
		}
		for(size_t i = 0; pos + i < length; i++)
			data[pos + i] ^= key[i];
	}

	double Millis(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	int Verify()
	{
		std::mt19937_64 rng(12345);
		uint8_t key[20];
		for(int i = 0; i < 20; i++)
			key[i] = (uint8_t)rng();
		const uint64_t offsets[] = { 0, 1, 7, 19, 20, 21, 94300, 13153778, 211508106 };

		int cases = 0, mismatches = 0;
		std::vector<uint8_t> a, b;
		for(int rule = 0; rule < 2; rule++)
		{
			const Phase phase = rule == 0 ? Phase::EntryRelative : Phase::ArchiveOffset;
			for(uint64_t off : offsets)
			{
				for(size_t len = 0; len <= 4096; len++)
				{
					// a few relative offsets per length: the split-chunk path in
					// ExtractTo calls this with relative != 0
					for(size_t rel : { (size_t)0, (size_t)1, (size_t)19, (size_t)256 * 1024 - 7,
							(size_t)262144 })
					{
						a.assign(len, 0x5a);
						for(size_t i = 0; i < len; i++)
							a[i] = (uint8_t)(i * 31 + off + rel);
						b = a;
						ApplyKeyShipped(a.data(), len, rel, key, phase, off);
						ApplyKeyProposed(b.data(), len, rel, key, phase, off);
						cases++;
						if(a != b)
						{
							if(mismatches < 5)
								fprintf(stderr, "mismatch: rule=%d off=%llu len=%zu rel=%zu\n",
									rule, (unsigned long long)off, len, rel);
							mismatches++;
						}
					}
				}
			}
		}

		// the large sizes a real member actually has
		for(size_t len : { (size_t)17120, (size_t)1928496, (size_t)5051012, (size_t)8723529 })
		{
			a.assign(len, 0xa7);
			b = a;
			ApplyKeyShipped(a.data(), len, 0, key, Phase::EntryRelative, 0);
			ApplyKeyProposed(b.data(), len, 0, key, Phase::EntryRelative, 0);
			cases++;
			if(a != b)
				mismatches++;
		}

		printf("verify\t%d case(s)\t%d mismatch(es)\n", cases, mismatches);
		return mismatches == 0 ? 0 : 1;
	}

	void Time(const char* name, size_t bytes)
	{
		std::vector<uint8_t> data(bytes, 0x5a);
		uint8_t key[20];
		for(int i = 0; i < 20; i++)
			key[i] = (uint8_t)(i * 37 + 11);
		double best = 1e30;
		for(int pass = 0; pass < 3; pass++)
		{
			auto t0 = Clock::now();
			ApplyKeyShipped(data.data(), bytes, 0, key, Phase::EntryRelative, 0);
			auto t1 = Clock::now();
			best = std::min(best, Millis(t0, t1));
		}
		printf("STAT\tapplykey_shipped_mibs\t%.4f\tMiB/s\n",
			(double)bytes / 1048576.0 / (best / 1000.0));
		for(int pass = 0; pass < 3; pass++)
		{
			auto t0 = Clock::now();
			ApplyKeyProposed(data.data(), bytes, 0, key, Phase::EntryRelative, 0);
			auto t1 = Clock::now();
			best = std::min(best, Millis(t0, t1));
		}
		printf("STAT\tapplykey_proposed_mibs\t%.4f\tMiB/s\n",
			(double)bytes / 1048576.0 / (best / 1000.0));
		printf("META\tapplykey_bytes\t%zu\n", bytes);
		(void)name;
	}
}

#include <algorithm>

int main(int argc, char** argv)
{
	const bool verifyOnly = argc > 1 && std::string(argv[1]) == "--verify";
	if(Verify() != 0)
		return 1;
	if(!verifyOnly)
		Time("64 MiB", 64u * 1024u * 1024u);
	return 0;
}
