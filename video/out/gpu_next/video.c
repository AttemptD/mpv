#include "video.h"
#include "mpv/render.h"
#include <limits.h>
#include <stddef.h>                        // for NULL
#include <stdint.h>                        // for uint64_t, uint32_t
#include "common/common.h"                 // for mp_rect, MPMAX, MP_ARRAY_SIZE
#include "common/msg.h"                    // for mp_msg, MSGL_ERR, MSGL_WARN
#include "options/m_config_core.h"
#include "options/options.h"
#include "options/path.h"
#include "stream/stream.h"
#include "video/out/gpu/video.h"
#include "libplacebo/shaders/custom.h"
#include <string.h>
#include "libplacebo/colorspace.h"         // for pl_color_adjustment, pl_co...
#include "libplacebo/filters.h"            // for pl_filter_nearest
#include "libplacebo/gpu.h"                // for pl_tex_params, pl_tex_t
#include "libplacebo/renderer.h"           // for pl_frame_mix, pl_frame
#include "sub/draw_bmp.h"                  // for mp_draw_sub_formats
#include "sub/osd.h"                       // for sub_bitmap, sub_bitmaps
#include "ta/ta_talloc.h"                  // for talloc_free, talloc_zero
#include "video/csputils.h"                // for mp_csp_params, mp_csp_equa...
#include "video/img_format.h"              // for mp_imgfmt
#include "video/mp_image.h"                // for mp_image, mp_image_params
#include "video/out/gpu_next/ra.h"         // for ra_next_find_fmt, ra_next_...
#include "video/out/vo.h"                  // for vo_frame

// Forward declarations
struct mp_log;
struct mpv_global;
struct osd_state;

/**
 * @brief Holds GPU resources for a single piece of the On-Screen Display (OSD).
 *
 * This struct contains a GPU texture (`pl_tex`) and an array of `pl_overlay_part`
 * which define how different parts of the texture should be drawn on the screen.
 * This is used for rendering subtitles and other OSD elements.
 */
struct pl_video_osd_entry {
    pl_tex tex;                     // The GPU texture containing the bitmap for this OSD part.
    struct pl_overlay_part *parts;  // Array of parts describing how to render the texture.
    int num_parts;                  // The number of parts in the array.
};

/**
 * @brief Manages the state for all OSD elements.
 *
 * This struct holds arrays for all possible OSD parts and the corresponding
 * libplacebo overlay structures that will be passed to the renderer.
 */
struct pl_video_osd_state {
    struct pl_video_osd_entry entries[MAX_OSD_PARTS]; // Storage for individual OSD parts.
    struct pl_overlay overlays[MAX_OSD_PARTS];      // The final overlays to be rendered.
};

/**
 * @brief Main structure managing synchronous libplacebo video rendering.
 *
 * This struct encapsulates all state needed for rendering video frames
 * and OSD elements using libplacebo. It includes the frame queue, current
 * video parameters, OSD resources, and color adjustment state.
 */
struct pl_video {
    struct mp_log *log;
    struct ra_next *ra;    // The libplacebo rendering abstraction
    struct pl_frame cached_frame;
    struct mp_image *cached_image;
    uint64_t cached_frame_id;
    struct m_config_cache *authorization;
    bool cached_mapping_access;
    struct mpv_global *global;
    struct m_config_cache *shader_options;
    char **shader_paths;
    int num_shader_paths;
    const struct pl_hook **hooks;
    int num_hooks;

    // Render State
    struct mp_image_params current_params; // Current video parameters (resolution, colorspace, etc.).
    struct mp_rect current_src;            // Source crop selected by mpv's aspect/zoom policy.
    struct mp_rect current_dst;            // The current destination rectangle on the target surface.
    struct osd_state *current_osd_state; // Pointer to the core's logical OSD state.

    // OSD rendering resources
    struct mp_osd_res osd_res;                  // OSD resolution and aspect ratio information.
    struct pl_video_osd_state osd_state_storage; // Storage for our GPU resources.
    pl_fmt osd_fmt[SUBBITMAP_COUNT];             // Cached libplacebo formats for different OSD bitmap types.
    bool osd_bgra_needs_conversion;
    pl_tex *sub_tex; // Texture pool for OSD textures.
    int num_sub_tex; // The number of textures in the pool.

    // Color adjustment state
    struct mp_csp_equalizer_state *video_eq; // Manages brightness, contrast, hue, etc.
};

/**
 * @brief Initializes the synchronous libplacebo rendering engine.
 * @param global The mpv global context.
 * @param log The logging context.
 * @param ra The rendering abstraction context.
 * @return A pointer to the newly created pl_video engine, or NULL on failure.
 */
struct pl_video *pl_video_init(struct mpv_global *global, struct mp_log *log, struct ra_next *ra) {
    struct pl_video *p = talloc_zero(NULL, struct pl_video);
    p->log = log;
    p->ra = ra;
    p->authorization = m_config_cache_alloc(p, global, &vo_sub_opts);
    p->global = global;
    p->shader_options = m_config_cache_alloc(p, global, &gl_video_conf);

    // Subtitle bitmaps carry a defined byte order. In particular, PGS and
    // other image subtitles arrive as premultiplied BGRA. A generic four
    // component lookup commonly returns rgba8 on OpenGL ES, which swaps the
    // red and blue channels (yellow subtitles then appear blue/cyan).
    pl_gpu gpu = ra_get_gpu(p->ra);
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(gpu, "r8");
    p->osd_fmt[SUBBITMAP_BGRA]   = pl_find_named_fmt(gpu, "bgra8");
    if (!p->osd_fmt[SUBBITMAP_BGRA]) {
        // OpenGL ES does not always expose a BGRA upload format. Convert the
        // packed bitmap to RGBA before uploading instead of passing a null
        // format to libplacebo when an image subtitle first appears.
        p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(gpu, "rgba8");
        if (!p->osd_fmt[SUBBITMAP_BGRA])
            p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(gpu, "rgba");
        p->osd_bgra_needs_conversion = p->osd_fmt[SUBBITMAP_BGRA] != NULL;
    }

    // Create the state object that tracks brightness, contrast, etc.
    p->video_eq = mp_csp_equalizer_create(p, global);

    return p;
}

/**
 * @brief Shuts down and destroys the rendering engine and its resources.
 * @param p_ptr A pointer to the pl_video engine pointer to be freed.
 */
void pl_video_uninit(struct pl_video **p_ptr) {
    struct pl_video *p = *p_ptr;
    if (!p) return;

    ra_cleanup_pl_frame(p->ra, &p->cached_frame);
    talloc_free(p->cached_image);

    // Clean up all allocated OSD GPU resources
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state_storage.entries); i++) {
        struct pl_video_osd_entry *entry = &p->osd_state_storage.entries[i];
        ra_next_tex_destroy(p->ra, &entry->tex);
        talloc_free(entry->parts);
    }
    for (int i = 0; i < p->num_sub_tex; i++) {
        ra_next_tex_destroy(p->ra, &p->sub_tex[i]);
    }
    talloc_free(p->sub_tex);
    for (int i = 0; i < p->num_hooks; i++)
        pl_mpv_user_shader_destroy(&p->hooks[i]);

    talloc_free(p);
    *p_ptr = NULL;
}

/**
 * @brief Updates the OSD overlays for the current video frame.
 * @param p The pl_video context.
 * @param res The OSD resolution.
 * @param flags The OSD update flags.
 * @param coords The overlay coordinates.
 * @param state The current OSD state.
 * @param frame The video frame to update.
 * @param src The source image for the frame.
 */
static void update_overlays(struct pl_video *p, struct mp_osd_res res,
                            int flags, enum pl_overlay_coords coords,
                            struct pl_video_osd_state *state, struct pl_frame *frame,
                            struct mp_image *src, bool flip_y)
{
    frame->num_overlays = 0;
    if (!p->current_osd_state)
        return;

    // Render the logical OSD state into a list of bitmaps.
    double pts = src ? src->pts : 0;
    struct sub_bitmap_list *subs = osd_render(p->current_osd_state, res, pts, flags, mp_draw_sub_formats);
    if (!subs) return;

    frame->overlays = state->overlays;

    // Iterate through each bitmap and convert it into a libplacebo overlay.
    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;

        if (item->format <= SUBBITMAP_EMPTY || item->format >= SUBBITMAP_COUNT) {
            MP_ERR(p, "Unsupported OSD bitmap format %d.\n", item->format);
            continue;
        }
        struct pl_video_osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!tex_fmt) {
            MP_ERR(p, "No GPU texture format for OSD bitmap %d.\n", item->format);
            continue;
        }

        const bool convert_bgra = item->format == SUBBITMAP_BGRA &&
                                  p->osd_bgra_needs_conversion;
        uint8_t *pixels = item->packed->planes[0];
        if (!pixels) {
            MP_ERR(p, "OSD bitmap has no pixel data.\n");
            continue;
        }
        int row_pitch = item->packed->stride[0];
        uint8_t *rgba = NULL;
        if (convert_bgra) {
            if (item->packed_w <= 0 || item->packed_h <= 0 ||
                item->packed_w > INT_MAX / 4 ||
                row_pitch < item->packed_w * 4 ||
                (size_t)item->packed_h > SIZE_MAX / ((size_t)item->packed_w * 4)) {
                MP_ERR(p, "Invalid BGRA OSD bitmap dimensions.\n");
                continue;
            }
            row_pitch = item->packed_w * 4;
            rgba = talloc_array(NULL, uint8_t, (size_t)row_pitch * item->packed_h);
            if (!rgba) {
                MP_ERR(p, "Could not allocate RGBA OSD bitmap.\n");
                continue;
            }
            for (int y = 0; y < item->packed_h; y++) {
                const uint8_t *source_row = pixels + (size_t)y * item->packed->stride[0];
                uint8_t *dst = rgba + (size_t)y * row_pitch;
                for (int x = 0; x < item->packed_w; x++) {
                    dst[4 * x + 0] = source_row[4 * x + 2];
                    dst[4 * x + 1] = source_row[4 * x + 1];
                    dst[4 * x + 2] = source_row[4 * x + 0];
                    dst[4 * x + 3] = source_row[4 * x + 3];
                }
            }
            pixels = rgba;
        }

        // Reuse a texture from the pool if available.
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);

        // Recreate the texture if its size needs to change.
        bool ok = ra_next_tex_recreate(p->ra, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            mp_msg(p->log, MSGL_ERR, "Failed recreating OSD texture!\n");
            talloc_free(rgba);
            break;
        }

        // Upload the new bitmap data to the GPU texture.
        ok = ra_next_tex_upload(p->ra, &(struct pl_tex_transfer_params) {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h, },
            .row_pitch  = row_pitch,
            .ptr        = pixels,
        });
        talloc_free(rgba);
        if (!ok) {
            mp_msg(p->log, MSGL_ERR, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        talloc_free(entry->parts);
        entry->parts = talloc_array(p, struct pl_overlay_part, item->num_parts);

        // Convert each sub-bitmap part into a pl_overlay_part.
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                }
            };
            // libmpv's flipped GL target needs the same vertical transform for
            // bitmap overlays as for the video crop. Reverse both endpoints so
            // subtitle pixels and their screen position remain upright.
            if (flip_y) {
                part.dst.y0 = res.h - b->y;
                part.dst.y1 = res.h - b->y - b->dh;
            }
            entry->parts[entry->num_parts++] = part;
        }

        // Create the final pl_overlay structure for rendering.
        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = { .primaries = PL_COLOR_PRIM_BT_709, .transfer = PL_COLOR_TRC_SRGB },
            .coords = coords,
        };

        // Set blending modes based on the OSD bitmap format.
        if (item->format == SUBBITMAP_BGRA) {
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            if (src) ol->color = src->params.color;
        } else if (item->format == SUBBITMAP_LIBASS) {
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            if (src && item->video_color_space) ol->color = src->params.color;
        }
    }

    talloc_free(subs);
}

// The render API backend must consume the same glsl-shaders option as the VO.
// Parse only when paths change, keeping disk I/O and parsing out of steady frames.
static void update_user_shaders(struct pl_video *p)
{
    m_config_cache_update(p->shader_options);
    const struct gl_video_opts *opts = p->shader_options->opts;
    int count = 0;
    while (opts->user_shaders && opts->user_shaders[count] && count < 32)
        count++;
    bool changed = count != p->num_shader_paths;
    for (int i = 0; !changed && i < count; i++)
        changed = strcmp(p->shader_paths[i], opts->user_shaders[i]) != 0;
    if (!changed)
        return;
    for (int i = 0; i < p->num_hooks; i++)
        pl_mpv_user_shader_destroy(&p->hooks[i]);
    talloc_free(p->hooks);
    talloc_free(p->shader_paths);
    p->hooks = talloc_zero_array(p, const struct pl_hook *, count);
    p->shader_paths = talloc_zero_array(p, char *, count);
    p->num_hooks = 0;
    p->num_shader_paths = count;
    for (int i = 0; i < count; i++) {
        p->shader_paths[i] = talloc_strdup(p->shader_paths, opts->user_shaders[i]);
        char *path = mp_get_user_path(NULL, p->global, opts->user_shaders[i]);
        bstr source = stream_read_file(path, NULL, p->global, 16 * 1024 * 1024);
        talloc_free(path);
        const struct pl_hook *hook = source.len
            ? pl_mpv_user_shader_parse(ra_get_gpu(p->ra), source.start, source.len)
            : NULL;
        talloc_free(source.start);
        if (hook)
            p->hooks[p->num_hooks++] = hook;
        else
            MP_WARN(p, "Could not load user shader %d.\n", i + 1);
    }
    MP_VERBOSE(p, "Loaded %d user shader hooks.\n", p->num_hooks);
    ra_pl_reset(p->ra);
}

/**
 * @brief Main rendering function for a single video frame.
 * @param p The pl_video engine context.
 * @param frame The mpv frame to render, containing the current image.
 * @param target_tex The destination GPU texture to render to.
 */
bool pl_video_render(struct pl_video *p, struct vo_frame *frame, pl_tex target_tex,
                     bool flip_y, const struct mpv_render_target_color *target_color,
                     struct mpv_render_dovi_status *status)
{
    if (status) *status = (struct mpv_render_dovi_status){0};
    m_config_cache_update(p->authorization);
    bool mapping_access = ((struct mp_vo_opts *)p->authorization->opts)->libmpv_dovi_mapping;
    // Describe the target surface for libplacebo.
    struct pl_frame target_frame = {
        .num_planes = 1,
        .planes[0] = { .texture = target_tex, .components = 4, .component_mapping = {0,1,2,3} },
        .crop = { .x0 = p->current_dst.x0, .y0 = p->current_dst.y0, .x1 = p->current_dst.x1, .y1 = p->current_dst.y1 },
        .color = pl_color_space_srgb,
        .repr = pl_color_repr_rgb,
    };
    if (target_color && target_color->linear_bt2020) {
        target_frame.color = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer = PL_COLOR_TRC_LINEAR,
            .hdr = {
                .min_luma = 0.0001f,
                .max_luma = MPMIN(10000.0f, MPMAX(203.0f, target_color->peak_nits)),
            },
        };
    }
    if (flip_y) {
        target_frame.crop.y0 = target_tex->params.h - p->current_dst.y0;
        target_frame.crop.y1 = target_tex->params.h - p->current_dst.y1;
    }

    // libmpv hands us an already selected frame. Upload it once per frame ID
    // and reuse the GPU textures for redraws. A frame queue expects strictly
    // monotonic PTS and fails after seeks or resumed streams with discontinuous
    // timestamps, even though libmpv has already selected the correct image.
    if (frame && frame->current &&
        (!p->cached_image || frame->frame_id != p->cached_frame_id ||
         mapping_access != p->cached_mapping_access)) {
        // Reuse compatible BL/EL plane storage; the uploader replaces all
        // image metadata and drops an EL when the next frame has none.
        talloc_free(p->cached_image);
        p->cached_image = mp_image_new_ref(frame->current);
        p->cached_frame_id = frame->frame_id;
        p->cached_mapping_access = mapping_access;
        if (!mapping_access) {
            struct mp_image_params *par = &p->cached_image->params;
            par->no_dovi = par->no_enhancement_layer = true;
            mp_image_params_restore_dovi_mapping(par);
            par->repr.dovi = NULL;
        }
        if (!ra_upload_mp_image(p->ra, &p->cached_frame, p->cached_image)) {
            mp_msg(p->log, MSGL_ERR, "Failed uploading video frame.\n");
            talloc_free(p->cached_image);
            p->cached_image = NULL;
            p->cached_frame = (struct pl_frame){0};
            return false;
        }
        pl_frame_set_chroma_location(&p->cached_frame,
                                     p->cached_image->params.chroma_location);
    }

    // ASS and OSD overlays remain attached to the render target, so they can
    // change without re-uploading the video frame.
    update_overlays(p, p->osd_res, 0, PL_OVERLAY_COORDS_DST_FRAME,
                   &p->osd_state_storage, &target_frame, p->cached_image, flip_y);

    update_user_shaders(p);

    // Prepare the rendering parameters for libplacebo
    struct pl_render_params params = {
        .upscaler = &pl_filter_nearest,
        .downscaler = &pl_filter_nearest,
        .prefer_fel_float32 = true,
        .hooks = p->hooks,
        .num_hooks = p->num_hooks,
    };

    // Declare a local struct to hold the color adjustment values.
    struct pl_color_adjustment color_adj = pl_color_adjustment_neutral;

    // Query the current brightness/contrast/etc values from the equalizer
    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    mp_csp_equalizer_state_get(p->video_eq, &cparams);

    // Fill our local struct with the values.
    color_adj.brightness = cparams.brightness;
    color_adj.contrast   = cparams.contrast;
    color_adj.hue        = cparams.hue;
    color_adj.saturation = cparams.saturation;
    color_adj.gamma      = cparams.gamma;

    // Point the render params' pointer to our local struct.
    params.color_adjustment = &color_adj;

    bool rendered;
    if (p->cached_image) {
        // Keep texture/metadata caching independent of viewport changes. mpv
        // clips the destination to the viewport and supplies the matching
        // source crop; mapping the full image into it would stretch fill/zoom.
        struct pl_frame source_frame = p->cached_frame;
        source_frame.rotation = p->cached_image->params.rotate / 90;
        source_frame.crop = (struct pl_rect2df) {
            .x0 = p->current_src.x0, .y0 = p->current_src.y0,
            .x1 = p->current_src.x1, .y1 = p->current_src.y1,
        };
        // Match vo_gpu_next: mpv's source rect is rotated, whereas libplacebo
        // takes the crop in the original image's coordinates.
        pl_rect2df_rotate(&source_frame.crop, -source_frame.rotation);
        if (source_frame.crop.x1 < source_frame.crop.x0) {
            source_frame.crop.x0 = p->cached_image->w - source_frame.crop.x0;
            source_frame.crop.x1 = p->cached_image->w - source_frame.crop.x1;
        }
        if (source_frame.crop.y1 < source_frame.crop.y0) {
            source_frame.crop.y0 = p->cached_image->h - source_frame.crop.y0;
            source_frame.crop.y1 = p->cached_image->h - source_frame.crop.y1;
        }
        rendered = ra_next_render_image(p->ra, &source_frame,
                                        &target_frame, &params);
    } else {
        struct pl_frame_mix empty = {0};
        rendered = ra_next_render_image_mix(p->ra, &empty,
                                            &target_frame, &params);
    }
    if (!rendered) {
        mp_msg(p->log, MSGL_ERR, "Rendering failed.\n");
    }
    if (rendered && status && p->cached_image) {
        const struct pl_frame *bl = &p->cached_frame;
        const struct pl_frame *el = bl->enhancement_layer;
        *status = (struct mpv_render_dovi_status){
            .mapped = bl->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && bl->repr.dovi,
            .enhancement_layer = el != NULL,
            .residual = el && bl->repr.dovi && bl->repr.dovi->nlq_active,
            .base_bit_depth = bl->repr.bits.color_depth,
            .enhancement_bit_depth = el ? el->repr.bits.color_depth : 0,
        };
    }
    return rendered;
}

 /**
  * @brief Takes a screenshot of the current video frame.
  * @param ctx The render_backend context.
  * @param frame The video frame to capture.
  * @param args The screenshot arguments.
  */
struct mp_image *pl_video_screenshot(struct pl_video *p, struct vo_frame *frame)
{
    if (!p || !p->ra || !frame || !frame->current) {
        mp_msg(p->log, MSGL_WARN, "pl_video_screenshot: invalid arguments\n");
        return NULL;
    }

    struct mp_image *res = NULL;
    struct pl_frame source_frame = {0};
    pl_tex fbo = NULL;

    struct mp_image *source_image = mp_image_new_ref(frame->current);
    if (!source_image) return NULL;
    m_config_cache_update(p->authorization);
    if (!((struct mp_vo_opts *)p->authorization->opts)->libmpv_dovi_mapping) {
        source_image->params.no_dovi = source_image->params.no_enhancement_layer = true;
        mp_image_params_restore_dovi_mapping(&source_image->params);
        source_image->params.repr.dovi = NULL;
    }
    // Screenshots obey the same native authorization as displayed frames.
    if (!ra_upload_mp_image(p->ra, &source_frame, source_image)) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: failed to upload source image\n");
        talloc_free(source_image);
        return NULL;
    }

    // Find an 8-bit RGBA renderable + host-readable format.
    pl_fmt fbo_fmt = ra_next_find_fmt(p->ra, PL_FMT_UNORM, 4, 8, 8,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    if (!fbo_fmt) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: failed to find screenshot format\n");
        goto done;
    }

    // Create a temporary renderable, host-readable texture sized to the source.
    int w = frame->current->w;
    int h = frame->current->h;
    fbo = ra_next_tex_create(p->ra, pl_tex_params(
        .w = w,
        .h = h,
        .format = fbo_fmt,
        .renderable = true,
        .host_readable = true
    ));
    if (!fbo) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: failed to create temporary texture\n");
        goto done;
    }

    // Describe the target as an sRGB SDR frame (so libplacebo performs tone-mapping).
    struct pl_frame target_frame = {
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
        .color = pl_color_space_srgb,
        .repr = pl_color_repr_rgb,
    };

    // Define the OSD canvas size to match the video frame for the screenshot.
    struct mp_osd_res osd_res = {
        .w = frame->current->w,
        .h = frame->current->h,
        .display_par = 1.0, // Screenshots have square pixels
    };

    // Generate and attach OSD/subtitle overlays to the screenshot's target frame.
    update_overlays(p, osd_res, 0, PL_OVERLAY_COORDS_DST_FRAME,
                    &p->osd_state_storage, &target_frame, frame->current, false);

    update_user_shaders(p);
    const struct pl_render_params params = {
        .upscaler = &pl_filter_nearest,
        .downscaler = &pl_filter_nearest,
        .prefer_fel_float32 = true,
        .hooks = p->hooks,
        .num_hooks = p->num_hooks,
    };

    if (!ra_next_render_image(p->ra, &source_frame, &target_frame, &params)) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: rendering failed\n");
        goto done;
    }

    // Allocate an mp_image for RGBA result and download the texture into it.
    res = mp_image_alloc(IMGFMT_RGBA, w, h);
    if (!res) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: failed to allocate mp_image\n");
        goto done;
    }

    bool ok = ra_next_tex_download(p->ra, &(struct pl_tex_transfer_params){
        .tex = fbo,
        .ptr = res->planes[0],
        .row_pitch = res->stride[0],
    });

    if (!ok) {
        mp_msg(p->log, MSGL_ERR, "pl_video_screenshot: texture download failed\n");
        talloc_free(res);
        res = NULL;
        goto done;
    }

done:
    if (fbo)
        ra_next_tex_destroy(p->ra, &fbo);

    ra_cleanup_pl_frame(p->ra, &source_frame);
    talloc_free(source_image);

    return res;
}

/**
 * @brief Informs the engine that the video parameters have changed.
 * @param p The pl_video engine context.
 * @param params The new image parameters.
 */
void pl_video_reconfig(struct pl_video *p, const struct mp_image_params *params) {
    if (params)
        p->current_params = *params;
}

/**
 * @brief Informs the engine that the output viewport has been resized.
 * @param p The pl_video engine context.
 * @param src The new source crop rectangle.
 * @param dst The new destination rectangle.
 * @param osd The new OSD resolution information.
 */
void pl_video_resize(struct pl_video *p, const struct mp_rect *src,
                     const struct mp_rect *dst, const struct mp_osd_res *osd) {
    if (src)
        p->current_src = *src;
    if (dst)
        p->current_dst = *dst;
    if (osd)
        p->osd_res = *osd;
}

/**
 * @brief Provides the engine with the current On-Screen Display state.
 * @param p The pl_video engine context.
 * @param osd A pointer to the current OSD state.
 */
void pl_video_update_osd(struct pl_video *p, struct osd_state *osd) {
    p->current_osd_state = osd;
}

/**
 * @brief Informs the engine that it should flush internal caches.
 * @param p The pl_video engine context.
 */
void pl_video_reset(struct pl_video *p) {
    if (!p || !p->ra) return;
    ra_pl_reset(p->ra);
    ra_cleanup_pl_frame(p->ra, &p->cached_frame);
    p->cached_frame = (struct pl_frame){0};
    talloc_free(p->cached_image);
    p->cached_image = NULL;
    p->cached_frame_id = 0;
}

/**
 * @brief Asks the engine if a specific image format is supported.
 * @param p The pl_video engine context.
 * @param imgfmt The image format to check.
 * @return True if the format is likely supported, false otherwise.
 */
bool pl_video_check_format(struct pl_video *p, int imgfmt) {
    // For simplicity, we assume libplacebo can handle it.
    // A more robust implementation might query libplacebo's capabilities.
    return true;
}
