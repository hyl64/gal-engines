/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/cbg2_alpha.c - unit test of the alpha plane of a version 2
 *   CompressedBG (src/core/cbg2.c: AlphaLz for mode 1, AlphaHuff for mode 2);
 *   built as bin/test_cbg2_alpha by `make test`
 *
 * The images of the games examined are all version 1, so there is no version 2
 * file to check the decoder against: the test carries an encoder written as
 * the decoder's inverse.  A small picture (30 x 20, three block rows) is
 * encoded with every DCT coefficient zero - a flat grey, which keeps the
 * bands trivial - and an alpha plane in either mode:
 *
 *   mode 1  the literal / LZ token stream: a zero flag byte and eight alpha
 *           bytes, repeated (the decoder's AlphaLz)
 *   mode 2  the Huffman coded plane: the symbol count, the 256 leaf weights
 *           and the bit stream of the block presence bits and the alpha bytes
 *           of the blocks whose bit is set.  The tree is built by the same
 *           construction as the decoder's (cbg2.c's BuildTree, the quirk
 *           included) and the bits are written as the decoder reads them,
 *           most significant bit first, a clear bit taking the left child.
 *
 * The groups of tests, in the order they run:
 *
 *   mode 2    a plane of four grey levels, one of a single value (the
 *             degenerate one-symbol tree), a transparent and an opaque
 *             picture, and a checkerboard of present and absent blocks: a
 *             present block decodes to its bytes, an absent one keeps the
 *             opaque default
 *   mode 1    the same picture through the literal stream, unchanged
 *   header    the in-memory image header of the decode, the flat colour, and
 *             a 24-bit picture (no alpha stream at all)
 *   legacy    Cbg2_Decode (the entry BGI_CbgDecode uses, which cannot know the
 *             file size) decodes a whole file like Cbg2_DecodeSized
 *   malformed the symbol count of a plane larger than the picture, weights
 *             that are all zero, a missing alpha stream, a plane cut short,
 *             a truncated file and a bad length: no crash, and the output
 *             buffer's guard bytes are untouched
 *
 * Every decode writes into a buffer with `GUARD` bytes of 0xa5 behind the
 * image, checked afterwards: Cbg2_DecodeSized bounds every read by the file
 * size it is given, so a malformed file cannot run past the buffer.
 */
#include "bgi/codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

#define GUARD 64 // bytes of guard behind every decoded image

// the picture the tests encode: 30 x 20 pixels, so the padded 32 x 24 rows have
// four block columns and three block rows
#define IMG_W 30
#define IMG_H 20
#define PAD_W ((IMG_W + 7) & ~7)
#define PAD_H ((IMG_H + 7) & ~7)
#define COLS (PAD_W / 8)
#define ROWS (PAD_H / 8)
#define MASK_BYTES ((COLS * ROWS + 7) / 8)
#define ENC_LEN 132 // the encrypted quantisation tables: 2 * 64 + 4 unused bytes

// ---- a growing buffer ---------------------------------------------------------------

typedef struct Buf
{
	uint8_t* p;
	uint32_t len, cap;
} Buf_t;

static void BufGrow(Buf_t* b, uint32_t add)
{
	if(b->len + add > b->cap)
	{
		uint32_t cap = b->cap ? b->cap * 2 : 256;
		uint8_t* p;
		while(cap < b->len + add)
			cap *= 2;
		p = (uint8_t*)realloc(b->p, cap);
		if(!p)
		{
			printf("  out of memory\n");
			exit(2);
		}
		b->p = p;
		b->cap = cap;
	}
}

static void BufPut(Buf_t* b, const void* p, uint32_t n)
{
	BufGrow(b, n);
	memcpy(b->p + b->len, p, n);
	b->len += n;
}

static void BufByte(Buf_t* b, uint32_t v)
{
	uint8_t c = (uint8_t)v;
	BufPut(b, &c, 1);
}

static void BufU32(Buf_t* b, uint32_t v)
{
	uint8_t c[4];
	c[0] = (uint8_t)v;
	c[1] = (uint8_t)(v >> 8);
	c[2] = (uint8_t)(v >> 16);
	c[3] = (uint8_t)(v >> 24);
	BufPut(b, c, 4);
}

// the CBG varint: 7 bits per byte, the low group first, 0x80 on every byte but the last
static void BufVarint(Buf_t* b, uint32_t v)
{
	do
	{
		uint8_t c = (uint8_t)(v & 0x7f);
		v >>= 7;
		if(v)
			c |= 0x80;
		BufByte(b, c);
	} while(v);
}

static uint32_t VarintLen(uint32_t v)
{
	uint32_t n = 1;
	while(v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

// ---- the Huffman tree of the decoder -------------------------------------------------

/* The same construction as cbg2.c's BuildTree: the two lightest valid nodes
 * are paired, the scan for the k-th pick starts at node k + 1 with the first
 * valid node as its candidate, and the parents follow the leaves.  The codes
 * the test writes have to be the codes the decoder reads, so the quirk is
 * copied here rather than using CbgBuildTree (which pairs textbook-style). */
static int TreeBuild(CbgNode_t* nodes, const uint32_t* weights, uint32_t leaves)
{
	uint32_t total = 0, i, n;
	for(i = 0; i < 2 * leaves - 1; i++)
	{
		nodes[i].valid = 0;
		nodes[i].weight = 0;
		nodes[i].isParent = i >= leaves;
		nodes[i].parent = nodes[i].left = nodes[i].right = 0xffffffffu;
	}
	for(i = 0; i < leaves; i++)
	{
		nodes[i].weight = weights[i];
		nodes[i].valid = weights[i] != 0;
		nodes[i].left = nodes[i].right = i;
		total += weights[i];
	}
	if(total == 0)
		return -1;
	for(n = leaves; n < 2 * leaves - 1; n++)
	{
		uint32_t pick[2], k, sum = 0;
		for(k = 0; k < 2; k++)
		{
			uint32_t cand = 0xffffffffu, j;
			for(j = 0; j < n; j++)
				if(nodes[j].valid)
				{
					cand = j;
					break;
				}
			for(j = k + 1; j < n; j++)
				if(nodes[j].valid && cand != 0xffffffffu && nodes[j].weight < nodes[cand].weight)
					cand = j;
			pick[k] = cand;
			if(cand != 0xffffffffu)
			{
				nodes[cand].valid = 0;
				nodes[cand].parent = n;
				sum += nodes[cand].weight;
			}
		}
		nodes[n].valid = 1;
		nodes[n].weight = sum;
		nodes[n].left = pick[0];
		nodes[n].right = pick[1];
		if(sum >= total)
			return (int)n;
	}
	return (int)(2 * leaves - 2);
}

// the code of every leaf; `bits` holds the code most significant bit first in its low `len` bits
typedef struct Code
{
	uint32_t bits;
	uint32_t len;
} Code_t;

static void TreeCodes(const CbgNode_t* nodes, int root, uint32_t leaves, Code_t* code)
{
	uint32_t i;
	for(i = 0; i < leaves; i++)
	{
		uint32_t node = i, bits = 0, len = 0;
		code[i].bits = 0;
		code[i].len = 0;
		if(nodes[i].weight == 0)
			continue; // a byte that does not occur has no code
		while((int)node != root && node != 0xffffffffu && len < 32)
		{
			uint32_t parent = nodes[node].parent;
			if(parent == 0xffffffffu)
				break;
			// collected from the leaf up, so the first bit taken is the last the
			// decoder reads: it lands in the lowest bit and the code comes out
			// ready to write most significant bit first
			if(nodes[parent].right == node) // a set bit takes the right child
				bits |= 1u << len;
			len++;
			node = parent;
		}
		code[i].bits = bits;
		code[i].len = len;
	}
}

// a bit stream, most significant bit first, exactly as the decoder reads one
typedef struct BitW
{
	Buf_t* b;
	uint32_t acc;
	uint32_t n;
} BitW_t;

static void BitWPut(BitW_t* w, uint32_t bits, uint32_t len)
{
	int k;
	for(k = (int)len - 1; k >= 0; k--)
	{
		w->acc = (w->acc << 1) | ((bits >> k) & 1u);
		if(++w->n == 8)
		{
			BufByte(w->b, w->acc);
			w->acc = 0;
			w->n = 0;
		}
	}
}

static void BitWFlush(BitW_t* w)
{
	if(w->n)
	{
		BufByte(w->b, w->acc << (8 - w->n));
		w->acc = 0;
		w->n = 0;
	}
}

// ---- the quantisation table ----------------------------------------------------------

// the encrypted table: CbgDecrypt subtracts the generator's bytes, so the encoder adds them
static void EncryptTable(uint8_t* out, const uint8_t* table, uint32_t len, uint32_t key)
{
	uint32_t i;
	for(i = 0; i < len; i++)
		out[i] = (uint8_t)(table[i] + (uint8_t)BGI_HashUpdate(&key));
}

// ---- the encoder ---------------------------------------------------------------------

// the alpha of the picture the tests use: eight levels, different in every block
static uint8_t AlphaAt(uint32_t x, uint32_t y)
{
	return (uint8_t)(((x + y * 3) & 7) * 32);
}

// the presence mask of the blocks: 1 for a block whose alpha the plane carries
static int PresentAt(uint32_t x, uint32_t y, int pattern)
{
	if(pattern == 0) // every block present
		return 1;
	return ((x / 8 + y / 8) & 1) == 0; // checkerboard of blocks
}

/* the bytes AlphaHuff decodes for a padded alpha plane: one presence bit per
 * block (row-major, the lowest bit of a byte first) followed by the alpha
 * bytes of every present block, row by row inside it.  Returns the count. */
static uint32_t PackPlane(uint8_t* plane, const uint8_t* alpha, int pattern)
{
	uint32_t cols = COLS, rows = ROWS, n = MASK_BYTES, x, y;
	memset(plane, 0, MASK_BYTES);
	for(y = 0; y < rows; y++)
	{
		uint32_t bh = PAD_H - y * 8 < 8 ? PAD_H - y * 8 : 8;
		for(x = 0; x < cols; x++)
		{
			uint32_t blk = y * cols + x, bw = PAD_W - x * 8 < 8 ? PAD_W - x * 8 : 8, r, c;
			if(!PresentAt(x * 8, y * 8, pattern))
				continue;
			plane[blk >> 3] |= (uint8_t)(1u << (blk & 7));
			for(r = 0; r < bh; r++)
				for(c = 0; c < bw; c++)
					plane[n++] = alpha[(y * 8 + r) * PAD_W + x * 8 + c];
		}
	}
	return n;
}

// the mode 1 stream of a padded alpha plane: a zero flag byte, then eight literals
static void PutAlphaLz(Buf_t* b, const uint8_t* alpha)
{
	uint32_t i, n = PAD_W * PAD_H;
	for(i = 0; i < n; i += 8)
	{
		uint32_t k;
		BufByte(b, 0);
		for(k = 0; k < 8 && i + k < n; k++)
			BufByte(b, alpha[i + k]);
	}
}

// the mode 2 stream: the symbol count, the 256 leaf weights (byte counts), the bit stream
static void PutAlphaHuff(Buf_t* b, const uint8_t* plane, uint32_t count)
{
	uint32_t weights[256], i;
	CbgNode_t nodes[511];
	Code_t code[256];
	BitW_t w;
	int root;

	memset(weights, 0, sizeof weights);
	for(i = 0; i < count; i++)
		weights[plane[i]]++;
	BufU32(b, count);
	for(i = 0; i < 256; i++)
		BufVarint(b, weights[i]);
	root = TreeBuild(nodes, weights, 256);
	CHECK(root >= 0);
	TreeCodes(nodes, root, 256, code);
	w.b = b;
	w.acc = 0;
	w.n = 0;
	for(i = 0; i < count; i++)
	{
		CHECK(code[plane[i]].len > 0); // a byte that occurs always has a code
		BitWPut(&w, code[plane[i]].bits, code[plane[i]].len);
	}
	BitWFlush(&w);
}

// how the alpha stream of the file is written; the flags below break it on purpose
#define A_NONE 0     // no alpha stream at all (a 24-bit picture)
#define A_MODE1 1    // the literal stream
#define A_MODE2 2    // the Huffman coded plane
#define F_FULL 0     // a well formed file
#define F_ZERO_WEIGHTS 1  // every leaf weight 0: the tree does not exist
#define F_BAD_COUNT 2     // a symbol count far past what the picture could hold
#define F_SHORT_PLANE 4   // a plane that ends after the presence bits
#define F_BAD_ALPHA_OFF 8 // an alpha offset past the end of the file

/* Encode the test picture as a version 2 CompressedBG file: the header, the
 * encrypted quantisation tables, the 16 + 176 weights, the offset table, one
 * band per block row (every block of the row present, every coefficient zero)
 * and the alpha stream.  `alphaPattern` selects the picture's alpha (0 the
 * four-level pattern of AlphaAt, 1 transparent, 2 opaque, 3 the checkerboard
 * of present blocks).  Returns the file (free it) and its size; the offset of
 * the alpha stream relative to the start of the stream goes to *alphaOff. */
static uint8_t* EncodeFile(int bpp, int mode, int alphaPattern, int flags, uint32_t* size, uint32_t* alphaOff)
{
	Buf_t stream, file;
	uint8_t alpha[PAD_W * PAD_H], plane[MASK_BYTES + PAD_W * PAD_H], table[ENC_LEN];
	uint32_t bandCoefs = COLS * 3 * 64, bandBytes, dcLen = VarintLen(bandCoefs), i, r, alphaPos;
	uint8_t sum = 0, xr = 0;
	uint32_t key = 0x12345678u;

	// the alpha plane of the padded rows
	for(i = 0; i < PAD_W * PAD_H; i++)
	{
		uint32_t x = i % PAD_W, y = i / PAD_W;
		if(alphaPattern == 1)
			alpha[i] = 0x00; // transparent
		else if(alphaPattern == 2)
			alpha[i] = 0xff; // opaque
		else
			alpha[i] = AlphaAt(x, y);
	}
	// the bands: one bitmap byte, the coefficient count, the DC bits (all zero)
	bandBytes = (COLS + 7) / 8 + dcLen + ((bandCoefs / 64) + 7) / 8;

	memset(&stream, 0, sizeof stream);
	for(i = 0; i < 16; i++) // the DC weights: a single symbol, a one bit code
		BufVarint(&stream, i == 0 ? 1 : 0);
	for(i = 0; i < 176; i++) // no AC coefficients at all
		BufVarint(&stream, 0);
	{
		// the offsets are relative to the start of the stream, the weights
		// included; the first band follows the table
		uint32_t first = stream.len + (ROWS + 1) * 4;
		for(r = 0; r <= ROWS; r++)
			BufU32(&stream, first + r * bandBytes);
	}
	for(r = 0; r < ROWS; r++)
	{
		BufByte(&stream, (1u << COLS) - 1u); // every block of the row is coded
		BufVarint(&stream, bandCoefs);
		for(i = 0; i < (bandCoefs / 64 + 7) / 8; i++)
			BufByte(&stream, 0); // the DC differences: symbol 0, one bit each
	}
	alphaPos = stream.len;
	if(flags & F_BAD_ALPHA_OFF)
	{
		uint8_t* off = stream.p + (stream.len - 4); // offsets[ROWS]
		off[0] = 0xf0;
		off[1] = 0xff;
		off[2] = 0xff;
		off[3] = 0xff;
	}
	else if(mode != A_NONE)
	{
		uint32_t count, countPos;
		BufU32(&stream, (uint32_t)mode);
		if(mode == 1)
			PutAlphaLz(&stream, alpha);
		else
		{
			count = PackPlane(plane, alpha, alphaPattern == 3 ? 1 : 0);
			if(flags & F_SHORT_PLANE)
				count = MASK_BYTES; // the presence bits only: no alpha byte follows
			countPos = stream.len; // the count field of the plane
			if(flags & F_ZERO_WEIGHTS)
			{
				BufU32(&stream, count);
				for(i = 0; i < 256; i++)
					BufVarint(&stream, 0);
				BufByte(&stream, 0);
			}
			else
				PutAlphaHuff(&stream, plane, count);
			if(flags & F_BAD_COUNT)
			{
				// trust nothing: the count claims four billion symbols
				stream.p[countPos] = stream.p[countPos + 1] = 0xff;
				stream.p[countPos + 2] = stream.p[countPos + 3] = 0xff;
			}
		}
	}

	file.p = NULL;
	file.len = file.cap = 0;
	{
		uint8_t header[0x30];
		memset(header, 0, sizeof header);
		memcpy(header, CBG_MAGIC, 16);
		header[0x10] = (uint8_t)IMG_W;
		header[0x11] = (uint8_t)(IMG_W >> 8);
		header[0x12] = (uint8_t)IMG_H;
		header[0x13] = (uint8_t)(IMG_H >> 8);
		header[0x14] = (uint8_t)bpp;
		header[0x24] = (uint8_t)key;
		header[0x25] = (uint8_t)(key >> 8);
		header[0x26] = (uint8_t)(key >> 16);
		header[0x27] = (uint8_t)(key >> 24);
		header[0x28] = (uint8_t)ENC_LEN;
		header[0x29] = (uint8_t)(ENC_LEN >> 8);
		header[0x2a] = (uint8_t)(ENC_LEN >> 16);
		header[0x2b] = (uint8_t)(ENC_LEN >> 24);
		header[0x2e] = 2; // version
		BufPut(&file, header, 0x30);
	}
	// the two 8 x 8 quantisation tables (flat 16) and the four unused bytes
	memset(table, 16, 128);
	memset(table + 128, 0, 4);
	for(i = 0; i < ENC_LEN; i++)
	{
		sum = (uint8_t)(sum + table[i]);
		xr ^= table[i];
	}
	file.p[0x2c] = sum;
	file.p[0x2d] = xr;
	EncryptTable(table, table, ENC_LEN, key);
	BufPut(&file, table, ENC_LEN);
	BufPut(&file, stream.p, stream.len);
	free(stream.p);
	*size = file.len;
	*alphaOff = alphaPos;
	{ // trim to the exact length, so a read past the file is a read past the block
		uint8_t* p = (uint8_t*)realloc(file.p, file.len);
		if(p)
			file.p = p;
	}
	return file.p;
}

// ---- the decoder under test ----------------------------------------------------------

/* Decode with Cbg2_DecodeSized into a buffer with guard bytes behind the image
 * and check that nothing was written past the image.  Returns the decoded
 * buffer (free it) or NULL when the size cannot be had from the header. */
static uint8_t* DecodeAndCheck(const uint8_t* file, uint32_t size, uint32_t* code)
{
	const CbgHeader_t* h = (const CbgHeader_t*)file;
	uint32_t n = 0x10u + (uint32_t)h->width * h->height * ((h->bpp & 0xffffu) >> 3);
	uint8_t* out = (uint8_t*)malloc(n + GUARD);
	uint32_t i;

	memset(out, 0xa5, n + GUARD);
	*code = Cbg2_DecodeSized(file, size, out);
	for(i = n; i < n + GUARD; i++)
		CHECK(out[i] == 0xa5); // the decoder stayed inside the image
	return out;
}

// the alpha byte of the decoded image at x, y of a 32-bit picture
static uint8_t DecodedAlpha(const uint8_t* img, uint32_t x, uint32_t y)
{
	return img[0x10 + (y * IMG_W + x) * 4 + 3];
}

// check the alpha of the whole picture against `expect` (padded rows, 0xff where absent)
static void CheckAlpha(const uint8_t* img, const uint8_t* expect, const char* what)
{
	uint32_t x, y, bad = 0;
	for(y = 0; y < IMG_H; y++)
		for(x = 0; x < IMG_W; x++)
			if(DecodedAlpha(img, x, y) != expect[y * PAD_W + x])
			{
				if(!bad)
					printf("  %s: alpha at (%u,%u) is %02x, expected %02x\n", what, x, y,
						DecodedAlpha(img, x, y), expect[y * PAD_W + x]);
				bad++;
			}
	CHECK(bad == 0);
}

// the alpha a mode 2 picture with the given presence must decode to: opaque where absent
static void ExpectMode2(uint8_t* expect, int alphaPattern)
{
	uint32_t i;
	for(i = 0; i < PAD_W * PAD_H; i++)
	{
		uint32_t x = i % PAD_W, y = i / PAD_W;
		int present = PresentAt(x & ~7u, y & ~7u, alphaPattern == 3 ? 1 : 0);
		if(!present)
			expect[i] = 0xff; // a cleared block keeps the opaque default
		else if(alphaPattern == 1)
			expect[i] = 0x00;
		else if(alphaPattern == 2)
			expect[i] = 0xff;
		else
			expect[i] = AlphaAt(x, y);
	}
}

// ---- the tests -----------------------------------------------------------------------

// one well formed mode 2 picture: every block present, four grey levels
static void TestMode2Alpha(void)
{
	const CbgHeader_t* h;
	uint8_t expect[PAD_W * PAD_H], *file, *img;
	uint32_t size = 0, alphaOff = 0, code = 0, x, y;

	printf(" mode 2\n");
	file = EncodeFile(32, A_MODE2, 0, F_FULL, &size, &alphaOff);
	h = (const CbgHeader_t*)file;
	CHECK(size > 0x30 + ENC_LEN);
	CHECK(h->width == IMG_W && h->height == IMG_H && (h->bpp & 0xffffu) == 32);
	CHECK(h->version == 2);
	img = DecodeAndCheck(file, size, &code);
	CHECK(code == 0);
	ExpectMode2(expect, 0);
	CheckAlpha(img, expect, "mode 2");
	// the in-memory header, and the flat grey the bands decode to
	CHECK(memcmp(img, file + 0x10, 0x10) == 0);
	for(y = 0; y < 2; y++)
		for(x = 0; x < 2; x++)
		{
			uint32_t o = 0x10 + (y * IMG_W + x) * 4;
			CHECK(img[o] == 128 && img[o + 1] == 128 && img[o + 2] == 128);
		}
	free(img);
	free(file);
}

// the pictures that single out the tree's and the mask's corner cases
static void TestMode2Special(void)
{
	static const int kPatterns[] = {1, 2, 3}; // transparent, opaque, checkerboard
	static const char* kNames[] = {"transparent", "opaque", "checkerboard"};
	int i;
	printf(" mode 2 special\n");
	for(i = 0; i < 3; i++)
	{
		uint8_t expect[PAD_W * PAD_H], *file, *img;
		uint32_t size = 0, alphaOff = 0, code = 0;
		file = EncodeFile(32, A_MODE2, kPatterns[i], F_FULL, &size, &alphaOff);
		img = DecodeAndCheck(file, size, &code);
		CHECK(code == 0);
		ExpectMode2(expect, kPatterns[i]);
		CheckAlpha(img, expect, kNames[i]);
		free(img);
		free(file);
	}
}

// the same picture through the literal stream, which must behave as before
static void TestMode1Alpha(void)
{
	uint8_t expect[PAD_W * PAD_H], *file, *img;
	uint32_t size = 0, alphaOff = 0, code = 0, i;

	printf(" mode 1\n");
	for(i = 0; i < PAD_W * PAD_H; i++)
		expect[i] = AlphaAt(i % PAD_W, i / PAD_W);
	file = EncodeFile(32, A_MODE1, 0, F_FULL, &size, &alphaOff);
	img = DecodeAndCheck(file, size, &code);
	CHECK(code == 0);
	CheckAlpha(img, expect, "mode 1");
	free(img);
	free(file);
}

// a 24-bit picture has no alpha stream: the decoder must not look for one
static void TestNoAlpha(void)
{
	uint8_t *file, *img;
	uint32_t size = 0, alphaOff = 0, code = 0;
	printf(" no alpha\n");
	file = EncodeFile(24, A_NONE, 0, F_FULL, &size, &alphaOff);
	img = DecodeAndCheck(file, size, &code);
	CHECK(code == 0);
	CHECK(((const CbgHeader_t*)file)->bpp == 24);
	free(img);
	free(file);
}

// the entry BGI_CbgDecode uses cannot know the size: it must decode the same way
static void TestLegacyEntry(void)
{
	uint8_t expect[PAD_W * PAD_H], *file, *img;
	uint32_t size = 0, alphaOff = 0;

	printf(" legacy entry\n");
	file = EncodeFile(32, A_MODE2, 0, F_FULL, &size, &alphaOff);
	img = (uint8_t*)malloc(0x10u + IMG_W * IMG_H * 4 + GUARD);
	CHECK(Cbg2_Decode(file, img) == 0); // the version 2 decoder, size unknown
	ExpectMode2(expect, 0);
	CheckAlpha(img, expect, "legacy");
	free(img);
	free(file);
}

// malformed files: the same buffer is decoded with a broken field, and the guards must hold
static void TestMalformed(void)
{
	uint32_t i;
	printf(" malformed\n");
	for(i = 0; i < 6; i++)
	{
		uint8_t *file, *img;
		uint32_t size = 0, alphaOff = 0, code = 0, cut = 0, n;
		switch(i)
		{
		case 0: // every weight zero: no tree, so no plane
			file = EncodeFile(32, A_MODE2, 0, F_ZERO_WEIGHTS, &size, &alphaOff);
			break;
		case 1: // a count past the picture
			file = EncodeFile(32, A_MODE2, 0, F_BAD_COUNT, &size, &alphaOff);
			break;
		case 2: // a plane that stops after the presence bits
			file = EncodeFile(32, A_MODE2, 0, F_SHORT_PLANE, &size, &alphaOff);
			break;
		case 3: // the alpha offset points past the file
			file = EncodeFile(32, A_MODE2, 0, F_BAD_ALPHA_OFF, &size, &alphaOff);
			break;
		case 4: // the file ends in the middle of the bands
			file = EncodeFile(32, A_MODE2, 0, F_FULL, &size, &alphaOff);
			cut = size / 2;
			break;
		default: // a file of the header only
			file = EncodeFile(32, A_MODE2, 0, F_FULL, &size, &alphaOff);
			cut = 0x40;
			break;
		}
		n = cut ? cut : size;
		img = DecodeAndCheck(file, n, &code); // no crash, no write past the image
		free(img);
		free(file);
	}
}

// the header fields that make the decoder refuse the file outright
static void TestReject(void)
{
	uint8_t *file, *img;
	uint32_t size = 0, alphaOff = 0, n = 0x10u + IMG_W * IMG_H * 4 + GUARD;

	printf(" reject\n");
	file = EncodeFile(32, A_MODE2, 0, F_FULL, &size, &alphaOff);
	img = (uint8_t*)malloc(n);
	// a file shorter than its quantisation tables
	CHECK(Cbg2_DecodeSized(file, 0x40, img) != 0);
	// a bad checksum byte
	file[0x2c] ^= 1;
	CHECK(Cbg2_DecodeSized(file, size, img) == 0x80000004u);
	file[0x2c] ^= 1;
	// a zero length is the "size unknown" case: it must decode
	CHECK(Cbg2_DecodeSized(file, 0, img) == 0);
	free(img);
	free(file);
}

int main(void)
{
	printf("test_cbg2_alpha:\n");
	TestMode2Alpha();
	TestMode2Special();
	TestMode1Alpha();
	TestNoAlpha();
	TestLegacyEntry();
	TestMalformed();
	TestReject();
	if(gFails)
	{
		printf("  %d check(s) failed\n", gFails);
		return 1;
	}
	printf("  all checks passed\n");
	return 0;
}
