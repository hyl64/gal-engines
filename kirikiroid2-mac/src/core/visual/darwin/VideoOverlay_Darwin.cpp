/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * VideoOverlay_Darwin.cpp - the script-visible video overlay, on FFmpeg.
 *
 * KAG's Movie class derives from VideoOverlay and drives it with
 * open(storage) / play() / stop() / close() plus a rectangle and a loop flag.
 * The Windows original hands the movie to DirectShow in "mixer" mode; here the
 * overlay decodes with FFmpeg and paints the current frame straight into the
 * window's primary layer (KAG's "overlay" mode), stepped from the host frame
 * loop by TVPDarwinTickVideo().
 *
 * The storage is read through the engine, so a movie inside an XP3 archive
 * plays like a loose file.
 */

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjsVariant.h"
#include "StorageIntf.h"
#include "DrawDevice.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"
#include "MsgIntf.h"

#include <SDL.h>
#include <SDL2/SDL_timer.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#include <SDL2/SDL_mixer.h>

#include <cstring>
#include <vector>

extern iTVPDrawDevice *TVPGetLastDrawDeviceDarwin();

namespace
{
	// ---- reading the movie out of the engine's storage --------------------
	struct MemoryReader
	{
		std::vector<uint8_t> Bytes;
		size_t Pos = 0;
	};

	int ReadPacket(void *opaque, uint8_t *buf, int buf_size)
	{
		MemoryReader *r = (MemoryReader*)opaque;
		const size_t left = r->Bytes.size() - r->Pos;
		const size_t n = left < (size_t)buf_size ? left : (size_t)buf_size;
		if(n == 0) return AVERROR_EOF;
		memcpy(buf, r->Bytes.data() + r->Pos, n);
		r->Pos += n;
		return (int)n;
	}

	int64_t SeekPacket(void *opaque, int64_t offset, int whence)
	{
		MemoryReader *r = (MemoryReader*)opaque;
		int64_t base = 0;
		switch(whence & ~AVSEEK_FORCE)
		{
		case AVSEEK_SIZE: return (int64_t)r->Bytes.size();
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = (int64_t)r->Pos; break;
		case SEEK_END: base = (int64_t)r->Bytes.size(); break;
		default: return AVERROR(EINVAL);
		}
		int64_t target = base + offset;
		if(target < 0 || target > (int64_t)r->Bytes.size()) return AVERROR(EINVAL);
		r->Pos = (size_t)target;
		return target;
	}

	std::vector<class OverlayInstance*> gOverlays;

	class OverlayInstance : public tTJSNativeInstance
	{
	public:
		tjs_int Mode = 0;
		bool Visible = false;
		bool Looping = false;
		tjs_int Frame = 0;
		tjs_int Left = 0;
		tjs_int Top = 0;
		tjs_int Width = 0;
		tjs_int Height = 0;
		tjs_int AudioBalance = 0;
		tjs_int AudioVolume = 0;
		tjs_int EnabledAudioStream = 0;
		tjs_int NumberOfAudioStream = 0;
		double Position = 0.0;
		double TotalTime = 0.0;
		bool Playing = false;

		// ---- FFmpeg state
		MemoryReader Reader;
		AVIOContext *Io = nullptr;
		AVFormatContext *Format = nullptr;
		AVCodecContext *Decoder = nullptr;
		AVFrame *Decoded = nullptr;
		AVFrame *Rgba = nullptr;
		SwsContext *Scaler = nullptr;
		uint8_t *RgbaBuffer = nullptr;
		int StreamIndex = -1;
		int SrcW = 0, SrcH = 0;
		Uint64 StartTick = 0;     // when the current play started
		double PausedAt = 0.0;    // position while paused
		bool FrameReady = false;
		bool HasPending = false;
		double PendingTime = 0.0;
		tjs_int PacketsRead = 0;

		// the window this overlay draws on: held so the layer tree cannot be
		// collected underneath a playing movie
		tTJSVariantClosure WindowRef;

		// ---- the movie's audio track, decoded to the mixer's format
		std::vector<int16_t> Audio;   // 44100 Hz, stereo, signed 16
		int AudioChannel = -1;
		Mix_Chunk AudioChunk;

		// the texture the current frame is uploaded to
		tTVPBaseTexture *Surface = nullptr;
		int SurfaceW = 0, SurfaceH = 0;

		// output scaler, per instance
		SwsContext *OutScaler = nullptr;
		uint8_t *OutBuf = nullptr;
		int OutW = 0, OutH = 0;

		~OverlayInstance() override
		{
			// leave the frame-loop registry first: a tick must never reach a
			// freed overlay (that was a hard crash after a script dropped one)
			for(auto it = gOverlays.begin(); it != gOverlays.end(); ++it)
			{
				if(*it == this) { gOverlays.erase(it); break; }
			}
			Close();
			WindowRef.Release();
		}

		void Close()
		{
			Playing = false;
			FrameReady = false;
			Position = 0.0;
			TotalTime = 0.0;
			Visible = false;
			HaltAudio();
			Audio.clear();
			Audio.shrink_to_fit();
			delete Surface; Surface = nullptr; SurfaceW = SurfaceH = 0;
			if(OutScaler) { sws_freeContext(OutScaler); OutScaler = nullptr; }
			if(OutBuf) { av_freep(&OutBuf); }
			OutW = OutH = 0;
			if(Scaler) { sws_freeContext(Scaler); Scaler = nullptr; }
			if(Rgba) { av_frame_free(&Rgba); }
			if(Decoded) { av_frame_free(&Decoded); }
			if(RgbaBuffer) { av_freep(&RgbaBuffer); }
			if(Decoder) { avcodec_free_context(&Decoder); }
			if(Format) { avformat_close_input(&Format); }
			if(Io)
			{
				av_freep(&Io->buffer);
				avio_context_free(&Io);
				Io = nullptr;
			}
			Reader.Bytes.clear();
			Reader.Bytes.shrink_to_fit();
			StreamIndex = -1;
		}

		bool Open(const ttstr &storage)
		{
			Close();
			// pull the whole movie through the engine's storage layer
			tTJSBinaryStream *stream = TVPCreateStream(storage);
			if(stream == nullptr) return false;
			char buffer[0x8000];
			tjs_uint read;
			while((read = stream->Read(buffer, sizeof buffer)) > 0)
				Reader.Bytes.insert(Reader.Bytes.end(), buffer, buffer + read);
			delete stream;
			if(Reader.Bytes.empty()) return false;
			Reader.Pos = 0;

			unsigned char *iobuf = (unsigned char*)av_malloc(0x8000);
			Io = avio_alloc_context(iobuf, 0x8000, 0, &Reader,
				&ReadPacket, nullptr, &SeekPacket);
			if(Io == nullptr) return false;
			Format = avformat_alloc_context();
			if(Format == nullptr) return false;
			Format->pb = Io;

			if(avformat_open_input(&Format, storage.AsStdString().c_str(), nullptr, nullptr) < 0)
				return false;
			if(avformat_find_stream_info(Format, nullptr) < 0) return false;

			StreamIndex = av_find_best_stream(Format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
			if(StreamIndex < 0) return false;

			const AVCodec *codec = avcodec_find_decoder(
				Format->streams[StreamIndex]->codecpar->codec_id);
			if(codec == nullptr) return false;
			Decoder = avcodec_alloc_context3(codec);
			if(Decoder == nullptr) return false;
			if(avcodec_parameters_to_context(Decoder,
				Format->streams[StreamIndex]->codecpar) < 0) return false;
			if(avcodec_open2(Decoder, codec, nullptr) < 0) return false;

			Decoded = av_frame_alloc();
			Rgba = av_frame_alloc();
			if(Decoded == nullptr || Rgba == nullptr) return false;

			SrcW = Decoder->width;
			SrcH = Decoder->height;
			if(SrcW <= 0 || SrcH <= 0) return false;

			RgbaBuffer = (uint8_t*)av_malloc((size_t)av_image_get_buffer_size(
				AV_PIX_FMT_BGRA, SrcW, SrcH, 1) + 64);
			if(RgbaBuffer == nullptr) return false;
			av_image_fill_arrays(Rgba->data, Rgba->linesize, RgbaBuffer,
				AV_PIX_FMT_BGRA, SrcW, SrcH, 1);
			Rgba->width = SrcW;
			Rgba->height = SrcH;
			Rgba->format = AV_PIX_FMT_BGRA;

			Scaler = sws_getContext(SrcW, SrcH, Decoder->pix_fmt, SrcW, SrcH,
				AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);

			// the container's duration wins when it has one
			TotalTime = 0.0;
			if(Format->duration != AV_NOPTS_VALUE)
				TotalTime = (double)Format->duration / (double)AV_TIME_BASE;
			else if(Format->streams[StreamIndex]->duration != AV_NOPTS_VALUE)
				TotalTime = (double)Format->streams[StreamIndex]->duration
					* av_q2d(Format->streams[StreamIndex]->time_base);

			NumberOfAudioStream = 0;
			for(unsigned i = 0; i < Format->nb_streams; i++)
				if(Format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
					NumberOfAudioStream++;

			DecodeAudio();

			// first frame, so a paused movie shows something
			if(DecodeOne())
			{
				Convert();
				FrameReady = true;
				HasPending = false;
			}
			Visible = true;
			return true;
		}

		// ---- audio: decode the whole track to the mixer's own format --------
		void HaltAudio()
		{
			if(AudioChannel >= 0)
			{
				Mix_HaltChannel(AudioChannel);
				AudioChannel = -1;
			}
		}

		// the mixer is shared with the wave buffers; open it if a movie is the
		// first thing to make a sound
		static bool EnsureMixerOpen()
		{
			static bool opened = false;
			static bool ok = false;
			if(!opened)
			{
				opened = true;
				ok = Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0;
				if(ok) Mix_AllocateChannels(16);
			}
			return ok;
		}

		void StartAudio()
		{
			if(Audio.empty()) return;
			if(!EnsureMixerOpen()) return;
			HaltAudio();
			// SDL_mixer plays a Mix_Chunk that points at our PCM; `allocated` stays
			// 0 so freeing the chunk never touches the buffer we own
			AudioChunk.allocated = 0;
			AudioChunk.abuf = (Uint8*)Audio.data();
			AudioChunk.alen = (Uint32)(Audio.size() * sizeof(int16_t));
			AudioChunk.volume = MIX_MAX_VOLUME;
			AudioChannel = Mix_PlayChannel(-1, &AudioChunk, Looping ? -1 : 0);
			if(getenv("KRKR_DEBUG_VIDEO"))
			{
				fprintf(stderr, "[video] audio play: channel=%d bytes=%u mixer=%d\n",
					AudioChannel, (unsigned)AudioChunk.alen, (int)Mix_QuerySpec(nullptr, nullptr, nullptr));
				fflush(stderr);
			}
		}

		void DecodeAudio()
		{
			Audio.clear();
			const int stream = av_find_best_stream(Format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
			if(stream < 0) return;
			const AVCodec *codec = avcodec_find_decoder(Format->streams[stream]->codecpar->codec_id);
			if(codec == nullptr) return;
			AVCodecContext *dec = avcodec_alloc_context3(codec);
			if(dec == nullptr) return;
			if(avcodec_parameters_to_context(dec, Format->streams[stream]->codecpar) < 0)
			{
				avcodec_free_context(&dec);
				return;
			}
			if(avcodec_open2(dec, codec, nullptr) < 0)
			{
				avcodec_free_context(&dec);
				return;
			}

			AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
			SwrContext *swr = nullptr;
			if(swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, 44100,
				&dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr) < 0 || swr == nullptr)
			{
				avcodec_free_context(&dec);
				return;
			}
			if(swr_init(swr) < 0)
			{
				swr_free(&swr);
				avcodec_free_context(&dec);
				return;
			}

			// restart the demuxer: the video stream was already read to its end
			av_seek_frame(Format, StreamIndex, 0, AVSEEK_FLAG_BACKWARD);
			if(Decoder) avcodec_flush_buffers(Decoder);
			Reader.Pos = 0;
			HasPending = false;

			AVPacket *packet = av_packet_alloc();
			AVFrame *frame = av_frame_alloc();
			std::vector<int16_t> out(8192 * 2);
			while(packet && frame && av_read_frame(Format, packet) >= 0)
			{
				if(packet->stream_index == stream)
				{
					if(avcodec_send_packet(dec, packet) >= 0)
					{
						while(avcodec_receive_frame(dec, frame) >= 0)
						{
							const int maxOut = (int)(out.size() / 2);
							// swr_convert wants an array of plane pointers, not a
							// pointer to the sample buffer
							uint8_t *outPlane[1] = { (uint8_t*)out.data() };
							const int got = swr_convert(swr, outPlane, maxOut,
								(const uint8_t**)frame->extended_data, frame->nb_samples);
							if(got > 0)
								Audio.insert(Audio.end(), out.begin(), out.begin() + got * 2);
						}
					}
				}
				av_packet_unref(packet);
			}
			// flush the resampler
			uint8_t *flushPlane[1] = { (uint8_t*)out.data() };
			const int got = swr_convert(swr, flushPlane, (int)(out.size() / 2), nullptr, 0);
			if(got > 0)
				Audio.insert(Audio.end(), out.begin(), out.begin() + got * 2);

			if(packet) av_packet_free(&packet);
			if(frame) av_frame_free(&frame);
			swr_free(&swr);
			avcodec_free_context(&dec);

			if(getenv("KRKR_DEBUG_VIDEO"))
			{
				fprintf(stderr, "[video] audio: %d sample(s) of stereo s16 at 44100\n",
					(int)Audio.size());
				fflush(stderr);
			}
			// put the video back at the start for playback
			av_seek_frame(Format, StreamIndex, 0, AVSEEK_FLAG_BACKWARD);
			if(Decoder) avcodec_flush_buffers(Decoder);
			Reader.Pos = 0;
			HasPending = false;
		}

		void Rewind()
		{
			if(Format == nullptr) return;
			av_seek_frame(Format, StreamIndex, 0, AVSEEK_FLAG_BACKWARD);
			if(Decoder) avcodec_flush_buffers(Decoder);
			Reader.Pos = 0;
			StartTick = SDL_GetTicks64();
			Position = 0.0;
		}

		void Play()
		{
			if(Format == nullptr) return;
			if(!Playing)
			{
				StartTick = SDL_GetTicks64() - (Uint64)(PausedAt * 1000.0);
				Playing = true;
				Visible = true;
				if(AudioChannel >= 0) Mix_Resume(AudioChannel);
				else StartAudio();
			}
		}

		void Stop()
		{
			Playing = false;
			PausedAt = 0.0;
			Position = 0.0;
			HaltAudio();
		}

		void Pause()
		{
			if(Playing)
			{
				PausedAt = Position;
				Playing = false;
				if(AudioChannel >= 0) Mix_Pause(AudioChannel);
			}
		}

		// Decode until a frame is due, then show it.  A frame whose timestamp is
		// ahead of the wall clock is held in `Decoded` for a later tick, so the
		// movie plays at its own speed instead of running as fast as the loop.
		bool DecodeOne()
		{
			if(Format == nullptr || Decoder == nullptr) return false;
			AVPacket *packet = av_packet_alloc();
			if(packet == nullptr) return false;
			bool got = false;
			while(true)
			{
				int ret = av_read_frame(Format, packet);
				if(ret < 0)
				{
					if(Looping) { Rewind(); continue; }
					Playing = false;
					break;
				}
				PacketsRead++;
				if(packet->stream_index != StreamIndex)
				{
					av_packet_unref(packet);
					continue;
				}
				ret = avcodec_send_packet(Decoder, packet);
				av_packet_unref(packet);
				if(ret < 0) continue;
				ret = avcodec_receive_frame(Decoder, Decoded);
				if(ret == AVERROR(EAGAIN)) continue;
				if(ret < 0) break;
				const int64_t pts = Decoded->best_effort_timestamp;
				PendingTime = pts == AV_NOPTS_VALUE ? Position
					: (double)pts * av_q2d(Format->streams[StreamIndex]->time_base);
				HasPending = true;
				got = true;
				break;
			}
			av_packet_free(&packet);
			return got;
		}

		void Advance()
		{
			if(Format == nullptr) return;
			// a frame that has come due is shown now
			if(HasPending && PendingTime <= Position + 0.0005)
			{
				Convert();
				FrameReady = true;
				HasPending = false;
			}
			if(HasPending) return; // the next one is still in the future
			while(DecodeOne())
			{
				if(PendingTime <= Position + 0.0005)
				{
					Convert();
					FrameReady = true;
					HasPending = false;
					break;
				}
				return; // hold it until the clock catches up
			}
		}

		void Convert()
		{
			if(Scaler == nullptr || Decoded == nullptr) return;
			sws_scale(Scaler, Decoded->data, Decoded->linesize, 0, SrcH,
				Rgba->data, Rgba->linesize);
		}

		// paint the current frame into the window's primary layer
		void Paint()
		{
			if(!Visible || !FrameReady || Rgba == nullptr) return;
			if(WindowRef.Object == nullptr) return; // no window to draw on
			iTVPDrawDevice *device = TVPGetLastDrawDeviceDarwin();
			if(device == nullptr) return;
			tTJSNI_BaseLayer *primary = device->GetPrimaryLayer();
			if(primary == nullptr) return;
			tTVPBaseTexture *screen = primary->GetMainImage();
			if(screen == nullptr) return;

			const int sw = (int)screen->GetWidth();
			const int sh = (int)screen->GetHeight();
			if(sw <= 0 || sh <= 0) return;

			// the destination rectangle; the movie's own size when unset
			const int dw = Width > 0 ? (int)Width : SrcW;
			const int dh = Height > 0 ? (int)Height : SrcH;
			const int dx = (int)Left, dy = (int)Top;
			if(dw <= 0 || dh <= 0) return;

			if(OutScaler == nullptr || OutW != dw || OutH != dh)
			{
				if(OutScaler) sws_freeContext(OutScaler);
				if(OutBuf) av_freep(&OutBuf);
				OutScaler = sws_getContext(SrcW, SrcH, AV_PIX_FMT_BGRA, dw, dh,
					AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
				OutBuf = (uint8_t*)av_malloc((size_t)dw * dh * 4 + 64);
				OutW = dw;
				OutH = dh;
			}
			if(OutScaler == nullptr || OutBuf == nullptr) return;

			uint8_t *dst[4] = { OutBuf, nullptr, nullptr, nullptr };
			int dstLines[4] = { dw * 4, 0, 0, 0 };
			sws_scale(OutScaler, Rgba->data, Rgba->linesize, 0, SrcH, dst, dstLines);

			// The primary layer's image is texture-backed: writing through its
			// scan lines never reaches the texture the window shows.  Copy the
			// frame through the bitmap's own CopyRect, which goes through the
			// render manager the way image loading does.
			// Push the pixels into a texture the render manager owns, then copy
			// that texture onto the layer: writing a bitmap's scan lines would not
			// reach the texture the window shows.
			if(Surface == nullptr || SurfaceW != dw || SurfaceH != dh)
			{
				delete Surface;
				Surface = new tTVPBaseTexture(dw, dh, 32);
				SurfaceW = dw;
				SurfaceH = dh;
			}
			// Upload into the same texture object CopyRect will read: the base
			// texture, not the render-side view Update() would pick.
			Surface->GetTexture()->Update(OutBuf, TVPTextureFormat::RGBA,
				(unsigned)(dw * 4), tTVPRect(0, 0, dw, dh));
			tTVPRect srcrect(0, 0, dw, dh);
			primary->CopyRect(dx, dy, Surface, nullptr, srcrect);
			// the dirty region is in the layer's coordinates, not the source's
			tTVPRect dirty(dx, dy, dx + dw, dy + dh);
			primary->Update(dirty);
		}

		// advance by the wall clock; called once per host frame
		void Tick()
		{
			if(Format == nullptr) return;
			if(Playing)
			{
				Position = (double)(SDL_GetTicks64() - StartTick) / 1000.0;
				if(TotalTime > 0.0 && Position > TotalTime && !Looping)
				{
					Position = TotalTime;
					Playing = false;
				}
				Advance();
			}
			Paint();
		}
	};

}

class tTJSNC_VideoOverlay_Darwin : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_VideoOverlay_Darwin();
	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance* CreateNativeInstance() override;
};

tjs_uint32 tTJSNC_VideoOverlay_Darwin::ClassID = (tjs_uint32)-1;

tTJSNC_VideoOverlay_Darwin::tTJSNC_VideoOverlay_Darwin()
	: inherited(TJS_W("VideoOverlay"))
{
	TJS_BEGIN_NATIVE_MEMBERS(VideoOverlay)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
		/*var.type*/ OverlayInstance, /*TJS class name*/ VideoOverlay)
	{
		if(numparams >= 1)
			_this->WindowRef = param[0]->AsObjectClosure();
		gOverlays.push_back(_this);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ VideoOverlay)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ open)
	{
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
		if(numparams < 1) return TJS_E_BADPARAMCOUNT;
		if(!_this->Open(*param[0]))
		{
			// KAG catches this and reports the movie as unplayable
			TVPThrowExceptionMessage(TVPCannotOpenStorage, ttstr(*param[0]));
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ open)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ close)
	{
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
		_this->Close();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ close)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ play)
	{
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
		_this->Play();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ play)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ stop)
	{
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
		_this->Stop();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ stop)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ pause)
	{
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
		_this->Pause();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ pause)

#define OVERLAY_INT_PROP(name, field)                                          \
	TJS_BEGIN_NATIVE_PROP_DECL(name)                                           \
	{                                                                          \
		TJS_BEGIN_NATIVE_PROP_GETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);                    \
			*result = (tjs_int)_this->field;                                    \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_GETTER                                             \
		TJS_BEGIN_NATIVE_PROP_SETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);                    \
			_this->field = (tjs_int)*param;                                     \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_SETTER                                             \
	}                                                                          \
	TJS_END_NATIVE_PROP_DECL(name)

	OVERLAY_INT_PROP(mode, Mode)
	OVERLAY_INT_PROP(frame, Frame)
	OVERLAY_INT_PROP(left, Left)
	OVERLAY_INT_PROP(top, Top)
	OVERLAY_INT_PROP(width, Width)
	OVERLAY_INT_PROP(height, Height)
	OVERLAY_INT_PROP(audioBalance, AudioBalance)
	OVERLAY_INT_PROP(audioVolume, AudioVolume)
	OVERLAY_INT_PROP(enabledAudioStream, EnabledAudioStream)
#undef OVERLAY_INT_PROP

	TJS_BEGIN_NATIVE_PROP_DECL(visible)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)(_this->Visible ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			_this->Visible = (tjs_int)*param != 0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(visible)

	TJS_BEGIN_NATIVE_PROP_DECL(loop)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)(_this->Looping ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			_this->Looping = (tjs_int)*param != 0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(loop)

	TJS_BEGIN_NATIVE_PROP_DECL(numberOfAudioStream)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)_this->NumberOfAudioStream;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(numberOfAudioStream)

	TJS_BEGIN_NATIVE_PROP_DECL(position)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_real)_this->Position;
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
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_real)_this->TotalTime;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(totalTime)

	TJS_END_NATIVE_MEMBERS
}

iTJSNativeInstance* tTJSNC_VideoOverlay_Darwin::CreateNativeInstance()
{
	return new OverlayInstance();
}

tTJSNativeClass* TVPCreateNativeClass_VideoOverlay()
{
	return new tTJSNC_VideoOverlay_Darwin();
}

//---------------------------------------------------------------------------
// the host's frame loop calls this; every live overlay advances and paints here
//---------------------------------------------------------------------------
void TVPDarwinTickVideo()
{
	for(OverlayInstance *o : gOverlays)
		o->Tick();
}
