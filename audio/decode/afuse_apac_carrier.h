// Private libmpv transport, never an IEC burst or HDMI bitstream. One APAC
// access unit retains its 1024-sample duration through MPV's unmodified clock.
#pragma once
#include <stdint.h>
#include <string.h>
#define AF_APAC_FRAMES 1024
#define AF_APAC_STRIDE 32
#define AF_APAC_BYTES (AF_APAC_FRAMES * AF_APAC_STRIDE)
#define AF_APAC_COOKIE_MAX 1024
struct af_apac_header {
    uint8_t magic[8];
    uint32_t version, payload_size, cookie_size, channels;
    uint32_t rate, leading_frames;
};
static const uint8_t af_apac_magic[8] = {'A','F','A','P','A','C',1,0};
static inline bool af_apac_sync(const void *data) {
    return !memcmp(data, af_apac_magic, sizeof(af_apac_magic));
}
