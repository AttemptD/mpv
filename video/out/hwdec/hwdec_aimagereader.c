/*
 * Copyright (c) 2021 sfan5 <sfan5@live.de>
 *
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

#include "video/out/gpu/shader_cache.h"

#include <assert.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkImageReader.h>
#include <android/native_window_jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/opengl.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/lut.h>

#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "options/options.h"
#include "options/m_config_core.h"
#include "video/out/vo.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#if HAVE_EGL_ANDROID
#include "video/out/opengl/android_gl_trace.h"
#endif
#include "video/out/opengl/ra_gl.h"
#include "video/out/placebo/utils.h"
#include "video/out/placebo/color_lut.h"
#include "android_yuv_shaders.h"

typedef void *GLeglImageOES;
#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    AImageReader *reader;
    jobject surface;
    void *lib_handle;
    bool drain_discard;
    struct mp_log *drain_log;
    mp_mutex consumer_lock, event_lock;
    mp_cond image_cond;
    int discarded;

    media_status_t (*AImageReader_newWithUsage)(
        int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    media_status_t (*AImageReader_getWindow)(
        AImageReader *, ANativeWindow **);
    media_status_t (*AImageReader_setImageListener)(
        AImageReader *, AImageReader_ImageListener *);
    media_status_t (*AImageReader_acquireLatestImage)(AImageReader *, AImage **);
    void (*AImageReader_delete)(AImageReader *);
    media_status_t (*AImage_getHardwareBuffer)(const AImage *, AHardwareBuffer **);
    media_status_t (*AImage_getCropRect)(const AImage *, AImageCropRect *);
    void (*AImage_delete)(AImage *);
    void (*AImage_deleteAsync)(AImage *, int);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    jobject (*ANativeWindow_toSurface)(JNIEnv *, ANativeWindow *);
};

struct priv {
    struct mp_log *log;
    struct priv_owner *reader_owner;
    AVBufferRef *stream_device;

    GLuint gl_texture;
    AImage *image;
    EGLImageKHR egl_image;

    mp_mutex lock;
    mp_cond cond;
    bool image_available;

    bool dovi_yuv;
    bool color_lut;
    bool lut_direct;
    bool lut_volume;
    bool lut_hdr;
    pl_gpu lut_gpu;
    pl_log lut_log;
    pl_dispatch lut_dispatch;
    pl_tex lut;
    pl_tex lut_volume_tex;
    void (GLAPIENTRY *CopyTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint,
                                       GLint, GLint, GLsizei, GLsizei);
    pl_shader_obj lut_state;
    int lut_size;
    bool lut_valid, lut_has_dovi;
    int lut_updates;
    struct mp_image_params lut_source;
    struct pl_dovi_metadata lut_dovi;
    struct pl_custom_lut lut_info;
    bool native_yuv420;
    bool yuv_logged;
    bool image_gpu_used;
    bool crop_sdr;
    bool crop_unavailable;
    bool logged_crop_sampling;
    bool logged_visible_copy;
    struct ra_tex *oes_tex;
    struct ra_tex *visible_tex;
    struct gl_shader_cache *visible_shader;
    bool release_sync_logged;
    struct mp_image *cached_source;
    GLuint yuv_program, yuv_fbo, yuv_vao;
    void (GLAPIENTRY *DrawBuffers)(GLsizei, const GLenum *);
    void (GLAPIENTRY *BindSampler)(GLuint, GLuint);
    void (GLAPIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLClientBuffer (EGLAPIENTRY *GetNativeClientBufferANDROID)(
        const struct AHardwareBuffer *);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
    EGLSyncKHR (EGLAPIENTRY *CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (EGLAPIENTRY *DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
};

static const struct { const char *symbol; int offset; } lib_functions[] = {
    { "AImageReader_newWithUsage", offsetof(struct priv_owner, AImageReader_newWithUsage) },
    { "AImageReader_getWindow", offsetof(struct priv_owner, AImageReader_getWindow) },
    { "AImageReader_setImageListener", offsetof(struct priv_owner, AImageReader_setImageListener) },
    { "AImageReader_acquireLatestImage", offsetof(struct priv_owner, AImageReader_acquireLatestImage) },
    { "AImageReader_delete", offsetof(struct priv_owner, AImageReader_delete) },
    { "AImage_getHardwareBuffer", offsetof(struct priv_owner, AImage_getHardwareBuffer) },
    { "AImage_delete", offsetof(struct priv_owner, AImage_delete) },
    { "AHardwareBuffer_describe", offsetof(struct priv_owner, AHardwareBuffer_describe) },
    { "ANativeWindow_toSurface", offsetof(struct priv_owner, ANativeWindow_toSurface) },
    { NULL, 0 },
};


static AVBufferRef *create_mediacodec_device_ref(jobject surface)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = surface;

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static bool load_lib_functions(struct priv_owner *p, struct mp_log *log)
{
    p->lib_handle = dlopen("libmediandk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!p->lib_handle)
        return false;
    for (int i = 0; lib_functions[i].symbol; i++) {
        const char *sym = lib_functions[i].symbol;
        void *fun = dlsym(p->lib_handle, sym);
        if (!fun)
            fun = dlsym(RTLD_DEFAULT, sym);
        if (!fun) {
            mp_warn(log, "Could not resolve symbol %s\n", sym);
            return false;
        }

        *(void **) ((uint8_t*)p + lib_functions[i].offset) = fun;
    }
    // Optional: older/vendor runtimes can keep working using a completed GL
    // copy before release. Do not change ordinary MediaCodec device selection.
    p->AImage_getCropRect = (void *)dlsym(p->lib_handle, "AImage_getCropRect");
    p->AImage_deleteAsync = (void *)dlsym(p->lib_handle, "AImage_deleteAsync");
    if (!p->AImage_deleteAsync)
        p->AImage_deleteAsync = (void *)dlsym(RTLD_DEFAULT, "AImage_deleteAsync");
    return true;
}

static void drain_image_callback(void *context, AImageReader *reader)
{
    struct priv_owner *p = context;
    mp_mutex_lock(&p->event_lock);
    mp_cond_signal(&p->image_cond);
    mp_mutex_unlock(&p->event_lock);
}

// consumer_lock serializes the complete render/acquire pair with the mapper.
// A listener is only a wakeup: an older notification is not proof that the
// newly rendered image is already available.
static media_status_t acquire_surface_image(struct priv_owner *p, AImage **image)
{
    int64_t deadline = mp_time_ns() + MP_TIME_MS_TO_NS(200);
    media_status_t status;
    mp_mutex_lock(&p->event_lock);
    while ((status = p->AImageReader_acquireLatestImage(p->reader, image)) ==
           AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
        int64_t remaining = deadline - mp_time_ns();
        if (remaining <= 0)
            break;
        mp_cond_timedwait(&p->image_cond, &p->event_lock, remaining);
    }
    mp_mutex_unlock(&p->event_lock);
    return status;
}

static void consume_discarded_buffer(void *opaque, void *buffer,
                                     int (*render)(void *buffer))
{
    struct priv_owner *p = opaque;
    mp_mutex_lock(&p->consumer_lock);
    int queued = render(buffer);
    if (queued > 0) {
        AImage *image = NULL;
        media_status_t status = acquire_surface_image(p, &image);
        if (status == AMEDIA_OK) {
            // No GPU reads were submitted for this image.
            p->AImage_delete(image);
            if (!p->discarded++)
                mp_info(p->drain_log, "Consuming discarded decoder images offscreen.\n");
        } else {
            mp_err(p->drain_log, "Discarded decoder image acquisition failed: %d\n", status);
        }
    } else if (queued < 0) {
        mp_err(p->drain_log, "Failed to render discarded decoder buffer: %d\n", queued);
    }
    mp_mutex_unlock(&p->consumer_lock);
}

// A stream's reader must live as long as the AVHWDeviceContext retained by
// its decoder, queued frames and GPU mapper, not merely as long as the VO.
static void free_stream_device(AVHWDeviceContext *ctx)
{
    struct priv_owner *p = ctx->user_opaque;
    if (p->surface) {
        JNIEnv *env = mp_jni_get_env(NULL);
        if (env)
            (*env)->DeleteGlobalRef(env, p->surface);
    }
    if (p->reader && p->drain_discard)
        p->AImageReader_setImageListener(p->reader, NULL);
    if (p->reader)
        p->AImageReader_delete(p->reader);
    if (p->drain_discard) {
        mp_mutex_destroy(&p->consumer_lock);
        mp_mutex_destroy(&p->event_lock);
        mp_cond_destroy(&p->image_cond);
    }
    if (p->lib_handle)
        dlclose(p->lib_handle);
    talloc_free(p);
}

static AVBufferRef *create_stream_device(void *priv)
{
    struct ra_hwdec *hw = priv;
    struct priv_owner *provider = hw->priv;
    // Independent lifetime is required by dual decoding and the discarded
    // buffer consumer. The provider may predate the current track's options.
    bool dual = hw->ra_ctx->vo->opts->android_dovi_dual;
    bool drain = hw->ra_ctx->vo->opts->android_surface_drain;
    if (!dual && !drain)
        return av_buffer_ref(provider->hwctx.av_device_ref);
    // Fail during decoder selection, not after decoding has succeeded into an
    // unmappable Surface. mpv can then choose its normal copy/software fallback.
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    const struct ra_format *fmt = ra_find_named_format(hw->ra_ctx->ra, "r32f");
    if (dual && (gl->es < 300 || !gl_check_extension(gl->extensions, "GL_EXT_YUV_target") ||
        !fmt || !fmt->renderable || !fmt->linear_filter)) {
        MP_WARN(hw, "P7 dual HW+ unavailable: requires raw YUV and renderable "
                    "linear R32F; trying normal decoder fallback.\n");
        return NULL;
    }
    JNIEnv *env = MP_JNI_GET_ENV(hw);
    struct priv_owner *p = talloc_zero(NULL, struct priv_owner);
    AVBufferRef *ref = NULL;
    if (!p)
        return NULL;
    p->drain_discard = drain;
    if (drain) {
        mp_mutex_init(&p->consumer_lock);
        mp_mutex_init(&p->event_lock);
        mp_cond_init(&p->image_cond);
        p->drain_log = mp_log_new(p, hw->log, "surface-drain");
    }
    if (!env || !load_lib_functions(p, hw->log))
        goto fail;
    if (p->AImageReader_newWithUsage(16, 16, AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 2, &p->reader) != AMEDIA_OK)
        goto fail;
    ANativeWindow *window = NULL;
    if (p->AImageReader_getWindow(p->reader, &window) != AMEDIA_OK || !window)
        goto fail;
    jobject local = p->ANativeWindow_toSurface(env, window);
    if (!local)
        goto fail;
    p->surface = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!p->surface)
        goto fail;
    ref = create_mediacodec_device_ref(p->surface);
    if (!ref)
        goto fail;
    AVHWDeviceContext *ctx = (void *)ref->data;
    ctx->user_opaque = p;
    ctx->free = free_stream_device;
    if (drain) {
        AImageReader_ImageListener listener = {
            .context = p, .onImageAvailable = drain_image_callback,
        };
        if (p->AImageReader_setImageListener(p->reader, &listener) != AMEDIA_OK) {
            av_buffer_unref(&ref);
            return NULL;
        }
        AVMediaCodecDeviceContext *mc = ctx->hwctx;
        mc->on_buffer_discard = consume_discarded_buffer;
        mc->on_buffer_discard_opaque = p;
    }
    MP_INFO(hw, "Allocated independent decoder Surface/reader %p (dual=%d, drain=%d).\n",
            p->reader, dual, drain);
    return ref;
fail:
    // Also handles partial allocation before a device context exists.
    free_stream_device(&(AVHWDeviceContext){.user_opaque = p});
    return NULL;
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (!ra_is_gl(hw->ra_ctx->ra))
        return -1;
    if (!eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!gl_check_extension(exts, "EGL_ANDROID_image_native_buffer"))
        return -1;

    JNIEnv *env = MP_JNI_GET_ENV(hw);
    if (!env)
        return -1;

    if (!load_lib_functions(p, hw->log))
        return -1;

    static const char *es2_exts[] = {"GL_OES_EGL_image_external", 0};
    static const char *es3_exts[] = {"GL_OES_EGL_image_external_essl3", 0};
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    if (gl_check_extension(gl->extensions, es3_exts[0]))
        hw->glsl_extensions = es3_exts;
    else
        hw->glsl_extensions = es2_exts;

    // This sets the upper bound for AImages we can acquire from this reader
    // (incl. ones currently queued) at the same time.
    // The documentation recommends a margin of 2 for acquireLatestImage to
    // work correctly.
    // dummy dimensions, AImageReader only transports hardware buffers
    // This mapper holds at most one image at a time. Two slots also allow
    // acquireLatestImage to discard an older image. More consumer slots can
    // exceed an Amlogic decoder's output-buffer limit at 4K.
    const int max_images = hw->ra_ctx->vo->opts->android_dovi_yuv ||
        hw->ra_ctx->vo->opts->android_color_lut ? 2 : 3;
    media_status_t ret = p->AImageReader_newWithUsage(16, 16,
        AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        max_images, &p->reader);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "newWithUsage failed: %d\n", ret);
        return -1;
    }
    mp_assert(p->reader);

    ANativeWindow *window = NULL;
    ret = p->AImageReader_getWindow(p->reader, &window);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "getWindow failed: %d\n", ret);
        return -1;
    }
    mp_assert(window);

    jobject surface = p->ANativeWindow_toSurface(env, window);
    if (!surface) {
        MP_ERR(hw, "toSurface returned null\n");
        return -1;
    }
    p->surface = (*env)->NewGlobalRef(env, surface);
    (*env)->DeleteLocalRef(env, surface);

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = create_mediacodec_device_ref(p->surface),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
        .create_stream_device = create_stream_device,
        .create_stream_priv = hw,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (p->surface) {
        JNIEnv *env = MP_JNI_GET_ENV(hw);
        mp_assert(env);
        (*env)->DeleteGlobalRef(env, p->surface);
        p->surface = NULL;
    }

    if (p->reader) {
        p->AImageReader_delete(p->reader);
        p->reader = NULL;
    }

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    if (p->lib_handle) {
        dlclose(p->lib_handle);
        p->lib_handle = NULL;
    }
}

static void image_callback(void *context, AImageReader *reader)
{
    struct priv *p = context;

    mp_mutex_lock(&p->lock);
    p->image_available = true;
    mp_cond_signal(&p->cond);
    mp_mutex_unlock(&p->lock);
}

static GLuint compile_yuv_shader(struct ra_hwdec_mapper *mapper, GLenum type,
                                 const char *source)
{
    GL *gl = ra_gl_get(mapper->ra);
    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    GLint ok;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl->GetShaderInfoLog(shader, sizeof(log), NULL, log);
        MP_ERR(mapper, "Android YUV shader compilation failed: %s\n", log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static const struct ra_format *yuv_bridge_format(struct ra_hwdec_mapper *mapper,
                                                const struct mp_image_params *src)
{
    struct priv *p = mapper->priv;
    if (p->native_yuv420) {
        // Integer source codes are stored as code/65535, exactly as software
        // yuv420p10 upload. UNORM16 is lossless here, unlike FLOAT16 or
        // storing code/1023 in UNORM16. Keep FEL composition itself float32.
        const struct ra_format *norm = ra_find_named_format(mapper->ra, "r16");
        if (norm && norm->renderable && norm->linear_filter)
            return norm;
        return ra_find_named_format(mapper->ra, "r32f");
    }
    // Half floats round the EL neutral code away from nlq_offset. That makes
    // sign(0) nonzero and can invent a half-LSB residual. Keep FEL input in
    // full float precision, while known zero-residual MEL keeps the old format.
    bool full = mapper->owner->ra_ctx->vo->opts->android_dovi_dual &&
        (src->repr.sys != PL_COLOR_SYSTEM_DOLBYVISION || !src->repr.dovi ||
         src->repr.dovi->nlq_active);
    // Color A/B candidate: normalized MEL input does not require a floating
    // point range. UNORM16 preserves more fractional bits than FLOAT16 while
    // retaining the same memory footprint. FEL and unknown input stay R32F.
    if (mapper->owner->ra_ctx->vo->opts->android_dovi_dual && !full) {
        const struct ra_format *norm = ra_find_named_format(mapper->ra, "r16");
        if (norm && norm->renderable && norm->linear_filter)
            return norm;
        return ra_find_named_format(mapper->ra, "r32f");
    }
    return full ? ra_find_named_format(mapper->ra, "r32f")
                : ra_find_float16_format(mapper->ra, 1);
}

// Evaluate libplacebo's complete color transform on a small 3D lattice. The
// full-resolution pass then needs only two bilinear LUT samples, rather than
// running Dolby reshaping, PQ, gamut mapping and tone mapping at every pixel.
// This remains opt-in until device throughput and reference errors are tested.
static bool update_color_lut(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    int64_t start = mp_time_ns();
    struct mp_image_params src = mapper->src->params;
    bool has_dovi = src.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && src.repr.dovi;
    // The TV compatibility target uses static BT.2390, rather than a new
    // scene-average-dependent display exposure on every frame. Retain the
    // full per-frame Dolby reshaping/matrices; omit only dynamic tone stats.
    src.color.hdr.avg_pq_y = src.color.hdr.max_pq_y = 0;
    src.color.hdr.scene_avg = 0;
    memset(src.color.hdr.scene_max, 0, sizeof(src.color.hdr.scene_max));
    struct mp_image_params key = src;
    key.repr.dovi = NULL;
    if (p->lut_valid && mp_image_params_equal(&key, &p->lut_source) &&
        has_dovi == p->lut_has_dovi &&
        (!has_dovi || !memcmp(src.repr.dovi, &p->lut_dovi, sizeof(p->lut_dovi))))
        return true;

    struct pl_color_space target = mapper->dst_params.color;
    pl_color_space_infer_map(&src.color, &target);
    int n = p->lut_size;
    int64_t prepared = mp_time_ns();
    bool ok = mp_color_lut_render(p->lut_gpu, p->lut_dispatch, p->lut,
        &p->lut_state, &src, &target, n, p->lut_volume);
    if (ok && p->lut_volume) {
        // GLES 3.0 can populate a 3D texture from framebuffer slices without
        // compute shaders or a CPU readback. The generation atlas is vertical:
        // x=R, y=B*N+G, so each contiguous N*N rectangle is one 3D Z slice.
        GL *gl = ra_gl_get(mapper->ra);
        GLuint fbo = 0;
        pl_opengl_unwrap(p->lut_gpu, p->lut, NULL, NULL, &fbo);
        GLuint tex = pl_opengl_unwrap(p->lut_gpu, p->lut_volume_tex, NULL, NULL, NULL);
        GLint old_read, old_texture;
        gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
        gl->GetIntegerv(GL_TEXTURE_BINDING_3D, &old_texture);
        gl->BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        gl->BindTexture(GL_TEXTURE_3D, tex);
        if (!fbo || !tex || gl->CheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ok = false;
        for (int z = 0; ok && z < n; z++)
            p->CopyTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, z, 0, z*n, n, n);
        GLenum error = gl->GetError();
        gl->BindTexture(GL_TEXTURE_3D, old_texture);
        gl->BindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
        if (error || !ok) {
            MP_ERR(mapper, "GPU 3D color LUT slice copy failed: GL=%x.\n", error);
            ok = false;
        }
    }
    if (!ok) return false;
    p->lut_source = key;
    p->lut_has_dovi = has_dovi;
    if (has_dovi) p->lut_dovi = *src.repr.dovi;
    p->lut_valid = true;
    if (p->lut_updates++ < 8)
        MP_INFO(mapper, "Color LUT update: prepare=%.2fms dispatch=%.2fms, Dolby=%d.\n",
            (prepared-start)/1e6, (mp_time_ns()-prepared)/1e6, has_dovi);
    return true;
}

static bool init_color_lut(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    p->lut_gpu = mapper->owner->ra_ctx->pl_gpu;
    if (!p->lut_gpu || gl->es < 300 ||
        !gl_check_extension(gl->extensions, "GL_EXT_YUV_target"))
        return false;
    p->DrawBuffers = (void *)eglGetProcAddress("glDrawBuffers");
    p->BindSampler = (void *)eglGetProcAddress("glBindSampler");
    p->ColorMask = (void *)eglGetProcAddress("glColorMask");
    if (!p->DrawBuffers || !p->BindSampler || !p->ColorMask) return false;
    p->lut_log = mppl_log_create(mapper, mapper->log);
    p->lut_dispatch = pl_dispatch_create(p->lut_log, p->lut_gpu);
    p->lut_volume = mapper->owner->ra_ctx->vo->opts->android_color_lut_3d;
    if (p->lut_volume && !p->lut_direct) {
        MP_ERR(mapper, "3D color LUT requires direct renderer sampling.\n");
        return false;
    }
    p->lut_size = mapper->owner->ra_ctx->vo->opts->android_color_lut_size;
    if (p->lut_size * p->lut_size > p->lut_gpu->limits.max_tex_2d_dim)
        p->lut_size = 33;
    const char *format = mapper->owner->ra_ctx->vo->opts->android_color_lut_format
        ? "rgb10_a2" : "rgba16f";
    pl_fmt fmt = pl_find_named_fmt(p->lut_gpu, format);
    if (!fmt || !(fmt->caps & PL_FMT_CAP_LINEAR) || !(fmt->caps & PL_FMT_CAP_RENDERABLE))
        return false;
    if (p->lut_volume && p->lut_size > p->lut_gpu->limits.max_tex_3d_dim) return false;
    p->lut = pl_tex_create(p->lut_gpu, pl_tex_params(
        .w = p->lut_volume ? p->lut_size : p->lut_size * p->lut_size,
        .h = p->lut_volume ? p->lut_size * p->lut_size : p->lut_size,
        .format = fmt, .renderable = true, .sampleable = true));
    if (!p->lut) return false;
    if (p->lut_volume) {
        p->CopyTexSubImage3D = (void *)eglGetProcAddress("glCopyTexSubImage3D");
        if (!p->CopyTexSubImage3D) return false;
        p->lut_volume_tex = pl_tex_create(p->lut_gpu, pl_tex_params(
            .w=p->lut_size, .h=p->lut_size, .d=p->lut_size,
            .format=fmt, .sampleable=true));
        if (!p->lut_volume_tex) return false;
    }
    static const char fragment[] =
        "#version 300 es\n#extension GL_EXT_YUV_target : require\n"
        "precision highp float;\n"
        "uniform highp __samplerExternal2DY2YEXT source;\n"
        "uniform highp sampler2D lut;\nuniform float lut_size;\n"
        "in vec2 uv;\nlayout(location=0) out vec4 result;\n"
        "void main() {\n"
        " vec3 q = clamp(texture(source, uv).xyz, 0.0, 1.0) * (lut_size-1.0);\n"
        " float y0=floor(q.y), y1=min(y0+1.0,lut_size-1.0);\n"
        " vec2 a=vec2((y0*lut_size+q.x+0.5)/(lut_size*lut_size),(q.z+0.5)/lut_size);\n"
        " vec2 b=vec2((y1*lut_size+q.x+0.5)/(lut_size*lut_size),a.y);\n"
        " result=vec4(mix(texture(lut,a).rgb,texture(lut,b).rgb,fract(q.y)),1.0);\n"
        "}\n";
    GLuint vs = compile_yuv_shader(mapper, GL_VERTEX_SHADER, android_yuv_vertex_shader);
    GLuint fs = compile_yuv_shader(mapper, GL_FRAGMENT_SHADER, fragment);
    p->yuv_program = gl->CreateProgram();
    if (vs && fs) {
        gl->AttachShader(p->yuv_program, vs); gl->AttachShader(p->yuv_program, fs);
        gl->LinkProgram(p->yuv_program);
    }
    gl->DeleteShader(vs); gl->DeleteShader(fs);
    GLint linked = 0;
    gl->GetProgramiv(p->yuv_program, GL_LINK_STATUS, &linked);
    if (!linked) return false;
    gl->GenFramebuffers(1, &p->yuv_fbo);
    gl->GenVertexArrays(1, &p->yuv_vao);
    struct ra_swapchain *sw = mapper->owner->ra_ctx->swapchain;
    struct pl_color_space swap_color = sw->fns->target_csp
        ? sw->fns->target_csp(sw) : pl_color_space_srgb;
    p->lut_hdr = pl_color_transfer_is_hdr(swap_color.transfer);
    mapper->dst_params.imgfmt = p->lut_hdr ? IMGFMT_X2BGR10 : IMGFMT_RGB0;
    mapper->dst_params.repr = pl_color_repr_rgb;
    mapper->dst_params.color = p->lut_hdr
        ? swap_color : (struct pl_color_space){
            .primaries = PL_COLOR_PRIM_BT_709, .transfer = PL_COLOR_TRC_BT_1886,
            .hdr = {.max_luma = PL_COLOR_SDR_WHITE, .min_luma = 0.203}};
    struct gl_video_opts *opts = mp_get_config_group(mapper,
        mapper->owner->ra_ctx->global, &gl_video_conf);
    if (opts->target_prim) mapper->dst_params.color.primaries = opts->target_prim;
    if (opts->target_trc) mapper->dst_params.color.transfer = opts->target_trc;
    if (opts->target_peak) {
        mapper->dst_params.color.hdr.max_luma = opts->target_peak;
        if (!p->lut_hdr) mapper->dst_params.color.hdr.min_luma = opts->target_peak / 1000.0;
    }
    talloc_free(opts);
    if (p->lut_direct) {
        p->lut_info = (struct pl_custom_lut) {
            .size = {p->lut_size, p->lut_size, p->lut_size},
            .atlas = p->lut_volume ? NULL : p->lut,
            .volume = p->lut_volume ? p->lut_volume_tex : NULL,
            .repr_out = mapper->dst_params.repr,
            .color_out = mapper->dst_params.color,
        };
        mapper->color_lut = &p->lut_info;
    }
    MP_INFO(mapper, "Experimental %d^3 color LUT: raw YUV -> %s at source resolution (%s, %s, %s).\n",
        p->lut_size, p->lut_hdr ? "HDR10 RGB" : "SDR RGB",
        p->lut_direct ? "direct renderer sampling, no intermediate frame" : "packed RGB copy",
        p->lut_volume ? "3D volume" : "2D atlas", format);
    return true;
}

static bool map_color_lut(struct ra_hwdec_mapper *mapper, int w, int h)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    if (!update_color_lut(mapper)) return false;
    const struct ra_format *fmt = p->lut_hdr ? ra_find_named_format(mapper->ra, "rgb10_a2")
                                          : ra_find_unorm_format(mapper->ra, 1, 4);
    if (!fmt || !fmt->renderable) return false;
    if (mapper->tex[0] && (mapper->tex[0]->params.w != w || mapper->tex[0]->params.h != h))
        ra_tex_free(mapper->ra, &mapper->tex[0]);
    if (!mapper->tex[0])
        mapper->tex[0] = ra_tex_create(mapper->ra, &(struct ra_tex_params){
            .dimensions=2, .w=w, .h=h, .d=1, .format=fmt,
            .render_src=true, .render_dst=true, .src_linear=true});
    if (!mapper->tex[0]) return false;
    GLint program, vao, fbo, viewport[4], active, oes, sampler0, sampler1, texture1, mask[4];
    const GLenum caps[] = {GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,GL_RASTERIZER_DISCARD};
    GLint enabled[MP_ARRAY_SIZE(caps)];
    gl->GetIntegerv(GL_CURRENT_PROGRAM,&program); gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fbo); gl->GetIntegerv(GL_VIEWPORT,viewport);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE,&active); gl->GetIntegerv(GL_COLOR_WRITEMASK,mask);
    gl->ActiveTexture(GL_TEXTURE0); gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES,&oes);
    gl->GetIntegerv(GL_SAMPLER_BINDING,&sampler0);
    gl->ActiveTexture(GL_TEXTURE1); gl->GetIntegerv(GL_TEXTURE_BINDING_2D,&texture1);
    gl->GetIntegerv(GL_SAMPLER_BINDING,&sampler1);
    for (int i=0;i<MP_ARRAY_SIZE(caps);i++) {gl->GetIntegerv(caps[i],&enabled[i]);gl->Disable(caps[i]);}
    p->ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    GLuint output=0; GLenum target=GL_TEXTURE_2D;
    ra_gl_get_raw_tex(mapper->ra,mapper->tex[0],&output,&target);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,p->yuv_fbo);
    gl->FramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,target,output,0);
    GLenum attachment=GL_COLOR_ATTACHMENT0; p->DrawBuffers(1,&attachment);
    bool ok=gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        gl->UseProgram(p->yuv_program); gl->BindVertexArray(p->yuv_vao); gl->Viewport(0,0,w,h);
        gl->Uniform1i(gl->GetUniformLocation(p->yuv_program,"source"),0);
        gl->Uniform1i(gl->GetUniformLocation(p->yuv_program,"lut"),1);
        gl->Uniform1f(gl->GetUniformLocation(p->yuv_program,"lut_size"),p->lut_size);
        gl->ActiveTexture(GL_TEXTURE0); p->BindSampler(0,0); gl->BindTexture(GL_TEXTURE_EXTERNAL_OES,p->gl_texture);
        gl->ActiveTexture(GL_TEXTURE1); p->BindSampler(1,0);
        gl->BindTexture(GL_TEXTURE_2D,pl_opengl_unwrap(p->lut_gpu,p->lut,NULL,NULL,NULL));
        // libplacebo normally supplies sampler objects. Its mutable texture
        // therefore still has GL's default mipmapped minification filter,
        // which makes a bare single-level texture incomplete (black samples).
        gl->TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        gl->DrawArrays(GL_TRIANGLES,0,3); ok=gl->GetError()==GL_NO_ERROR;
    }
    gl->ActiveTexture(GL_TEXTURE1);gl->BindTexture(GL_TEXTURE_2D,texture1);p->BindSampler(1,sampler1);
    gl->ActiveTexture(GL_TEXTURE0);gl->BindTexture(GL_TEXTURE_EXTERNAL_OES,oes);p->BindSampler(0,sampler0);
    gl->ActiveTexture(active); gl->BindVertexArray(vao);gl->UseProgram(program);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);gl->Viewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    p->ColorMask(mask[0],mask[1],mask[2],mask[3]);
    for (int i=0;i<MP_ARRAY_SIZE(caps);i++) if(enabled[i])gl->Enable(caps[i]);
    return ok;
}

static bool init_yuv_bridge(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    if (gl->es < 300 || !gl_check_extension(gl->extensions, "GL_EXT_YUV_target")) {
        MP_ERR(mapper, "android-dovi-yuv requires GLES 3 and GL_EXT_YUV_target; "
                       "use mediacodec-copy on this device.\n");
        return false;
    }
    p->DrawBuffers = (void *)eglGetProcAddress("glDrawBuffers");
    p->BindSampler = (void *)eglGetProcAddress("glBindSampler");
    p->ColorMask = (void *)eglGetProcAddress("glColorMask");
    GLint buffers, attachments;
    gl->GetIntegerv(GL_MAX_DRAW_BUFFERS, &buffers);
    gl->GetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &attachments);
    const struct ra_format *fmt = yuv_bridge_format(mapper, &mapper->src_params);
    if (!p->DrawBuffers || !p->BindSampler || !p->ColorMask ||
        buffers < 3 || attachments < 3 || !fmt || !fmt->renderable ||
        !fmt->linear_filter) {
        MP_ERR(mapper, "Android YUV bridge needs three renderable targets "
                       "with sufficient source precision.\n");
        return false;
    }

    GLuint vs = compile_yuv_shader(mapper, GL_VERTEX_SHADER, android_yuv_vertex_shader);
    GLuint fs = compile_yuv_shader(mapper, GL_FRAGMENT_SHADER, android_yuv_fragment_shader);
    if (!vs || !fs) {
        gl->DeleteShader(vs);
        gl->DeleteShader(fs);
        return false;
    }
    p->yuv_program = gl->CreateProgram();
    gl->AttachShader(p->yuv_program, vs);
    gl->AttachShader(p->yuv_program, fs);
    gl->LinkProgram(p->yuv_program);
    gl->DeleteShader(vs);
    gl->DeleteShader(fs);
    GLint ok;
    gl->GetProgramiv(p->yuv_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl->GetProgramInfoLog(p->yuv_program, sizeof(log), NULL, log);
        MP_ERR(mapper, "Android YUV shader link failed: %s\n", log);
        return false;
    }
    gl->GenFramebuffers(1, &p->yuv_fbo);
    gl->GenVertexArrays(1, &p->yuv_vao);
    mapper->dst_params.imgfmt = p->native_yuv420 ? IMGFMT_420PF : IMGFMT_444PF;
    // The sampler returns normalized source codes, not P010's left-aligned
    // storage representation. Preserve the source precision without bit shifts.
    mapper->dst_params.repr.bits.sample_depth =
        mapper->dst_params.repr.bits.color_depth;
    mapper->dst_params.repr.bits.bit_shift = 0;
    if (p->native_yuv420) {
        // Use the same logical UNORM16 sample encoding as a software 10-bit
        // plane (code / 65535), in lossless UNORM16 or float32 storage. This
        // avoids making RPU offset conversion depend on the decoder's storage
        // format. No source bits are discarded or CPU copies introduced.
        struct mp_imgfmt_desc src = mp_imgfmt_get_desc(mapper->src_params.hw_subfmt);
        mapper->dst_params.repr.bits.sample_depth = 16;
        mapper->dst_params.repr.bits.color_depth =
            src.comps[0].size - abs(src.comps[0].pad);
    }
    MP_INFO(mapper, "Experimental HW+ Dolby Vision YUV bridge initialized "
                    "(OES -> three %s planes, no CPU readback).\n", fmt->name);
    if (p->native_yuv420)
        MP_INFO(mapper, "Native 4:2:0 source codes: %d effective bits in 16-bit encoding.\n",
                mapper->dst_params.repr.bits.color_depth);
    return true;
}

static bool map_yuv_bridge(struct ra_hwdec_mapper *mapper, int w, int h)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    const struct ra_format *fmt = yuv_bridge_format(mapper, &mapper->src->params);
    if (!fmt || !fmt->renderable || !fmt->linear_filter)
        return false;
    for (int i = 0; i < 3; i++) {
        int pw = p->native_yuv420 && i ? (w + 1) / 2 : w;
        int ph = p->native_yuv420 && i ? (h + 1) / 2 : h;
        struct ra_tex *tex = mapper->tex[i];
        if (tex && (tex->params.w != pw || tex->params.h != ph ||
                    tex->params.format != fmt))
            ra_tex_free(mapper->ra, &mapper->tex[i]);
        if (!mapper->tex[i]) {
            mapper->tex[i] = ra_tex_create(mapper->ra, &(struct ra_tex_params) {
                .dimensions = 2, .w = pw, .h = ph, .d = 1, .format = fmt,
                .render_src = true, .render_dst = true, .src_linear = true,
            });
            if (!mapper->tex[i])
                return false;
        }
    }

    // libplacebo and mpv share this GL context. Restore every state touched here.
    GLint program, vao, draw_fbo, viewport[4], active, oes, sampler, mask[4];
    const GLenum caps[] = {GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                           GL_STENCIL_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD};
    GLint enabled[MP_ARRAY_SIZE(caps)];
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    gl->GetIntegerv(GL_VIEWPORT, viewport);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl->GetIntegerv(GL_COLOR_WRITEMASK, mask);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &oes);
    gl->GetIntegerv(GL_SAMPLER_BINDING, &sampler);
    for (int i = 0; i < MP_ARRAY_SIZE(caps); i++) {
        gl->GetIntegerv(caps[i], &enabled[i]);
        gl->Disable(caps[i]);
    }
    p->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, p->yuv_fbo);
    gl->UseProgram(p->yuv_program);
    gl->Uniform1i(gl->GetUniformLocation(p->yuv_program, "source"), 0);
    gl->Uniform1f(gl->GetUniformLocation(p->yuv_program, "source_max"),
        p->native_yuv420 ? (1 << mapper->dst_params.repr.bits.color_depth) - 1 : 0);
    gl->BindVertexArray(p->yuv_vao);
    p->BindSampler(0, 0);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    bool ok = true;
    // Preserve the native chroma grid. Asking the external sampler for a
    // linearly filtered 4:4:4 image irreversibly interpolates and rounds UV
    // before libplacebo can apply the sign-sensitive FEL residual mapping.
    // Copy Y and UV separately without filtering; let libplacebo reconstruct
    // chroma once, using the same grid and siting as its software path.
    for (int pass = 0; pass < (p->native_yuv420 ? 2 : 1); pass++) {
        GLenum attachments[3];
        for (int i = 0; i < 3; i++) {
            bool use = !p->native_yuv420 || (pass == 0 ? i == 0 : i != 0);
            GLuint texture = 0;
            GLenum target = GL_TEXTURE_2D;
            if (use)
                ra_gl_get_raw_tex(mapper->ra, mapper->tex[i], &texture, &target);
            gl->FramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0 + i, target, texture, 0);
            attachments[i] = use ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        }
        p->DrawBuffers(3, attachments);
        ok = gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!ok)
            break;
        gl->Viewport(0, 0, pass ? (w + 1) / 2 : w, pass ? (h + 1) / 2 : h);
        gl->DrawArrays(GL_TRIANGLES, 0, 3);
        ok = gl->GetError() == GL_NO_ERROR;
        if (!ok)
            break;
    }
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, oes);
    p->BindSampler(0, sampler);
    gl->ActiveTexture(active);
    gl->BindVertexArray(vao);
    gl->UseProgram(program);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    gl->Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    p->ColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < MP_ARRAY_SIZE(caps); i++) {
        if (enabled[i])
            gl->Enable(caps[i]);
    }
    if (!ok)
        MP_ERR(mapper, "Android YUV bridge framebuffer/draw failed.\n");
    else if (!p->yuv_logged) {
        MP_INFO(mapper, "HW+ Dolby Vision raw YUV sampling active: %dx%d (%s).\n",
                w, h, p->native_yuv420 ? "native 4:2:0" : "4:4:4");
        p->yuv_logged = true;
    }
    return ok;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    p->log = mapper->log;
    p->crop_sdr = gl->es >= 300 &&
                  mapper->src_params.repr.sys != PL_COLOR_SYSTEM_DOLBYVISION &&
                  !pl_color_transfer_is_hdr(mapper->src_params.color.transfer);
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->cond);

    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    p->GetNativeClientBufferANDROID =
        (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
#if HAVE_EGL_ANDROID
    if (mapper->owner->ra_ctx->vo->opts->android_gl_trace)
        p->EGLImageTargetTexture2DOES =
            android_gl_trace_get_proc_address(NULL, "glEGLImageTargetTexture2DOES");
#endif

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->GetNativeClientBufferANDROID || !p->EGLImageTargetTexture2DOES)
        return -1;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;

    p->color_lut = mapper->owner->ra_ctx->vo->opts->android_color_lut &&
        !mapper->owner->ra_ctx->vo->opts->android_dovi_dual &&
        (pl_color_transfer_is_hdr(mapper->src_params.color.transfer) ||
         mapper->src_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION);
    p->lut_direct = p->color_lut &&
        mapper->owner->ra_ctx->vo->opts->android_color_lut_direct;

    p->dovi_yuv = mapper->owner->ra_ctx->vo->opts->android_dovi_dual ||
        (mapper->owner->ra_ctx->vo->opts->android_dovi_yuv &&
         mapper->src_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
         mapper->src_params.repr.dovi);
    if ((p->dovi_yuv || p->color_lut) && gl_check_extension(
            eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS),
            "EGL_ANDROID_native_fence_sync")) {
        p->CreateSyncKHR = (void *)eglGetProcAddress("eglCreateSyncKHR");
        p->DestroySyncKHR = (void *)eglGetProcAddress("eglDestroySyncKHR");
        p->DupNativeFenceFDANDROID =
            (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    }
    struct mp_imgfmt_desc hwfmt = mp_imgfmt_get_desc(mapper->src_params.hw_subfmt);
    p->native_yuv420 = mapper->owner->ra_ctx->vo->opts->android_dovi_dual &&
                      hwfmt.chroma_xs == 1 && hwfmt.chroma_ys == 1;
    if (p->color_lut ? !init_color_lut(mapper) : p->dovi_yuv && !init_yuv_bridge(mapper))
        return -1;

    // texture creation
    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    bool raw_nearest = p->native_yuv420;
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, raw_nearest ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, raw_nearest ? GL_NEAREST : GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    if ((p->dovi_yuv || p->color_lut) && !p->lut_direct)
        return 0; // Allocate planar targets when actual buffer dimensions are known.

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .external_oes = true,
    };

    if (params.format->ctype != RA_CTYPE_UNORM)
        return -1;

    p->oes_tex = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    mapper->tex[0] = p->oes_tex;
    if (!mapper->tex[0])
        return -1;

    return 0;
}

// Filtering must clamp at the decoded picture edge, not the larger
// allocator's padded edge. Sample source texel centers into a texture
// whose bounds match the visible rectangle, then use the normal scaler.
static bool copy_visible_sdr(struct ra_hwdec_mapper *mapper, AImageCropRect crop)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    int w = crop.right - crop.left;
    int h = crop.bottom - crop.top;
    const struct ra_format *format = NULL;
    int depth = mapper->src_params.repr.bits.color_depth;
    if (depth > 0 && depth <= 10)
        format = ra_find_named_format(mapper->ra, "rgb10_a2");
    if (!format || !format->renderable || !format->linear_filter)
        format = ra_find_float16_format(mapper->ra, 4);
    if (!format || !format->renderable || !format->linear_filter)
        return false;
    if (p->visible_tex &&
        (p->visible_tex->params.w != w || p->visible_tex->params.h != h))
    {
        ra_tex_free(mapper->ra, &p->visible_tex);
    }
    if (!p->visible_tex) {
        p->visible_tex = ra_tex_create(mapper->ra, &(struct ra_tex_params) {
            .dimensions = 2, .w = w, .h = h, .d = 1, .format = format,
            .render_src = true, .render_dst = true, .src_linear = true,
        });
    }
    if (!p->visible_tex)
        return false;
    if (!p->visible_shader) {
        p->visible_shader = gl_sc_create(mapper->ra, mapper->owner->global, mapper->log);
        if (gl_check_extension(eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS),
                               "EGL_ANDROID_native_fence_sync"))
        {
            p->CreateSyncKHR = (void *)eglGetProcAddress("eglCreateSyncKHR");
            p->DestroySyncKHR = (void *)eglGetProcAddress("eglDestroySyncKHR");
            p->DupNativeFenceFDANDROID = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
        }
    }

    GLint old_external;
    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);

    struct gl_shader_cache *sc = p->visible_shader;
    for (const char *const *ext = mapper->owner->glsl_extensions; ext && *ext; ext++)
        gl_sc_enable_extension(sc, (char *)*ext);
    gl_sc_uniform_texture(sc, "input_surface", p->oes_tex);
    gl_sc_uniform_vec2(sc, "crop_origin", (float[]){crop.left, crop.top});
    gl_sc_uniform_vec2(sc, "buffer_size", (float[]){p->oes_tex->params.w, p->oes_tex->params.h});
    gl_sc_add(sc, "vec2 uv = (gl_FragCoord.xy + crop_origin) / buffer_size;\n"
                  "color = vec4(texture(input_surface, uv).rgb, 1.0);\n");
    static const struct ra_renderpass_input position = {
        .name = "position", .type = RA_VARTYPE_FLOAT, .dim_v = 2, .dim_m = 1,
    };
    float vertices[][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    p->image_gpu_used = true;
    gl_sc_dispatch_draw(sc, p->visible_tex, true, &position, 1,
                        sizeof(vertices[0]), vertices, MP_ARRAY_SIZE(vertices));

    gl->GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &old_external);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, old_external);

    if (gl_sc_error_state(sc))
        return false;
    mapper->tex[0] = p->visible_tex;
    if (!p->logged_visible_copy) {
        MP_VERBOSE(mapper, "Visible rectangle GPU copy active: %dx%d\n", w, h);
        p->logged_visible_copy = true;
    }
    return true;
}

static void release_image(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = p->reader_owner;
    int release_fd = EGL_NO_NATIVE_FENCE_FD_ANDROID;

    // DrawArrays only submits the OES -> private-plane copy. AImage_delete
    // without a release fence lets the decoder overwrite that input while
    // the GPU still reads it, producing spatially torn/shifted blocks.
    if (p->image && p->image_gpu_used) {
        GL *gl = ra_gl_get(mapper->ra);
        if (o->AImage_deleteAsync && p->CreateSyncKHR && p->DestroySyncKHR &&
            p->DupNativeFenceFDANDROID) {
            EGLDisplay display = eglGetCurrentDisplay();
            const EGLint attrs[] = {EGL_NONE};
            EGLSyncKHR sync = p->CreateSyncKHR(display,
                EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (sync != EGL_NO_SYNC_KHR) {
                // EGL_ANDROID_native_fence_sync creates the native fence on
                // Flush. The returned duplicate is owned by deleteAsync.
                gl->Flush();
                release_fd = p->DupNativeFenceFDANDROID(display, sync);
                p->DestroySyncKHR(display, sync);
            }
        }
        if (release_fd == EGL_NO_NATIVE_FENCE_FD_ANDROID)
            gl->Finish(); // Safe fallback, never release an unfinished copy.
        if (!p->release_sync_logged) {
            if (p->crop_sdr && p->oes_tex) {
                MP_VERBOSE(mapper, "Android copied image release synchronized using %s.\n",
                    release_fd >= 0 ? "native GPU fence" : "completed GPU copy");
            } else {
                MP_INFO(mapper, "Dolby YUV buffer release synchronized using %s.\n",
                    release_fd >= 0 ? "native GPU fence" : "completed GPU copy");
            }
            p->release_sync_logged = true;
        }
    }

    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = 0;
    }
    if (p->image) {
        if (release_fd >= 0)
            o->AImage_deleteAsync(p->image, release_fd);
        else
            o->AImage_delete(p->image);
        p->image = NULL;
    }
    p->image_gpu_used = false;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    if (p->reader_owner && !p->reader_owner->drain_discard)
        p->reader_owner->AImageReader_setImageListener(p->reader_owner->reader, NULL);

    release_image(mapper);
    talloc_free(p->cached_source);
    av_buffer_unref(&p->stream_device);

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    if (p->oes_tex) {
        mapper->tex[0] = NULL;
        ra_tex_free(mapper->ra, &p->oes_tex);
    }
    ra_tex_free(mapper->ra, &p->visible_tex);
    if (p->visible_shader)
        gl_sc_destroy(p->visible_shader);
    for (int i = 0; i < 3; i++)
        ra_tex_free(mapper->ra, &mapper->tex[i]);
    if (p->yuv_program)
        gl->DeleteProgram(p->yuv_program);
    if (p->yuv_fbo)
        gl->DeleteFramebuffers(1, &p->yuv_fbo);
    if (p->yuv_vao)
        gl->DeleteVertexArrays(1, &p->yuv_vao);
    if (p->lut_gpu) {
        pl_tex_destroy(p->lut_gpu, &p->lut_volume_tex);
        pl_tex_destroy(p->lut_gpu, &p->lut);
        pl_shader_obj_destroy(&p->lut_state);
    }
    pl_dispatch_destroy(&p->lut_dispatch);
    pl_log_destroy(&p->lut_log);

    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    // OES still needs the last valid image when the next Surface frame is late.
    // The Dolby Vision bridge has already copied its pixels to private textures.
    if ((p->dovi_yuv || p->color_lut) && !p->lut_direct)
        release_image(mapper);
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    AVBufferRef *device = NULL;
    if (mapper->src->hwctx) {
        AVHWFramesContext *frames = (void *)mapper->src->hwctx->data;
        if (frames->device_ctx && frames->device_ctx->free == free_stream_device) {
            o = frames->device_ctx->user_opaque;
            device = frames->device_ref;
        }
    }
    if (p->reader_owner != o) {
        if (p->reader_owner && !p->reader_owner->drain_discard)
            p->reader_owner->AImageReader_setImageListener(p->reader_owner->reader, NULL);
        release_image(mapper);
        talloc_free(p->cached_source);
        p->cached_source = NULL;
        av_buffer_unref(&p->stream_device);
        p->stream_device = device ? av_buffer_ref(device) : NULL;
        p->reader_owner = o;
        mp_mutex_lock(&p->lock);
        p->image_available = false;
        mp_mutex_unlock(&p->lock);
        AImageReader_ImageListener listener = {
            .context = p, .onImageAvailable = image_callback,
        };
        if (!o->drain_discard)
            o->AImageReader_setImageListener(o->reader, &listener);
    }

    if (p->dovi_yuv)
        MP_TRACE(mapper, "YUV map begin pts=%f buffer=%p cached_pts=%f cached_buffer=%p\n",
                 mapper->src->pts, mapper->src->planes[3],
                 p->cached_source ? p->cached_source->pts : MP_NOPTS_VALUE,
                 p->cached_source ? p->cached_source->planes[3] : NULL);

    // A MediaCodec output buffer can be sent to the Surface only once. Keep a
    // reference so redraws reuse the copied YUV planes or the retained OES image.
    if (p->cached_source &&
        p->cached_source->planes[3] == mapper->src->planes[3] &&
        (p->dovi_yuv || p->color_lut || p->image))
        return 0;

    AImage *next_image = NULL;
    media_status_t ret;
    if (o->drain_discard) {
        if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
            return -1;
        mp_mutex_lock(&o->consumer_lock);
        int err = av_mediacodec_release_buffer((void *)mapper->src->planes[3], 1);
        ret = err < 0 ? AMEDIA_ERROR_UNKNOWN : acquire_surface_image(o, &next_image);
        mp_mutex_unlock(&o->consumer_lock);
        if (ret != AMEDIA_OK) {
            MP_ERR(mapper, "Rendered decoder image acquisition failed: %d\n", ret);
            return -1;
        }
    } else {
        mp_mutex_lock(&p->lock);
        p->image_available = false;
        mp_mutex_unlock(&p->lock);

        {
            if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
                return -1;
            AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)mapper->src->planes[3];
            int err = av_mediacodec_release_buffer(buffer, 1);
            if (err != 0) {
                char errstr[80];
                av_strerror(err, errstr, sizeof(errstr));
                MP_ERR(p, "Failed to render buffer: %s\n", errstr);
                // HACK: If the buffer has been released already, return fake
                // success to avoid flashing frames of render errors.
                // The VOs need to be fixed to handle this gracefully.
                return err == AVERROR(ENOENT) &&
                       (p->image || ((p->dovi_yuv || p->color_lut) && p->cached_source)) ? 0 : -1;
            }
        }

        bool image_available = false;
        mp_mutex_lock(&p->lock);
        if (!p->image_available) {
            mp_cond_timedwait(&p->cond, &p->lock, MP_TIME_MS_TO_NS(100));
        }
        image_available = p->image_available;
        mp_mutex_unlock(&p->lock);
        if (!image_available)
            MP_WARN(p, "Waiting for frame timed out!\n");

        ret = o->AImageReader_acquireLatestImage(o->reader, &next_image);
        if (ret != AMEDIA_OK) {
            MP_ERR(mapper, "acquireLatestImage failed: %d\n", ret);
            // Reuse only an image that is still owned by this mapper. Returning
            // success with a deleted EGLImage samples undefined GPU memory.
            if (ret == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE &&
                !image_available && !p->dovi_yuv && p->image) {
                talloc_free(p->cached_source);
                p->cached_source = mp_image_new_ref(mapper->src);
                return 0;
            }
            return -1;
        }
    }
    mp_assert(next_image);

    mapper->sample_crop = (struct mp_rect){0};
    if (p->oes_tex)
        mapper->tex[0] = p->oes_tex;

    AHardwareBuffer *hwbuf = NULL;
    ret = o->AImage_getHardwareBuffer(next_image, &hwbuf);
    if (ret != AMEDIA_OK) {
        MP_ERR(mapper, "getHardwareBuffer failed: %d\n", ret);
        o->AImage_delete(next_image);
        return -1;
    }
    mp_assert(hwbuf);

    // Update texture size since it may differ
    AHardwareBuffer_Desc d;
    o->AHardwareBuffer_describe(hwbuf, &d);
    if (p->color_lut && !p->yuv_logged) {
        MP_INFO(mapper, "Android LUT source buffer: %ux%u stride=%u format=0x%x usage=0x%llx.\n",
            d.width, d.height, d.stride, d.format, (unsigned long long)d.usage);
        p->yuv_logged = true;
    }
    if ((p->lut_direct || (!p->dovi_yuv && !p->color_lut)) &&
        (mapper->tex[0]->params.w != d.width || mapper->tex[0]->params.h != d.height)) {
        MP_VERBOSE(p, "Texture dimensions changed to %dx%d\n", d.width, d.height);
        mapper->tex[0]->params.w = d.width;
        mapper->tex[0]->params.h = d.height;
    }

    EGLClientBuffer buf = p->GetNativeClientBufferANDROID(hwbuf);
    if (!buf) {
        MP_ERR(mapper, "eglGetNativeClientBufferANDROID failed: EGL error 0x%04x, buffer=%ux%u stride=%u format=0x%x.\n",
               (unsigned)eglGetError(), d.width, d.height, d.stride, d.format);
        o->AImage_delete(next_image);
        return -1;
    }

    const int attribs[] = {EGL_NONE};
    EGLImageKHR next_egl_image = p->CreateImageKHR(eglGetCurrentDisplay(),
        EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, attribs);
    if (!next_egl_image) {
        MP_ERR(mapper, "eglCreateImageKHR failed: EGL error 0x%04x, buffer=%ux%u stride=%u format=0x%x.\n",
               (unsigned)eglGetError(), d.width, d.height, d.stride, d.format);
        o->AImage_delete(next_image);
        return -1;
    }

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, next_egl_image);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    release_image(mapper);
    p->image = next_image;
    p->egl_image = next_egl_image;

    if (p->color_lut) {
        p->image_gpu_used = true;
        if (p->lut_direct) {
            if (!update_color_lut(mapper)) return -1;
        } else if (!map_color_lut(mapper, d.width, d.height)) {
            return -1;
        }
    } else if (p->dovi_yuv) {
        MP_TRACE(mapper, "YUV image acquired pts=%f\n", mapper->src->pts);
        p->image_gpu_used = true; // Also synchronize a partially failed copy.
        if (!map_yuv_bridge(mapper, d.width, d.height))
            return -1;
        MP_TRACE(mapper, "YUV copy submitted pts=%f\n", mapper->src->pts);
    }

    if (p->crop_sdr && p->oes_tex && o->AImage_getCropRect &&
        (mapper->can_clamp_crop || !p->crop_unavailable) &&
        (d.width > mapper->src->w || d.height > mapper->src->h))
    {
        AImageCropRect crop;
        if (o->AImage_getCropRect(next_image, &crop) == AMEDIA_OK &&
            crop.left >= 0 && crop.top >= 0 &&
            crop.right > crop.left && crop.bottom > crop.top &&
            (uint32_t)crop.right <= d.width && (uint32_t)crop.bottom <= d.height &&
            crop.right - crop.left == mapper->src->w &&
            crop.bottom - crop.top == mapper->src->h)
        {
            if (mapper->can_clamp_crop && mapper->tex[0] == p->oes_tex) {
                mapper->sample_crop = (struct mp_rect){
                    crop.left, crop.top, crop.right, crop.bottom,
                };
                if (!p->logged_crop_sampling) {
                    MP_VERBOSE(mapper, "Clamping HW+ sampling to visible rectangle: %dx%d\n",
                               mapper->src->w, mapper->src->h);
                    p->logged_crop_sampling = true;
                }
            } else if (!p->crop_unavailable && !copy_visible_sdr(mapper, crop)) {
                MP_WARN(mapper, "Could not crop padded HW+ texture.\n");
                p->crop_unavailable = true;
            }
        }
    }

    talloc_free(p->cached_source);
    p->cached_source = mp_image_new_ref(mapper->src);

    return 0;
}


const struct ra_hwdec_driver ra_hwdec_aimagereader = {
    .name = "aimagereader",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_MEDIACODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
