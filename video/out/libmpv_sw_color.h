/* Background-safe HDR/Dolby Vision colour conversion for libmpv's CPU output.
 * The RPU polynomial/MMR, NLQ and LMS transforms follow libplacebo's
 * shaders/colorspace.c (LGPL-2.1-or-later). Reuse its parsed metadata, matrices
 * and tone curves; never reinterpret Profile 5 as ordinary BT.2020 YCbCr.
 */
#include <math.h>
#include <libplacebo/tone_mapping.h>
#include "options/m_config_core.h"
#include "options/options.h"

#define SW_COLOR_LUT 4096

struct sw_color {
    struct mp_sws_context *bl_sws, *el_sws;
    struct mp_image *bl, *el;
    struct m_config_cache *authorization;
    float pq[SW_COLOR_LUT + 1], gamma[SW_COLOR_LUT + 1];
    float pq_encode[SW_COLOR_LUT + 1];
    float *chroma;
    int chroma_width;
    float *subtitle_overlay;
    size_t subtitle_pixels;
    float tone[SW_COLOR_LUT];
    struct pl_tone_map_params tone_params;
    bool initialized;
};

static float sw_color_lookup(const float *lut, int count, float x)
{
    x = fminf(fmaxf(x, 0), 1) * (count - 1);
    int i = fminf(x, count - 2);
    return lut[i] + (lut[i + 1] - lut[i]) * (x - i);
}

static void sw_color_init(struct sw_color *p, struct render_backend *ctx)
{
    p->bl_sws = mp_sws_alloc(ctx->priv);
    p->el_sws = mp_sws_alloc(ctx->priv);
    p->authorization = m_config_cache_alloc(ctx->priv, ctx->global, &vo_sub_opts);
    for (int i = 0; i <= SW_COLOR_LUT; i++) {
        float x = (float)i / SW_COLOR_LUT;
        p->pq[i] = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, x);
        p->pq_encode[i] = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 10000 * x * x);
        p->gamma[i] = x < 0.018f ? 4.5f * x : 1.099f * powf(x, 0.45f) - 0.099f;
    }
    p->initialized = true;
}

static bool sw_color_is_hdr(const struct mp_image *img)
{
    return img && (img->params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION ||
        img->params.color.transfer == PL_COLOR_TRC_PQ ||
        img->params.color.transfer == PL_COLOR_TRC_HLG);
}

static bool sw_color_scale(struct mp_sws_context *sws, struct mp_image **out,
                           const struct mp_image *src, int w, int h)
{
    if (!*out || (*out)->w != w || (*out)->h != h) {
        mp_image_unrefp(out);
        *out = mp_image_alloc(mp_imgfmt_from_name(bstr0("yuv444p16le")), w, h);
    }
    if (!*out)
        return false;
    struct mp_image raw = *src;
    // This pass only resizes the coded Y/U/V planes. In particular, the RPU
    // must run before any YUV->RGB transform or clipping by libswscale.
    raw.params.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
    raw.params.repr.dovi = NULL;
    raw.params.repr.levels = PL_COLOR_LEVELS_FULL;
    struct mp_image_params dst = raw.params;
    dst.imgfmt = mp_imgfmt_from_name(bstr0("yuv444p16le"));
    dst.w = w; dst.h = h;
    mp_image_set_params(*out, &dst);
    return mp_sws_scale(sws, *out, &raw) >= 0;
}

static void sw_color_reshape(const struct pl_dovi_metadata *dovi, float sig[3])
{
    const float x[3] = {fminf(fmaxf(sig[0], 0), 1),
                       fminf(fmaxf(sig[1], 0), 1),
                       fminf(fmaxf(sig[2], 0), 1)};
    for (int c = 0; c < 3; c++) {
        const struct pl_reshape_data *p = &dovi->comp[c];
        if (p->num_pivots < 2 || p->num_pivots > 9)
            continue;
        int segment = 0;
        while (segment < p->num_pivots - 2 && x[c] >= p->pivots[segment + 1])
            segment++;
        float s;
        if (p->method[segment] == 0) {
            const float *coeff = p->poly_coeffs[segment];
            s = (coeff[2] * x[c] + coeff[1]) * x[c] + coeff[0];
        } else {
            float terms[7] = {x[0], x[1], x[2], x[0] * x[1],
                x[0] * x[2], x[1] * x[2], x[0] * x[1] * x[2]};
            float powers[7];
            for (int k = 0; k < 7; k++) powers[k] = terms[k];
            s = p->mmr_constant[segment];
            for (int order = 0; order < MPMIN(p->mmr_order[segment], 3); order++) {
                for (int k = 0; k < 7; k++) {
                    s += p->mmr_coeffs[segment][order][k] * powers[k];
                    powers[k] *= terms[k];
                }
            }
        }
        sig[c] = fminf(fmaxf(s, p->pivots[0]), p->pivots[p->num_pivots - 1]);
    }
}

static float sw_color_hlg(float x)
{
    x = fmaxf(x, 0);
    return x <= 0.5f ? x * x / 3.0f
        : (expf((x - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
}

// Render the same libass/bitmap output as the foreground player. Store
// premultiplied linear BT.2020 in nits so caption white stays at 203 nits;
// compositing must not convert the HDR video to an SDR intermediate.
static const float *sw_color_subtitles(struct sw_color *p, struct render_backend *ctx,
                                      struct sub_bitmap_list *list, int w, int h)
{
    if (!list || !list->num_items)
        return NULL;
    if (!p->initialized) sw_color_init(p, ctx);
    size_t pixels = (size_t)w * h;
    if (p->subtitle_pixels != pixels) {
        talloc_free(p->subtitle_overlay);
        p->subtitle_overlay = talloc_array(ctx->priv, float, pixels * 4);
        p->subtitle_pixels = pixels;
    }
    memset(p->subtitle_overlay, 0, pixels * 4 * sizeof(float));
    pl_matrix3x3 gamut = pl_get_color_mapping_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_BT_709),
        pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020), PL_INTENT_RELATIVE_COLORIMETRIC);
    for (int n = 0; n < list->num_items; n++) {
        struct sub_bitmaps *item = list->items[n];
        if (item->format != SUBBITMAP_LIBASS && item->format != SUBBITMAP_BGRA)
            continue;
        for (int b = 0; b < item->num_parts; b++) {
            const struct sub_bitmap *part = &item->parts[b];
            if (!part->bitmap || part->w <= 0 || part->h <= 0 || part->dw <= 0 || part->dh <= 0)
                continue;
            int x0 = MPMAX(part->x, 0), y0 = MPMAX(part->y, 0);
            int x1 = MPMIN(part->x + part->dw, w), y1 = MPMIN(part->y + part->dh, h);
            for (int y = y0; y < y1; y++) {
                int sy = (int64_t)(y - part->y) * part->h / part->dh;
                const uint8_t *row = (uint8_t *)part->bitmap + sy * part->stride;
                for (int x = x0; x < x1; x++) {
                    int sx = (int64_t)(x - part->x) * part->w / part->dw;
                    float rgb[3], alpha;
                    if (item->format == SUBBITMAP_LIBASS) {
                        uint32_t color = part->libass.color;
                        alpha = row[sx] * (255 - (color & 255)) / 65025.0f;
                        rgb[0] = (color >> 24) / 255.0f;
                        rgb[1] = ((color >> 16) & 255) / 255.0f;
                        rgb[2] = ((color >> 8) & 255) / 255.0f;
                    } else {
                        const uint8_t *pixel = row + sx * 4;
                        alpha = pixel[3] / 255.0f;
                        if (alpha <= 0) continue;
                        rgb[0] = pixel[2] / (255 * alpha);
                        rgb[1] = pixel[1] / (255 * alpha);
                        rgb[2] = pixel[0] / (255 * alpha);
                    }
                    if (alpha <= 0) continue;
                    for (int c = 0; c < 3; c++) {
                        float v = fminf(fmaxf(rgb[c], 0), 1);
                        rgb[c] = 203 * (v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f));
                    }
                    pl_matrix3x3_apply(&gamut, rgb);
                    float *out = p->subtitle_overlay + ((size_t)y * w + x) * 4;
                    for (int c = 0; c < 3; c++) out[c] = rgb[c] * alpha + out[c] * (1 - alpha);
                    out[3] = alpha + out[3] * (1 - alpha);
                }
            }
        }
    }
    return p->subtitle_overlay;
}

static int sw_color_render(struct sw_color *p, struct render_backend *ctx,
                           struct mp_image *dst, const struct mp_image *input,
                           struct mpv_render_dovi_status *status,
                           struct mpv_render_sw_p010 *hdr,
                           const float *subtitles)
{
    if (!hdr && dst->imgfmt != IMGFMT_BGRA && dst->imgfmt != IMGFMT_RGBA &&
        dst->imgfmt != IMGFMT_BGR0 && dst->imgfmt != IMGFMT_RGB0)
        return MPV_ERROR_UNSUPPORTED;
    if (!p->initialized)
        sw_color_init(p, ctx);
    m_config_cache_update(p->authorization);
    bool allowed = ((struct mp_vo_opts *)p->authorization->opts)->libmpv_dovi_mapping;
    struct mp_image src = *input;
    bool dovi = src.params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
                src.params.repr.dovi && !src.params.no_dovi;
    const struct pl_dovi_metadata *rpu = dovi ? src.params.repr.dovi : NULL;
    bool fel = rpu && rpu->nlq_active && src.enhancement_layer &&
               !src.params.no_enhancement_layer;
    if (dovi && (!allowed || (rpu->nlq_active && !fel))) {
        // Match gpu-next: an incomplete FEL reconstructs from its compatible
        // HDR10 base. P5 has no compatible base and must never be mislabelled.
        if (src.params.sys_orig == PL_COLOR_SYSTEM_DOLBYVISION)
            return MPV_ERROR_UNSUPPORTED;
        mp_image_params_restore_dovi_mapping(&src.params);
        dovi = fel = false; rpu = NULL;
    }
    if (!sw_color_scale(p->bl_sws, &p->bl, &src, dst->w, dst->h))
        return MPV_ERROR_GENERIC;
    if (fel && !sw_color_scale(p->el_sws, &p->el, src.enhancement_layer, dst->w, dst->h))
        return MPV_ERROR_GENERIC;

    struct pl_color_repr repr = src.params.repr;
    repr.bits = (struct pl_bit_encoding){.sample_depth = 16, .color_depth = 16};
    pl_transform3x3 decode = pl_color_repr_decode(&repr, NULL);
    pl_matrix3x3 lms = {{{ 3.06441879f, -2.16597676f, 0.10155818f},
                         {-0.65612108f, 1.78554118f, -0.12943749f},
                         { 0.01736321f, -0.04725154f, 1.03004253f}}};
    if (dovi) pl_matrix3x3_mul(&lms, &rpu->linear);
    struct pl_color_space color = src.params.color;
    if (dovi) color.primaries = PL_COLOR_PRIM_BT_2020;
    pl_color_space_infer(&color);
    pl_matrix3x3 gamut = pl_get_color_mapping_matrix(pl_raw_primaries_get(color.primaries),
        pl_raw_primaries_get(hdr ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709), PL_INTENT_RELATIVE_COLORIMETRIC);
    float peak = 0;
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(.color = &color,
        .metadata = PL_HDR_METADATA_ANY, .scaling = PL_HDR_NITS, .out_max = &peak));
    peak = fminf(fmaxf(peak, 203), 10000);
    struct pl_tone_map_params tone = {
        .function = &pl_tone_map_spline, .constants = {PL_TONE_MAP_CONSTANTS},
        .input_scaling = PL_HDR_SQRT, .output_scaling = PL_HDR_NITS,
        .lut_size = SW_COLOR_LUT, .input_max = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_SQRT, peak),
        .output_max = 203, .hdr = color.hdr,
    };
    pl_tone_map_params_infer(&tone);
    if (!hdr && !pl_tone_map_params_equal(&p->tone_params, &tone)) {
        pl_tone_map_generate(p->tone, &tone);
        p->tone_params = tone;
    }
    if (hdr) {
        hdr->peak_nits = peak;
        hdr->max_cll = color.hdr.max_cll > 0 ? color.hdr.max_cll : peak;
        hdr->max_fall = color.hdr.max_fall;
        if (p->chroma_width != dst->w) {
            talloc_free(p->chroma);
            p->chroma = talloc_array(ctx->priv, float, dst->w);
            p->chroma_width = dst->w;
        }
    }
    const bool blue_first = dst->imgfmt == IMGFMT_BGRA || dst->imgfmt == IMGFMT_BGR0;
    for (int y = 0; y < dst->h; y++) {
        const uint16_t *bl[3], *el[3] = {0};
        for (int c = 0; c < 3; c++) {
            bl[c] = (void *)(p->bl->planes[c] + y * p->bl->stride[c]);
            if (fel) el[c] = (void *)(p->el->planes[c] + y * p->el->stride[c]);
        }
        uint8_t *out = dst->planes[0] + y * dst->stride[0];
        if (hdr && !(y & 1)) memset(p->chroma, 0, dst->w * sizeof(float));
        for (int x = 0; x < dst->w; x++) {
            float rgb[3] = {bl[0][x] / 65535.0f, bl[1][x] / 65535.0f, bl[2][x] / 65535.0f};
            if (dovi) {
                sw_color_reshape(rpu, rgb);
                if (fel) {
                    for (int c = 0; c < 3; c++) {
                        float residual = el[c][x] / 65535.0f - rpu->nlq[c].offset;
                        if (residual != 0) rgb[c] += (residual > 0 ? 1 : -1) * (fabsf(residual) *
                            rpu->nlq[c].deadzone_slope + rpu->nlq[c].deadzone_threshold);
                    }
                }
            }
            pl_transform3x3_apply(&decode, rgb);
            for (int c = 0; c < 3; c++) {
                rgb[c] = color.transfer == PL_COLOR_TRC_HLG && !dovi ? sw_color_hlg(rgb[c])
                    : sw_color_lookup(p->pq, SW_COLOR_LUT + 1, rgb[c]);
            }
            if (dovi) pl_matrix3x3_apply(&lms, rgb);
            if (color.transfer == PL_COLOR_TRC_HLG && !dovi) {
                float hlg_luma = fmaxf(0.2627f * rgb[0] + 0.678f * rgb[1] + 0.0593f * rgb[2], 0);
                float ootf = peak * powf(hlg_luma, 0.2f);
                for (int c = 0; c < 3; c++) rgb[c] *= ootf;
            }
            pl_matrix3x3_apply(&gamut, rgb);
            if (hdr) {
                if (subtitles) {
                    const float *caption = subtitles + ((size_t)y * dst->w + x) * 4;
                    for (int c = 0; c < 3; c++) rgb[c] = rgb[c] * (1 - caption[3]) + caption[c];
                }
                for (int c = 0; c < 3; c++) rgb[c] = sw_color_lookup(p->pq_encode, SW_COLOR_LUT + 1,
                    sqrtf(fmaxf(rgb[c], 0) / 10000));
                float y_pq = 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];
                ((uint16_t *)out)[x] = (uint16_t)lrintf(fminf(fmaxf(64 + 876 * y_pq, 64), 940)) << 6;
                int uv = (x / 2) * 2;
                p->chroma[uv] += (rgb[2] - y_pq) / 1.8814f;
                p->chroma[uv + 1] += (rgb[0] - y_pq) / 1.4746f;
                if ((x & 1) && (y & 1)) {
                    uint16_t *uv_row = (void *)(dst->planes[1] + (y / 2) * dst->stride[1]);
                    for (int c = 0; c < 2; c++) uv_row[uv + c] = (uint16_t)lrintf(
                        fminf(fmaxf(512 + 224 * p->chroma[uv + c], 64), 960)) << 6;
                }
                continue;
            }
            float maximum = fmaxf(fmaxf(rgb[0], rgb[1]), rgb[2]);
            float mapped = sw_color_lookup(p->tone, SW_COLOR_LUT, sqrtf(fmaxf(maximum, 0) / peak));
            float gain = maximum > 1e-8f ? mapped / (203 * maximum) : 0;
            for (int c = 0; c < 3; c++) rgb[c] *= gain;
            // Compress out-of-gamut colours towards luminance, preserving hue
            // instead of independently clipping negative wide-gamut channels.
            float luma = fminf(fmaxf(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2], 0), 1);
            float saturation = 1;
            for (int c = 0; c < 3; c++) {
                if (rgb[c] < 0) saturation = fminf(saturation, luma / (luma - rgb[c]));
                if (rgb[c] > 1) saturation = fminf(saturation, (1 - luma) / (rgb[c] - luma));
            }
            for (int c = 0; c < 3; c++) {
                float value = luma + saturation * (rgb[c] - luma);
                uint8_t encoded = lrintf(255 * sw_color_lookup(p->gamma, SW_COLOR_LUT + 1, value));
                out[x * 4 + (blue_first ? 2 - c : c)] = encoded;
            }
            out[x * 4 + 3] = 255;
        }
    }
    if (status) *status = (struct mpv_render_dovi_status){
        .mapped = dovi, .enhancement_layer = fel, .residual = fel,
        .base_bit_depth = input->params.repr.bits.color_depth,
        .enhancement_bit_depth = fel ? input->enhancement_layer->params.repr.bits.color_depth : 0,
    };
    return 0;
}

static int sw_color_render_p010(struct sw_color *p, struct render_backend *ctx,
                                int w, int h, struct mpv_render_sw_p010 *hdr,
                                const struct mp_image *src,
                                struct mpv_render_dovi_status *status,
                                const float *subtitles)
{
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1) || !hdr->planes[0] || !hdr->planes[1] ||
        hdr->strides[0] < (size_t)w * 2 || hdr->strides[1] < (size_t)w * 2)
        return MPV_ERROR_INVALID_PARAMETER;
    struct mp_image dst = {0};
    struct mp_image_params params = {
        .imgfmt = mp_imgfmt_from_name(bstr0("p010le")), .w = w, .h = h,
    };
    mp_image_set_params(&dst, &params);
    for (int c = 0; c < 2; c++) { dst.planes[c] = hdr->planes[c]; dst.stride[c] = hdr->strides[c]; }
    if (!src) {
        mp_image_clear(&dst, 0, 0, w, h);
        return 0;
    }
    return sw_color_render(p, ctx, &dst, src, status, hdr, subtitles);
}
