/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/movie_arc.c - the archive movie player ("92 F0"), the loose movie
 *                    player ("90 F0") as it falls back to ffmpeg, and the
 *                    video textures ("92 F2 / F4 / F5", "91 F1 .. F7")
 *                    with their cache (src/gfx/movie.c, src/sys/moviefx.c,
 *                    src/gfx/vidtex.c); built as bin/test_movie_arc by
 *                    `make test`
 *
 * A scratch directory is made the working directory.  A two-second
 * MPEG-1 clip is generated with ffmpeg (the parts that need it are
 * skipped when the program is not on the PATH) and written both loose
 * and into a PackFile archive next to a BF_Movie built with
 * BfMovieEnc_*; the archive is opened through a FileSet_t chain and
 * installed as gArcMgr.
 *
 *   extract  - the entry is unpacked byte for byte to a host file; a
 *              second call reuses the file (the stats count one
 *              extraction and one hit)
 *   play     - "90 F0" on the loose clip and "92 F0" on the archived one:
 *              a movie starts, the length matches the clip's real length,
 *              frames are decoded, the volume and the playing state
 *              follow, the end is reported once and a stop in the middle
 *              leaves nothing running
 *   texture  - attach a BF_Movie to a bitmap slot: the first frame is in
 *              the pixels, the duration and the position are what the
 *              movie's fps and frame count say, the clock advances the
 *              decoded frame, "91 F7" reports it, "91 F5" advances it, a
 *              released texture answers 1 / 4, and the error codes of a
 *              missing entry, a non-movie and a mismatching slot are the
 *              attach's own
 *   cleanup  - a freed slot drops its texture on the next tick
 *
 * SDL runs with the dummy drivers: the player blits only while the
 * engine's window is alive, and this test has none (gWindowAlive), so the
 * decoded frames are counted by the player instead.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/bfmovie.h"
#include "bgi/file.h"
#include "bgi/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int gFails;
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// ---- files -------------------------------------------------------------------------------------

// store a 32-bit value little-endian
static void Put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Write a PackFile holding `count` entries given by name / data / size:
 * the 16-byte header, one 32-byte index entry per file (the data offset
 * counted from the end of the index), then the data. */
static int WriteArchive(const char* path, int count, const char* const* names, uint8_t* const* data,
	const uint32_t* sizes)
{
	uint8_t hdr[16 + 3 * 32];
	FILE* f = fopen(path, "wb");
	uint32_t off = 0;
	int i;
	if(f == NULL)
		return 0;
	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, PACKFILE_MAGIC, 12);
	Put32(hdr + 12, (uint32_t)count);
	for(i = 0; i < count; i++)
	{
		uint8_t* e = hdr + 16 + i * 32;
		snprintf((char*)e, 16, "%s", names[i]);
		Put32(e + 16, off);
		Put32(e + 20, sizes[i]);
		off += sizes[i];
	}
	fwrite(hdr, 1, 16 + (size_t)count * 32, f);
	for(i = 0; i < count; i++)
		fwrite(data[i], 1, sizes[i], f);
	fclose(f);
	return 1;
}

// read a whole file into a fresh buffer; *size receives its length (NULL on failure)
static uint8_t* ReadWhole(const char* path, uint32_t* size)
{
	FILE* f = fopen(path, "rb");
	long n;
	uint8_t* p;
	if(f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = (uint8_t*)malloc((size_t)n + 1);
	if(p == NULL || (n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n))
	{
		free(p);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (uint32_t)n;
	return p;
}

// ---- fixtures ----------------------------------------------------------------------------------

#define ANIM_W 64
#define ANIM_H 48
#define ANIM_FPS 10
#define ANIM_FRAMES 5

static uint32_t gAnimPix[ANIM_FRAMES][ANIM_W * ANIM_H];

// a BGRA picture that changes every frame (every pixel differs, so a stale frame cannot pass)
static void AnimFill(uint32_t* pix, int frame)
{
	int i;
	for(i = 0; i < ANIM_W * ANIM_H; i++)
	{
		uint32_t x = (uint32_t)(i % ANIM_W), y = (uint32_t)(i / ANIM_W);
		pix[i] = 0xff000000u | ((uint32_t)(x * 4 + frame * 20) & 0xff) << 16 |
				 ((uint32_t)(y * 5 + frame * 10) & 0xff) << 8 | ((uint32_t)(x + y + frame * 7) & 0xff);
	}
}

// build the BF_Movie of ANIM_FRAMES pictures; returns its bytes (BGI_Free) and size
static uint8_t* BuildAnimMovie(uint32_t* size)
{
	BfMovieEnc_t* e = BfMovieEnc_Begin(ANIM_W, ANIM_H, 32, ANIM_FPS);
	int i;
	uint8_t* file;
	if(e == NULL)
		return NULL;
	for(i = 0; i < ANIM_FRAMES; i++)
	{
		AnimFill(gAnimPix[i], i);
		BfMovieEnc_AddFrame(e, (const uint8_t*)gAnimPix[i], ANIM_W * 4);
	}
	file = BfMovieEnc_Finish(e, size);
	return file;
}

// a slot's pixels against a picture, row by row (the pitch may pad the rows)
static int PixelsMatch(const Bmp_t* b, const uint32_t* pix)
{
	int y;
	for(y = 0; y < b->h; y++)
		if(memcmp((const uint8_t*)b->pixels + (size_t)y * b->pitch, pix + (size_t)y * b->w, (size_t)b->w * 4) != 0)
			return 0;
	return 1;
}

// ---- the archive cache ("92 F0") ---------------------------------------------------------------

static void TestExtract(const char* arc, const char* entry, const uint8_t* bytes, uint32_t size)
{
	char path[0x400], path2[0x400];
	int extracts = 0, hits = 0;
	FILE* f;
	uint8_t* copy;
	long n;
	OsNativeStat_t st;
	printf("extract\n");
	CHECK(MovieArc_Extract(NULL, arc, "no_such_entry", path, sizeof path) == 0);
	CHECK(MovieArc_Extract(NULL, arc, entry, path, sizeof path));
	CHECK(OS_NativeStat(path, &st) == OS_NATIVE_FILE && st.size == (uint64_t)size);
	f = fopen(path, "rb");
	CHECK(f != NULL);
	copy = (uint8_t*)malloc((size_t)size + 1);
	n = f ? (long)fread(copy, 1, (size_t)size, f) : -1;
	if(f)
		fclose(f);
	CHECK(n == (long)size && memcmp(copy, bytes, size) == 0);
	free(copy);
	MovieArc_Stats(&extracts, &hits);
	CHECK(extracts == 1 && hits == 0);
	CHECK(MovieArc_Extract(NULL, arc, entry, path2, sizeof path2));
	CHECK(strcmp(path, path2) == 0);
	MovieArc_Stats(&extracts, &hits);
	CHECK(extracts == 1 && hits == 1);
	remove(path); // the cache file of this run
}

// ---- "90 F0" / "92 F0": the movie players ------------------------------------------------------

// run a started movie to its end (max `limitMs`), counting the ticks; 1 when it ended by itself
static int RunToEnd(int limitMs, int* frames)
{
	uint32_t start = OS_TicksMs();
	while(Movie_IsPlaying() && OS_TicksMs() - start <= (uint32_t)limitMs)
	{
		Movie_Tick();
		OS_SleepMs(5);
	}
	if(frames)
		*frames = MovieFx_FramesShown();
	return !Movie_IsPlaying();
}

static void TestLooseMovie(const char* clip)
{
	int32_t length = -1;
	int frames = 0;
	uint32_t start;
	printf("player loose\n");
	CHECK(Movie_Play(&length, "no_such_clip.mpg", 0, 0, 320, 240) == 0);
	CHECK(Movie_SetVolume(0x81) == 0); // above the maximum: refused
	CHECK(Movie_SetVolume(0x80) == 1);
	CHECK(Movie_Play(&length, clip, 0, 0, 320, 240) == 1);
	CHECK(Movie_IsPlaying());
	CHECK(length >= 1800 && length <= 2300); // ffprobe's duration of the two-second clip
	CHECK(Movie_SetVolume(0) == 1);          // silence while it plays
	// a few frames, then a stop in the middle
	start = OS_TicksMs();
	while(MovieFx_FramesShown() == 0 && Movie_IsPlaying() && OS_TicksMs() - start < 5000)
	{
		Movie_Tick();
		OS_SleepMs(5);
	}
	CHECK(MovieFx_FramesShown() > 0);
	CHECK(Movie_Stop() == 1);
	CHECK(!Movie_IsPlaying());
	CHECK(Movie_Stop() == 0); // nothing to stop the second time
	// now let one run to its end: the end is reported once and the player is gone
	CHECK(Movie_Play(&length, clip, 10, 10, 160, 120) == 1);
	CHECK(RunToEnd(15000, &frames));
	CHECK(frames >= 20); // the two-second clip at 25 fps: a real decode, not a stale frame
	CHECK(!Movie_IsPlaying());
}

static void TestArchiveMovie(const char* arc, const char* entry)
{
	int32_t length = -1;
	int frames = 0;
	printf("player archive\n");
	CHECK(Movie_PlayArc(&length, arc, "no_such_entry", 0, 0, 320, 240) == 0);
	CHECK(Movie_PlayArc(&length, arc, entry, 0, 0, 320, 240) == 1);
	CHECK(Movie_IsPlaying());
	CHECK(length >= 1800 && length <= 2300);
	CHECK(RunToEnd(15000, &frames));
	CHECK(frames >= 20);
	CHECK(!Movie_IsPlaying());
}

// ---- the video textures ("92 F2" .. / "91 F1" ..) ----------------------------------------------

static void TestVideoTexture(const char* arc)
{
	Bmp_t b;
	int32_t duration = -1, position = -1, frame = -1;
	uint32_t start;
	printf("video texture\n");
	// the registry's errors first: no such slot, an empty slot, a wrong-size slot, bad entries
	CHECK(VidTex_Attach(9, arc, "anim", 0, 64) == 2); // the slot does not exist
	CHECK(BmpMgr_Create(gBmpMgr, 8, ANIM_W / 2, ANIM_H, PM_ARGB32));
	CHECK(VidTex_Attach(8, arc, "anim", 0, 64) == 3); // wrong size
	// the slot the movie decodes into, empty first
	CHECK(BmpMgr_Create(gBmpMgr, 7, ANIM_W, ANIM_H, PM_ARGB32));
	CHECK(BmpMgr_Clear(gBmpMgr, 7, 0));
	CHECK(VidTex_Attach(7, arc, "no_such_entry", 0, 64) == 1);
	CHECK(VidTex_Attach(7, arc, "notmovie", 0, 64) == 1);
	CHECK(VidTex_State(7, &duration) == 1); // no texture yet
	CHECK(VidTex_Position(7, &position) == 1);
	CHECK(VidTex_Attach(7, arc, "anim", 1, 64) == 0);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &b, 7));
	CHECK(PixelsMatch(&b, gAnimPix[0])); // attach decoded frame 0
	CHECK(VidTex_State(7, &duration) == 0);
	CHECK(duration == ANIM_FRAMES * 1000 / ANIM_FPS); // 5 frames at 10 fps: 500 ms
	CHECK(VidTex_Position(7, &position) == 0 && position == 0); // the clock has not started
	CHECK(VidTex_Frame(7, &frame) == 1 && frame == 0);
	CHECK(VidTex_Control(7, 0) == 1); // start; 1 = ready, what the scripts' wait loop wants
	start = OS_TicksMs();
	while(OS_TicksMs() - start < 260)
	{
		VidTex_Tick();
		OS_SleepMs(5);
	}
	CHECK(VidTex_Position(7, &position) == 0);
	CHECK(position >= 200 && position <= 500); // about 260 ms in, clamped to the duration
	CHECK(VidTex_Frame(7, &frame) == 1);
	CHECK(frame >= 2 && frame <= ANIM_FRAMES - 1); // the clock advanced the decode
	CHECK(BmpMgr_GetInfo(gBmpMgr, &b, 7));
	CHECK(PixelsMatch(&b, gAnimPix[frame])); // and the pixels are that frame
	// "91 F5" advances at once
	{
		int32_t before = frame;
		CHECK(VidTex_Advance(7) == 0);
		CHECK(VidTex_Frame(7, &frame) == 1);
		CHECK(frame == (before + 1 < ANIM_FRAMES ? before + 1 : before));
	}
	// to the end: the position stops at the duration, the last frame stays
	start = OS_TicksMs();
	while(OS_TicksMs() - start < 400)
	{
		VidTex_Tick();
		OS_SleepMs(5);
	}
	CHECK(VidTex_Position(7, &position) == 0 && position == duration);
	CHECK(VidTex_Frame(7, &frame) == 1 && frame == ANIM_FRAMES - 1);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &b, 7));
	CHECK(PixelsMatch(&b, gAnimPix[ANIM_FRAMES - 1]));
	// pause and restart
	CHECK(VidTex_Control(7, 1) == 1);
	CHECK(VidTex_Position(7, &position) == 0 && position == 0);
	CHECK(VidTex_Control(7, 2) == 1); // restart: frame 0 is decoded again
	CHECK(VidTex_Frame(7, &frame) == 1 && frame == 0);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &b, 7));
	CHECK(PixelsMatch(&b, gAnimPix[0]));
	// release: the texture is gone, the pixels stay
	VidTex_Release(7);
	CHECK(VidTex_State(7, &duration) == 1);
	CHECK(VidTex_Position(7, &position) == 1);
	CHECK(VidTex_Advance(7) == 1);
	CHECK(VidTex_Frame(7, &frame) == 1 && frame == 0); // a slot with a bitmap: 0 without a texture
	CHECK(VidTex_State(9, &duration) == 4);           // no such slot
	CHECK(VidTex_Frame(9, &frame) == 0);
	// a freed slot drops its texture on the next tick; a re-created one must not get frames
	CHECK(VidTex_Attach(10, arc, "anim", 0, 64) == 2); // needs a bitmap
	CHECK(BmpMgr_Create(gBmpMgr, 10, ANIM_W, ANIM_H, PM_ARGB32));
	CHECK(VidTex_Attach(10, arc, "anim", 0, 64) == 0);
	CHECK(VidTex_Control(10, 0) == 1);
	CHECK(BmpMgr_FreeSlot(gBmpMgr, 10));
	VidTex_Tick();
	CHECK(VidTex_State(10, &duration) == 4); // the slot is gone, so is the texture
	// a slot re-created behind the texture's back: dropped, not decoded into
	CHECK(BmpMgr_Create(gBmpMgr, 11, ANIM_W, ANIM_H, PM_ARGB32));
	CHECK(VidTex_Attach(11, arc, "anim", 0, 64) == 0);
	CHECK(BmpMgr_FreeSlot(gBmpMgr, 11));
	CHECK(BmpMgr_Create(gBmpMgr, 11, ANIM_W, ANIM_H, PM_ARGB32));
	VidTex_Tick();
	CHECK(VidTex_State(11, &duration) == 1);
	VidTex_FreeAll();
}

// ---- the ffmpeg clip ---------------------------------------------------------------------------

static int HaveProgram(const char* name)
{
	const char* argv[] = {name, "-version", NULL};
	char buf[256];
	OsPipeProc_t* p = OS_PipeProcStart(argv);
	int n;
	if(!p)
		return 0;
	n = OS_PipeProcRead(p, buf, (int)sizeof buf);
	OS_PipeProcStop(p);
	return n > 0;
}

static int MakeClip(const char* path)
{
	const char* argv[] = {"ffmpeg", "-v", "quiet", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
		"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "2", "-c:v", "mpeg1video", "-c:a",
		"mp2", "-b:a", "128k", path, NULL};
	char buf[64];
	OsPipeProc_t* p = OS_PipeProcStart(argv);
	if(!p)
		return 0;
	while(OS_PipeProcRead(p, buf, (int)sizeof buf) > 0)
		;
	OS_PipeProcStop(p);
	return OS_NativeStat(path, NULL) == OS_NATIVE_FILE;
}

int main(void)
{
	char dir[0x100], arcName[0x100];
	char clipName[0x40], animName[0x10];
	uint8_t *clipData = NULL, *animData = NULL, *textData = NULL;
	uint32_t clipSize = 0, animSize = 0, textSize = 0;
	const char* names[3];
	uint8_t* datas[3];
	uint32_t sizes[3];
	FileSet_t* mgr;
	int haveFfmpeg;

	snprintf(dir, sizeof dir, "/tmp/bgi_movie_arc_%ld", (long)getpid());
	mkdir(dir, 0755);
	if(chdir(dir) != 0)
		return 1;
	// no window, no audio device: the engine's SDL use must not need one
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	OS_Init();
	SetBaseDir(NULL); // "C:\" is the scratch directory (the OS layer's drive is the working directory)
	printf("archive\n");

	// the fixtures: a generated clip (when ffmpeg is there), the BF_Movie and a non-movie entry
	haveFfmpeg = HaveProgram("ffmpeg") != 0;
	snprintf(clipName, sizeof clipName, "clip.mpg");
	snprintf(animName, sizeof animName, "anim");
	animData = BuildAnimMovie(&animSize);
	CHECK(animData != NULL && animSize > 0x80);
	textData = (uint8_t*)malloc(64);
	memset(textData, 'x', 64);
	textSize = 64;
	if(haveFfmpeg)
	{
		CHECK(MakeClip(clipName));
		clipData = ReadWhole(clipName, &clipSize);
		CHECK(clipData != NULL && clipSize > 0x1000);
	}
	snprintf(arcName, sizeof arcName, "movies_%ld.arc", (long)getpid());
	{
		int n = 0;
		if(clipData)
		{
			names[n] = clipName;
			datas[n] = clipData;
			sizes[n] = clipSize;
			n++;
		}
		names[n] = animName;
		datas[n] = animData;
		sizes[n] = animSize;
		n++;
		names[n] = "notmovie";
		datas[n] = textData;
		sizes[n] = textSize;
		n++;
		CHECK(WriteArchive(arcName, n, names, datas, sizes));
	}
	// open the archive through a chain of our own and make it the engine's manager
	mgr = FileSet_New();
	CHECK(mgr != NULL);
	CHECK(ArcMgr_Exists(mgr, arcName, "anim"));
	gArcMgr = mgr;
	gBmpMgr = BmpMgr_New(64);
	CHECK(gBmpMgr != NULL);
	{
		Bmp_t b;
		CHECK(BmpMgr_GetInfo(gBmpMgr, &b, 0) == 0); // the manager is empty
	}

	if(haveFfmpeg)
	{
		TestExtract(arcName, clipName, clipData, clipSize);
		TestLooseMovie(clipName);
		TestArchiveMovie(arcName, clipName);
		{ // the play left the entry in the unpack cache: drop this run's file
			char cachePath[0x400];
			if(MovieArc_Extract(NULL, arcName, clipName, cachePath, sizeof cachePath))
				remove(cachePath);
		}
	}
	else
		printf("ffmpeg not found: the player tests are skipped\n");
	printf("texture\n");
	TestVideoTexture(arcName);

	// clean up
	VidTex_FreeAll();
	BmSeq_FreeAll();
	BmpMgr_Delete(gBmpMgr);
	gBmpMgr = NULL;
	FileSet_Delete(gArcMgr);
	gArcMgr = NULL;
	remove(arcName);
	remove(clipName);
	remove(animName);
	free(clipData);
	BGI_Free(animData);
	free(textData);
	OS_Shutdown();
	rmdir(dir); // the scratch directory is empty again
	printf("%s\n", gFails ? "FAILED" : "ok");
	return gFails ? 1 : 0;
}
