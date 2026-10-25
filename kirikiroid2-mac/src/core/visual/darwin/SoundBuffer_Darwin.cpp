/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * SoundBuffer_Darwin.cpp - the script-visible wave sound buffer
 *
 * Windows plays wave buffers through waveOut; this back end plays them
 * through SDL_mixer.  A buffer carries either a Mix_Chunk (WAV: its own
 * channel, its own volume, its own pause - this is what sound effects and
 * the [ws] wait need) or, when the container is not a plain WAV (OGG and
 * friends), a Mix_Music on the single music stream.  Fades are stepped from
 * the host's frame loop (TVPDarwinTickSoundFades) so all engine-visible
 * calls stay on one thread, and a finished fade posts the same
 * onFadeCompleted event the engine's own base class posts.
 */

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjsVariant.h"
#include "StorageIntf.h"
#include "EventIntf.h"

#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	bool EnsureMixer()
	{
		static bool opened = false;
		static bool ok = false;
		if(!opened)
		{
			opened = true;
			ok = Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0;
			if(ok)
				Mix_AllocateChannels(16);
		}
		return ok;
	}

	int VolumeToMixer(tjs_int db)
	{
		// the engine's volume is -10000..0 dB; 0 dB is full scale
		if(db <= -10000) return 0;
		if(db >= 0) return MIX_MAX_VOLUME;
		const double linear = std::pow(10.0, db / 2000.0);
		return (int)(linear * MIX_MAX_VOLUME);
	}
}

namespace
{
	class WaveInstance; // defined just below

	// every live instance, so the frame-loop fade stepper can find them
	std::vector<WaveInstance*>& FadeRegistry()
	{
		static std::vector<WaveInstance*> v;
		return v;
	}

	class WaveInstance : public tTJSNativeInstance
	{
	public:
		enum class Kind { None, Chunk, Music };

		iTJSDispatch2* Owner = nullptr; // no addref; the TJS object owns this instance
		std::string Storage;
		std::string Bytes;             // the decoded-from-storage bytes SDL reads
		tjs_int Volume = 0;            // -10000 .. 0 dB
		tjs_int Pan = 0;
		bool Looping = false;
		bool Opened = false;
		Kind kind = Kind::None;
		Mix_Chunk* Chunk = nullptr;
		Mix_Music* Music = nullptr;
		int Channel = -1;              // chunk mode: the channel this buffer plays on
		bool MusicPlaying = false;     // music mode: this buffer owns the music stream
		double TotalTime = 0.0;

		// where the sound is, for the position property
		Uint64 PlayStart = 0;
		Uint64 PausedTotal = 0;
		Uint64 PauseStart = 0;

		double CurrentPosition()
		{
			if(kind == Kind::Music && Music != nullptr)
				return Mix_GetMusicPosition(Music);
			if(kind != Kind::Chunk || Channel < 0) return 0.0;
			Uint64 played = SDL_GetTicks64() - PlayStart - PausedTotal;
			return (double)played / 1000.0;
		}

		void NoteStarted()
		{
			PlayStart = SDL_GetTicks64();
			PausedTotal = 0;
			PauseStart = 0;
		}

		void NotePaused(bool paused)
		{
			const Uint64 now = SDL_GetTicks64();
			if(paused && PauseStart == 0) PauseStart = now;
			else if(!paused && PauseStart != 0)
			{
				PausedTotal += now - PauseStart;
				PauseStart = 0;
			}
		}

		// fade state, stepped by the host's frame loop on the main thread
		bool Fading = false;
		bool FadeDelaying = false;
		tjs_int FadeTarget = 0;
		tjs_int FadeFrom = 0;
		Uint64 FadeStart = 0;          // ms, when the delay ends / fade begins
		Uint64 FadeMs = 0;

		~WaveInstance() override
		{
			auto& registry = FadeRegistry();
			registry.erase(std::remove(registry.begin(), registry.end(), this),
				registry.end());
			HaltLocked();
		}

		void HaltLocked()
		{
			// stop whatever this buffer is doing before its bytes go away;
			// freeing a playing chunk or music is undefined behaviour
			if(kind == Kind::Chunk && Channel >= 0)
			{
				Mix_HaltChannel(Channel);
				Channel = -1;
			}
			if(kind == Kind::Music && MusicPlaying)
			{
				Mix_HaltMusic();
				MusicPlaying = false;
			}
			PlayStart = PausedTotal = PauseStart = 0;
			if(Chunk != nullptr) { Mix_FreeChunk(Chunk); Chunk = nullptr; }
			if(Music != nullptr) { Mix_FreeMusic(Music); Music = nullptr; }
			Bytes.clear();
			Bytes.shrink_to_fit();
			kind = Kind::None;
			Fading = false;
			Opened = false;
		}

		void ApplyVolume()
		{
			if(kind == Kind::Chunk)
			{
				if(Chunk != nullptr) Mix_VolumeChunk(Chunk, VolumeToMixer(Volume));
				if(Channel >= 0) Mix_Volume(Channel, VolumeToMixer(Volume));
			}
			else if(kind == Kind::Music && MusicPlaying)
			{
				Mix_VolumeMusic(VolumeToMixer(Volume));
			}
		}

		bool IsPlaying()
		{
			if(kind == Kind::Chunk)
				return Channel >= 0 && Mix_Playing(Channel) != 0;
			if(kind == Kind::Music)
				return MusicPlaying && Mix_PlayingMusic() != 0;
			return false;
		}

		bool IsPaused()
		{
			if(kind == Kind::Chunk)
				return Channel >= 0 && Mix_Paused(Channel) != 0;
			if(kind == Kind::Music)
				return MusicPlaying && Mix_PausedMusic() != 0;
			return false;
		}
	};

	void PostFadeCompleted(WaveInstance* _this)
	{
		// the same event the engine's own base class posts when a fade ends
		if(_this->Owner)
		{
			static ttstr eventname(TJS_W("onFadeCompleted"));
			TVPPostEvent(_this->Owner, _this->Owner, eventname, 0, TVP_EPT_POST, 0, nullptr);
		}
	}
}

class tTJSNC_WaveSoundBuffer_Darwin : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_WaveSoundBuffer_Darwin();
	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance* CreateNativeInstance() override;
};

tjs_uint32 tTJSNC_WaveSoundBuffer_Darwin::ClassID = (tjs_uint32)-1;

tTJSNC_WaveSoundBuffer_Darwin::tTJSNC_WaveSoundBuffer_Darwin()
	: inherited(TJS_W("WaveSoundBuffer"))
{
	TJS_BEGIN_NATIVE_MEMBERS(WaveSoundBuffer)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
		/*var.type*/ WaveInstance, /*TJS class name*/ WaveSoundBuffer)
	{
		if(numparams >= 1)
			_this->Owner = objthis; // the TJS object; callbacks come back to it
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ WaveSoundBuffer)

	// open: read through the engine's storage (archives work), then hand the
	// bytes to SDL_mixer; plain WAV becomes a chunk, everything else music
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ open)
	{
		TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
		_this->Owner = objthis;
		if(numparams >= 1 && param[0]->Type() == tvtString)
		{
			_this->HaltLocked();
			_this->Opened = false;
			tTJSNarrowStringHolder narrow(param[0]->GetString());
			_this->Storage = (const char*)narrow;
			if(!EnsureMixer())
				return TJS_S_OK;

			tTJSBinaryStream* stream = TVPCreateStream(param[0]->GetString());
			if(stream == nullptr)
				return TJS_S_OK;
			_this->Bytes.clear();
			char buffer[0x4000];
			tjs_uint read;
			while((read = stream->Read(buffer, sizeof buffer)) > 0)
				_this->Bytes.insert(_this->Bytes.end(), buffer, buffer + read);
			delete stream;
			if(_this->Bytes.empty())
				return TJS_S_OK;

			SDL_RWops* rw = SDL_RWFromMem(&_this->Bytes[0], (int)_this->Bytes.size());
			if(rw == nullptr)
				return TJS_S_OK;
			// chunk first: a chunk has its own channel, volume and pause,
			// which is what sound effects and [ws] waits are built on
			_this->Chunk = Mix_LoadWAV_RW(rw, 0);
			if(_this->Chunk != nullptr)
			{
				_this->kind = WaveInstance::Kind::Chunk;
				int freq = 0, channels = 0;
				Uint16 format = 0;
				double bytesPerSample = 1;
				if(Mix_QuerySpec(&freq, &format, &channels) != 0)
					bytesPerSample = (SDL_AUDIO_BITSIZE(format) / 8) * channels;
				if(bytesPerSample > 0 && freq > 0)
					_this->TotalTime =
						(double)_this->Chunk->alen / (freq * bytesPerSample);
			}
			else
			{
				SDL_RWseek(rw, 0, RW_SEEK_SET);
				_this->Music = Mix_LoadMUS_RW(rw, 0);
				if(_this->Music != nullptr)
				{
					_this->kind = WaveInstance::Kind::Music;
					_this->TotalTime = (double)Mix_MusicDuration(_this->Music);
				}
			}
			SDL_FreeRW(rw);
			_this->Opened = _this->kind != WaveInstance::Kind::None;
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ open)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ close)
	{
		TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
		_this->HaltLocked();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ close)

#define WAVE_SIMPLE(name, action)                                              \
	TJS_BEGIN_NATIVE_METHOD_DECL(name)                                          \
	{                                                                          \
		TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);                           \
		do { action } while(0);                                                 \
		return TJS_S_OK;                                                        \
	}                                                                          \
	TJS_END_NATIVE_METHOD_DECL(name)

	WAVE_SIMPLE(/*func. name*/ play,
		if(_this->kind == WaveInstance::Kind::None) break;
		_this->Fading = false;
		if(_this->kind == WaveInstance::Kind::Chunk)
		{
			if(_this->Chunk == nullptr) break;
			// one music stream exists but channels are per-sound, so BGM and
			// sound effects can actually play at the same time
			if(_this->Channel >= 0 && Mix_Playing(_this->Channel))
				Mix_HaltChannel(_this->Channel);
			const int loops = _this->Looping ? -1 : 0;
			_this->Channel = Mix_PlayChannel(-1, _this->Chunk, loops);
			_this->NoteStarted();
		}
		else if(_this->kind == WaveInstance::Kind::Music)
		{
			if(_this->Music == nullptr) break;
			Mix_HaltMusic(); // the music stream is single; a new song takes it
			const int loops = _this->Looping ? -1 : 0;
			_this->MusicPlaying = Mix_PlayMusic(_this->Music, loops) == 0;
			if(_this->MusicPlaying) _this->NoteStarted();
			_this->ApplyVolume();
		})
	WAVE_SIMPLE(/*func. name*/ stop,
		_this->Fading = false;
		if(_this->kind == WaveInstance::Kind::Chunk)
		{
			if(_this->Channel >= 0)
			{
				Mix_HaltChannel(_this->Channel);
				_this->Channel = -1;
			}
		}
		else if(_this->kind == WaveInstance::Kind::Music && _this->MusicPlaying)
		{
			Mix_HaltMusic();
			_this->MusicPlaying = false;
		})
	WAVE_SIMPLE(/*func. name*/ pause,
		if(_this->kind == WaveInstance::Kind::Chunk)
		{
			if(_this->Channel >= 0) Mix_Pause(_this->Channel);
		}
		else if(_this->kind == WaveInstance::Kind::Music && _this->MusicPlaying)
		{
			Mix_PauseMusic();
		})
	WAVE_SIMPLE(/*func. name*/ resume,
		if(_this->kind == WaveInstance::Kind::Chunk)
		{
			if(_this->Channel >= 0) Mix_Resume(_this->Channel);
		}
		else if(_this->kind == WaveInstance::Kind::Music && _this->MusicPlaying)
		{
			Mix_ResumeMusic();
		})
	/* krkr2's fade(to, time, delay): the base class steps the volume itself;
	 * here the host's frame loop does the stepping (TVPDarwinTickSoundFades) */
	WAVE_SIMPLE(/*func. name*/ fade,
		_this->Owner = objthis;
		if(numparams < 2) return TJS_E_BADPARAMCOUNT;
		const tjs_int to = (tjs_int)(*param[0]);
		const tjs_int time = (tjs_int)(*param[1]);
		tjs_int delay = 0;
		if(numparams >= 3 && param[2]->Type() != tvtVoid) delay = (tjs_int)(*param[2]);
		_this->Fading = time > 0 || delay > 0;
		_this->FadeDelaying = delay > 0;
		_this->FadeTarget = to;
		_this->FadeFrom = _this->Volume;
		_this->FadeStart = SDL_GetTicks64() + (Uint64)(delay < 0 ? 0 : delay);
		_this->FadeMs = (Uint64)(time < 0 ? 0 : time);
		if(_this->Fading)
		{
			auto& registry = FadeRegistry();
			if(std::find(registry.begin(), registry.end(), _this) == registry.end())
				registry.push_back(_this);
		}
		else
		{
			_this->Volume = to;
			_this->ApplyVolume();
		}
		)
	WAVE_SIMPLE(/*func. name*/ stopFade,
		const bool was = _this->Fading;
		_this->Fading = false;
		if(was)
		{
			// upstream's StopFade(settargetvol=true) lands on the target
			_this->Volume = _this->FadeTarget;
			_this->ApplyVolume();
			PostFadeCompleted(_this);
		})
#undef WAVE_SIMPLE

#define WAVE_INT_PROP(name, field)                                             \
	TJS_BEGIN_NATIVE_PROP_DECL(name)                                           \
	{                                                                          \
		TJS_BEGIN_NATIVE_PROP_GETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);                      \
			*result = (tjs_int)_this->field;                                    \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_GETTER                                             \
		TJS_BEGIN_NATIVE_PROP_SETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);                      \
			_this->field = (tjs_int)*param;                                     \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_SETTER                                             \
	}                                                                          \
	TJS_END_NATIVE_PROP_DECL(name)
	// volume is per buffer: a sound effect must not steal the BGM's level
	TJS_BEGIN_NATIVE_PROP_DECL(volume)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_int)_this->Volume;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			_this->Volume = (tjs_int)*param;
			_this->ApplyVolume();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(volume)
	WAVE_INT_PROP(pan, Pan)
#undef WAVE_INT_PROP

	/* 0 stopped, 1 playing, 2 paused - read from the mixer, so a naturally
	 * finished sound effect reports stop and a [ws] wait can end */
	TJS_BEGIN_NATIVE_PROP_DECL(status)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			tjs_int st = 0;
			if(_this->IsPaused()) st = 2;
			else if(_this->IsPlaying()) st = 1;
			if(_this->kind == WaveInstance::Kind::Chunk && st == 0 && _this->Channel >= 0)
				_this->Channel = -1; // the channel ended; let the next play take it
			*result = st;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			return TJS_E_NOTIMPL;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(status)

	TJS_BEGIN_NATIVE_PROP_DECL(playing)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_int)(_this->IsPlaying() ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(playing)

	TJS_BEGIN_NATIVE_PROP_DECL(paused)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_int)(_this->IsPaused() ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			if(_this->kind == WaveInstance::Kind::Chunk)
			{
				if(*param) { if(_this->Channel >= 0) Mix_Pause(_this->Channel); }
				else { if(_this->Channel >= 0) Mix_Resume(_this->Channel); }
				_this->NotePaused((tjs_int)*param != 0);
			}
			else if(_this->kind == WaveInstance::Kind::Music && _this->MusicPlaying)
			{
				if(*param) Mix_PauseMusic();
				else Mix_ResumeMusic();
				_this->NotePaused((tjs_int)*param != 0);
			}
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(paused)

	TJS_BEGIN_NATIVE_PROP_DECL(looping)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_int)(_this->Looping ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			_this->Looping = (tjs_int)*param != 0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(looping)

	// Position in seconds.  SDL_mixer reports it for a music stream; a chunk has
	// no such query, so the elapsed time since it started is used, which is what
	// a script showing a seek bar needs and is exact enough at that granularity.
	TJS_BEGIN_NATIVE_PROP_DECL(position)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_real)_this->CurrentPosition();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(position)

	TJS_BEGIN_NATIVE_PROP_DECL(totalTime)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, WaveInstance);
			*result = (tjs_real)_this->TotalTime;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(totalTime)

	TJS_END_NATIVE_MEMBERS
}

iTJSNativeInstance* tTJSNC_WaveSoundBuffer_Darwin::CreateNativeInstance()
{
	return new WaveInstance();
}

tTJSNativeClass* TVPCreateNativeClass_WaveSoundBuffer()
{
	return new tTJSNC_WaveSoundBuffer_Darwin();
}

/* the host's frame loop calls this once per frame; fades step here so the
 * volume calls and the completion event stay on the main thread */
void TVPDarwinTickSoundFades()
{
	auto& registry = FadeRegistry();
	const Uint64 now = SDL_GetTicks64();
	for(WaveInstance* inst : registry)
	{
		if(!inst->Fading)
			continue;
		if(now < inst->FadeStart)
			continue; // still inside the delay
		if(inst->FadeMs == 0)
		{
			inst->Fading = false;
			inst->Volume = inst->FadeTarget;
			inst->ApplyVolume();
			PostFadeCompleted(inst);
			continue;
		}
		const Uint64 elapsed = now - inst->FadeStart;
		const double t = elapsed >= inst->FadeMs ? 1.0
			: (double)elapsed / (double)inst->FadeMs;
		const tjs_int v = inst->FadeFrom +
			(tjs_int)((inst->FadeTarget - inst->FadeFrom) * t);
		if(v != inst->Volume)
		{
			inst->Volume = v;
			inst->ApplyVolume();
		}
		if(t >= 1.0)
		{
			inst->Fading = false;
			inst->Volume = inst->FadeTarget;
			inst->ApplyVolume();
			PostFadeCompleted(inst);
		}
	}
	// drop finished entries lazily; the vector is tiny
	registry.erase(std::remove_if(registry.begin(), registry.end(),
		[](WaveInstance* i) { return !i->Fading; }), registry.end());
}
