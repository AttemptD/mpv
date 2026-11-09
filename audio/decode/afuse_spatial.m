// Afusekt MPV spatial bridge. Object rendering attribution/license is in
// afuse_object_renderer.h; the MPV integration is LGPL-2.1-or-later.
// Conversion is deliberately before the AO queue: decoder lookahead must not
// advance MPV's playback clock or start video/danmaku ahead of audible sound.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include "audio/aframe.h"
#include "audio/format.h"
#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "afuse_spatial.h"
#include "afuse_object_renderer.h"
#include "afuse_apac_carrier.h"

struct audio_node { struct mp_aframe *frame; struct audio_node *next; };
struct held_block {
    struct held_block *next;
    double pts;
    unsigned frames, channels;
    float samples[];
};
struct afuse_spatial {
    struct mp_log *log;
    bool truehd, fallback, finished, announced;
    TrueHDAtmosDecoder *decoder;
    AVCodecContext *dts;
    AVPacket *packet;
    AVFrame *frame;
    AVRational timebase;
    unsigned dts_frames, dts_without_heights;
    struct af_object_renderer renderer;
    struct held_block *held, *held_tail;
    unsigned held_frames;
    struct audio_node *pcm, *pcm_tail, *encoded, *encoded_tail;
    int pcm_frames;
    double delivered_encoded_end, anchor, input_end, source_anchor;
    int64_t encoded_frames, input_frames;
    AVAudioConverter *converter;
    AVAudioPCMBuffer *input;
    AVAudioCompressedBuffer *output;
    NSData *cookie;
    uint32_t leading;
    float pending[12][AF_APAC_FRAMES];
    unsigned pending_frames;
};

static void queue_push(struct audio_node **head, struct audio_node **tail, struct mp_aframe *frame) {
    struct audio_node *n = talloc_zero(NULL, struct audio_node);
    n->frame = frame;
    if (*tail) (*tail)->next = n; else *head = n;
    *tail = n;
}
static struct mp_aframe *queue_pop(struct audio_node **head, struct audio_node **tail) {
    if (!*head) return NULL;
    struct audio_node *n = *head;
    struct mp_aframe *f = n->frame;
    *head = n->next;
    if (!*head) *tail = NULL;
    talloc_free(n);
    return f;
}
static void queue_clear(struct audio_node **head, struct audio_node **tail) {
    struct mp_aframe *f;
    while ((f = queue_pop(head, tail))) talloc_free(f);
}
static void clear_held(struct afuse_spatial *s) {
    while (s->held) {
        struct held_block *n = s->held->next;
        talloc_free(s->held); s->held = n;
    }
    s->held_tail = NULL; s->held_frames = 0;
}
static void fallback(struct afuse_spatial *s, const char *why) {
    if (s->fallback) return;
    s->fallback = true;
    MP_WARN(s, "AfuseAPACFallback: %s; retaining original multichannel PCM\n", why);
    queue_clear(&s->encoded, &s->encoded_tail);
    clear_held(s);
}

static bool create_encoder(struct afuse_spatial *s) {
    if (@available(macOS 26.0, iOS 26.0, tvOS 26.0, *)) {
        @try {
            AVAudioChannelLayout *layout = [[AVAudioChannelLayout alloc] initWithLayoutTag:kAudioChannelLayoutTag_Atmos_7_1_4];
            AVAudioFormat *pcm = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channelLayout:layout];
            AudioStreamBasicDescription asbd = {.mFormatID = kAudioFormatAPAC, .mSampleRate = 48000, .mChannelsPerFrame = 12};
            UInt32 size = sizeof(asbd);
            OSStatus err = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &asbd);
            AVAudioFormat *apac = err == noErr ? [[AVAudioFormat alloc] initWithStreamDescription:&asbd channelLayout:layout] : nil;
            s->converter = apac ? [[AVAudioConverter alloc] initFromFormat:pcm toFormat:apac] : nil;
            [apac release]; [layout release];
            if (s->converter) {
                // A live spatial conversion needs bounded lookahead. Capture
                // preserves Apple's mandatory loudness/DRC metadata (~1.56 s),
                // unlike disabling DRC or Movie's multi-second/offline analysis.
                s->converter.contentSource = AVAudioContentSource_AV_Spatial_Live;
                s->converter.dynamicRangeControlConfiguration = AVAudioDynamicRangeControlConfiguration_Capture;
                s->converter.audioSyncPacketFrequency = 1; // independent packet after an exact seek
                s->converter.bitRate = 12 * 256000;
                s->cookie = [s->converter.magicCookie copy];
                MP_VERBOSE(s, "APAC encoder format flags=%u frames=%u bytes=%u cookie=%lu\n",
                    s->converter.outputFormat.streamDescription->mFormatFlags,
                    s->converter.outputFormat.streamDescription->mFramesPerPacket,
                    s->converter.outputFormat.streamDescription->mBytesPerPacket, (unsigned long)s->cookie.length);
                s->leading = s->converter.primeInfo.leadingFrames;
                s->input = [[AVAudioPCMBuffer alloc] initWithPCMFormat:pcm frameCapacity:AF_APAC_FRAMES];
                s->output = [[AVAudioCompressedBuffer alloc] initWithFormat:s->converter.outputFormat
                    packetCapacity:8 maximumPacketSize:MAX(1, s->converter.maximumOutputPacketSize)];
            }
            [pcm release];
            return s->converter && s->input && s->output && s->cookie.length > 0 && s->cookie.length <= AF_APAC_COOKIE_MAX;
        } @catch (NSException *e) {
            MP_WARN(s, "APAC encoder unavailable: %s\n", e.reason.UTF8String);
        }
    }
    return false;
}

static bool collect_packets(struct afuse_spatial *s) {
    AVAudioCompressedBuffer *output = s->output;
    const AudioStreamPacketDescription *descriptions = output.packetDescriptions;
    if (output.packetCount) {
        NSData *cookie = s->converter.magicCookie;
        if (!cookie.length || cookie.length > AF_APAC_COOKIE_MAX) return false;
        if (![s->cookie isEqualToData:cookie]) {
            [s->cookie release]; s->cookie = [cookie copy];
        }
    }
    for (unsigned n = 0; n < output.packetCount; n++) {
        if (!descriptions) return false;
        AudioStreamPacketDescription d = descriptions[n];
        if (d.mStartOffset < 0 || !d.mDataByteSize || d.mStartOffset + d.mDataByteSize > output.byteLength ||
            sizeof(struct af_apac_header) + s->cookie.length + d.mDataByteSize > AF_APAC_BYTES) return false;
        struct mp_aframe *f = mp_aframe_create();
        struct mp_chmap map;
        mp_chmap_set_unknown(&map, 16); // internal carrier, not the rendered speaker layout
        mp_aframe_set_format(f, AF_FORMAT_S_APAC);
        mp_aframe_set_rate(f, 48000);
        mp_aframe_set_chmap(f, &map);
        if (!mp_aframe_alloc_data(f, AF_APAC_FRAMES)) { talloc_free(f); return false; }
        uint8_t *data = mp_aframe_get_data_rw(f)[0];
        memset(data, 0, AF_APAC_BYTES);
        struct af_apac_header header = {.version = 1, .payload_size = d.mDataByteSize,
            .cookie_size = (uint32_t)s->cookie.length, .channels = 12, .rate = 48000,
            // Keep priming on every carrier: an exact seek may discard the
            // first packets before the AO sees this encoder run.
            .leading_frames = s->leading};
        memcpy(header.magic, af_apac_magic, sizeof(header.magic));
        memcpy(data, &header, sizeof(header));
        memcpy(data + sizeof(header), s->cookie.bytes, s->cookie.length);
        memcpy(data + sizeof(header) + s->cookie.length, (uint8_t *)output.data + d.mStartOffset, d.mDataByteSize);
        mp_aframe_set_pts(f, s->anchor + s->encoded_frames / 48000.0);
        s->encoded_frames += AF_APAC_FRAMES;
        queue_push(&s->encoded, &s->encoded_tail, f);
        if (!s->announced) {
            s->announced = true;
            MP_INFO(s, "AfuseSpatialAPAC: %s -> rendered 7.1.4 -> APAC; DRC=Capture; first packet after %.3f s input; priming=%u\n",
                s->truehd ? "TrueHD objects" : "DTS:X fixed heights", s->input_frames / 48000.0, s->leading);
        }
    }
    return true;
}

static bool encode_pending(struct afuse_spatial *s, bool eof) {
    @autoreleasepool {
        @try {
            __block unsigned offset = 0;
            unsigned count = s->pending_frames;
            for (int attempt = 0; attempt < 1024; attempt++) {
                s->output.packetCount = 0; s->output.byteLength = 0;
                NSError *error = nil;
                AVAudioConverterOutputStatus result = [s->converter convertToBuffer:s->output error:&error
                    withInputFromBlock:^AVAudioBuffer *(AVAudioPacketCount wanted, AVAudioConverterInputStatus *state) {
                        unsigned take = MIN(wanted, count - offset);
                        if (!take) { *state = eof ? AVAudioConverterInputStatus_EndOfStream : AVAudioConverterInputStatus_NoDataNow; return nil; }
                        s->input.frameLength = take;
                        for (int c = 0; c < 12; c++) memcpy(s->input.floatChannelData[c], s->pending[c] + offset, take * sizeof(float));
                        offset += take;
                        *state = AVAudioConverterInputStatus_HaveData; return s->input;
                    }];
                if (result == AVAudioConverterOutputStatus_Error || !collect_packets(s)) {
                    MP_WARN(s, "APAC conversion failed: %s\n", error.localizedDescription.UTF8String ?: "invalid packet");
                    return false;
                }
                if (result != AVAudioConverterOutputStatus_HaveData || !s->output.packetCount) {
                    if (offset != count) return false;
                    s->pending_frames = 0;
                    return true;
                }
            }
        } @catch (NSException *e) {
            MP_WARN(s, "APAC conversion exception: %s\n", e.reason.UTF8String);
        }
    }
    return false;
}

static bool encode_planes(struct afuse_spatial *s, const float *const *planes, unsigned count, double pts) {
    if (s->anchor == MP_NOPTS_VALUE) s->anchor = pts;
    if (pts == MP_NOPTS_VALUE || !isfinite(pts)) return false;
    // Do not compress over a discontinuity, guessed TrueHD offset or damaged span.
    if (s->input_end != MP_NOPTS_VALUE && fabs(pts - s->input_end) > .005) {
        if (!s->announced && s->input_frames <= AF_APAC_FRAMES) {
            // Some MKV seeks prepend a major-sync AU with an earlier PTS.
            // Keep it for decoder state, but don't encode its stale timestamp.
            [s->converter reset];
            s->pending_frames = 0; s->input_frames = 0; s->anchor = pts;
        } else {
            MP_WARN(s, "Spatial timeline discontinuity: %.6f -> %.6f\n", s->input_end, pts);
            return false;
        }
    }
    s->input_end = pts + count / 48000.0;
    s->input_frames += count;
    unsigned offset = 0;
    while (count) {
        unsigned take = MPMIN(count, AF_APAC_FRAMES - s->pending_frames);
        for (int c = 0; c < 12; c++) memcpy(s->pending[c] + s->pending_frames, planes[c] + offset, take * sizeof(float));
        s->pending_frames += take; offset += take; count -= take;
        if (s->pending_frames == AF_APAC_FRAMES && !encode_pending(s, false)) return false;
    }
    return true;
}

static bool render_block(struct afuse_spatial *s, const float *const *planes, unsigned frames,
                          const TrueHDAtmosMetadata *metadata, double pts) {
    if (frames > 4096) return false;
    float *storage = talloc_array(NULL, float, 12 * frames);
    float *rendered[12];
    for (int c = 0; c < 12; c++) rendered[c] = storage + c * frames;
    bool ok = af_object_render(&s->renderer, planes, frames, metadata, rendered) &&
              encode_planes(s, (const float *const *)rendered, frames, pts);
    talloc_free(storage);
    return ok;
}

static bool pull_truehd(struct afuse_spatial *s) {
    TrueHDAtmosBlock b;
    int status;
    while ((status = truehd_atmos_decoder_pull(s->decoder, &b)) == TRUEHD_ATMOS_OK) {
        if (b.presentation != 3 || b.sample_rate != 48000 || !b.channel_count || b.channel_count > 16 ||
            b.flags & TRUEHD_ATMOS_BLOCK_INPUT_OFFSET_ESTIMATED) {
            MP_WARN(s, "TrueHD spatial rejection: presentation=%u rate=%u channels=%u flags=%u\n", b.presentation, b.sample_rate, b.channel_count, b.flags);
            return false;
        }
        if (s->renderer.serial != b.layout_serial) {
            if (s->announced) return false;
            memset(&s->renderer, 0, sizeof(s->renderer));
            s->renderer.serial = b.layout_serial;
            s->renderer.count = b.channel_count;
        }
        // Matroska lacing may omit or quantize access-unit PTS. The decoder's
        // input timeline counts skipped major-sync pre-roll too, so it is the
        // exact sample clock anchored at the first demuxed packet after reset.
        double offset = (double)b.input_frame_offset / b.sample_rate;
        double pts = s->source_anchor != MP_NOPTS_VALUE ? s->source_anchor + offset : MP_NOPTS_VALUE;
        if (b.pts != TRUEHD_ATMOS_NO_PTS && pts != MP_NOPTS_VALUE) {
            double observed = b.pts / 1e9 + (double)b.pts_au_index * b.access_unit_frames / b.sample_rate;
            if (fabs(observed - pts) > .005) {
                if (s->announced || s->input_frames > AF_APAC_FRAMES) return false;
                s->source_anchor = observed - offset;
                pts = observed;
            }
        }
        if (!s->renderer.known[0]) {
            if (!b.metadata || !b.metadata->update_count) {
                if (s->held_frames + b.frame_count > 12000) return false;
                struct held_block *h = talloc_size(NULL, sizeof(*h) + b.frame_count * b.channel_count * sizeof(float));
                *h = (struct held_block){.pts = pts, .frames = b.frame_count, .channels = b.channel_count};
                for (int c = 0; c < b.channel_count; c++) memcpy(h->samples + c * b.frame_count, b.channels[c], b.frame_count * sizeof(float));
                if (s->held_tail) s->held_tail->next = h; else s->held = h;
                s->held_tail = h; s->held_frames += b.frame_count;
                continue;
            }
            if (!af_object_update(&s->renderer, b.metadata->updates[0].elements, b.metadata->element_count, 0, true)) return false;
            for (struct held_block *h = s->held; h; h = h->next) {
                if (h->channels != b.channel_count) return false;
                const float *planes[16];
                for (int c = 0; c < h->channels; c++) planes[c] = h->samples + c * h->frames;
                if (!render_block(s, planes, h->frames, NULL, h->pts)) return false;
            }
            clear_held(s);
        }
        if (!render_block(s, b.channels, b.frame_count, b.metadata, pts)) return false;
    }
    return status == TRUEHD_ATMOS_NEED_MORE_DATA;
}

static bool pull_dtsx(struct afuse_spatial *s) {
    static const enum AVChannel channels[] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
        AV_CHAN_LOW_FREQUENCY, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT,
        AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT, AV_CHAN_TOP_BACK_LEFT, AV_CHAN_TOP_BACK_RIGHT};
    int result;
    while ((result = avcodec_receive_frame(s->dts, s->frame)) == 0) {
        AVFrame *f = s->frame;
        s->dts_frames++;
        int index[12]; bool heights = f->sample_rate == 48000 && f->ch_layout.nb_channels == 12;
        for (int c = 0; c < 12; c++) {
            index[c] = av_channel_layout_index_from_channel(&f->ch_layout, channels[c]);
            heights &= index[c] >= 0;
        }
        if (!heights) {
            av_frame_unref(f);
            if (s->announced || ++s->dts_without_heights >= 8) {
                fallback(s, "DTS has no supported standard 7.1.4 height presentation (alternate/IMAX presentations remain PCM)");
                return false;
            }
            continue;
        }
        if (f->nb_samples <= 0 || f->nb_samples > 8192 || (f->format != AV_SAMPLE_FMT_S32P && f->format != AV_SAMPLE_FMT_FLTP)) return false;
        float *storage = talloc_array(NULL, float, f->nb_samples * 12);
        float *planes[12];
        for (int c = 0; c < 12; c++) {
            planes[c] = storage + c * f->nb_samples;
            for (int n = 0; n < f->nb_samples; n++) {
                float value = f->format == AV_SAMPLE_FMT_FLTP ? ((float *)f->extended_data[index[c]])[n] :
                    ((int32_t *)f->extended_data[index[c]])[n] / 2147483648.0f;
                if (!isfinite(value)) { talloc_free(storage); return false; }
                // XLL residuals require the core's filter history after seeking.
                planes[c][n] = s->dts_frames <= 2 ? 0 : value;
            }
        }
        double pts = mp_pts_from_av(f->pts, &s->timebase);
        bool ok = encode_planes(s, (const float *const *)planes, f->nb_samples, pts);
        talloc_free(storage); av_frame_unref(f);
        if (!ok) return false;
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
}

struct afuse_spatial *afuse_spatial_create(struct mp_log *log, struct mp_codec_params *codec) {
    if (!codec->codec || (strcmp(codec->codec, "truehd") && strcmp(codec->codec, "dts"))) return NULL;
    if (@available(macOS 26.0, iOS 26.0, tvOS 26.0, *)) {} else return NULL;
    struct afuse_spatial *s = talloc_zero(NULL, struct afuse_spatial);
    s->log = log; s->truehd = !strcmp(codec->codec, "truehd");
    s->anchor = s->input_end = s->delivered_encoded_end = s->source_anchor = MP_NOPTS_VALUE;
    s->timebase = mp_get_codec_timebase(codec);
    if (s->truehd) {
        s->decoder = truehd_atmos_decoder_create(TRUEHD_ATMOS_PRESENTATION_HIGHEST);
        if (!s->decoder) goto error;
    } else {
        const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_DTS);
        s->dts = avcodec_alloc_context3(decoder);
        s->frame = av_frame_alloc(); s->packet = av_packet_alloc();
        if (!s->dts || !s->frame || !s->packet) goto error;
        s->dts->pkt_timebase = s->timebase;
        // The optional decoder must never advertise 7.1.4 with fabricated
        // silent heights when an extension is damaged or unsupported. The
        // independent baseline decoder remains available for PCM recovery.
        s->dts->err_recognition |= AV_EF_EXPLODE;
        if (mp_set_avctx_codec_headers(s->dts, codec) < 0 ||
            av_opt_set_int(s->dts, "x_heights", 1, AV_OPT_SEARCH_CHILDREN) < 0 ||
            avcodec_open2(s->dts, decoder, NULL) < 0) goto error;
    }
    if (!create_encoder(s)) goto error;
    return s;
error:
    MP_WARN(s, "AfuseAPACFallback: decoder/encoder unavailable; original multichannel PCM\n");
    afuse_spatial_destroy(s);
    return NULL;
}

void afuse_spatial_packet(struct afuse_spatial *s, struct demux_packet *packet) {
    if (!s || s->fallback || s->finished || !packet) return;
    bool ok;
    if (s->truehd) {
        if (s->source_anchor == MP_NOPTS_VALUE) s->source_anchor = packet->pts;
        int64_t pts = packet->pts != MP_NOPTS_VALUE && isfinite(packet->pts) ? llrint(packet->pts * 1e9) : TRUEHD_ATMOS_NO_PTS;
        ok = truehd_atmos_decoder_push(s->decoder, packet->buffer, packet->len, pts) == TRUEHD_ATMOS_OK && pull_truehd(s);
    } else {
        mp_set_av_packet(s->packet, packet, &s->timebase);
        ok = avcodec_send_packet(s->dts, s->packet) >= 0 && pull_dtsx(s);
    }
    if (!ok) fallback(s, "unsupported spatial presentation, discontinuity or conversion failure");
}

void afuse_spatial_pcm(struct afuse_spatial *s, struct mp_aframe *f) {
    s->pcm_frames += mp_aframe_get_size(f);
    queue_push(&s->pcm, &s->pcm_tail, f);
    // Bound encoder/metadata lookahead and preserve baseline PCM for recovery.
    if (!s->fallback && s->pcm_frames > 48000 * 4) fallback(s, "APAC lookahead exceeded four seconds");
}

struct mp_aframe *afuse_spatial_pop(struct afuse_spatial *s) {
    if (!s) return NULL;
    struct mp_aframe *f = !s->fallback ? queue_pop(&s->encoded, &s->encoded_tail) : NULL;
    if (f) s->delivered_encoded_end = mp_aframe_end_pts(f);
    // Only delivered APAC defines the recovery cutoff. PCM packet PTS may be
    // rounded by the container (40 TrueHD samples are shorter than a Matroska
    // millisecond). Advancing this cutoff with PCM would repeatedly discard
    // valid samples before mpv can repair their timestamps.
    while (s->pcm) {
        struct mp_aframe *pcm = s->pcm->frame;
        double end = mp_aframe_end_pts(pcm);
        if (s->delivered_encoded_end == MP_NOPTS_VALUE || end == MP_NOPTS_VALUE || end > s->delivered_encoded_end + .00001) break;
        s->pcm_frames -= mp_aframe_get_size(pcm);
        talloc_free(queue_pop(&s->pcm, &s->pcm_tail));
    }
    if (!f && s->fallback && s->pcm) {
        f = queue_pop(&s->pcm, &s->pcm_tail);
        s->pcm_frames -= mp_aframe_get_size(f);
        double pts = mp_aframe_get_pts(f);
        if (s->delivered_encoded_end != MP_NOPTS_VALUE && pts != MP_NOPTS_VALUE && pts < s->delivered_encoded_end) {
            unsigned skip = MPMIN(mp_aframe_get_size(f), llrint((s->delivered_encoded_end - pts) * mp_aframe_get_rate(f)));
            mp_aframe_skip_samples(f, skip);
        }
    }
    return f;
}

void afuse_spatial_finish(struct afuse_spatial *s) {
    if (!s || s->finished) return;
    s->finished = true;
    if (s->fallback) return;
    if (s->held || s->anchor == MP_NOPTS_VALUE || !encode_pending(s, true)) fallback(s, "incomplete spatial stream");
}

void afuse_spatial_reset(struct afuse_spatial *s) {
    if (!s) return;
    queue_clear(&s->pcm, &s->pcm_tail); queue_clear(&s->encoded, &s->encoded_tail); clear_held(s);
    s->pcm_frames = s->pending_frames = 0;
    s->input_frames = s->encoded_frames = 0;
    s->anchor = s->input_end = s->delivered_encoded_end = s->source_anchor = MP_NOPTS_VALUE;
    s->finished = s->announced = false;
    s->dts_frames = s->dts_without_heights = 0;
    memset(&s->renderer, 0, sizeof(s->renderer));
    if (s->decoder) truehd_atmos_decoder_reset(s->decoder);
    if (s->dts) avcodec_flush_buffers(s->dts);
    [s->converter reset];
    // A rejected presentation stays PCM for this track, including after a seek.
}

void afuse_spatial_destroy(struct afuse_spatial *s) {
    if (!s) return;
    afuse_spatial_reset(s);
    if (s->decoder) truehd_atmos_decoder_destroy(s->decoder);
    avcodec_free_context(&s->dts); av_frame_free(&s->frame); mp_free_av_packet(&s->packet);
    [s->converter release]; [s->input release]; [s->output release]; [s->cookie release];
    talloc_free(s);
}
