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

#include <assert.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkImageReader.h>
#include <android/native_window_jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "options/options.h"
#include "video/out/vo.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/opengl/ra_gl.h"
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

    media_status_t (*AImageReader_newWithUsage)(
        int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    media_status_t (*AImageReader_getWindow)(
        AImageReader *, ANativeWindow **);
    media_status_t (*AImageReader_setImageListener)(
        AImageReader *, AImageReader_ImageListener *);
    media_status_t (*AImageReader_acquireLatestImage)(AImageReader *, AImage **);
    void (*AImageReader_delete)(AImageReader *);
    media_status_t (*AImage_getHardwareBuffer)(const AImage *, AHardwareBuffer **);
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
    bool native_yuv420;
    bool yuv_logged;
    bool image_gpu_used;
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
    p->AImage_deleteAsync = (void *)dlsym(p->lib_handle, "AImage_deleteAsync");
    if (!p->AImage_deleteAsync)
        p->AImage_deleteAsync = (void *)dlsym(RTLD_DEFAULT, "AImage_deleteAsync");
    return true;
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
    if (p->reader)
        p->AImageReader_delete(p->reader);
    if (p->lib_handle)
        dlclose(p->lib_handle);
    talloc_free(p);
}

static AVBufferRef *create_stream_device(void *priv)
{
    struct ra_hwdec *hw = priv;
    struct priv_owner *provider = hw->priv;
    // Keep the ordinary shared reader unless the current track explicitly
    // opts into dual decoding. The provider may predate a P7 track change.
    if (!hw->ra_ctx->vo->opts->android_dovi_dual)
        return av_buffer_ref(provider->hwctx.av_device_ref);
    // Fail during decoder selection, not after decoding has succeeded into an
    // unmappable Surface. mpv can then choose its normal copy/software fallback.
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    const struct ra_format *fmt = ra_find_named_format(hw->ra_ctx->ra, "r32f");
    if (gl->es < 300 || !gl_check_extension(gl->extensions, "GL_EXT_YUV_target") ||
        !fmt || !fmt->renderable || !fmt->linear_filter) {
        MP_WARN(hw, "P7 dual HW+ unavailable: requires raw YUV and renderable "
                    "linear R32F; trying normal decoder fallback.\n");
        return NULL;
    }
    JNIEnv *env = MP_JNI_GET_ENV(hw);
    struct priv_owner *p = talloc_zero(NULL, struct priv_owner);
    AVBufferRef *ref = NULL;
    if (!p)
        return NULL;
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
    MP_INFO(hw, "P7 dual: allocated independent decoder Surface/reader %p.\n",
            p->reader);
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
    const int max_images = hw->ra_ctx->vo->opts->android_dovi_yuv ? 2 : 3;
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
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->cond);

    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    p->GetNativeClientBufferANDROID =
        (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->GetNativeClientBufferANDROID || !p->EGLImageTargetTexture2DOES)
        return -1;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;

    p->dovi_yuv = mapper->owner->ra_ctx->vo->opts->android_dovi_dual ||
        (mapper->owner->ra_ctx->vo->opts->android_dovi_yuv &&
         mapper->src_params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
         mapper->src_params.repr.dovi);
    if (p->dovi_yuv && gl_check_extension(
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
    if (p->dovi_yuv && !init_yuv_bridge(mapper))
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

    if (p->dovi_yuv)
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

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    if (!mapper->tex[0])
        return -1;

    return 0;
}

static void release_image(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = p->reader_owner;
    int release_fd = EGL_NO_NATIVE_FENCE_FD_ANDROID;

    // DrawArrays only submits the OES -> private-plane copy. AImage_delete
    // without a release fence lets the decoder overwrite that input while
    // the GPU still reads it, producing spatially torn/shifted blocks. This
    // synchronization is limited to the explicit Dolby YUV bridge; ordinary
    // retained-OES playback is unchanged.
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
            MP_INFO(mapper, "Dolby YUV buffer release synchronized using %s.\n",
                release_fd >= 0 ? "native GPU fence" : "completed GPU copy");
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

    if (p->reader_owner)
        p->reader_owner->AImageReader_setImageListener(p->reader_owner->reader, NULL);

    release_image(mapper);
    talloc_free(p->cached_source);
    av_buffer_unref(&p->stream_device);

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    for (int i = 0; i < 3; i++)
        ra_tex_free(mapper->ra, &mapper->tex[i]);
    if (p->yuv_program)
        gl->DeleteProgram(p->yuv_program);
    if (p->yuv_fbo)
        gl->DeleteFramebuffers(1, &p->yuv_fbo);
    if (p->yuv_vao)
        gl->DeleteVertexArrays(1, &p->yuv_vao);

    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    // OES still needs the last valid image when the next Surface frame is late.
    // The Dolby Vision bridge has already copied its pixels to private textures.
    if (p->dovi_yuv)
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
        if (p->reader_owner)
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
        (p->dovi_yuv || p->image))
        return 0;

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
                   (p->image || (p->dovi_yuv && p->cached_source)) ? 0 : -1;
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

    AImage *next_image = NULL;
    media_status_t ret = o->AImageReader_acquireLatestImage(o->reader, &next_image);
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
    mp_assert(next_image);

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
    if (!p->dovi_yuv &&
        (mapper->tex[0]->params.w != d.width || mapper->tex[0]->params.h != d.height)) {
        MP_VERBOSE(p, "Texture dimensions changed to %dx%d\n", d.width, d.height);
        mapper->tex[0]->params.w = d.width;
        mapper->tex[0]->params.h = d.height;
    }

    EGLClientBuffer buf = p->GetNativeClientBufferANDROID(hwbuf);
    if (!buf) {
        o->AImage_delete(next_image);
        return -1;
    }

    const int attribs[] = {EGL_NONE};
    EGLImageKHR next_egl_image = p->CreateImageKHR(eglGetCurrentDisplay(),
        EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, attribs);
    if (!next_egl_image) {
        o->AImage_delete(next_image);
        return -1;
    }

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, next_egl_image);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    release_image(mapper);
    p->image = next_image;
    p->egl_image = next_egl_image;

    if (p->dovi_yuv) {
        MP_TRACE(mapper, "YUV image acquired pts=%f\n", mapper->src->pts);
        p->image_gpu_used = true; // Also synchronize a partially failed copy.
        if (!map_yuv_bridge(mapper, d.width, d.height))
            return -1;
        MP_TRACE(mapper, "YUV copy submitted pts=%f\n", mapper->src->pts);
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
