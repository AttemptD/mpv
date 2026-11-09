#include "config.h"
#include "libmpv_apple.h"
#include "mpv/client.h"
#include "ta/ta_talloc.h"
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dovi_meta.h>
#if HAVE_DARWIN
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>
#endif

struct native_frame_owner {
    struct mp_image *image;
    struct sub_bitmap_list *captions;
    uint8_t *rpu;
    void *format;
};

void *mp_apple_native_format(struct mp_image *img)
{
#if HAVE_DARWIN
    if (img && img->imgfmt != IMGFMT_VIDEOTOOLBOX) img = img->apple_native;
    if (img && img->imgfmt == IMGFMT_VIDEOTOOLBOX && img->planes[3])
        return (void *)CVBufferGetAttachment((CVPixelBufferRef)img->planes[3],
                                            CFSTR("AfuseNativeDolbyFormat"), NULL);
#endif
    return NULL;
}

static void release_frame(void *opaque)
{
    struct native_frame_owner *owner = opaque;
    mp_image_unrefp(&owner->image);
    talloc_free(owner->captions);
    av_free(owner->rpu);
#if HAVE_DARWIN
    if (owner->format) CFRelease(owner->format);
#endif
    talloc_free(owner);
}

int mp_apple_native_render(mpv_render_apple_frame *out, struct mp_image *img,
                          struct mp_rect src, struct mp_rect dst,
                          struct osd_state *osd, struct mp_osd_res osd_res)
{
    if (!img || img->enhancement_layer)
        return MPV_ERROR_NOT_IMPLEMENTED;
    struct mp_image *native = img->imgfmt == IMGFMT_VIDEOTOOLBOX ? img : img->apple_native;
    if (!native) return MPV_ERROR_NOT_IMPLEMENTED;
    void *format = mp_apple_native_format(native);
    if (!format) return MPV_ERROR_NOT_IMPLEMENTED;
    struct native_frame_owner *owner = talloc_zero(NULL, struct native_frame_owner);
    owner->image = mp_image_new_ref(native);
    out->pixel_buffer = owner->image->planes[3];
    out->source_format = format;
    out->owner = owner; out->release = release_frame;
    mp_apple_native_geometry(out, img, src, dst, osd, osd_res);
    return 0;
}

void mp_apple_native_geometry(mpv_render_apple_frame *out, struct mp_image *img,
                             struct mp_rect src, struct mp_rect dst,
                             struct osd_state *osd, struct mp_osd_res osd_res)
{
    struct native_frame_owner *owner = out->owner;
    out->src_x = src.x0; out->src_y = src.y0;
    out->src_w = mp_rect_w(src); out->src_h = mp_rect_h(src);
    out->dst_x = dst.x0; out->dst_y = dst.y0;
    out->dst_w = mp_rect_w(dst); out->dst_h = mp_rect_h(dst);
    out->rotation = img->params.rotate;
    out->pts = img->pts;
    const bool formats[SUBBITMAP_COUNT] = {
        [SUBBITMAP_LIBASS] = true, [SUBBITMAP_BGRA] = true,
    };
    owner->captions = osd ? osd_render(osd, osd_res, img->pts, OSD_DRAW_SUB_ONLY, formats) : NULL;
    if (owner->captions) {
        out->subtitle_change_id = owner->captions->change_id;
        for (int i = 0; i < owner->captions->num_items; i++)
            out->subtitle_count += owner->captions->items[i]->num_parts;
        out->subtitles = talloc_array(owner, mpv_apple_subtitle_part, out->subtitle_count);
        int index = 0;
        for (int i = 0; i < owner->captions->num_items; i++) {
            struct sub_bitmaps *group = owner->captions->items[i];
            for (int j = 0; j < group->num_parts; j++) {
                struct sub_bitmap *part = &group->parts[j];
                out->subtitles[index++] = (mpv_apple_subtitle_part){
                    .pixels = part->bitmap, .stride = part->stride,
                    .w = part->w, .h = part->h, .x = part->x, .y = part->y,
                    .dw = part->dw, .dh = part->dh,
                    .ass_mask = group->format == SUBBITMAP_LIBASS,
                    .color = group->format == SUBBITMAP_LIBASS ? part->libass.color : 0,
                };
            }
        }
    }
}

#if HAVE_DARWIN
static const AVDOVIMetadata *frame_metadata(struct mp_image *img)
{
    if (!img) return NULL;
    for (int i = 0; i < img->num_ff_side_data; i++) {
        struct mp_ff_side_data *sd = &img->ff_side_data[i];
        if (sd->type == AV_FRAME_DATA_DOVI_METADATA)
            return (const AVDOVIMetadata *)sd->buf->data;
    }
    return NULL;
}

static CVPixelBufferRef native_pixels(struct mp_image *img)
{
    if (!img) return NULL;
    struct mp_image *native = img->imgfmt == IMGFMT_VIDEOTOOLBOX ? img : img->apple_native;
    return native ? (CVPixelBufferRef)native->planes[3] : NULL;
}

static void *p7_display_format(int width, int height)
{
    // P7 is reconstructed by the host, then described as 8.1 (identity
    // reshaping, no EL in the output). This describes display criteria only.
    const uint8_t config[24] = {1, 0, 8 << 1, (9 << 3) | 5, 1 << 4};
    CFDataRef data = CFDataCreate(NULL, config, sizeof(config));
    const void *atom_keys[] = { CFSTR("dvvC") };
    const void *atom_values[] = { data };
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, atom_keys, atom_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = { kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
        kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey, kCVImageBufferYCbCrMatrixKey };
    const void *values[] = { atoms, kCVImageBufferColorPrimaries_ITU_R_2020,
        kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ, kCVImageBufferYCbCrMatrix_ITU_R_2020 };
    CFDictionaryRef ext = CFDictionaryCreate(NULL, keys, values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = NULL;
    CMVideoFormatDescriptionCreate(NULL, 'dvh1', width, height, ext, &format);
    CFRelease(ext); CFRelease(atoms); CFRelease(data);
    return (void *)format;
}
#endif

int mp_apple_p7_prepare(mpv_render_apple_p7_frame *out, struct mp_image *img)
{
#if HAVE_DARWIN
    CVPixelBufferRef bl = native_pixels(img);
    CVPixelBufferRef el = native_pixels(img ? img->enhancement_layer : NULL);
    const AVDOVIMetadata *metadata = frame_metadata(img);
    if (!metadata && img) metadata = frame_metadata(img->enhancement_layer);
    if (!bl || !metadata || CVPixelBufferGetPixelFormatType(bl) !=
        kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
        return MPV_ERROR_NOT_IMPLEMENTED;
    const AVDOVIRpuDataHeader *h = av_dovi_get_header(metadata);
    const AVDOVIDataMapping *m = av_dovi_get_mapping(metadata);
    if (h->rpu_type != 2 || h->bl_bit_depth != 10 || h->bl_video_full_range_flag ||
        h->vdr_bit_depth < 10 || h->vdr_bit_depth > 16 || h->coef_log2_denom > 30 ||
        m->mapping_color_space || m->mapping_chroma_format_idc ||
        m->num_x_partitions != 1 || m->num_y_partitions != 1)
        return MPV_ERROR_NOT_IMPLEMENTED;
    bool residual = !h->disable_residual_flag;
    if (residual && m->nlq_method_idc != AV_DOVI_NLQ_LINEAR_DZ)
        return MPV_ERROR_NOT_IMPLEMENTED;
    bool trivial = true;
    for (int c = 0; c < 3; c++) {
        const AVDOVINLQParams *nlq = &m->nlq[c];
        trivial &= nlq->nlq_offset == 0 && nlq->vdr_in_max == (UINT64_C(1) << h->coef_log2_denom) &&
                   !nlq->linear_deadzone_slope && !nlq->linear_deadzone_threshold;
    }
    residual &= !trivial;
    if (residual && !el)
        return MPV_ERROR_NOT_IMPLEMENTED; // Never silently discard FEL.
    float el_texture_scale = 0;
    if (el) {
        OSType type = CVPixelBufferGetPixelFormatType(el);
        if (type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange && h->el_bit_depth == 10)
            el_texture_scale = 65535.0f / 64;
        else if (type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && h->el_bit_depth == 8)
            el_texture_scale = 255;
        else if (residual) return MPV_ERROR_NOT_IMPLEMENTED;
    }
    mpv_apple_p7_params p = {.base_scale = 1024, .vdr_scale = 1 << h->vdr_bit_depth,
        .enhancement_texture_scale = el_texture_scale, .residual = residual};
    double coef_scale = ldexp(1.0, -h->coef_log2_denom);
    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *in = &m->curves[c];
        mpv_apple_p7_curve *curve = &p.curves[c];
        if (in->num_pivots < 2 || in->num_pivots > 9) return MPV_ERROR_NOT_IMPLEMENTED;
        curve->count = in->num_pivots;
        for (int n = 0; n < in->num_pivots; n++) {
            if (in->pivots[n] > 1023 || (n && in->pivots[n] <= in->pivots[n-1]))
                return MPV_ERROR_NOT_IMPLEMENTED;
            curve->pivots[n] = in->pivots[n] / 1024.0f;
        }
        for (int n = 0; n < in->num_pivots - 1; n++) {
            curve->method[n] = in->mapping_idc[n];
            if (in->mapping_idc[n] == AV_DOVI_MAPPING_POLYNOMIAL) {
                if (in->poly_order[n] < 1 || in->poly_order[n] > 2) return MPV_ERROR_NOT_IMPLEMENTED;
                for (int k = 0; k <= in->poly_order[n]; k++)
                    curve->polynomial[n][k] = in->poly_coef[n][k] * coef_scale;
            } else if (in->mapping_idc[n] == AV_DOVI_MAPPING_MMR) {
                if (in->mmr_order[n] < 1 || in->mmr_order[n] > 3) return MPV_ERROR_NOT_IMPLEMENTED;
                curve->order[n] = in->mmr_order[n];
                curve->constant[n] = in->mmr_constant[n] * coef_scale;
                for (int o = 0; o < in->mmr_order[n]; o++)
                    for (int k = 0; k < 7; k++) curve->mmr[n][o][k] = in->mmr_coef[n][o][k] * coef_scale;
            } else return MPV_ERROR_NOT_IMPLEMENTED;
        }
        p.nlq[c][0] = m->nlq[c].nlq_offset;
        p.nlq[c][1] = m->nlq[c].linear_deadzone_slope * coef_scale * p.vdr_scale / 2;
        p.nlq[c][2] = m->nlq[c].linear_deadzone_threshold * coef_scale * p.vdr_scale;
        p.nlq[c][3] = floor(m->nlq[c].vdr_in_max * coef_scale * p.vdr_scale);
    }
    struct native_frame_owner *owner = talloc_zero(NULL, struct native_frame_owner);
    int result = av_dovi_generate_apple_profile8(metadata, &owner->rpu, &out->rpu_size);
    if (result < 0 || !(owner->format = p7_display_format(img->w, img->h))) {
        release_frame(owner);
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
    owner->image = mp_image_new_ref(img); // Own both matched decoder surfaces.
    out->frame.pixel_buffer = bl;
    out->frame.source_format = owner->format;
    out->frame.owner = owner; out->frame.release = release_frame;
    out->enhancement_buffer = residual ? el : NULL;
    out->params = p; out->rpu = owner->rpu;
    return 0;
#else
    return MPV_ERROR_NOT_IMPLEMENTED;
#endif
}
