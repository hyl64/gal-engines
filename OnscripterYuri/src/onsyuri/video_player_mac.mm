/* -*- ObjC++ -*-
 *
 *  video_player_mac.mm - AVFoundation decoder half of the macOS movie
 *  backend for OnscripterYuri (task T1).
 *
 *  This TU decodes and delivers frames; it never includes ONScripter.h.
 *  Presentation (surface blit, texture upload, renderer present) and the
 *  SDL event pump live in video_player_bridge.cpp, because AVFoundation's
 *  umbrella header drags in Quickdraw.h's `struct FontInfo`, which cannot
 *  coexist with the engine's `class FontInfo` in one translation unit.
 *  video_player_frame.h is the seam; its comment has the full story.
 *
 *  Video path: AVURLAsset -> AVAssetReader -> AVAssetReaderTrackOutput
 *  (kCVPixelFormatType_32BGRA) -> CVPixelBufferLockBaseAddress -> host->
 *  frame_cb (the bridge copies/scales it into accumulation_surface and
 *  presents through the engine renderer, exactly like the USE_SMPEG
 *  branch does). NO separate window is ever created.
 *
 *  Audio path (best effort): the audio mix is decoded up-front via
 *  AVAssetReaderAudioMixOutput to interleaved f32 44.1k stereo and played
 *  through a dedicated SDL_OpenAudioDevice stream, so the engine's
 *  Mix_OpenAudio state (BGM/SFX) is untouched. Any failure => silent
 *  playback, never a video failure.
 *  TODO(t1-audio-followup): whole-track pre-decode is fine for OP-length
 *  movies per contract; very long videos should stream blocks instead
 *  (~700 KB PCM per minute at f32/stereo/44.1k).
 *
 *  Test hook (env-gated, production-neutral):
 *    ONS_VIDEO_CLICK_AFTER=<ms> inject a left-click after N ms (CI can
 *                               exercise the click_stop interrupt)
 *
 *  Copyright (c) 2026 gal-engines project (task T1).
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */
#ifdef MACOSX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <ctype.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>

#include <SDL2/SDL.h>

/* Framework-only TU -- do NOT include ONScripter.h here.
 *
 * AVFoundation's umbrella header reaches ApplicationServices -> Quickdraw.h,
 * which defines `struct FontInfo` unconditionally.  The engine's FontInfo.h
 * defines `class FontInfo`.  Same TU => redefinition error, and suppressing
 * Quickdraw breaks HIServices.  So presentation and event handling moved to
 * video_player_bridge.cpp (engine TU, no frameworks) and the two halves talk
 * through video_player_frame.h.  See that file's comment for the long story. */
#include "video_player_frame.h"
#include "Utils.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

/* ------------------------------------------------------------------ */
/* Optional libmpv fallback (task-4): extern weak declaration + empty  */
/* in-TU definition. Chosen over dlsym per lead's decision 2026-10-25: */
/* fewer runtime failure modes; zero compile-time dependency on T4.    */
/* ------------------------------------------------------------------ */
extern "C" bool mpvPlayBlocking(const char *abs_path, bool click_stop, bool loop);

/* The mpv fallback and the videoPlayBlocking/videoMapLookup entry points live
 * in video_player_bridge.cpp (engine TU).  This TU decodes only. */

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */
static std::string normalizeSepLower(const char *s)
{
    std::string out;
    for (const char *p = s; *p; p++){
        char c = *p;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out += c;
    }
    return out;
}

static std::string normRoot(const char *root)
{
    std::string r = normalizeSepLower(root);
    if (!r.empty() && r[r.size()-1] != '/') r += '/';
    return r;
}

static std::string stripRoot(const std::string &root_norm, const std::string &p_norm)
{
    if (p_norm.size() >= root_norm.size() &&
        p_norm.compare(0, root_norm.size(), root_norm) == 0)
        return p_norm.substr(root_norm.size());
    return p_norm;
}

static std::string joinRoot(const char *root, const char *rel)
{
    std::string r(root);
    if (!r.empty() && r[r.size()-1] != '/')
        r += '/';
    r += rel;
    return r;
}

/* ------------------------------------------------------------------ */
/* videoMapLookup -- game_root/_video/video_map.json sidecar           */
/* ------------------------------------------------------------------ */
/* Contract JSON (written by task-2 T2):
 * { "version": 1,
 *   "mappings": [ { "from": "af.mpg", "to": "_video/af.mp4",
 *                   "codec": "h264", "source": "loose" }, ... ] }
 * The engine links no JSON library, so a tolerant scanner pulls the
 * "from"/"to" string pairs in document order. Malformed or missing
 * fields are skipped -- contract: "找不到文件/字段就忽略该条" --
 * never fatal. Case-insensitive, '/' and '\\' equivalent. */

static bool parseJsonString(const char *&p, const char *end, std::string &out)
{
    while (p < end && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) p++;
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end && *p != '"'){
        if (*p == '\\' && p + 1 < end){
            char esc = p[1];
            switch (esc){
              case 'n': out += '\n'; break;
              case 'r': out += '\r'; break;
              case 't': out += '\t'; break;
              default:  out += esc;  break;
            }
            p += 2;
            continue;
        }
        out += *p;
        p++;
    }
    if (p >= end) return false;
    p++;                       // closing quote
    return true;
}

/* Fetch the next from/to pair. Returns false at end-of-document or when
 * malformed (the caller's loop then stops scanning further entries). */
static bool nextFromTo(const char *&p, const char *end,
                       std::string &from, std::string &to)
{
    const char *f = strstr(p, "\"from\"");
    if (!f) return false;
    f += 6;
    std::string fv;
    if (!parseJsonString(f, end, fv)) return false;
    const char *t = strstr(f, "\"to\"");
    const char *f2 = strstr(f, "\"from\"");
    if (!t || (f2 && f2 < t)) return false;
    t += 4;
    std::string tv;
    if (!parseJsonString(t, end, tv)) return false;
    from = fv;
    to = tv;
    p = t;
    return true;
}

static bool videoMapLookupRaw(const char *game_root, const char *script_path,
                              std::string &out_abs)
{
    if (!game_root || !script_path || !script_path[0]) return false;

    const std::string mapfile = joinRoot(game_root, "_video/video_map.json");
    FILE *fp = fopen(mapfile.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024){ fclose(fp); return false; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf){ fclose(fp); return false; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    const char *end = buf + got;
    const std::string root_norm = normRoot(game_root);
    const std::string want = stripRoot(root_norm, normalizeSepLower(script_path));

    bool found = false;
    const char *p = buf;
    std::string from, to;
    while (!found && nextFromTo(p, end, from, to)){
        const std::string from_norm = normalizeSepLower(from.c_str());
        if (from_norm == want || stripRoot(root_norm, from_norm) == want){
            // contract: "to" is relative to game_root; a hit is only valid
            // when the transcoded artifact actually exists on disk.
            std::string abs = joinRoot(game_root, to.c_str());
            FILE *tf = fopen(abs.c_str(), "rb");
            if (tf){
                fclose(tf);
                out_abs = abs;
                found = true;
            }
            else {
                utils::printInfo("video_map: [%s] -> [%s] ignored, target missing\n",
                                 from.c_str(), to.c_str());
            }
        }
    }
    free(buf);
    return found;
}

extern "C" char *videoMapLookup(const char *game_root, const char *script_path)
{
    std::string abs;
    if (!videoMapLookupRaw(game_root, script_path, abs)) return NULL;
    char *ret = (char *)malloc(abs.size() + 1);
    if (ret) memcpy(ret, abs.c_str(), abs.size() + 1);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Audio: whole-track f32 decode -> dedicated SDL audio stream         */
/* ------------------------------------------------------------------ */
class VideoAudio {
public:
    bool  active = false;
    SDL_AudioDeviceID dev = 0;
    std::vector<float> samples;   // interleaved stereo f32
    size_t play_frames = 0;       // frames consumed by the callback
    int channels = 2;
    int rate = 44100;
    SDL_mutex *mtx = NULL;

    bool decodeTrack(AVAsset *asset)
    {
        AVAssetTrack *atrack = nil;
        for (AVAssetTrack *t in [asset tracksWithMediaType:AVMediaTypeAudio]){
            if (!atrack) atrack = t;      // first audio track
        }
        if (!atrack) return false;

        AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:nil];
        if (!r) return false;
        r.timeRange = CMTimeRangeMake(CMTimeMake(0, 1), asset.duration);

        // NOT compiled with ARC (default for .mm without -fobjc-arc): the
        // alloc/init result below is autoreleased; do not send -release.
        AVAudioFormat *fmt = [[AVAudioFormat alloc]
                                initWithCommonFormat:AVAudioPCMFormatFloat32
                                          sampleRate:44100
                                            channels:2
                                         interleaved:YES];
        if (!fmt) return false;

        AVAssetReaderAudioMixOutput *out =
            [AVAssetReaderAudioMixOutput
                assetReaderAudioMixOutputWithAudioTracks:@[atrack]
                                           audioSettings:fmt.settings];
        if (!out || ![r canAddOutput:out]) return false;
        [r addOutput:out];
        if (![r startReading]) return false;

        double dur = CMTimeGetSeconds(asset.duration);
        if (dur > 0 && dur < 3600.0)
            samples.reserve((size_t)(dur * 44100.0 * 2.0) + 1024);

        while (r.status == AVAssetReaderStatusReading){
            CMSampleBufferRef sb = [out copyNextSampleBuffer];
            if (!sb) break;
            // Two-pass call: ask for the required AudioBufferList size first,
            // then hand it a buffer of exactly that size.  (The one-pass
            // variant with a NULL list does not fill anything in.)
            size_t needed = 0;
            OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                sb, &needed, NULL, 0, kCFAllocatorDefault, kCFAllocatorDefault,
                0, NULL);
            if (st == noErr && needed > 0){
                AudioBufferList *abl = (AudioBufferList *)calloc(1, needed);
                CMBlockBufferRef bb = NULL;
                if (abl){
                    st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                        sb, NULL, abl, needed, kCFAllocatorDefault,
                        kCFAllocatorDefault, 0, &bb);
                    if (st == noErr){
                        for (UInt32 i = 0; i < abl->mNumberBuffers; i++){
                            const float *f = (const float *)abl->mBuffers[i].mData;
                            size_t n = abl->mBuffers[i].mDataByteSize / sizeof(float);
                            if (f && n) samples.insert(samples.end(), f, f + n);
                        }
                    }
                    free(abl);
                }
                if (bb) CFRelease(bb);
            }
            CFRelease(sb);
        }

        if (r.status != AVAssetReaderStatusCompleted || samples.empty()){
            samples.clear();
            return false;
        }

        // "Decoded a track" and "decoded actual sound" are different claims.
        // Report the peak so a silent-but-present track cannot masquerade as
        // working audio (and so a mute fixture is obvious in the log).
        {
            float peak = 0.0f;
            double sumsq = 0.0;
            for (size_t i = 0; i < samples.size(); i++){
                float v = samples[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
                sumsq += (double)samples[i] * samples[i];
            }
            const double rms = samples.empty()
                ? 0.0 : sqrt(sumsq / (double)samples.size());
            utils::printInfo("video: audio decoded %.2fs, %zu samples, peak=%.3f rms=%.4f%s\n",
                             (double)samples.size() / (double)(channels * rate),
                             samples.size(), peak, rms,
                             peak < 0.0005f ? " (SILENT TRACK)" : "");
        }
        return openStream();
    }

    bool openStream()
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = rate;
        want.format = AUDIO_F32SYS;
        want.channels = (Uint8)channels;
        want.samples = 2048;
        want.callback = &VideoAudio::sCallback;
        want.userdata = this;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                  SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        if (dev == 0){
            utils::printInfo("video: audio stream open failed (%s) -- playing silent\n",
                             SDL_GetError());
            samples.clear();
            return false;
        }
        rate = have.freq;
        channels = have.channels;
        mtx = SDL_CreateMutex();
        active = true;
        SDL_PauseAudioDevice(dev, 0);
        return true;
    }

    static void sCallback(void *userdata, Uint8 *stream, int len)
    {
        VideoAudio *va = (VideoAudio*)userdata;
        if (!va->mtx) return;
        SDL_LockMutex(va->mtx);
        const size_t frame_bytes = sizeof(float) * (size_t)va->channels;
        if (frame_bytes == 0){ SDL_UnlockMutex(va->mtx); return; }
        size_t avail_frames = va->samples.size() / (size_t)va->channels;
        size_t want_frames = (size_t)len / frame_bytes;
        size_t remain = (va->play_frames < avail_frames)
                          ? (avail_frames - va->play_frames) : 0;
        size_t take = (want_frames < remain) ? want_frames : remain;
        float *dst = (float*)stream;
        if (take)
            memcpy(dst, va->samples.data() + va->play_frames * (size_t)va->channels,
                   take * frame_bytes);
        va->play_frames += take;
        if (take < want_frames)
            memset(dst + take * (size_t)va->channels, 0,
                   (want_frames - take) * frame_bytes);
        SDL_UnlockMutex(va->mtx);
    }

    double playedSeconds()
    {
        if (!active || !mtx) return -1.0;
        SDL_LockMutex(mtx);
        double s = (double)play_frames / (double)rate;
        SDL_UnlockMutex(mtx);
        return s;
    }

    void resetPosition()
    {
        if (!active || !mtx) return;
        SDL_LockMutex(mtx);
        play_frames = 0;
        SDL_UnlockMutex(mtx);
    }

    void stop()
    {
        if (dev){
            SDL_PauseAudioDevice(dev, 1);
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        active = false;
        if (mtx){ SDL_DestroyMutex(mtx); mtx = NULL; }
        samples.clear();
        samples.shrink_to_fit();
    }
};

/* ------------------------------------------------------------------ */
/* Frame delivery: hand the decoded buffer to the engine via video_host */
/* Presentation (surface blit, texture upload, renderer present) lives  */
/* in video_player_bridge.cpp, which is the only TU allowed to know     */
/* about ONScripter's private members.                                  */
/* ------------------------------------------------------------------ */
static void deliverFrame(const video_host *host,
                         const uint8_t *bgra, size_t w, size_t h, size_t src_pitch)
{
    if (!host || !host->frame_cb || !bgra) return;
    host->frame_cb(host->user, bgra, (int)w, (int)h, (int)src_pitch,
                   VIDEO_PIX_BGRA);
}

static bool pollAbort(const video_host *host)
{
    return host && host->abort_cb && host->abort_cb(host->user);
}

/* ------------------------------------------------------------------ */
/* Test hooks (env-gated; no effect in production runs)                */
/*   ONS_VIDEO_CLICK_AFTER=<ms>  inject a left click after N ms        */
/* ------------------------------------------------------------------ */
static Uint32 s_click_after_ms = 0;
static bool   s_click_inited = false;
static bool   s_click_pushed = false;

static void maybePushSyntheticClick(Uint32 play_started)
{
    if (!s_click_inited){
        s_click_inited = true;
        const char *e = getenv("ONS_VIDEO_CLICK_AFTER");
        if (e && *e) s_click_after_ms = (Uint32)atoi(e);
    }
    if (s_click_after_ms && !s_click_pushed &&
        SDL_GetTicks() - play_started >= s_click_after_ms){
        s_click_pushed = true;
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = SDL_PRESSED;
        ev.button.x = ev.button.y = 8;
        SDL_PushEvent(&ev);
        ev.type = SDL_MOUSEBUTTONUP;
        ev.button.state = SDL_RELEASED;
        SDL_PushEvent(&ev);
        utils::printInfo("video: test hook injected left click after %ums\n",
                         s_click_after_ms);
    }
}

/* ------------------------------------------------------------------ */
/* AVFoundation playback core                                           */
/* ------------------------------------------------------------------ */
int avfPlayFrames(const char *abs_path, int click_stop, int loop,
                  const video_host *host)
{
    bool played_any = false;
    long frames_presented = 0;
    const Uint32 play_t0 = SDL_GetTicks();

    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *nspath = [NSString stringWithUTF8String:abs_path];
        if (![fm fileExistsAtPath:nspath]){
            utils::printInfo("video: file not found: %s\n", abs_path);
            return false;
        }

        NSURL *url = [NSURL fileURLWithPath:nspath];
        AVURLAsset *asset =
            [AVURLAsset URLAssetWithURL:url
                              options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
        AVAssetTrack *vtrack = nil;
        for (AVAssetTrack *t in [asset tracksWithMediaType:AVMediaTypeVideo]){
            if (!vtrack) vtrack = t;
        }
        if (!vtrack || CMTIME_IS_INVALID(asset.duration) ||
            CMTimeGetSeconds(asset.duration) <= 0){
            utils::printInfo("video: no playable video track (%s)\n", abs_path);
            return false;
        }

        double dur_s = CMTimeGetSeconds(asset.duration);
        double fps = vtrack.nominalFrameRate;
        if (fps <= 0 || fps > 240) fps = 30.0;

        // Whole-track audio, best effort. Any failure => silent playback.
        // This happens BEFORE the frame clock starts because it is a blocking
        // pre-pass: for a long opening movie it is the difference between
        // "starts instantly" and "starts a second late", so it gets its own
        // timing line.
        VideoAudio audio;
        const Uint32 audio_t0 = SDL_GetTicks();
        const bool audio_ok = audio.decodeTrack(asset);
        const Uint32 audio_ms = SDL_GetTicks() - audio_t0;
        if (audio_ms > 20)
            utils::printInfo("video: audio pre-decode took %ums\n", audio_ms);
        utils::printInfo("video: playing %s (%.2fs @%.1ffps, audio=%s)\n",
                         abs_path, dur_s, fps, audio_ok ? "on" : "silent");

        NSDictionary *oopts = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{}
        };

        bool quit_requested = false;
        bool decode_failed  = false;

        do {
            AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
            if (!reader){ decode_failed = true; break; }
            reader.timeRange = CMTimeRangeMake(CMTimeMake(0, 1), asset.duration);

            AVAssetReaderTrackOutput *trackOut =
                [AVAssetReaderTrackOutput
                    assetReaderTrackOutputWithTrack:vtrack
                                     outputSettings:oopts];
            if (!trackOut || ![reader canAddOutput:trackOut]){ decode_failed = true; break; }
            [reader addOutput:trackOut];
            if (![reader startReading]){
                utils::printInfo("video: startReading failed (%s)\n", abs_path);
                decode_failed = true;
                break;
            }

            const Uint32 play_started = SDL_GetTicks();
            s_click_pushed = false;

            while (true){
                @autoreleasepool {
                    CMSampleBufferRef sb = [trackOut copyNextSampleBuffer];
                    if (!sb){
                        if (reader.status == AVAssetReaderStatusFailed)
                            decode_failed = true;
                        break;                       // this pass finished
                    }
                    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
                    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
                    if (!pb){ CFRelease(sb); continue; }

                    played_any = true;
                    frames_presented++;
                    const size_t w = CVPixelBufferGetWidth(pb);
                    const size_t h = CVPixelBufferGetHeight(pb);

                    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)
                            == kCVReturnSuccess){
                        const uint8_t *bgra =
                            (const uint8_t*)CVPixelBufferGetBaseAddress(pb);
                        if (bgra)
                            deliverFrame(host, bgra, w, h,
                                         CVPixelBufferGetBytesPerRow(pb));
                        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                    }
                    CFRelease(sb);

                    // Wall-clock pacing from the presentation timestamp.
                    // Behind schedule? Don't sleep -- present the next frame
                    // immediately (contract: soft-decode fallback may drop).
                    double target = CMTimeGetSeconds(pts);
                    Uint32 due = play_started + (Uint32)(target * 1000.0);
                    Uint32 now = SDL_GetTicks();
                    if ((int)(due - now) > 0)
                        SDL_Delay(due - now);

                    maybePushSyntheticClick(play_started);

                    if (pollAbort(host)){ quit_requested = true; break; }
                }
            }

            if (audio.active) audio.resetPosition();
        } while (loop && !quit_requested && !decode_failed);
        // NOTE(loop + click_stop=false): would never terminate by contract.
        // We play a single pass in that combination -- same documented
        // compromise as the mpv backend. See report to lead.

        audio.stop();

        if (played_any){
            // The honest performance number is dropped frames, not
            // "presented / wall": wall includes asset loading and the audio
            // pre-pass, which for a 3 s clip dwarfs the frames themselves.
            const Uint32 wall = SDL_GetTicks() - play_t0;
            const double wall_s = wall / 1000.0;
            const long expected = (long)(dur_s * fps + 0.5);
            const long dropped  = expected > frames_presented
                                    ? expected - frames_presented : 0;
            const double pct = expected > 0
                                ? 100.0 * frames_presented / expected : 100.0;
            utils::printInfo("video: %ld/%ld frames (%.0f%%), %ld dropped, %.2fs wall%s\n",
                             frames_presented, expected, pct, dropped, wall_s,
                             dropped > 0 ? " -- FRAMES DROPPED" : "");
        }

        if (decode_failed && !played_any){
            utils::printInfo("video: AVFoundation failed on %s\n", abs_path);
            return false;
        }
    }

    return played_any ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Asynchronous playback (movie ... async / movie stop)                */
/*                                                                     */
/* A worker thread decodes and publishes the newest frame into a slot  */
/* under a mutex; it never touches SDL.  The engine's main thread calls */
/* avfTickAsync() from its loop and does the rendering.  SDL's renderer */
/* is not safe off the main thread, so this split is not optional.      */
/* ------------------------------------------------------------------ */
struct AsyncSlot {
    std::mutex mtx;
    std::vector<uint8_t> frame;   // BGRA, pitch = w*4
    int w = 0, h = 0;
    uint64_t serial = 0;          // bumped when a new frame lands
    uint64_t consumed = 0;        // last serial the main thread drew
    bool playing = false;
    bool stop_requested = false;
    std::thread worker;
};

static AsyncSlot g_async;

/* A joinable std::thread destroyed at exit calls std::terminate, which shows up
 * as "Abort trap: 6" on the way out -- exactly what happened the first time an
 * async movie was still running when the script ended.  Join it explicitly. */
static void asyncAtExitCleanup(void)
{
    avfStopAsync();
}

/* Pull every video frame out of the asset and publish it, pacing with the
 * presentation timestamps.  Runs entirely on the worker thread. */
static void asyncDecodeLoop(NSString *nspath, bool loop)
{
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:nspath];
        AVURLAsset *asset =
            [AVURLAsset URLAssetWithURL:url
                               options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
        AVAssetTrack *vtrack = nil;
        for (AVAssetTrack *t in [asset tracksWithMediaType:AVMediaTypeVideo]){
            if (!vtrack) vtrack = t;
        }
        if (!vtrack || CMTIME_IS_INVALID(asset.duration)){
            utils::printInfo("video(async): no video track in %s\n",
                             [nspath UTF8String]);
            std::lock_guard<std::mutex> lk(g_async.mtx);
            g_async.playing = false;
            return;
        }

        NSDictionary *oopts = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{}
        };

        const Uint32 started = SDL_GetTicks();
        do {
            AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
            if (!reader) break;
            reader.timeRange = CMTimeRangeMake(CMTimeMake(0, 1), asset.duration);
            AVAssetReaderTrackOutput *trackOut =
                [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vtrack
                                                           outputSettings:oopts];
            if (!trackOut || ![reader canAddOutput:trackOut]) break;
            [reader addOutput:trackOut];
            if (![reader startReading]) break;

            while (true){
                {
                    std::lock_guard<std::mutex> lk(g_async.mtx);
                    if (g_async.stop_requested) return;
                }
                @autoreleasepool {
                    CMSampleBufferRef sb = [trackOut copyNextSampleBuffer];
                    if (!sb) break;
                    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
                    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
                    if (!pb){ CFRelease(sb); continue; }

                    const size_t w = CVPixelBufferGetWidth(pb);
                    const size_t h = CVPixelBufferGetHeight(pb);
                    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)
                            == kCVReturnSuccess){
                        const uint8_t *src = (const uint8_t*)CVPixelBufferGetBaseAddress(pb);
                        const size_t spitch = CVPixelBufferGetBytesPerRow(pb);
                        if (src){
                            std::lock_guard<std::mutex> lk(g_async.mtx);
                            const size_t need = w * h * 4;
                            if (g_async.frame.size() != need){
                                g_async.frame.resize(need);
                                g_async.w = (int)w;
                                g_async.h = (int)h;
                            }
                            // tighten the pitch: the consumer assumes w*4
                            for (size_t y = 0; y < h; y++)
                                memcpy(g_async.frame.data() + y * w * 4,
                                       src + y * spitch, w * 4);
                            g_async.serial++;
                        }
                        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                    }
                    CFRelease(sb);

                    // pace against the presentation timestamp; behind schedule
                    // means keep going (dropping frames is allowed async)
                    double target = CMTimeGetSeconds(pts);
                    Uint32 due = started + (Uint32)(target * 1000.0);
                    Uint32 now = SDL_GetTicks();
                    if ((int)(due - now) > 0) SDL_Delay(due - now);
                }
            }
        } while (loop && !g_async.stop_requested);

        std::lock_guard<std::mutex> lk(g_async.mtx);
        if (!loop) g_async.playing = false;   // single pass finished
    }
}

extern "C" int avfStartAsync(const char *abs_path, int loop)
{
    if (!abs_path || !abs_path[0]) return -1;
    avfStopAsync();                       // replace any current async movie

    NSString *nspath = [NSString stringWithUTF8String:abs_path];
    if (![[NSFileManager defaultManager] fileExistsAtPath:nspath]){
        utils::printInfo("video(async): file not found: %s\n", abs_path);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lk(g_async.mtx);
        g_async.playing = true;
        g_async.stop_requested = false;
        g_async.serial = 0;
        g_async.consumed = 0;
        g_async.frame.clear();
    }
    static bool atexit_registered = false;
    if (!atexit_registered){
        atexit_registered = true;
        atexit(asyncAtExitCleanup);   // never let a live thread reach exit()
    }

    g_async.worker = std::thread(asyncDecodeLoop, nspath, loop != 0);
    utils::printInfo("video(async): started %s (loop=%d)\n", abs_path, loop);
    return 0;
}

extern "C" void avfStopAsync(void)
{
    {
        std::lock_guard<std::mutex> lk(g_async.mtx);
        g_async.stop_requested = true;
        g_async.playing = false;
    }
    if (g_async.worker.joinable()) g_async.worker.join();
    std::lock_guard<std::mutex> lk(g_async.mtx);
    g_async.frame.clear();
    g_async.serial = 0;
    g_async.consumed = 0;
}

extern "C" int avfIsAsyncPlaying(void)
{
    std::lock_guard<std::mutex> lk(g_async.mtx);
    return g_async.playing ? 1 : 0;
}

/* Main thread only.  Copies the newest frame out under the lock, then calls
 * the host outside it so rendering never runs while holding the mutex. */
extern "C" int avfTickAsync(const video_host *host)
{
    if (!host || !host->frame_cb) return 0;

    std::vector<uint8_t> copy;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(g_async.mtx);
        if (g_async.serial == g_async.consumed || g_async.frame.empty()) return 0;
        copy = g_async.frame;             // small enough to copy per tick
        w = g_async.w;
        h = g_async.h;
        g_async.consumed = g_async.serial;
    }
    host->frame_cb(host->user, copy.data(), w, h, w * 4, VIDEO_PIX_BGRA);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Decoder side of the wcmpg layer plugin.                             */
/*                                                                     */
/* Shares the async player's mechanism: a worker thread decodes and     */
/* publishes the newest frame into a slot under a mutex, the main thread */
/* pulls it.  Differences from the async player:                        */
/*   - open() starts paused (the script decides when to play)           */
/*   - play()/pause() gate the decode loop                              */
/*   - no host callback: the Layer::refresh() blits the frame itself    */
/* ------------------------------------------------------------------ */
static unsigned crc32Of(const char *text)
{
    unsigned c = 0xffffffffu;
    for (const unsigned char *p = (const unsigned char*)text; p && *p; p++){
        c ^= *p;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Where an archive entry is staged for AVFoundation, which wants a URL.  The
 * name carries a hash so two entries that differ only in path do not collide. */
static std::string layerCachePath(const char *entry_name)
{
    const char *tmp = getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp";
    if (dir[dir.size()-1] != '/') dir += "/";
    dir += "onsyuri-movie";
    mkdir(dir.c_str(), 0700);
    std::string base;
    for (const char *p = entry_name; p && *p; p++)
        base += (isalnum((unsigned char)*p) ? *p : '_');
    if (base.size() > 80) base = base.substr(base.size() - 80);
    /* keep the real extension: AVFoundation picks the demuxer from it, and a
     * MPEG-1 program stream named .mp4 gets "no video track" */
    std::string ext = ".mpg";
    const char *dot = strrchr(entry_name, '.');
    if (dot && strlen(dot) <= 6) ext = dot;
    char suffix[64];
    snprintf(suffix, sizeof suffix, "_%08x", crc32Of(entry_name));
    return dir + "/" + base + suffix + ext;
}

struct LayerPlayer {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> frame;      // the newest decoded frame
    int w = 0, h = 0;
    uint64_t serial = 0;
    uint64_t consumed = 0;
    bool open = false;               // a worker exists / the file was accepted
    bool paused = true;              // the script drives this: open starts paused
    bool loop = false;               // setloop/N: read by the worker every pass
    bool stop_requested = false;
    bool eof = false;
    bool paused_restart = false;     // play() after EOF: start another pass
    std::thread worker;
};

static LayerPlayer g_layer;
static std::vector<uint8_t> g_layer_snapshot;   // main-thread copy

/* AVFoundation is only reliable with ONE asset reader alive at a time: the
 * title screen drives a movie layer while `mpegplay` may still be finishing
 * the previous one, and two readers on two threads crashed inside
 * -[AVAssetReaderOutput copyNextSampleBuffer].  Every reader in this file takes
 * this lock for its whole life. */
static std::mutex g_avfReaderMutex;

/* Everything AVFoundation touches lives on this one thread: the asset, the
 * reader and its output are created and released here and nowhere else.
 * (Flushing a decoder from another thread - what the first cut did when the
 * script sent setloop - is a SIGBUS inside FigMediaProcessorFlush, reproducibly.)
 * The loop flag and the pause flag are read under the mutex, so setloop /
 * pause / play never have to restart the reader. */
static void layerDecodeLoop(NSString *nspath)
{
    @autoreleasepool {
        std::unique_lock<std::mutex> readerLock(g_avfReaderMutex, std::defer_lock);
        NSURL *url = [NSURL fileURLWithPath:nspath];
        AVURLAsset *asset =
            [AVURLAsset URLAssetWithURL:url
                               options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
        AVAssetTrack *vtrack = nil;
        for (AVAssetTrack *t in [asset tracksWithMediaType:AVMediaTypeVideo]){
            if (!vtrack) vtrack = t;
        }
        if (!vtrack || CMTIME_IS_INVALID(asset.duration)){
            utils::printInfo("wcmpg: no video track in %s\n", [nspath UTF8String]);
            std::lock_guard<std::mutex> lk(g_layer.mtx);
            g_layer.open = false;
            g_layer.eof = true;
            g_layer.cv.notify_all();
            return;
        }

        NSDictionary *oopts = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{}
        };

        long last_reader_status = -1;
        while (true){
            @autoreleasepool {                     // one pass over the file
                {
                    std::unique_lock<std::mutex> lk(g_layer.mtx);
                    if (g_layer.stop_requested) return;
                }
                readerLock.lock();
                AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
                if (!reader){ readerLock.unlock(); break; }
                reader.timeRange = CMTimeRangeMake(CMTimeMake(0, 1), asset.duration);
                AVAssetReaderTrackOutput *trackOut =
                    [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vtrack
                                                               outputSettings:oopts];
                if (!trackOut || ![reader canAddOutput:trackOut]){ readerLock.unlock(); break; }
                [reader addOutput:trackOut];
                if (![reader startReading]){
                    /* A second startReading on a reader that a previous pass
                     * cancelled does not reliably work, and 'break' here used to
                     * end the whole worker -- which is exactly how a looping
                     * clip stopped after one pass.  Report it instead of leaving
                     * the trace silent, and let the loop retry with a fresh
                     * reader next time round. */
                    utils::printInfo("wcmpg: startReading failed on this pass "
                                     "(status %ld); %s\n", (long)[reader status],
                                     g_layer.loop ? "retrying" : "giving up");
                    readerLock.unlock();
                    if (!g_layer.loop) break;
                    SDL_Delay(50);
                    continue;
                }

                const double t0 = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
                bool eof = false;
                while (!eof){
                    {
                        std::unique_lock<std::mutex> lk(g_layer.mtx);
                        if (g_layer.stop_requested) return;   // readerLock releases
                        /* wait while the script has the layer paused; `close`
                         * and `stop` wake us through the same variable */
                        while (g_layer.paused && !g_layer.stop_requested && g_layer.open)
                            g_layer.cv.wait_for(lk, std::chrono::milliseconds(50));
                        if (g_layer.stop_requested || !g_layer.open) return;
                    }
                    @autoreleasepool {
                        CMSampleBufferRef sb = [trackOut copyNextSampleBuffer];
                        if (!sb){ eof = true; break; }
                        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
                        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
                        if (pb &&
                            CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)
                                == kCVReturnSuccess){
                            const size_t w = CVPixelBufferGetWidth(pb);
                            const size_t h = CVPixelBufferGetHeight(pb);
                            const uint8_t *src = (const uint8_t*)CVPixelBufferGetBaseAddress(pb);
                            const size_t spitch = CVPixelBufferGetBytesPerRow(pb);
                            if (src){
                                std::lock_guard<std::mutex> lk(g_layer.mtx);
                                const size_t need = w * h * 4;
                                if (g_layer.frame.size() != need){
                                    g_layer.frame.resize(need);
                                    g_layer.w = (int)w;
                                    g_layer.h = (int)h;
                                }
                                for (size_t y = 0; y < h; y++)
                                    memcpy(g_layer.frame.data() + y * w * 4,
                                           src + y * spitch, w * 4);
                                g_layer.serial++;
                                {
                                    /* First publish only: report the actual pixel
                                     * bytes.  'black on screen' could be a decode
                                     * problem, a copy problem or a blit problem,
                                     * and this tells them apart in one line. */
                                    static int seen = 0;
                                    static int on = -1;
                                    if (on < 0){ const char *e = getenv("ONS_TRACE_LAYER_FRAME"); on = (e && *e) ? 1 : 0; }
                                    if (on && seen++ < 2){
                                        const uint8_t *q = g_layer.frame.data() + (h/2)*w*4 + (w/2)*4;
                                        FILE *df = fopen("ons_layercolor.log", "a");
                                        if (df){
                                            fprintf(df, "publish %zux%zu centre=%02x%02x%02x "
                                                        "serial=%llu consumed=%llu framesz=%zu\n",
                                                    w, h, q[0], q[1], q[2],
                                                    (unsigned long long)g_layer.serial,
                                                    (unsigned long long)g_layer.consumed,
                                                    g_layer.frame.size());
                                            fclose(df);
                                        }
                                    }
                                }
                            }
                            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                        }
                        CFRelease(sb);

                        /* pace by presentation time so the movie runs at its own
                         * speed without dropping everything at once */
                        const double target = CMTimeGetSeconds(pts);
                        const double elapsed =
                            CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())) - t0;
                        if (target > elapsed){
                            double wait = (target - elapsed) * 1000.0;
                            if (wait > 200.0) wait = 200.0;   // a gap in the time line
                            SDL_Delay((Uint32)wait);
                        }
                    }
                }
                last_reader_status = (long)[reader status];
                if ([reader status] == AVAssetReaderStatusReading)
                    [reader cancelReading];   // only a reader that is still reading
                /* VideoToolbox delivers decoded frames from its own XPC queue, after
                 * cancelReading returns.  Releasing the reader and the asset while
                 * that queue is still emitting crashed inside
                 * vtDecompressionDuctEmitDecodedFrame (signal 11, reproducible within
                 * a minute of playback).  Wait for a terminal status and let the
                 * queue drain before anything is released. */
                for (int spin = 0; spin < 100; spin++){
                    if ([reader status] != AVAssetReaderStatusReading) break;
                    [NSThread sleepForTimeInterval:0.005];
                }
                [NSThread sleepForTimeInterval:0.05];   // let the emit queue settle
                readerLock.unlock();
            }
            /* The pass is over.  Looping: go round again (the reader is built
             * anew inside the worker, never from another thread).  Not looping:
             * say so, and wait for a play() that asks for another pass or for a
             * close. */
            std::unique_lock<std::mutex> lk(g_layer.mtx);
            {
                /* One line per pass end.  'loop' says whether the flag survived
                 * to this point, 'status' what AVFoundation thought of the
                 * pass -- between them they explain why a looping clip stops. */
                static int lp = -1;
                if (lp < 0){ const char *e = getenv("ONS_TRACE_LOOP"); lp = (e && *e) ? 1 : 0; }
                if (lp){
                    static FILE *lf = NULL;
                    if (!lf) lf = fopen("ons_loop.log", "w");
                    if (lf){
                        fprintf(lf, "pass end loop=%d stop=%d open=%d status=%ld eof=%d "
                                    "serial=%llu consumed=%llu\n",
                                (int)g_layer.loop, (int)g_layer.stop_requested,
                                (int)g_layer.open, (long)last_reader_status,
                                (int)g_layer.eof,
                                (unsigned long long)g_layer.serial,
                                (unsigned long long)g_layer.consumed);
                        fflush(lf);
                    }
                }
            }
            if (g_layer.stop_requested) return;
            if (g_layer.loop) continue;
            g_layer.eof = true;
            g_layer.paused = true;
            g_layer.cv.notify_all();
            while (!g_layer.stop_requested && !g_layer.paused_restart)
                g_layer.cv.wait_for(lk, std::chrono::milliseconds(50));
            if (g_layer.stop_requested) return;
            g_layer.paused_restart = false;
            g_layer.eof = false;
            lk.unlock();
        }
    }
}

extern "C" int avfLayerOpen(const char *abs_path, int loop)
{
    if (!abs_path || !abs_path[0]) return -1;
    {
        static int st = -1, n = 0;
        if (st < 0){ const char *e = getenv("ONS_TRACE_LAYER_FRAME"); st = (e && *e) ? 1 : 0; }
        if (st){
            n++;
            FILE *f = fopen("ons_layerframe.log", "a");
            if (f){ fprintf(f, "avfLayerOpen#%d path=%s loop=%d\n", n, abs_path, loop); fclose(f); }
        }
    }
    avfLayerClose();

    NSString *nspath = [NSString stringWithUTF8String:abs_path];
    if (![[NSFileManager defaultManager] fileExistsAtPath:nspath]){
        utils::printInfo("wcmpg: file not found: %s\n", abs_path);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lk(g_layer.mtx);
        g_layer.open = true;
        g_layer.paused = true;          /* open() leaves it paused */
        g_layer.loop = loop != 0;
        g_layer.stop_requested = false;
        g_layer.eof = false;
        g_layer.serial = 0;
        g_layer.consumed = 0;
        g_layer.paused_restart = false;
        g_layer.frame.clear();
    }
    g_layer.worker = std::thread(layerDecodeLoop, nspath);
    return 0;
}

extern "C" int avfLayerOpenMemory(const char *entry_name, const void *data, int size, int loop)
{
    if (!data || size <= 0) return -1;
    std::string path = layerCachePath(entry_name ? entry_name : "entry");
    if (path.empty()) return -1;
    /* Always rewrite, atomically.  Reusing a file on a matching size was the likely
     * source of the intermittent black frames: a cache file left over from an
     * interrupted run can have the right length and wrong contents, and then every
     * later run decodes garbage.  Writing to a side file and renaming removes both
     * the truncation window and the stale-reuse case. */
    std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    if (!fp) return -1;
    size_t wrote = fwrite(data, 1, (size_t)size, fp);
    fclose(fp);
    if ((int)wrote != size) { remove(tmp.c_str()); return -1; }
    if (rename(tmp.c_str(), path.c_str()) != 0) { remove(tmp.c_str()); return -1; }
    return avfLayerOpen(path.c_str(), loop);
}

extern "C" int avfLayerPlay(void)
{
    std::lock_guard<std::mutex> lk(g_layer.mtx);
    if (!g_layer.open) return 0;
    if (g_layer.eof){
        /* the file was played to its end before: ask the worker for a new pass */
        g_layer.eof = false;
        g_layer.paused_restart = true;
    }
    g_layer.paused = false;
    g_layer.cv.notify_all();
    return 1;
}

extern "C" int avfLayerPause(int pause)
{
    std::lock_guard<std::mutex> lk(g_layer.mtx);
    g_layer.paused = pause ? true : false;
    g_layer.cv.notify_all();
    return 1;
}

extern "C" void avfLayerClose(void)
{
    {
        std::lock_guard<std::mutex> lk(g_layer.mtx);
        g_layer.stop_requested = true;
        g_layer.open = false;
        g_layer.cv.notify_all();
    }
    if (g_layer.worker.joinable()) g_layer.worker.join();
    std::lock_guard<std::mutex> lk(g_layer.mtx);
    g_layer.frame.clear();
    g_layer.serial = g_layer.consumed = 0;
    g_layer.w = g_layer.h = 0;
    g_layer.eof = false;
    g_layer.stop_requested = false;
}

/* Newest frame, or 0 when nothing new since the previous call. */
/* Peek: is there a frame the main thread has not drawn yet?  Used by play() to
 * wait briefly for the decoder's first frame, so the first refresh has something
 * to blit instead of depending on when the worker happened to finish. */
extern "C" int avfLayerHasFrame(void)
{
    std::lock_guard<std::mutex> lk(g_layer.mtx);
    return (!g_layer.frame.empty() && g_layer.serial != g_layer.consumed) ? 1 : 0;
}

extern "C" int avfLayerFrame(const unsigned char **px, int *w, int *h, int *pitch)
{
    std::lock_guard<std::mutex> lk(g_layer.mtx);
    if (!px || !w || !h || !pitch) return 0;
    if (g_layer.frame.empty() || g_layer.serial == g_layer.consumed) return 0;
    /* Hand out a copy, not a pointer into the worker's buffer.  The worker keeps
     * writing g_layer.frame while the renderer blits from it, and the vector can
     * reallocate -- that is a use-after-free, and it shows up as frames that are
     * torn, repeated or never change.  g_layer_snapshot is owned by this thread
     * and stays valid until the next call, which is the documented lifetime. */
    g_layer_snapshot = g_layer.frame;
    *px = g_layer_snapshot.data();
    *w = g_layer.w;
    *h = g_layer.h;
    *pitch = g_layer.w * 4;
    g_layer.consumed = g_layer.serial;
    return 1;
}

#else   // !MACOSX: this TU is only added on Darwin (see CMakeLists.txt)
int avfPlayFrames(const char *, int, int, const void *){ return 0; }
int  avfLayerOpen(const char *, int){ return -1; }
int  avfLayerPlay(void){ return 0; }
int  avfLayerPause(int){ return 0; }
void avfLayerClose(void){}
int  avfLayerFrame(const void *, int *, int *, int *){ return 0; }
int  avfStartAsync(const char *, int){ return -1; }
void avfStopAsync(void){}
int  avfIsAsyncPlaying(void){ return 0; }
int  avfTickAsync(const void *){ return 0; }
#endif  // MACOSX
