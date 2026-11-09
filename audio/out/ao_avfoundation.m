/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ao.h"
#include "audio/format.h"
#include "audio/out/ao_coreaudio_chmap.h"
#include "audio/out/ao_coreaudio_utils.h"
#include "common/common.h"
#include "common/msg.h"
#include "internal.h"
#include "osdep/timer.h"
#include "ta/ta_talloc.h"
#include <stdatomic.h>

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <CoreAudioTypes/CoreAudioTypes.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include "ao_avfoundation_eac3.h"
#include "audio/decode/afuse_apac_carrier.h"
#include "afuse_eac3_hls.h"


@interface AVObserver : NSObject {
    struct ao *ao;
    NSLock *lock;
}
- (void)handleRestartNotification:(NSNotification*)notification;
- (void)invalidate;
@end

struct priv {
    AVSampleBufferAudioRenderer *renderer;
    AVSampleBufferRenderSynchronizer *synchronizer;
    dispatch_queue_t queue;
    dispatch_source_t refill_timer;
    bool running, paused, requesting;
    CMAudioFormatDescriptionRef format_description;
    AVObserver *observer;
    int64_t end_time_av;
    id drain_observer;
    atomic_bool failed;
    bool encoded;
    bool apac;
    bool hls_mode, hls_draining;
    AFEAC3HLSBridge *hls;
    atomic_int_fast64_t hls_delay_ns;
    uint8_t pending[AF_APAC_BYTES];
    int pending_size;
    int64_t pending_time;
    _Atomic(const char *) failure_stage;
    atomic_int failure_status;
    bool clock_probe;
    int64_t clock_probe_time;
    uint64_t clock_probe_callbacks;
};

// A terminal AVFoundation failure is delivered through the client log queue.
// Afusekt then changes ao to the fallback driver on the mpv client thread. Do
// not repeatedly reload this same failed driver or destroy it from a callback.
static void request_fallback(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!atomic_exchange(&p->failed, true)) {
        NSError *error = p->hls_mode ? [p->hls error] : p->renderer.error;
        NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
        MP_ERR(ao, "%s: stage=%s; status=%d; %s (%s:%ld); underlying=%s (%s:%ld)\n",
               p->apac ? "AfuseAPACOutputFailure" : p->encoded ? "AfuseNativeAudioFailure" : "AfuseSpatialAudioFailure",
               p->failure_stage ?: "Apple audio renderer", (int)p->failure_status,
               error.localizedDescription.UTF8String ?: "no renderer error",
               error.domain.UTF8String ?: "none", (long)error.code,
               underlying.localizedDescription.UTF8String ?: "none",
               underlying.domain.UTF8String ?: "none", (long)underlying.code);
    }
}

static int64_t CMTimeGetNanoseconds(CMTime time)
{
    time = CMTimeConvertScale(time, 1000000000, kCMTimeRoundingMethod_Default);
    return time.value;
}

static CMTime CMTimeFromNanoseconds(int64_t time)
{
    return CMTimeMake(time, 1000000000);
}

#include "afuse_apac_output.h"

static bool enqueue_eac3(struct ao *ao, const uint8_t *bytes, int count, int64_t pts)
{
    struct priv *p = ao->priv;
    while (count > 0) {
        if (!p->pending_size) p->pending_time = pts;
        int copied = MPMIN(count, AF_EAC3_BURST_BYTES - p->pending_size);
        memcpy(p->pending + p->pending_size, bytes, copied);
        p->pending_size += copied; bytes += copied; count -= copied;
        pts += CMTimeGetNanoseconds(CMTimeMake(copied / 4, ao->samplerate));
        // Exact seeking may trim the carrier inside a burst. Discard only that
        // incomplete access unit and resynchronize; never decode IEC padding.
        int skip = 0;
        while (skip + 8 <= p->pending_size && !af_eac3_sync(p->pending + skip)) skip += 4;
        if (skip) {
            memmove(p->pending, p->pending + skip, p->pending_size - skip);
            p->pending_size -= skip;
            p->pending_time += CMTimeGetNanoseconds(CMTimeMake(skip / 4, ao->samplerate));
        }
        if (p->pending_size < AF_EAC3_BURST_BYTES) continue;
        size_t size = AV_RL16(p->pending + 6);
        if (size < 6 || size > AF_EAC3_BURST_BYTES - 8 || (size & 1)) {
            p->failure_stage = "invalid E-AC-3 IEC payload size";
            p->failure_status = (OSStatus)size;
            return false;
        }
        uint8_t *packet = CFAllocatorAllocate(NULL, size, 0);
        if (!packet) { p->failure_stage = "E-AC-3 packet allocation"; return false; }
        for (int i = 0; i < size; i += 2) {
            packet[i] = p->pending[i + 9]; packet[i + 1] = p->pending[i + 8];
        }
        p->pending_size = 0;
        if (!p->format_description) {
            p->failure_stage = "E-AC-3 format/dec3";
            p->format_description = af_eac3_format(packet, size, ao->samplerate / 4);
            if (p->format_description)
                MP_INFO(ao, "AfuseNativeEAC3: original compressed audio with dec3 metadata\n");
            // Explicit developer opt-in: keep only the failed access unit
            // (at most one IEC burst) for reproducing a format/parser failure.
            // Normal launches do not record media, including release builds.
            const char *probe = getenv("AFUSE_MPV_CAPTURE_EAC3");
            if (!p->format_description && probe && !strcmp(probe, "1")) {
                NSURL *directory = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory
                                                                         inDomains:NSUserDomainMask].firstObject;
                NSURL *destination = [directory URLByAppendingPathComponent:@"mpv-eac3-format-probe.ec3"];
                NSData *sample = [NSData dataWithBytes:packet length:size];
                if ([sample writeToURL:destination options:NSDataWritingAtomic error:nil])
                    [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0600}
                                                   ofItemAtPath:destination.path error:nil];
            }
        }
        if (p->hls_mode) {
            if (!p->hls && p->format_description) {
                p->hls = [[AFEAC3HLSBridge alloc] initWithFormat:p->format_description];
                [p->hls setPaused:p->paused];
                [p->hls setMuted:p->renderer.muted];
                [p->hls setVolume:p->renderer.volume];
                if (p->hls) MP_INFO(ao, "AfuseNativeEAC3HLS: original compressed E-AC-3/JOC via AVPlayer\n");
            }
            p->failure_stage = "E-AC-3 HLS remux/player";
            bool ok = p->hls && [p->hls appendPacket:packet length:size pts:p->pending_time];
            CFAllocatorDeallocate(NULL, packet);
            if (!ok) return false;
            p->failure_stage = NULL;
            continue;
        }
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        OSStatus err = p->format_description ? noErr : -1;
        if (err == noErr) {
            p->failure_stage = "E-AC-3 block buffer";
            err = CMBlockBufferCreateWithMemoryBlock(NULL, packet,
                size, NULL, NULL, 0, size, 0, &block);
        }
        if (block) packet = NULL;
        CMSampleTimingInfo timing = {
            .duration = CMTimeMake(1536, ao->samplerate / 4),
            .presentationTimeStamp = CMTimeFromNanoseconds(p->pending_time),
            .decodeTimeStamp = kCMTimeInvalid,
        };
        if (err == noErr) {
            p->failure_stage = "E-AC-3 sample buffer";
            err = CMSampleBufferCreateReady(NULL, block,
                p->format_description, 1, 1, &timing, 1, &size, &sample);
        }
        p->failure_status = err;
        if (err == noErr) p->failure_stage = NULL;
        if (sample) [p->renderer enqueueSampleBuffer:sample];
        if (sample) CFRelease(sample);
        if (block) CFRelease(block);
        if (packet) CFAllocatorDeallocate(NULL, packet);
        if (err != noErr) return false;
    }
    return true;
}

// The renderer can hold about two seconds after MPV has supplied its final
// packet. Signaling stop at input EOF makes the player flush those samples.
// Observe the actual presentation clock instead; pause/seek/uninit must also
// cancel the observer on this same serial queue before releasing the AO.
static void cancel_drain(struct priv *p)
{
    if (!p->drain_observer) return;
    [p->synchronizer removeTimeObserver:p->drain_observer];
    [p->drain_observer release];
    p->drain_observer = nil;
}

// The bridge is owned by the AO serial queue. Delay queries run under mpv's
// buffer lock: read an atomic snapshot to avoid inverting it with the feeder.
static int64_t presentation_time(struct priv *p)
{
    return p->hls_mode ? [p->hls currentTimeNanoseconds]
                       : CMTimeGetNanoseconds(p->synchronizer.currentTime);
}

static void update_hls_delay(struct priv *p)
{
    if (p->hls_mode)
        atomic_store(&p->hls_delay_ns, MPMAX(0, p->end_time_av - presentation_time(p)));
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;
    state->delay = p->hls_mode ? atomic_load(&p->hls_delay_ns) / 1e9 : -1;
}

static void feed(struct ao *ao);

// AVFoundation repeatedly calls a ready block even if the source has no data.
// Yield on a short queue-owned timer instead of spinning during decode/cache
// waits. Bound compressed read-ahead so it cannot drain the decoder instantly
// and report an underrun while Apple still has seconds of queued audio.
static void request_more(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->running || p->paused || p->requesting || atomic_load(&p->failed)) return;
    if (p->hls_mode) {
        @autoreleasepool { feed(ao); }
        return;
    }
    p->requesting = true;
    [p->renderer requestMediaDataWhenReadyOnQueue:p->queue usingBlock:^{
        @autoreleasepool { feed(ao); }
    }];
}

static void stop_requesting(struct priv *p)
{
    [p->renderer stopRequestingMediaData];
    p->requesting = false;
    dispatch_source_set_timer(p->refill_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
}

static void retry_refill(struct priv *p)
{
    stop_requesting(p);
    if (p->running && !p->paused)
        dispatch_source_set_timer(p->refill_timer, dispatch_time(DISPATCH_TIME_NOW, 5000000), DISPATCH_TIME_FOREVER, 1000000);
}

static void drain_at_eof(struct ao *ao)
{
    struct priv *p = ao->priv;
    stop_requesting(p);
    if (p->hls_mode) {
        [p->hls finish];
        p->hls_draining = true;
        retry_refill(p);
        return;
    }
    // A short final tail cannot satisfy a fresh preroll requirement. Let it
    // drain rather than waiting for packets beyond EOF. start() rearms this.
    p->synchronizer.delaysRateChangeUntilHasSufficientMediaData = NO;
    if (p->drain_observer) return;
    if (p->end_time_av <= CMTimeGetNanoseconds(p->synchronizer.currentTime)) {
        ao_stop_streaming(ao);
        return;
    }
    p->drain_observer = [[p->synchronizer
        addBoundaryTimeObserverForTimes:@[[NSValue valueWithCMTime:CMTimeFromNanoseconds(p->end_time_av)]]
        queue:p->queue usingBlock:^{
            cancel_drain(p);
            ao_stop_streaming(ao);
        }] retain];
}

static void feed(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->running || p->paused) return;
    update_hls_delay(p);
    // Software gain can change while EOF audio is still draining.
    if (p->hls_mode) {
        p->renderer.volume = MPCLAMP(atomic_load(&ao->gain), 0, 1);
        [p->hls setVolume:p->renderer.volume];
    }
    if (p->hls_mode && [p->hls failed]) {
        p->failure_stage = "E-AC-3 HLS remux/player";
        request_fallback(ao);
    }
    if (p->hls_mode && atomic_load(&p->failed)) {
        stop_requesting(p);
        return;
    }
    if (p->hls_draining) {
        if ([p->hls ended] || !p->hls) {
            atomic_store(&p->hls_delay_ns, 0);
            stop_requesting(p);
            ao_stop_streaming(ao);
        } else {
            retry_refill(p);
        }
        return;
    }
    if (p->clock_probe) p->clock_probe_callbacks++;
    if (p->clock_probe && mp_time_ns() - p->clock_probe_time >= 500000000) {
        p->clock_probe_time = mp_time_ns();
        if (p->hls_mode) {
            MP_WARN(ao, "AfuseAudioClock: transport=hls calls=%llu clock=%.6f end=%.6f failed=%d\n",
                (unsigned long long)p->clock_probe_callbacks, presentation_time(p) / 1e9,
                p->end_time_av / 1e9, [p->hls failed]);
        } else MP_WARN(ao, "AfuseAudioClock: calls=%llu clock=%.6f end=%.6f rate=%.2f sufficient=%d ready=%d status=%d muted=%d volume=%.3f\n",
            (unsigned long long)p->clock_probe_callbacks,
            CMTimeGetSeconds(p->synchronizer.currentTime), p->end_time_av / 1e9, p->synchronizer.rate,
            p->renderer.hasSufficientMediaDataForReliablePlaybackStart, p->renderer.readyForMoreMediaData,
            (int)p->renderer.status, p->renderer.muted, p->renderer.volume);
    }
    if (p->renderer.status == AVQueuedSampleBufferRenderingStatusFailed)
        request_fallback(ao);
    if (atomic_load(&p->failed)) {
        stop_requesting(p);
        return;
    }
    int64_t cur_time_av = presentation_time(p);
    // Give compressed spatial audio the same Bluetooth/preroll headroom as
    // PCM. The buffered read below distinguishes an empty decoder queue from
    // an audible underrun while AVFoundation still has samples to present.
    // HLS needs a complete live window before AVPlayer can start. Its bounded
    // prefill is independent of the sample-buffer renderer's 600 ms headroom.
    int64_t ahead = p->hls_mode ? 6000000000LL : 600000000;
    if (p->end_time_av - cur_time_av > ahead) {
        retry_refill(p);
        return;
    }
    // The compressed carrier must not be scaled as PCM. Apply mpv's gain in
    // Apple's renderer after native decoding (including mute).
    if (!p->hls_mode && (p->encoded || p->apac))
        p->renderer.volume = MPCLAMP(atomic_load(&ao->gain), 0, 1);
    int samplerate = ao->samplerate;
    int sstride = ao->sstride;

    CMBlockBufferRef block_buffer = NULL;
    CMSampleBufferRef sample_buffer = NULL;
    OSStatus err;

    int request_sample_count = p->apac ? AF_APAC_FRAMES : p->encoded ? AF_EAC3_BURST_FRAMES : samplerate / 10;
    int buffer_size = request_sample_count * sstride;
    void *data[] = {CFAllocatorAllocate(NULL, buffer_size, 0)};

    int64_t cur_time_mp = mp_time_ns();
    int64_t end_time_av = MPMAX(p->end_time_av, cur_time_av);
    int64_t time_delta = CMTimeGetNanoseconds(CMTimeMake(request_sample_count, samplerate));
    bool eof;
    int real_sample_count = ao_read_data_buffered(ao, data, request_sample_count,
        end_time_av - cur_time_av + cur_time_mp + time_delta, &eof);
    if (real_sample_count == 0) {
        if (!eof) retry_refill(p);
        goto finish;
    }

    if (p->apac) {
        if (!enqueue_apac(ao, data[0], real_sample_count * sstride, end_time_av))
            goto error;
        p->end_time_av = end_time_av + CMTimeGetNanoseconds(CMTimeMake(real_sample_count, samplerate));
        goto finish;
    }
    if (p->encoded) {
        if (!enqueue_eac3(ao, data[0], real_sample_count * sstride, end_time_av))
            goto error;
        p->end_time_av = end_time_av + CMTimeGetNanoseconds(CMTimeMake(real_sample_count, samplerate));
        goto finish;
    }

    if ((err = CMBlockBufferCreateWithMemoryBlock(
        NULL,
        data[0],
        buffer_size,
        NULL,
        NULL,
        0,
        real_sample_count * sstride,
        0,
        &block_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create block buffer\n");
        MP_VERBOSE(ao, "CMBlockBufferCreateWithMemoryBlock returned %d\n", err);
        p->failure_stage = "PCM block buffer"; p->failure_status = err;
        goto error;
    }
    data[0] = NULL;

    CMSampleTimingInfo sample_timing_into[] = {(CMSampleTimingInfo) {
        .duration = CMTimeMake(1, samplerate),
        .presentationTimeStamp = CMTimeFromNanoseconds(end_time_av),
        .decodeTimeStamp = kCMTimeInvalid
    }};
    size_t sample_size_array[] = {sstride};
    if ((err = CMSampleBufferCreateReady(
        NULL,
        block_buffer,
        p->format_description,
        real_sample_count,
        1,
        sample_timing_into,
        1,
        sample_size_array,
        &sample_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create sample buffer\n");
        MP_VERBOSE(ao, "CMSampleBufferCreateReady returned %d\n", err);
        p->failure_stage = "PCM sample buffer"; p->failure_status = err;
        goto error;
    }

    [p->renderer enqueueSampleBuffer:sample_buffer];

    time_delta = CMTimeGetNanoseconds(CMTimeMake(real_sample_count, samplerate));
    p->end_time_av = end_time_av + time_delta;

    goto finish;

error:
    request_fallback(ao);
finish:
    update_hls_delay(p);
    if (p->hls_mode && !eof && !atomic_load(&p->failed)) retry_refill(p);
    if (data[0]) CFAllocatorDeallocate(NULL, data[0]);
    if (block_buffer) CFRelease(block_buffer);
    if (sample_buffer) CFRelease(sample_buffer);
    if (eof && !atomic_load(&p->failed)) drain_at_eof(ao);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    dispatch_sync(p->queue, ^{
        // EOF can reuse this AO for a following item. A completed HLS event
        // cannot accept more packets; give the next item a fresh timeline.
        if (p->hls_mode && p->hls_draining) {
            [p->hls stop]; [p->hls release]; p->hls = nil;
            p->pending_size = 0;
            if (p->format_description) {
                CFRelease(p->format_description); p->format_description = NULL;
            }
        }
        p->running = true; p->paused = false;
        p->end_time_av = p->hls_mode ? 0 : -1;
        p->hls_draining = false;
        atomic_store(&p->hls_delay_ns, 0);
        p->synchronizer.delaysRateChangeUntilHasSufficientMediaData = YES;
        [p->synchronizer setRate:1];
        request_more(ao);
    });
}

static void stop(struct ao *ao)
{
    struct priv *p = ao->priv;

    dispatch_sync(p->queue, ^{
        cancel_drain(p);
        p->running = false; p->paused = false;
        stop_requesting(p);
        [p->renderer flush];
        [p->synchronizer setRate:0];
        [p->hls stop];
        [p->hls release];
        p->hls = nil;
        p->hls_draining = false;
        p->end_time_av = 0;
        atomic_store(&p->hls_delay_ns, 0);
        p->pending_size = 0;
        if ((p->encoded || p->apac) && p->format_description) {
            CFRelease(p->format_description); p->format_description = NULL;
        }
    });
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    dispatch_sync(p->queue, ^{
        p->paused = paused;
        [p->hls setPaused:paused];
        update_hls_delay(p);
        if (paused) {
            stop_requesting(p);
            [p->synchronizer setRate:0];
        } else {
            [p->synchronizer setRate:1];
            request_more(ao);
        }
    });

    return true;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;

    switch (cmd) {
    case AOCONTROL_GET_MUTE:
        *(bool*)arg = [p->renderer isMuted];
        return CONTROL_OK;
    case AOCONTROL_GET_VOLUME:
        *(float*)arg = [p->renderer volume] * 100;
        return CONTROL_OK;
    case AOCONTROL_SET_MUTE: {
        bool muted = *(bool*)arg;
        dispatch_sync(p->queue, ^{
            [p->renderer setMuted:muted];
            [p->hls setMuted:muted];
        });
        return CONTROL_OK;
    }
    case AOCONTROL_SET_VOLUME: {
        float volume = *(float*)arg / 100;
        dispatch_sync(p->queue, ^{
            [p->renderer setVolume:volume];
            [p->hls setVolume:volume];
        });
        return CONTROL_OK;
    }
    default:
        return CONTROL_UNKNOWN;
    }
}

@implementation AVObserver
- (instancetype)initWithAO:(struct ao*)_ao {
    self = [super init];
    if (self) {
        ao = _ao;
        lock = [[NSLock alloc] init];
    }
    return self;
}
- (void)handleRestartNotification:(NSNotification*)notification {
    [lock lock];
    // Let mpv refill audio from its playhead and resynchronize video. Calling
    // stop/start here discards queued samples while leaving the media clock
    // ahead, and can deadlock when AVFoundation posts on the feeding queue.
    if (ao) ao_request_reload(ao);
    [lock unlock];
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                      change:(NSDictionary *)change context:(void *)context {
    [lock lock];
    if (ao && [(AVSampleBufferAudioRenderer *)object status] == AVQueuedSampleBufferRenderingStatusFailed)
        request_fallback(ao);
    [lock unlock];
}
- (void)invalidate {
    [lock lock]; ao = NULL; [lock unlock];
}
- (void)dealloc {
    [lock release];
    [super dealloc];
}
@end

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    AudioChannelLayout *layout = NULL;
    atomic_init(&p->failed, false);
    atomic_init(&p->hls_delay_ns, 0);
    atomic_init(&p->failure_stage, NULL);
    atomic_init(&p->failure_status, 0);
    const char *clock_probe = getenv("AFUSE_MPV_AUDIO_CLOCK_PROBE");
    p->clock_probe = clock_probe && !strcmp(clock_probe, "1");

#if TARGET_OS_IPHONE
    AVAudioSession *instance = AVAudioSession.sharedInstance;
    // AirPlay/A2DP are implicit for output-only playback. Carrying options
    // from playAndRecord into this category makes newer iOS return paramErr.
    NSError *session_error = nil;
    if ((![instance.category isEqualToString:AVAudioSessionCategoryPlayback] ||
         ![instance.mode isEqualToString:AVAudioSessionModeMoviePlayback] || instance.categoryOptions) &&
        ![instance setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeMoviePlayback
                      options:0 error:&session_error]) {
        MP_ERR(ao, "AVAudioSession category failed: %s (%ld)\n",
               session_error.localizedDescription.UTF8String, (long)session_error.code);
        goto error;
    }
    if (!instance.supportsMultichannelContent)
        [instance setSupportsMultichannelContent:YES error:nil];
    if (![instance setActive:YES error:&session_error]) {
        MP_ERR(ao, "AVAudioSession activation failed: %s (%ld)\n",
               session_error.localizedDescription.UTF8String, (long)session_error.code);
        goto error;
    }
    // Headphones may have two physical outputs but accept a multichannel
    // spatial presentation. Preserve the source layout for AVFoundation.
#endif

    if ((p->renderer = [[AVSampleBufferAudioRenderer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create audio renderer\n");
        MP_VERBOSE(ao, "AVSampleBufferAudioRenderer failed to initialize\n");
        goto error;
    }
    if ((p->synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create rendering synchronizer\n");
        MP_VERBOSE(ao, "AVSampleBufferRenderSynchronizer failed to initialize\n");
        goto error;
    }
    if ((p->queue = dispatch_queue_create(
        "avfoundation event",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)
    )) == NULL) {
        MP_FATAL(ao, "failed to create dispatch queue\n");
        MP_VERBOSE(ao, "dispatch_queue_create failed\n");
        goto error;
    }

#if !TARGET_OS_IPHONE
    if (ao->device && ao->device[0]) {
        [p->renderer setAudioOutputDeviceUniqueID:(NSString*)cfstr_from_cstr(ao->device)];
    }
#endif

    p->refill_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, p->queue);
    if (!p->refill_timer) goto error;
    dispatch_source_set_timer(p->refill_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_source_set_event_handler(p->refill_timer, ^{
        dispatch_source_set_timer(p->refill_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        request_more(ao);
    });
    dispatch_resume(p->refill_timer);

    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *))
        p->renderer.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMonoStereoAndMultichannel;

    [p->synchronizer addRenderer:p->renderer];
    if (@available(tvOS 14.5, iOS 14.5, macOS 11.3, *)) {
        [p->synchronizer setDelaysRateChangeUntilHasSufficientMediaData:YES];
    }

    p->encoded = ao->format == AF_FORMAT_S_EAC3;
    p->apac = ao->format == AF_FORMAT_S_APAC;
    p->hls_mode = p->encoded && TARGET_OS_TV;
    if (af_fmt_is_spdif(ao->format) && !p->encoded && !p->apac) {
        MP_FATAL(ao, "avfoundation does not support SPDIF\n");
#if HAVE_COREAUDIO
        MP_FATAL(ao, "please use coreaudio_exclusive instead\n");
#endif
        goto error;
    }

    if (p->encoded) {
        if (ao->channels.num != 2 ||
            (ao->samplerate != 192000 && ao->samplerate != 176400 && ao->samplerate != 128000))
            goto error;
        goto observe;
    }

    if (p->apac) {
        if (ao->channels.num != 16 || ao->samplerate != 48000) goto error;
        goto observe;
    }

    // AVSampleBufferAudioRenderer only supports interleaved formats
    ao->format = af_fmt_from_planar(ao->format);
    if (af_fmt_is_planar(ao->format)) {
        MP_FATAL(ao, "planar audio formats are unsupported\n");
        goto error;
    }

    AudioStreamBasicDescription asbd = {0};
    ca_fill_asbd(ao, &asbd);
    size_t layout_size;
    layout = ca_get_acl(ao, &layout_size);

    OSStatus err;
    if ((err = CMAudioFormatDescriptionCreate(
        NULL,
        &asbd,
        layout_size,
        layout,
        0,
        NULL,
        NULL,
        &p->format_description
    )) != noErr) {
        MP_FATAL(ao, "failed to create audio format description\n");
        MP_VERBOSE(ao, "CMAudioFormatDescriptionCreate returned %d\n", err);
        goto error;
    }
    talloc_free(layout);
    layout = NULL;

observe:
    // AVSampleBufferAudioRenderer read ahead aggressively
    ao->device_buffer = ao->samplerate * 2;

    p->observer = [[AVObserver alloc] initWithAO:ao];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *)) {
        [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererOutputConfigurationDidChangeNotification object:p->renderer];
    }
    [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:p->renderer];
    [p->renderer addObserver:p->observer forKeyPath:@"status" options:0 context:NULL];

    return CONTROL_OK;

error:
    talloc_free(layout);
    if (p->refill_timer) { dispatch_source_cancel(p->refill_timer); dispatch_release(p->refill_timer); }
    if (p->renderer) [p->renderer release];
    if (p->synchronizer) [p->synchronizer release];
    if (p->queue) dispatch_release(p->queue);
    if (p->format_description) CFRelease(p->format_description);

    return CONTROL_ERROR;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    [p->observer invalidate];
    [[NSNotificationCenter defaultCenter] removeObserver:p->observer];
    [p->renderer removeObserver:p->observer forKeyPath:@"status"];

    stop(ao);
    dispatch_sync(p->queue, ^{
        dispatch_source_cancel(p->refill_timer);
        dispatch_source_set_event_handler(p->refill_timer, nil);
    });
    dispatch_release(p->refill_timer);

    [p->renderer release];
    [p->synchronizer release];
    dispatch_release(p->queue);
    if (p->format_description) CFRelease(p->format_description);

    [p->observer release];
    // The application owns AVAudioSession activation across output fallback,
    // route changes and player replacement. Do not deactivate its new route.
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_avfoundation = {
    .description    = "AVFoundation AVSampleBufferAudioRenderer",
    .name           = "avfoundation",
    .uninit         = uninit,
    .init           = init,
    .control        = control,
    .reset          = stop,
    .start          = start,
    .set_pause      = set_pause,
    .get_state      = get_state,
#if !TARGET_OS_IPHONE
    .list_devs      = ca_get_device_list,
#endif
    .priv_size      = sizeof(struct priv),
};
