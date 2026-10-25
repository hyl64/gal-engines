/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the macOS (SDL2) audio output, CD audio and movie stubs
 *
 * The mixer hands 16-bit stereo PCM over through a pull callback; SDL2
 * runs the callback on its audio thread, which is exactly the contract of
 * OS_AudioStart (bgi/os.h).  CD audio (MCI), PlaySound and movie playback
 * have no macOS counterpart here and fail like on POSIX, so the engine
 * sees a drive without a CD and a machine without a movie graph.
 */

#include "darwin_internal.h"

#include <SDL.h>

#include <stdint.h>
#include <stdlib.h>

// the opened device; 0 = none (SDL's id 0 is never a valid device)
static SDL_AudioDeviceID gDev;

// SDL passes userdata through to the callback; keep the real fill there
static OsAudioFill_t gFill;

/* SDL's audio callback: hand the whole buffer to the mixer.  The mixer
 * must fill every frame, and SDL hands interleaved stereo 16-bit frames,
 * so frames = len / 4. */

static void AudioCbImpl(void* user, Uint8* stream, int len)
{
	if(gFill != NULL)
		gFill(user, (int16_t*)stream, len / 4);
	else
		memset(stream, 0, (size_t)len);
}

int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	if(gDev != 0 || rate <= 0 || fill == NULL)
		return 0;
	SDL_AudioSpec want;
	memset(&want, 0, sizeof(want));
	want.freq = rate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	// about 20 ms of frames, rounded up to a power of two as SDL prefers
	int frames = rate / 50;
	int pow2 = 64;
	while(pow2 < frames)
		pow2 *= 2;
	want.samples = (Uint16)pow2;
	want.callback = AudioCbImpl;
	want.userdata = user;
	gFill = fill;
	SDL_AudioSpec have;
	gDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if(gDev == 0)
	{
		if(getenv("BGI_DEBUG_OS")) fprintf(stderr, "[os] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		gFill = NULL;
		return 0;
	}
	SDL_PauseAudioDevice(gDev, 0);
	return 1;
}

void OS_AudioStop(void)
{
	if(gDev == 0)
		return;
	// SDL_CloseAudioDevice blocks until the callback has returned, so no
	// fill runs after this, as the contract requires
	SDL_CloseAudioDevice(gDev);
	gDev = 0;
	gFill = NULL;
}

// the mixer initialises and runs regardless; a silent device still counts
int OS_AudioAvailable(void)
{
	return 1;
}

// ---- CD audio (MCI) ---------------------------------------------------------------
// No CD audio: every call fails, so the engine sees a drive without a CD.

uint32_t OS_MciOpenCdAudio(const char* element)
{
	(void)element;
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	(void)dev;
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	(void)dev;
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	(void)dev;
	(void)item;
	(void)out;
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	(void)dev;
	(void)from;
	(void)to;
	(void)notify;
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	(void)dev;
	return 0;
}

// PlaySound: not accepted
int OS_PlaySoundFile(const char* path)
{
	(void)path;
	return 0;
}

// ---- movies ----------------------------------------------------------------------
// No movie playback: the open fails, and the rest reports a graph that is not there.

int OS_MovieOpen(const char* path)
{
	(void)path;
	return OS_MOVIE_FAILED;
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)volume;
	(void)lengthMs;
	return 0;
}

void OS_MovieStop(void)
{
}

int OS_MovieIsRunning(void)
{
	return 0;
}

void OS_MovieSetVolume(int32_t vol)
{
	(void)vol;
}
