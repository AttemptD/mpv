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

#include "video/out/android_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    bool hdr_active;
};

struct config_refine_ctx {
    EGLDisplay display;
    bool request_hdr;
};

static int refine_config(void *user_data, EGLConfig *configs, int num_configs)
{
    struct config_refine_ctx *refine = user_data;
    if (!refine->request_hdr)
        return 0;

    // HDR10 output needs a 10-bit swapchain. Prefer RGB10_A2, but allow any
    // config with at least 10 bits per color component.
    for (int n = 0; n < num_configs; n++) {
        EGLint r = 0, g = 0, b = 0;
        eglGetConfigAttrib(refine->display, configs[n], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(refine->display, configs[n], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(refine->display, configs[n], EGL_BLUE_SIZE, &b);
        if (r >= 10 && g >= 10 && b >= 10)
            return n;
    }

    return 0;
}

static pl_color_space_t android_preferred_csp(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    return p->hdr_active ? pl_color_space_hdr10 : pl_color_space_srgb;
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
    eglSwapBuffers(p->egl_display, p->egl_surface);
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_android_uninit(ctx->vo);
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    struct config_refine_ctx refine = {
        .display = p->egl_display,
        .request_hdr = ctx->vo->opts->android_hdr_output,
    };
    EGLConfig config;
    if (!mpegl_create_context_cb(ctx, p->egl_display, (struct mpegl_cb) {
            .refine_config = refine_config,
            .user_data = &refine,
        }, &p->egl_context, &config))
        goto fail;

    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
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
    bool use_hdr = refine.request_hdr && pq_supported &&
                   red_size >= 10 && green_size >= 10 && blue_size >= 10;
    const EGLint hdr_attribs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_BT2020_PQ_EXT,
        EGL_NONE,
    };

    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
        (EGLNativeWindowType)native_window, use_hdr ? hdr_attribs : NULL);

    if (p->egl_surface == EGL_NO_SURFACE && use_hdr) {
        MP_WARN(ctx, "Could not create BT.2020/PQ EGL surface; falling back to SDR.\n");
        p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
            (EGLNativeWindowType)native_window, NULL);
        use_hdr = false;
    }

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "Could not create EGL surface!\n");
        goto fail;
    }

    p->hdr_active = use_hdr;
    MP_INFO(ctx, "Android output colorspace: %s (%d/%d/%d-bit EGL)\n",
            p->hdr_active ? "BT.2020/PQ HDR10" : "sRGB SDR",
            red_size, green_size, blue_size);

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_FATAL(ctx, "Failed to set context!\n");
        goto fail;
    }

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
