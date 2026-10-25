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
#include <vector>
#include <thread>
#include <mutex>
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

#else   // !MACOSX: this TU is only added on Darwin (see CMakeLists.txt)
int avfPlayFrames(const char *, int, int, const void *){ return 0; }
int  avfStartAsync(const char *, int){ return -1; }
void avfStopAsync(void){}
int  avfIsAsyncPlaying(void){ return 0; }
int  avfTickAsync(const void *){ return 0; }
#endif  // MACOSX
