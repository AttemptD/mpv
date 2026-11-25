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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include "video/out/android_common.h"
#include "android_gl_trace.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"
#include "android_video_layer.h"
#include "ra_gl.h"

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct android_gl_trace *trace;
    EGLint last_swap_error;
    bool hdr_active;
    int32_t (*set_dataspace)(ANativeWindow *, int32_t);
    void *nativewindow_lib;
    struct mp_android_video_layer *video_layer;
    int layer_w, layer_h;
};

struct config_refine_ctx {
    EGLDisplay display;
    bool request_hdr;
};

static int refine_config(void *user_data, EGLConfig *configs, int num_configs)
{
    struct config_refine_ctx *refine = user_data;
    // HDR10 output needs a 10-bit swapchain. Prefer RGB10_A2, but allow any
    // config with at least 10 bits per color component.
    for (int n = 0; n < num_configs; n++) {
        EGLint r = 0, g = 0, b = 0;
        eglGetConfigAttrib(refine->display, configs[n], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(refine->display, configs[n], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(refine->display, configs[n], EGL_BLUE_SIZE, &b);
        if (refine->request_hdr ? r >= 10 && g >= 10 && b >= 10
                                : r == 8 && g == 8 && b == 8)
            return n;
    }

    return 0;
}

static pl_color_space_t android_preferred_csp(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct pl_color_space csp = pl_color_space_srgb;
    if (p->hdr_active) {
        csp = pl_color_space_hdr10;
        csp.hdr.max_luma = 1000;
        csp.hdr.min_luma = 0.0001;
    }
    return csp;
}

static void set_hdr_metadata(struct ra_ctx *ctx, const char *extensions)
{
    struct priv *p = ctx->priv;
    if (!p->hdr_active || !extensions ||
        !strstr(extensions, "EGL_EXT_surface_SMPTE2086_metadata"))
        return;
    // The compatibility conversion targets this BT.2020/D65, 1000-nit
    // output volume. Coordinates and luminances use EGL's 50000 scale.
    const EGLint metadata[][2] = {
        {EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, 35400},
        {EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, 14600},
        {EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, 8500},
        {EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, 39850},
        {EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, 6550},
        {EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, 2300},
        {EGL_SMPTE2086_WHITE_POINT_X_EXT, 15635},
        {EGL_SMPTE2086_WHITE_POINT_Y_EXT, 16450},
        {EGL_SMPTE2086_MAX_LUMINANCE_EXT, 1000 * EGL_METADATA_SCALING_EXT},
        {EGL_SMPTE2086_MIN_LUMINANCE_EXT, 5},
    };
    bool ok = true;
    for (int i = 0; i < MP_ARRAY_SIZE(metadata); i++)
        ok = eglSurfaceAttrib(p->egl_display, p->egl_surface,
                              metadata[i][0], metadata[i][1]) && ok;
    MP_INFO(ctx, "HDR output mastering metadata %s.\n", ok ? "accepted" : "rejected");
}

static bool android_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    if (params)
        params->color = android_preferred_csp(ctx);
    return true;
}

static void android_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (p->video_layer) {
        mp_android_video_layer_present(p->video_layer);
        return;
    }
    if (p->hdr_active && p->set_dataspace)
        p->set_dataspace(vo_android_native_window(ctx->vo),
            (6 << 16) | (7 << 22) | (1 << 27)); // BT.2020 / ST2084 / full RGB
    if (eglSwapBuffers(p->egl_display, p->egl_surface)) {
        p->last_swap_error = EGL_SUCCESS;
    } else {
        EGLint error = eglGetError();
        if (error != p->last_swap_error) {
            MP_ERR(ctx, "eglSwapBuffers failed: EGL error 0x%04x, surface=%dx%d.\n",
                   (unsigned)error, ctx->vo->dwidth, ctx->vo->dheight);
            p->last_swap_error = error;
        }
    }
}

static bool android_layer_acquire(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    return !p->video_layer || mp_android_video_layer_acquire(p->video_layer);
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);
    mp_android_video_layer_destroy(&p->video_layer);
    android_gl_trace_destroy(p->trace);

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_android_uninit(ctx->vo);
    if (p->nativewindow_lib) dlclose(p->nativewindow_lib);
}

static bool android_refresh_parent(struct ra_ctx *ctx, EGLConfig config,
                                   ANativeWindow *window, int width, int height)
{
    struct priv *p = ctx->priv;
    EGLint format;
    if (!eglGetConfigAttrib(p->egl_display, config, EGL_NATIVE_VISUAL_ID, &format) ||
        ANativeWindow_setBuffersGeometry(window, width, height, format) != 0)
        return false;
    EGLSurface parent = eglCreateWindowSurface(p->egl_display, config,
        (EGLNativeWindowType)window, NULL);
    if (parent == EGL_NO_SURFACE) return false;
    bool ok = eglMakeCurrent(p->egl_display, parent, parent, p->egl_context);
    if (ok) {
        // A normal player can leave an old force-window buffer on BLAST.
        // Its dimensions clip/scale child SurfaceControls even after resizing
        // the holder. Replace it once at the new size; video stays on the
        // independent YUV layer and never copies through this RGB buffer.
        p->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        p->gl.Disable(GL_SCISSOR_TEST);
        p->gl.ClearColor(0, 0, 0, 0);
        p->gl.Clear(GL_COLOR_BUFFER_BIT);
        ok = eglSwapBuffers(p->egl_display, parent);
        p->gl.Finish();
    }
    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context))
        ok = false;
    eglDestroySurface(p->egl_display, parent);
    if (ok) MP_VERBOSE(ctx, "Refreshed direct-video parent buffer: %dx%d.\n", width, height);
    return ok;
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "eglInitialize failed: EGL error 0x%04x.\n", (unsigned)eglGetError());
        goto fail;
    }

    struct config_refine_ctx refine = {
        .display = p->egl_display,
        .request_hdr = ctx->vo->opts->android_hdr_output && !ctx->vo->opts->android_video_layer,
    };
    EGLConfig config;
    if (!mpegl_create_context_cb(ctx, p->egl_display, (struct mpegl_cb) {
            .refine_config = refine_config,
            .user_data = &refine,
        }, &p->egl_context, &config))
        goto fail;

    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (ctx->vo->opts->android_video_layer) {
        if (strcmp(ctx->vo->driver->name, "gpu-next")) {
            MP_ERR(ctx, "android-video-layer requires gpu-next.\n");
            goto fail;
        }
        p->egl_surface = eglCreatePbufferSurface(p->egl_display, config,
            (EGLint[]){EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE});
        if (p->egl_surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(p->egl_display,p->egl_surface,p->egl_surface,p->egl_context))
            goto fail;
        mpegl_load_functions(&p->gl, ctx->log);
        if (p->gl.es < 300 || !gl_check_extension(p->gl.extensions, "GL_EXT_YUV_target"))
            goto fail;
        if (!vo_android_surface_size(ctx->vo, &p->layer_w, &p->layer_h) ||
            !android_refresh_parent(ctx, config, native_window, p->layer_w, p->layer_h)) {
            MP_ERR(ctx, "Could not refresh the direct-video parent buffer.\n");
            goto fail;
        }
        p->hdr_active = ctx->vo->opts->android_hdr_output;
        if (!ra_gl_ctx_init(ctx, &p->gl, (struct ra_ctx_params){
                .preferred_csp=android_preferred_csp, .set_color=android_set_color,
                .check_visible=android_layer_acquire,
                .swap_buffers=android_swap_buffers}))
            goto fail;
        p->video_layer = mp_android_video_layer_create(ctx, native_window,
            p->layer_w, p->layer_h, p->hdr_active);
        if (!p->video_layer) goto fail;
        ra_gl_ctx_resize(ctx->swapchain, p->layer_w,p->layer_h,
            mp_android_video_layer_fbo(p->video_layer));
        return true;
    }
    EGLint format;
    eglGetConfigAttrib(p->egl_display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, format);

    EGLint red_size = 0, green_size = 0, blue_size = 0;
    eglGetConfigAttrib(p->egl_display, config, EGL_RED_SIZE, &red_size);
    eglGetConfigAttrib(p->egl_display, config, EGL_GREEN_SIZE, &green_size);
    eglGetConfigAttrib(p->egl_display, config, EGL_BLUE_SIZE, &blue_size);

    const char *extensions = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    bool pq_supported = extensions &&
        strstr(extensions, "EGL_EXT_gl_colorspace_bt2020_pq");
    bool use_hdr = refine.request_hdr &&
                   red_size >= 10 && green_size >= 10 && blue_size >= 10;
    if (use_hdr && !pq_supported)
        MP_INFO(ctx, "Probing a PQ EGL surface despite the missing extension advertisement.\n");
    const EGLint hdr_attribs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_BT2020_PQ_EXT,
        EGL_NONE,
    };

    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
        (EGLNativeWindowType)native_window, use_hdr ? hdr_attribs : NULL);

    if (p->egl_surface == EGL_NO_SURFACE && use_hdr) {
        MP_WARN(ctx, "eglCreateWindowSurface failed for BT.2020/PQ: EGL error 0x%04x. Retrying SDR.\n",
                (unsigned)eglGetError());
        p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
            (EGLNativeWindowType)native_window, NULL);
        use_hdr = false;
    }

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "eglCreateWindowSurface failed: EGL error 0x%04x, color=%d/%d/%d-bit.\n",
                 (unsigned)eglGetError(), red_size, green_size, blue_size);
        goto fail;
    }

    // Some Android TV EGL drivers omit the PQ EGL extension but support the
    // public NDK native-window dataspace API. The shader still generates real
    // PQ pixels; this sets their matching contract, not a relabelled SDR image.
    if (!use_hdr && refine.request_hdr && red_size >= 10 && green_size >= 10 && blue_size >= 10) {
        p->set_dataspace = (void *)dlsym(RTLD_DEFAULT, "ANativeWindow_setBuffersDataSpace");
        if (!p->set_dataspace) {
            p->nativewindow_lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
            if (p->nativewindow_lib)
                p->set_dataspace = (void *)dlsym(p->nativewindow_lib, "ANativeWindow_setBuffersDataSpace");
        }
        if (p->set_dataspace && !p->set_dataspace(native_window,
                (6 << 16) | (7 << 22) | (1 << 27))) {
            use_hdr = true;
            MP_INFO(ctx, "PQ output requested through NDK native-window dataspace.\n");
        } else {
            MP_WARN(ctx, "Native-window PQ dataspace request unavailable or rejected.\n");
            p->set_dataspace = NULL;
        }
    }
    p->hdr_active = use_hdr;
    set_hdr_metadata(ctx, extensions);
    MP_INFO(ctx, "Android output colorspace: %s (%d/%d/%d-bit EGL)\n",
            p->hdr_active ? "BT.2020/PQ HDR10" : "sRGB SDR",
            red_size, green_size, blue_size);

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_FATAL(ctx, "eglMakeCurrent failed: EGL error 0x%04x.\n", (unsigned)eglGetError());
        goto fail;
    }

    if (ctx->vo->opts->android_gl_trace)
        p->trace = android_gl_trace_create(p, ctx->log);
    if (p->trace)
        mpegl_load_functions_cb(&p->gl, ctx->log, android_gl_trace_get_proc_address, p->trace);
    else
        mpegl_load_functions(&p->gl, ctx->log);

    struct ra_ctx_params params = {
        .preferred_csp = android_preferred_csp,
        .set_color = android_set_color,
        .swap_buffers = android_swap_buffers,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    android_uninit(ctx);
    return false;
}

static bool android_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_android_surface_size(ctx->vo, &w, &h))
        return false;

    struct priv *p = ctx->priv;
    if (p->video_layer) {
        if (w != p->layer_w || h != p->layer_h) {
            MP_VERBOSE(ctx, "Android video-layer size changed; recreate the output.\n");
            return false;
        }
        ctx->vo->dwidth=w; ctx->vo->dheight=h;
        return true;
    }

    // Update window geometry to prevent screen tearing
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (native_window) {
        int32_t current_format = ANativeWindow_getFormat(native_window);
        ANativeWindow_setBuffersGeometry(native_window, w, h, current_format);
    }

    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;
    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);
    return true;
}

static int android_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    struct priv *p = ctx->priv;
    if (request == VOCTRL_GET_ANDROID_VIDEO_LAYER && p->video_layer) {
        mp_android_video_layer_get_stats(p->video_layer, arg);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_android = {
    .type           = "opengl",
    .name           = "android",
    .description    = "Android/EGL",
    .reconfig       = android_reconfig,
    .control        = android_control,
    .init           = android_init,
    .uninit         = android_uninit,
};
