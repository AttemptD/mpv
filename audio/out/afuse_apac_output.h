// Included by ao_avfoundation.m. Validate the private carrier before creating
// CoreMedia samples; no APAC bytes are ever interpreted or scaled as PCM.
static bool enqueue_apac(struct ao *ao, const uint8_t *bytes, int count, int64_t pts)
{
    struct priv *p = ao->priv;
    while (count > 0) {
        if (!p->pending_size) p->pending_time = pts;
        int copied = MPMIN(count, AF_APAC_BYTES - p->pending_size);
        memcpy(p->pending + p->pending_size, bytes, copied);
        p->pending_size += copied; bytes += copied; count -= copied;
        pts += CMTimeGetNanoseconds(CMTimeMake(copied / AF_APAC_STRIDE, 48000));
        int skip = 0;
        while (skip + 8 <= p->pending_size && !af_apac_sync(p->pending + skip)) skip += AF_APAC_STRIDE;
        if (skip) {
            memmove(p->pending, p->pending + skip, p->pending_size - skip);
            p->pending_size -= skip;
            p->pending_time += CMTimeGetNanoseconds(CMTimeMake(skip / AF_APAC_STRIDE, 48000));
        }
        if (p->pending_size < AF_APAC_BYTES) continue;
        struct af_apac_header h;
        memcpy(&h, p->pending, sizeof(h));
        p->pending_size = 0;
        p->failure_stage = "APAC private packet";
        if (h.version != 1 || h.channels != 12 || h.rate != 48000 || h.leading_frames > 48000 || !h.payload_size ||
            !h.cookie_size || h.cookie_size > AF_APAC_COOKIE_MAX ||
            (uint64_t)sizeof(h) + h.cookie_size + h.payload_size > AF_APAC_BYTES) return false;
        const void *cookie = p->pending + sizeof(h);
        const void *payload = p->pending + sizeof(h) + h.cookie_size;
        OSStatus err = noErr;
        if (!p->format_description) {
            AudioStreamBasicDescription asbd = {.mFormatID = kAudioFormatAPAC,
                .mSampleRate = 48000, .mChannelsPerFrame = 12, .mFramesPerPacket = AF_APAC_FRAMES};
            AudioChannelLayout layout = {.mChannelLayoutTag = kAudioChannelLayoutTag_Atmos_7_1_4};
            p->failure_stage = "APAC format/dapa";
            err = CMAudioFormatDescriptionCreate(NULL, &asbd, sizeof(layout), &layout,
                h.cookie_size, cookie, NULL, &p->format_description);
            if (err != noErr) { p->failure_status = err; return false; }
            MP_INFO(ao, "AfuseSpatialAPACOutput: rendered 7.1.4, Apple APAC; not original Atmos/DTS:X passthrough\n");
        }
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        size_t size = h.payload_size;
        p->failure_stage = "APAC sample buffer";
        err = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, size, NULL, NULL, 0, size, 0, &block);
        if (err == noErr) err = CMBlockBufferReplaceDataBytes(payload, block, 0, size);
        CMSampleTimingInfo timing = {.duration = CMTimeMake(AF_APAC_FRAMES, 48000),
            // AVSampleBufferAudioRenderer (unlike the HLS container path)
            // does not subtract encoder priming. Present 2048 frames earlier
            // for this encoder, avoiding a persistent ~42.7 ms audio delay.
            .presentationTimeStamp = CMTimeSubtract(CMTimeFromNanoseconds(p->pending_time),
                                                    CMTimeMake(h.leading_frames, h.rate)),
            .decodeTimeStamp = kCMTimeInvalid};
        if (err == noErr) err = CMSampleBufferCreateReady(NULL, block, p->format_description,
            1, 1, &timing, 1, &size, &sample);
        p->failure_status = err;
        if (sample) [p->renderer enqueueSampleBuffer:sample];
        if (sample) CFRelease(sample);
        if (block) CFRelease(block);
        if (err != noErr) return false;
        p->failure_stage = NULL;
    }
    return true;
}
