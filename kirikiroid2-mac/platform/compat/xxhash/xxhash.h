// xxhash.h - XXH32, the hash the render method table is keyed with
//
// visual/RenderManager.cpp hashes a run of pixels to recognise an empty line,
// and only needs XXH32.  The algorithm is Yann Collet's, public domain; this is
// the 32-bit variant cut down to the one entry point with its own seed.
#ifndef KRKR_COMPAT_XXHASH_H
#define KRKR_COMPAT_XXHASH_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#define XXH_VERSION_MAJOR 0
#define XXH_VERSION_MINOR 8
#define XXH_VERSION_RELEASE 2

typedef uint32_t XXH32_hash_t;

static inline uint32_t krkr_xxh32_rotl(uint32_t x, int r)
{
	return (x << r) | (x >> (32 - r));
}

static inline uint32_t XXH32(const void* input, size_t length, uint32_t seed)
{
	const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
	const uint8_t* p = (const uint8_t*)input;
	const uint8_t* end = p + length;
	uint32_t h;

	if(length >= 16)
	{
		uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
		const uint8_t* limit = end - 16;
		do
		{
			uint32_t a, b, c, d;
			memcpy(&a, p, 4); p += 4;
			memcpy(&b, p, 4); p += 4;
			memcpy(&c, p, 4); p += 4;
			memcpy(&d, p, 4); p += 4;
			v1 = krkr_xxh32_rotl(v1 + a * P2, 13) * P1;
			v2 = krkr_xxh32_rotl(v2 + b * P2, 13) * P1;
			v3 = krkr_xxh32_rotl(v3 + c * P2, 13) * P1;
			v4 = krkr_xxh32_rotl(v4 + d * P2, 13) * P1;
		}
		while(p <= limit);
		h = krkr_xxh32_rotl(v1, 1) + krkr_xxh32_rotl(v2, 7)
			+ krkr_xxh32_rotl(v3, 12) + krkr_xxh32_rotl(v4, 18);
	}
	else
		h = seed + P5;

	h += (uint32_t)length;

	while(p + 4 <= end)
	{
		uint32_t v;
		memcpy(&v, p, 4);
		p += 4;
		h += v * P3;
		h = krkr_xxh32_rotl(h, 17) * P4;
	}
	while(p < end)
	{
		h += (*p) * P5;
		h = krkr_xxh32_rotl(h, 11) * P1;
		p++;
	}

	h ^= h >> 15;
	h *= P2;
	h ^= h >> 13;
	h *= P3;
	h ^= h >> 16;
	return h;
}

#endif // KRKR_COMPAT_XXHASH_H
