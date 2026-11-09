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

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <android/native_window_jni.h>
#include <string.h>

#include "common/common.h"
#include "misc/jni.h"
#include "sub/osd.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    jobject host;
    jobject video_surface;
    jmethodID geometry, visibility;
    ANativeWindow *captions;
    struct mp_osd_res osd;
    int64_t osd_change;
    int64_t submitted, osd_updates;
    double last_pts;
    bool failed, visible, paused, decoder_failed;
};

static bool java_ok(struct vo *vo, JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env))
        return true;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    ((struct priv *)vo->priv)->failed = true;
    MP_ERR(vo, "Native Dolby Vision Surface operation failed.\n");
    return false;
}

static void visible(struct vo *vo, bool show)
{
    struct priv *p = vo->priv;
    if (p->visible == show)
        return;
    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (env && p->host && p->visibility) {
        (*env)->CallVoidMethod(env, p->host, p->visibility, (jboolean)show);
        if (java_ok(vo, env))
            p->visible = show;
    }
}

static bool init_host(struct vo *vo)
{
    struct priv *p = vo->priv;
    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (!env || !vo->opts->android_native_dolby_surface)
        return false;
    p->host = (*env)->NewGlobalRef(env,
                    (jobject)(intptr_t)vo->opts->android_native_dolby_surface);
    if (!p->host)
        return false;
    jclass cls = (*env)->GetObjectClass(env, p->host);
    jmethodID video = (*env)->GetMethodID(env, cls, "getVideoSurface", "()Landroid/view/Surface;");
    jmethodID captions = (*env)->GetMethodID(env, cls, "getCaptionSurface", "()Landroid/view/Surface;");
    p->geometry = (*env)->GetMethodID(env, cls, "setGeometry", "(IIIIIIIIIII)V");
    p->visibility = (*env)->GetMethodID(env, cls, "setVisible", "(Z)V");
    (*env)->DeleteLocalRef(env, cls);
    if (!java_ok(vo, env) || !video || !captions || !p->geometry || !p->visibility)
        return false;
    jobject surface = (*env)->CallObjectMethod(env, p->host, video);
    if (!java_ok(vo, env) || !surface)
        return false;
    p->video_surface = (*env)->NewGlobalRef(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    surface = (*env)->CallObjectMethod(env, p->host, captions);
    if (!java_ok(vo, env) || !surface)
        return false;
    p->captions = ANativeWindow_fromSurface(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    if (!p->video_surface || !p->captions)
        return false;
    p->osd_change = -1;
    return true;
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    struct priv *p = vo->priv;
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;
    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = p->video_surface ? p->video_surface :
                    (void *)(intptr_t)vo->opts->WinID;
    if (!hwctx->surface || hwctx->surface == (void *)(intptr_t)-1 ||
        av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);
    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->last_pts = MP_NOPTS_VALUE;
    if (!strcmp(vo->driver->name, "android-native-dolby") &&
        !vo->opts->android_native_dolby)
        return -1;
    if (vo->opts->android_native_dolby && !init_host(vo)) {
        MP_ERR(vo, "Native Dolby Vision requires the video/caption Surface host.\n");
        return -1;
    }
    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };
    if (!p->hwctx.av_device_ref)
        return -1;
    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;
}

static void update_geometry(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->host || !vo->params)
        return;
    int w = vo->opts->android_surface_size.w;
    int h = vo->opts->android_surface_size.h;
    if (w <= 0 || h <= 0)
        return;
    vo->dwidth = w;
    vo->dheight = h;
    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd);
    // mpv's crop is in rotated image space; SurfaceControl expects buffer space.
    mp_rect_rotate(&src, vo->params->w, vo->params->h,
                   (360 - vo->params->rotate) % 360);
    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (!env)
        return;
    (*env)->CallVoidMethod(env, p->host, p->geometry,
        src.x0, src.y0, src.x1, src.y1, dst.x0, dst.y0, dst.x1, dst.y1,
        vo->params->rotate, w, h);
    java_ok(vo, env);
    if (ANativeWindow_setBuffersGeometry(p->captions, w, h, WINDOW_FORMAT_RGBA_8888) < 0)
        p->failed = true;
    p->osd_change = -1;
    vo_event(vo, VO_EVENT_RESIZE);
}

/* Premultiplied BGRA subtitles stay on their own SDR layer. Never paint them
 * into Dolby decoder buffers or replace the decoder's dynamic metadata. */
static void draw_captions(struct vo *vo, double pts)
{
    struct priv *p = vo->priv;
    if (!p->captions || p->osd.w <= 0 || p->osd.h <= 0)
        return;
    const bool formats[SUBBITMAP_COUNT] = {[SUBBITMAP_BGRA] = true};
    struct sub_bitmap_list *list = osd_render(vo->osd, p->osd, pts, 0, formats);
    if (list->change_id == p->osd_change) {
        talloc_free(list);
        return;
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(p->captions, &buffer, NULL) < 0) {
        p->failed = true;
        talloc_free(list);
        return;
    }
    for (int y = 0; y < buffer.height; y++)
        memset((uint8_t *)buffer.bits + y * buffer.stride * 4, 0, buffer.width * 4);
    for (int i = 0; i < list->num_items; i++) {
        struct sub_bitmaps *images = list->items[i];
        for (int j = 0; j < images->num_parts; j++) {
            struct sub_bitmap *b = &images->parts[j];
            if (b->w <= 0 || b->h <= 0 || b->dw <= 0 || b->dh <= 0)
                continue;
            for (int y = MPMAX(0, b->y); y < MPMIN(buffer.height, b->y + b->dh); y++) {
                int sy = (int64_t)(y - b->y) * b->h / b->dh;
                const uint8_t *row = (uint8_t *)b->bitmap + sy * b->stride;
                uint8_t *dest = (uint8_t *)buffer.bits + y * buffer.stride * 4;
                for (int x = MPMAX(0, b->x); x < MPMIN(buffer.width, b->x + b->dw); x++) {
                    int sx = (int64_t)(x - b->x) * b->w / b->dw;
                    const uint8_t *s = row + sx * 4;
                    uint8_t *d = dest + x * 4;
                    int inverse = 255 - s[3];
                    d[0] = s[2] + (d[0] * inverse + 127) / 255;
                    d[1] = s[1] + (d[1] * inverse + 127) / 255;
                    d[2] = s[0] + (d[2] * inverse + 127) / 255;
                    d[3] = s[3] + (d[3] * inverse + 127) / 255;
                }
            }
        }
    }
    if (ANativeWindow_unlockAndPost(p->captions) < 0) {
        p->failed = true;
    } else {
        p->osd_change = list->change_id;
        p->osd_updates++;
    }
    talloc_free(list);
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;
    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    if (!buffer) {
        p->failed = true;
        mp_image_unrefp(&p->next_image);
        return;
    }
    if (av_mediacodec_release_buffer(buffer, 1) < 0)
        p->failed = true;
    else {
        p->submitted++;
        visible(vo, true);
    }
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);
    if (!frame->redraw && !frame->repeat)
        p->next_image = mp_image_new_ref(frame->current);
    if (frame->current)
        p->last_pts = frame->current->pts;
    if (p->last_pts != MP_NOPTS_VALUE)
        draw_captions(vo, p->last_pts);
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_PREPARE_DECODER_UNINIT:
        if (!p->host)
            return VO_NOTIMPL;
        mp_image_unrefp(&p->next_image);
        p->last_pts = MP_NOPTS_VALUE;
        return VO_TRUE;
    case VOCTRL_ANDROID_NATIVE_DOLBY_DECODER_FAILED:
        p->decoder_failed = true;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        p->paused = true;
        return VO_TRUE;
    case VOCTRL_RESUME:
        p->paused = false;
        return VO_TRUE;
    case VOCTRL_CHECK_EVENTS:
        // NORETAIN forbids re-submitting a MediaCodec buffer. Captions can
        // still redraw independently at the last video PTS while paused.
        if (p->paused && p->last_pts != MP_NOPTS_VALUE)
            draw_captions(vo, p->last_pts);
        return VO_TRUE;
    case VOCTRL_EXTERNAL_RESIZE:
    case VOCTRL_SET_PANSCAN:
    case VOCTRL_VO_OPTS_CHANGED:
        update_geometry(vo);
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_GET_ANDROID_NATIVE_DOLBY:
        *(struct voctrl_android_native_dolby *)data = (struct voctrl_android_native_dolby){
            .active = p->host && p->submitted > 0 && !p->failed && !p->decoder_failed,
            .failed = p->failed,
            .decoder_failed = p->decoder_failed,
            .frames = p->submitted,
            .osd_updates = p->osd_updates,
        };
        return VO_TRUE;
    case VOCTRL_RESET:
        mp_image_unrefp(&p->next_image);
        p->last_pts = MP_NOPTS_VALUE;
        p->osd_change = -1;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    update_geometry(vo);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    visible(vo, false);
    mp_image_unrefp(&p->next_image);
    if (vo->hwdec_devs && p->hwctx.av_device_ref)
        hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->captions)
        ANativeWindow_release(p->captions);
    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (env) {
        if (p->video_surface) (*env)->DeleteGlobalRef(env, p->video_surface);
        if (p->host) (*env)->DeleteGlobalRef(env, p->host);
    }
}

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};

const struct vo_driver video_out_android_native_dolby = {
    .description = "Android native Dolby Vision with a separate caption Surface",
    .name = "android-native-dolby",
    .caps = VO_CAP_NORETAIN | VO_CAP_ROTATE90,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
