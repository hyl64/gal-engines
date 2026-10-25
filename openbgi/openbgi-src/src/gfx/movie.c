/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - movie playback of "90 F0" .. "90 F3" and "92 F0".  Interface:
 *           bgi/gfx.h (Movie_*, gMoviePlaying).
 *
 * The original plays a video file through a DirectShow filter graph shown
 * in a child window over the picture; the graph's event notifications
 * reach the main window procedure as WM_APP.  All of the COM work is in the
 * OS layer (OS_Movie*); this file keeps the engine-visible state: whether
 * a movie runs (the window then paints black and the display style cannot
 * be toggled), the volume, the error reporting and the length result.
 * When the OS layer has no player for the file (POSIX always; Windows
 * when DirectShow cannot render it) the ffmpeg player of
 * src/sys/moviefx.c takes over: it draws its frames from the engine's
 * pass (Movie_Tick) and its end is reported the way the graph's was.
 *
 * "92 F0" plays a movie that is stored inside an archive entry.  The
 * original fed DirectShow from the entry through its own source filter;
 * here the entry is unpacked to a host file first (MovieArc_Extract) and
 * that file is handed to the player as a native path - ffmpeg cannot seek
 * in a pipe, and a 500 MB opening movie should be copied once, not on
 * every play.  The cache lives in $TMPDIR/openbgi-movies and a file is
 * reused while its name and size match; the copy is streamed in 1 MB
 * ranges, so the entry never sits in memory as a whole.
 *
 * BGI_DEBUG_MOVIE=1 prints what was opened and how many frames were
 * decoded to stderr (off by default and one getenv per run).
 */
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"
#include "bgi/file.h"

#include <stdio.h>

int gMoviePlaying;           // a movie is running: read by the window procedure and the display style toggle
static int gMovieGraph;      // a graph exists
static int gMovieLoaded;     // a file is rendered in it
static int gMovieFx;         // the ffmpeg player runs the movie instead of the OS layer's graph
static int32_t gMovieVolume; // IBasicAudio units, -10000 .. 0; applied to every movie started

// ---- the diagnostic switch (BGI_DEBUG_MOVIE) ---------------------------------------------------

static int MovieDebug(void)
{
	static int on = -1; // -1: not looked up yet
	if(on < 0)
		on = getenv("BGI_DEBUG_MOVIE") != NULL;
	return on;
}

// "90 F1" (and engine shutdown): release the graph, if any.  1 when there was one, else 0
int Movie_Stop(void)
{
	int had = gMovieGraph || gMovieFx;
	if(!had)
		return 0;
	if(gMovieFx)
		MovieFx_Stop();
	else
		OS_MovieStop(); // IMediaControl::Stop when playing, hide and disown the video window, release all
	gMoviePlaying = 0;
	gMovieGraph = 0;
	gMovieFx = 0;
	gMovieLoaded = 0;
	return had;
}

// ---- the archive entry cache ("92 F0") ---------------------------------------------------------

#define MOVIE_ARC_CHUNK (4u << 20) // the copy runs in ranges of this many bytes (the archive is opened per range)

static int gMovieArcExtracts; // entries copied out
static int gMovieArcHits;     // calls that found a usable file already there

void MovieArc_Stats(int* extracts, int* hits)
{
	if(extracts)
		*extracts = gMovieArcExtracts;
	if(hits)
		*hits = gMovieArcHits;
}

// copy `src` into dst, every character that is not safe in a file name becoming '_'
static void MovieArc_SafeName(char* dst, size_t n, const char* src)
{
	size_t o = 0;
	if(n == 0)
		return;
	for(; src && *src && o + 1 < n; src++)
	{
		unsigned char c = (unsigned char)*src;
		int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
		dst[o++] = safe ? (char)c : '_';
	}
	dst[o] = 0;
}

/* Build the cache path of an entry: $TMPDIR/openbgi-movies/
 * <arc>__<name>__<size>.bin.  TMPDIR is the host's (macOS: a per-user
 * directory); the host file system is used through the native services,
 * never through the engine's drive mapping. */
static int MovieArc_CachePath(char* dst, size_t n, const char* arc, const char* name, uint32_t size)
{
	const char* tmp = getenv("TMPDIR");
	char safeArc[0x68], safeName[0x68];
	if(tmp == NULL || tmp[0] == 0)
		tmp = "/tmp";
	MovieArc_SafeName(safeArc, sizeof safeArc, arc && arc[0] ? arc : "loose");
	MovieArc_SafeName(safeName, sizeof safeName, name && name[0] ? name : "movie");
	return snprintf(dst, n, "%s%copenbgi-movies%c%s__%s__%u.mpg", tmp, OS_NATIVE_SEP, OS_NATIVE_SEP, safeArc,
			   safeName, (unsigned)size) < (int)n;
}

/* Resolve the archive name a script gives ("data02xxx.arc") the way the
 * loader's reads resolve it: relative names are those of files in the
 * game's directory, so the base directory is joined first, then the
 * alternative (disc) directory, then the name as it stands.  A list
 * archive ("80 38": "data02xxx.arc" standing for data02900.arc and its
 * siblings) only matches a path with a directory part, which is the other
 * reason the join happens here.  Returns the entry's size (0 not found)
 * and copies the path that answered to `resolved`. */
static uint32_t MovieArc_Resolve(struct FileSet* mgr, const char* arc, const char* name, char* resolved, size_t n)
{
	char path[0x104];
	int pass;
	for(pass = 0; pass < 3; pass++)
	{
		uint32_t size;
		if(pass == 0)
		{
			if(arc[0] && (arc[1] == ':' || arc[0] == '\\' || arc[0] == '/'))
				snprintf(path, sizeof path, "%s", arc);
			else
				PathJoin(path, gBaseDir, arc);
		}
		else if(pass == 1)
		{
			if(gAltDir[0] == 0)
				continue;
			PathJoin(path, gAltDir, arc);
		}
		else
			snprintf(path, sizeof path, "%s", arc);
		size = ArcMgr_SizeOf(mgr, path, name);
		if(size != ARC_ERR_NOT_FOUND && size != ARC_ERR_OPEN && size != 0)
		{
			snprintf(resolved, n, "%s", path);
			return size;
		}
	}
	return 0;
}

/* unpack entry (arc, name) of `mgr` (gArcMgr when NULL) to the host file
 * `nativeOut` when it is not there with the entry's size yet.  The writes
 * go to a ".part" file that is renamed when complete, so an interrupted
 * run never leaves a short file that a later play would take for the
 * cache.  1 ok, 0 unreadable entry / no writable cache directory. */
int MovieArc_Extract(struct FileSet* mgr, const char* arc, const char* name, char* nativeOut, size_t n)
{
	uint32_t size, off, got;
	char arcPath[0x104], path[0x400], part[0x404];
	FILE* f;
	OsNativeStat_t st;

	if(mgr == NULL)
		mgr = gArcMgr;
	if(mgr == NULL || arc == NULL || name == NULL)
		return 0;
	size = MovieArc_Resolve(mgr, arc, name, arcPath, sizeof arcPath);
	if(size == 0)
	{
		if(MovieDebug())
			fprintf(stderr, "[movie] archive entry %s:%s not found\n", arc, name);
		return 0;
	}
	if(!MovieArc_CachePath(path, sizeof path, arcPath, name, size))
		return 0;

	if(OS_NativeStat(path, &st) == OS_NATIVE_FILE && st.size == (uint64_t)size)
	{
		gMovieArcHits++;
		if(nativeOut && n)
			snprintf(nativeOut, n, "%s", path);
		if(MovieDebug())
			fprintf(stderr, "[movie] archive %s:%s reused from the cache (%u bytes)\n", arcPath, name, (unsigned)size);
		return 1;
	}
	{
		char dir[0x400];
		snprintf(dir, sizeof dir, "%s", path);
		{
			char* sep = strrchr(dir, OS_NATIVE_SEP);
			if(sep)
				sep[1] = 0;
		}
		OS_NativeMkdir(dir); // (1 when it is there afterwards; the cache is nowhere otherwise)
	}
	{
		uint8_t* buf = (uint8_t*)malloc(MOVIE_ARC_CHUNK);
		uint32_t start = OS_TicksMs();
		snprintf(part, sizeof part, "%s.part", path);
		f = buf ? fopen(part, "wb") : NULL;
		if(f == NULL)
		{
			free(buf);
			if(MovieDebug())
				fprintf(stderr, "[movie] archive %s:%s: cannot write %s\n", arcPath, name, part);
			return 0;
		}
		for(off = 0; off < size;)
		{
			uint32_t chunk = size - off < MOVIE_ARC_CHUNK ? size - off : MOVIE_ARC_CHUNK;
			got = ArcMgr_ReadRange(mgr, buf, arcPath, name, off, chunk);
			if(got != chunk || fwrite(buf, 1, got, f) != got)
				break;
			off += got;
		}
		free(buf);
		if(fclose(f) != 0 || off != size)
		{
			remove(part);
			if(MovieDebug())
				fprintf(stderr, "[movie] archive %s:%s: read failed at %u of %u bytes\n", arcPath, name, (unsigned)off, (unsigned)size);
			return 0;
		}
		// (a plain rename: both names are host paths in the same directory, and
		// the engine's path mapping must not see them)
		if(rename(part, path) != 0 && (OS_NativeStat(path, &st) != OS_NATIVE_FILE || st.size != (uint64_t)size))
		{
			remove(part);
			return 0;
		}
		gMovieArcExtracts++;
		if(MovieDebug())
			fprintf(stderr, "[movie] archive %s:%s unpacked to %s (%u bytes in %u ms)\n", arcPath, name, path, (unsigned)size,
				   (unsigned)(OS_TicksMs() - start));
	}
	if(nativeOut && n)
		snprintf(nativeOut, n, "%s", path);
	return 1;
}

// ---- the player ---------------------------------------------------------------------------------

/* Stop whatever runs, build a player for `path` and start it in the (x, y,
 * w, h) rectangle of the picture (mapped through the display scaling) at
 * the current volume.  `native` says that the path is a host path (the
 * unpacked archive entry) rather than an engine path.  1 when playback
 * started, *outLength then holding the length in ms as the player reports
 * it.  A missing audio device or renderer is a script error (thrown; the
 * graph is kept for the next stop); any other failure returns 0. */
static int Movie_Start(int32_t* outLength, const char* path, int native, int x, int y, int w, int h)
{
	Bmp_t scaled;
	Rect_t r, m;
	int32_t volume = gMovieVolume;

	Movie_Stop();
	// build the graph with its interfaces and render the file into it (IGraphBuilder::RenderFile)
	switch(OS_MovieOpen(path))
	{
		case OS_MOVIE_OK: gMovieGraph = 1; break;
		case OS_MOVIE_FAILED:
			// no native player for the file: ffmpeg, which scales the frames to the rectangle itself
			if(!(native ? MovieFx_StartNative(path, x, y, w, h, volume, outLength) :
                          MovieFx_Start(path, x, y, w, h, volume, outLength)))
			{
				if(MovieDebug())
					fprintf(stderr, "[movie] no player could be started for \"%s\"\n", path);
				return 0;
			}
			gMovieFx = 1;
			gMovieLoaded = 1;
			gMoviePlaying = 1;
			return 1;
		case OS_MOVIE_NO_AUDIO_DEVICE:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_NO_AUDIO_DEVICE);
			return 0;
		case OS_MOVIE_VIDEO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_VIDEO_NOT_RENDERED);
			return 0;
		case OS_MOVIE_AUDIO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_AUDIO_NOT_RENDERED);
			return 0;
		default: return 0; // the original keeps a failed graph until the next stop; the OS layer releases it at once
	}
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + h - 1;
	if(Gfx_GetScaledBmp(gGfx, &scaled)) // a scaled display: map the picture rectangle to window coordinates
	{
		Gfx_MapRectScaled(gGfx, &m, &r);
		x = m.l;
		y = m.t;
		w = m.r - m.l + 1;
		h = m.b - m.t + 1;
	}
	if(!OS_MovieStart(x, y, w, h, volume, outLength))
		return 0;
	gMovieLoaded = 1;
	gMoviePlaying = 1;
	return 1;
}

int Movie_Play(int32_t* outLength, const char* path, int x, int y, int w, int h)
{
	return Movie_Start(outLength, path, 0, x, y, w, h);
}

int Movie_PlayNative(int32_t* outLength, const char* nativePath, int x, int y, int w, int h)
{
	return Movie_Start(outLength, nativePath, 1, x, y, w, h);
}

/* "92 F0": unpack the entry of the archive manager to the cache and play
 * it.  1 started (*outLength the length), 0 no such entry / no player. */
int Movie_PlayArc(int32_t* outLength, const char* arc, const char* name, int x, int y, int w, int h)
{
	char native[0x400];
	int32_t length = 0;
	if(!MovieArc_Extract(NULL, arc, name, native, sizeof native))
		return 0;
	if(!Movie_PlayNative(&length, native, x, y, w, h))
		return 0;
	if(outLength)
		*outLength = length;
	if(MovieDebug())
		fprintf(stderr, "[movie] playing archive %s:%s at (%d, %d) %dx%d, length %d ms\n", arc, name, x, y, w, h, (int)length);
	return 1;
}

// "90 F2": a movie was started and the player still runs it
int Movie_IsPlaying(void)
{
	if(gMovieFx)
		return gMoviePlaying && MovieFx_IsRunning();
	return gMoviePlaying && OS_MovieIsRunning();
}

/* "90 F3": the volume, 0 .. 0x80, for the running movie and the ones to
 * come; 0 is silence (-10000), otherwise the attenuation
 * -(12800 - 100 v) * 0.375 in hundredths of a decibel.  1 ok, 0 for a
 * value above 0x80 (nothing changes). */
int Movie_SetVolume(uint32_t v)
{
	int32_t vol;
	if(v > 0x80)
		return 0;
	if(v == 0)
		vol = -10000;
	else
		vol = (int32_t) - ((double)(int64_t)(0x3200 - (int32_t)(v * 100)) * 0.375000000009375); // the original's constant: 0.375 with rounding noise
	OS_MovieSetVolume(vol);                                                                     // the original applies it only when a graph exists; the OS layer ignores the call without one
	MovieFx_SetVolume(vol);
	gMovieVolume = vol;
	return 1;
}

// every pass of the scheduler: the ffmpeg player shows its due frame; its end is the graph's EC_COMPLETE
void Movie_Tick(void)
{
	VidTex_Tick(); // the video textures of the bitmap slots decode their due frames here too
	if(gMovieFx && gMoviePlaying && !MovieFx_Tick())
		Movie_OnEvent(1);
}

/* the WM_APP notification of the main window: the OS layer has drained
 * the graph's events, `completed` says the movie reached its end.  A
 * movie that is over (or was stopped meanwhile) is released and the
 * picture restored. */
void Movie_OnEvent(int completed)
{
	if(completed)
		gMoviePlaying = 0;
	if(!gMoviePlaying)
	{
		Movie_Stop();
		Window_Repaint();
	}
}
