/* Native E-AC-3 transport for AVFoundation. LGPL-2.1-or-later, like mpv.
 * IEC61937 is only mpv's internal packet transport here. Never send the carrier
 * to a PCM device: unwrap it and give Apple the original compressed access unit.
 */
#include <libavformat/avformat.h>
#include <libavcodec/ac3_parser.h>
#include <libavutil/intreadwrite.h>

#define AF_EAC3_BURST_BYTES 24576
#define AF_EAC3_BURST_FRAMES (AF_EAC3_BURST_BYTES / 4)

static bool af_eac3_sync(const uint8_t *data)
{
    return AV_RL32(data) == 0x4e1ff872 && AV_RL16(data + 4) == 0x15;
}

static bool af_eac3_dependent(const uint8_t *data, int size)
{
    return size >= 7 && (data[5] >> 3) > 10 && (data[2] >> 6) == 1;
}

// This bounded ISO-BMFF walk examines only the muxer's format boxes, never mdat.
static const uint8_t *af_eac3_box(const uint8_t *data, size_t size,
                                  const char *type, size_t *box_size)
{
    while (size >= 8) {
        size_t n = AV_RB32(data);
        if (n < 8 || n > size) return NULL;
        if (!memcmp(data + 4, type, 4)) { *box_size = n; return data; }
        data += n; size -= n;
    }
    return NULL;
}

static CMAudioFormatDescriptionRef af_eac3_format(const uint8_t *data, int size,
                                                  int sample_rate)
{
    // A six-block IEC burst can contain several small independent frames and
    // their dependent substreams. The MOV muxer already parses all of these,
    // including the JOC complexity extension. Reuse it to construct dec3 rather
    // than guessing a six-channel cookie or dropping dependent/object metadata.
    uint8_t bsid;
    uint16_t frame_size;
    if (size < 7 || av_ac3_parse_header(data, size, &bsid, &frame_size) < 0 ||
        frame_size > size || af_eac3_dependent(data, size) ||
        (bsid > 10 && (data[2] & 0x38))) return NULL;
    const int rates[] = {48000, 44100, 32000};
    int rate_code = data[4] >> 6;
    int rate_shift = bsid <= 10 && bsid > 8 ? bsid - 8 : 0;
    if (rate_code == 3 || (rates[rate_code] >> rate_shift) != sample_rate) return NULL;
    const int channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
    int acmod = (data[4] >> 1) & 7, lfe = data[4] & 1;
    if (bsid <= 10) {
        // Blu-ray DD+ may carry an AC-3-compatible bed followed by E-AC-3
        // dependent/JOC data. The first header then uses AC-3 syntax.
        acmod = data[6] >> 5;
        int lfe_bit = 4;
        if (acmod == 2) lfe_bit -= 2;
        else {
            if ((acmod & 1) && acmod != 1) lfe_bit -= 2;
            if (acmod & 4) lfe_bit -= 2;
        }
        lfe = (data[6] >> lfe_bit) & 1;
    }
    int channel_count = channels[acmod] + lfe;
    AVFormatContext *mux = NULL;
    AVPacket *pkt = NULL;
    AVDictionary *opts = NULL;
    uint8_t *output = NULL;
    CMAudioFormatDescriptionRef result = NULL;
    if (avformat_alloc_output_context2(&mux, NULL, "mp4", NULL) < 0 || !mux)
        goto finish;
    AVStream *stream = avformat_new_stream(mux, NULL);
    if (!stream || avio_open_dyn_buf(&mux->pb) < 0) goto finish;
    stream->time_base = (AVRational){1, sample_rate};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_EAC3;
    stream->codecpar->sample_rate = sample_rate;
    av_channel_layout_default(&stream->codecpar->ch_layout, channel_count);
    av_dict_set(&opts, "movflags", "delay_moov+frag_custom+empty_moov", 0);
    if (avformat_write_header(mux, &opts) < 0) goto finish;
    pkt = av_packet_alloc();
    if (!pkt) goto finish;
    // A second independent access unit terminates the muxer's substream scan.
    // These copies are format probing only; none of this MP4 is played.
    int64_t timestamp = 0;
    for (int repeat = 0; repeat < 2; repeat++) {
        int offset = 0;
        while (offset < size) {
            int group_start = offset;
            if (size - offset < 7 || af_eac3_dependent(data + offset, size - offset) ||
                av_ac3_parse_header(data + offset, size - offset, &bsid, &frame_size) < 0)
                goto finish;
            const int blocks[] = {1, 2, 3, 6};
            int duration = bsid <= 10 ? 1536 : blocks[(data[offset + 4] >> 4) & 3] * 256;
            do {
                if (av_ac3_parse_header(data + offset, size - offset, &bsid, &frame_size) < 0 ||
                    frame_size < 7 || frame_size > size - offset) goto finish;
                offset += frame_size;
            } while (af_eac3_dependent(data + offset, size - offset));
            if (av_new_packet(pkt, offset - group_start) < 0) goto finish;
            memcpy(pkt->data, data + group_start, pkt->size);
            pkt->duration = duration; pkt->pts = pkt->dts = timestamp;
            pkt->flags = AV_PKT_FLAG_KEY;
            timestamp += duration;
            if (av_write_frame(mux, pkt) < 0) goto finish;
            av_packet_unref(pkt);
        }
    }
    if (av_write_trailer(mux) < 0) goto finish;
    int output_size = avio_close_dyn_buf(mux->pb, &output);
    mux->pb = NULL;
    if (output_size <= 0) goto finish;
    const uint8_t *box = output;
    size_t n = output_size;
    const char *path[] = {"moov", "trak", "mdia", "minf", "stbl", "stsd"};
    for (int i = 0; i < MP_ARRAY_SIZE(path); i++) {
        box = af_eac3_box(box, n, path[i], &n);
        if (!box || n < 8) goto finish;
        box += 8; n -= 8;
    }
    if (n < 8 || AV_RB32(box + 4) != 1) goto finish;
    size_t entry_size;
    const uint8_t *entry = af_eac3_box(box + 8, n - 8, "ec-3", &entry_size);
    if (!entry || entry_size < 36) goto finish;
    size_t cookie_size;
    const uint8_t *cookie = af_eac3_box(entry + 36, entry_size - 36, "dec3", &cookie_size);
    if (!cookie || cookie_size < 13 || (cookie[9] & 7)) goto finish;
    // Add the dependent substream's speaker positions to the independent bed.
    // Object count (the JOC extension) is not a speaker/channel count.
    if ((cookie[12] >> 1) & 15) {
        if (cookie_size < 14) goto finish;
        int locations = ((cookie[12] & 1) << 8) | cookie[13];
        const int extra_channels[] = {2, 2, 1, 1, 2, 2, 2, 1, 1};
        for (int i = 0; i < 9; i++) if (locations & (1 << i)) channel_count += extra_channels[i];
    }
    AudioStreamBasicDescription asbd = {
        .mSampleRate = sample_rate, .mFormatID = kAudioFormatEnhancedAC3,
        .mFramesPerPacket = 1536, .mChannelsPerFrame = channel_count,
    };
    NSDictionary *extensions = @{
        (NSString *)kCMFormatDescriptionExtension_VerbatimISOSampleEntry:
            [NSData dataWithBytes:entry length:entry_size]
    };
    // Apple's E-AC-3 cookie includes the complete dec3 box (size and fourcc).
    if (CMAudioFormatDescriptionCreate(NULL, &asbd, 0, NULL, cookie_size, cookie,
            (CFDictionaryRef)extensions, &result) != noErr) result = NULL;
finish:
    av_packet_free(&pkt);
    av_dict_free(&opts);
    if (mux && mux->pb) { uint8_t *unused = NULL; avio_close_dyn_buf(mux->pb, &unused); av_free(unused); }
    avformat_free_context(mux);
    av_free(output);
    return result;
}
