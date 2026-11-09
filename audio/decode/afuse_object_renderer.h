// Spatial bed renderer, adapted from AetherEngine's AllocentricPanner and
// ObjectAudioRenderer (Vincent Herbst, 2026), LGPL-3.0 with App Store exception.
// See licenses/AetherEngine-LICENSE. This is rendered 7.1.4, not object passthrough.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "truehd_atmos.h"

#define AF_OBJECT_CHANNELS 12
struct af_object_renderer {
    float current[16][12], target[16][12];
    uint32_t remaining[16];
    bool known[16];
    uint32_t serial, count;
    float envelope;
};

// CoreAudio Atmos 7.1.4 order: L R C LFE Ls Rs Lrs Rrs Ltf Rtf Ltr Rtr.
static const float af_speaker_positions[12][3] = {
    {0,0,0}, {1,0,0}, {.5,0,0}, {.5,0,0}, {0,.5,0}, {1,.5,0},
    {0,1,0}, {1,1,0}, {.25,.25,1}, {.75,.25,1}, {.25,.75,1}, {.75,.75,1}
};
static inline float af_clamp(float f, float lo, float hi) {
    return fminf(hi, fmaxf(lo, f));
}

// Balance over the two distinct coordinate planes/rows/columns bracketing p.
static void af_pan_axis(const float p[3], uint16_t mask, int axis, float gain, float out[12]) {
    if (axis < 0) {
        for (int c = 0; c < 12; c++) if (mask & (1 << c)) out[c] += gain;
        return;
    }
    float lo = -2, hi = 2, minimum = 2, maximum = -2;
    for (int c = 0; c < 12; c++) if (mask & (1 << c)) {
        float v = af_speaker_positions[c][axis];
        minimum = fminf(minimum, v); maximum = fmaxf(maximum, v);
        if (v <= p[axis]) lo = fmaxf(lo, v);
        if (v >= p[axis]) hi = fminf(hi, v);
    }
    if (lo == -2) lo = minimum;
    if (hi == 2) hi = maximum;
    uint16_t lower = 0, upper = 0;
    for (int c = 0; c < 12; c++) if (mask & (1 << c)) {
        if (af_speaker_positions[c][axis] == lo) lower |= 1 << c;
        if (af_speaker_positions[c][axis] == hi) upper |= 1 << c;
    }
    if (lo == hi) af_pan_axis(p, lower, axis - 1, gain, out);
    else {
        float t = af_clamp((p[axis] - lo) / (hi - lo), 0, 1) * (float)M_PI_2;
        af_pan_axis(p, lower, axis - 1, gain * cosf(t), out);
        af_pan_axis(p, upper, axis - 1, gain * sinf(t), out);
    }
}

static void af_object_gains(const TrueHDAtmosElement *e, float out[12]) {
    memset(out, 0, 12 * sizeof(float));
    if (!(e->flags & TRUEHD_ATMOS_ELEMENT_ACTIVE)) return;
    if (e->kind == TRUEHD_ATMOS_ELEMENT_LFE) { out[3] = e->gain; return; }
    uint16_t mask = 0xff7; // exclude LFE
    if (e->kind == TRUEHD_ATMOS_ELEMENT_OBJECT) {
        switch (e->zone) {
        case TRUEHD_ATMOS_ZONE_NO_BACK: mask &= ~0xc0; break;
        case TRUEHD_ATMOS_ZONE_NO_SIDES: mask &= ~0x30; break;
        case TRUEHD_ATMOS_ZONE_CENTER_BACK: mask &= ~0x33; break;
        case TRUEHD_ATMOS_ZONE_SCREEN_ONLY: mask &= ~0xf0; break;
        case TRUEHD_ATMOS_ZONE_SURROUND_ONLY: mask &= ~0x7; break;
        }
        if (!(e->flags & TRUEHD_ATMOS_ELEMENT_ELEVATION)) mask &= 0xff;
    }
    if (!mask) mask = 0xff7;
    float p[3];
    for (int a = 0; a < 3; a++) p[a] = af_clamp(e->position[a], 0, 1);
    if (e->flags & TRUEHD_ATMOS_ELEMENT_SCREEN_REF) p[2] *= .5f;
    if (e->flags & TRUEHD_ATMOS_ELEMENT_SNAP) {
        int nearest = -1; float distance = INFINITY;
        for (int c = 0; c < 12; c++) if (mask & (1 << c)) {
            float d = 0;
            for (int a = 0; a < 3; a++) d += powf(p[a] - af_speaker_positions[c][a], 2);
            if (d < distance) { distance = d; nearest = c; }
        }
        if (nearest >= 0) out[nearest] = e->gain;
        return;
    }
    float size = af_clamp(fmaxf(e->size[0], fmaxf(e->size[1], e->size[2])), 0, 1);
    if (e->kind != TRUEHD_ATMOS_ELEMENT_OBJECT || size <= .001f) {
        af_pan_axis(p, mask, 2, e->gain, out);
        return;
    }
    int steps = size < .2f ? 3 : 5;
    for (int x = 0; x < steps; x++) for (int y = 0; y < steps; y++) for (int z = 0; z < steps; z++) {
        int grid[3] = {x,y,z}; float point[3], gains[12] = {0};
        for (int a = 0; a < 3; a++) point[a] = af_clamp(p[a] + ((float)grid[a] / (steps - 1) * 2 - 1) * size, 0, 1);
        af_pan_axis(point, mask, 2, 1, gains);
        for (int c = 0; c < 12; c++) out[c] += gains[c] * gains[c];
    }
    for (int c = 0; c < 12; c++) out[c] = sqrtf(out[c] / (steps * steps * steps)) * e->gain;
}

static bool af_object_update(struct af_object_renderer *r, const TrueHDAtmosElement *elements,
                             unsigned count, unsigned ramp, bool first) {
    if (count != r->count || count > 16) return false;
    for (int e = 0; e < count; e++) {
        if (elements[e].kind == TRUEHD_ATMOS_ELEMENT_ISF) return false;
        if (!first && !(elements[e].flags & TRUEHD_ATMOS_ELEMENT_CHANGED)) continue;
        af_object_gains(&elements[e], r->target[e]);
        for (int c = 0; c < 12; c++) if (!isfinite(r->target[e][c])) return false;
        r->remaining[e] = r->known[e] && !first ? ramp : 0;
        if (!r->remaining[e]) memcpy(r->current[e], r->target[e], sizeof(r->current[e]));
        r->known[e] = true;
    }
    return true;
}

static bool af_object_render(struct af_object_renderer *r, const float *const *input,
                             unsigned frames, const TrueHDAtmosMetadata *metadata, float *out[12]) {
    unsigned update = 0;
    for (int c = 0; c < 12; c++) memset(out[c], 0, frames * sizeof(float));
    for (unsigned n = 0; n < frames; n++) {
        while (metadata && update < metadata->update_count && metadata->updates[update].frame_offset <= n) {
            const TrueHDAtmosMetadataUpdate *u = &metadata->updates[update++];
            if (!af_object_update(r, u->elements, metadata->element_count, u->ramp_frames, false)) return false;
        }
        for (unsigned e = 0; e < r->count; e++) {
            float sample = input[e][n];
            if (!isfinite(sample)) return false;
            for (int c = 0; c < 12; c++) {
                if (r->remaining[e]) r->current[e][c] += (r->target[e][c] - r->current[e][c]) / r->remaining[e];
                out[c][n] += sample * r->current[e][c];
            }
            if (r->remaining[e]) r->remaining[e]--;
        }
        // Linked peak limiter preserves the image and introduces no lookahead.
        float peak = 0;
        for (int c = 0; c < 12; c++) peak = fmaxf(peak, fabsf(out[c][n]));
        r->envelope = fmaxf(peak, r->envelope * .99991667f); // 250 ms at 48 kHz
        if (r->envelope > .98f) for (int c = 0; c < 12; c++) out[c][n] *= .98f / r->envelope;
    }
    return true;
}
