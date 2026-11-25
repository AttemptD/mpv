#pragma once

#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// Source and destination both use premultiplied alpha. Keep the original
// integer rounding exactly, including when several subtitle parts overlap.
static inline void mp_android_caption_rgba_over(uint8_t *dst, const uint8_t *src, int pixels)
{
    int x = 0;
#if defined(__ARM_NEON)
    for (; x + 8 <= pixels; x += 8) {
        uint8x8x4_t s = vld4_u8(src + 4 * x);
        uint64_t alpha = vget_lane_u64(vreinterpret_u64_u8(s.val[3]), 0);
        if (!alpha) continue;
        if (alpha == UINT64_MAX) {
            vst4_u8(dst + 4 * x, s);
            continue;
        }
        uint8x8x4_t d = vld4_u8(dst + 4 * x);
        uint8x8_t inverse = vmvn_u8(s.val[3]);
        for (int c = 0; c < 4; c++) {
            uint16x8_t value = vaddq_u16(vmull_u8(d.val[c], inverse), vdupq_n_u16(127));
            // floor(value / 255), for the bounded 0..65152 product above.
            value = vaddq_u16(vaddq_u16(value, vdupq_n_u16(1)), vshrq_n_u16(value, 8));
            d.val[c] = vadd_u8(s.val[c], vshrn_n_u16(value, 8));
        }
        vst4_u8(dst + 4 * x, d);
    }
#endif
    for (; x < pixels; x++) {
        const uint8_t *s = src + 4 * x;
        uint8_t *d = dst + 4 * x;
        if (!s[3]) continue;
        if (s[3] == 255) {
            memcpy(d, s, 4);
        } else {
            int inverse = 255 - s[3];
            for (int c = 0; c < 4; c++)
                d[c] = s[c] + (d[c] * inverse + 127) / 255;
        }
    }
}

// Scratch buffers need canvas_width ints and canvas_width * 4 bytes. Resolve
// the horizontal scaling once per part, then reuse scaled rows when upscaling.
// This removes 64-bit division from the pixel loop without changing sampling.
static inline void mp_android_caption_blend_bgra(
    uint8_t *dst, int dst_stride, int canvas_width, int canvas_height,
    const uint8_t *src, int src_stride, int source_width, int source_height,
    int left, int top, int width, int height, int *xmap, uint8_t *rgba)
{
    if (source_width <= 0 || source_height <= 0 || width <= 0 || height <= 0) return;
    int x0 = left > 0 ? left : 0;
    int y0 = top > 0 ? top : 0;
    int x1 = left + width < canvas_width ? left + width : canvas_width;
    int y1 = top + height < canvas_height ? top + height : canvas_height;
    if (x0 >= x1 || y0 >= y1) return;
    int pixels = x1 - x0;
    for (int x = 0; x < pixels; x++)
        xmap[x] = (int64_t)(x0 + x - left) * source_width / width * 4;
    int previous_row = -1;
    for (int y = y0; y < y1; y++) {
        int sy = (int64_t)(y - top) * source_height / height;
        if (sy != previous_row) {
            const uint8_t *row = src + sy * src_stride;
            for (int x = 0; x < pixels; x++) {
                const uint8_t *s = row + xmap[x];
                uint8_t *d = rgba + x * 4;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
            previous_row = sy;
        }
        mp_android_caption_rgba_over(dst + y * dst_stride + x0 * 4, rgba, pixels);
    }
}
