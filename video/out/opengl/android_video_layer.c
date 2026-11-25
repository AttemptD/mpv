// Opt-in GPU color conversion into a real Android video layer. The public
// 8-bit YUV allocation is explicit: no hidden resolution or precision change.
#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/common.h"
#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "sub/osd.h"
#include "video/out/gpu/context.h"
#include "video/out/vo.h"
#include "video/out/opengl/common.h"
#include "video/out/opengl/ra_gl.h"
#include "android_video_layer.h"
#include "android_caption_blend.h"

// Keep OSD preparation and posting off the video presentation thread.
struct mp_android_captions {
    struct vo *vo;
    jobject host;
    jmethodID geometry;
    jmethodID scaled_geometry, max_width;
    ANativeWindow *window;
    int w, h, display_w, display_h;
    int64_t change;
    atomic_int_fast64_t updates;
    atomic_bool failed;
    mp_thread thread;
    mp_mutex lock;
    mp_cond wakeup;
    bool thread_started, stopping;
    bool pending;
    struct mp_osd_res pending_res;
    double pending_pts;
};

static void *caption_worker(void *opaque);

static bool captions_java_ok(struct mp_android_captions *p, JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    MP_ERR(p->vo, "Independent caption Surface operation failed.\n");
    return false;
}

struct mp_android_captions *mp_android_captions_create(struct vo *vo, bool native_presentation)
{
    JNIEnv *env = MP_JNI_GET_ENV(vo);
    if (!env || !vo->opts->android_native_dolby_surface) return NULL;
    struct mp_android_captions *p = talloc_zero(NULL, struct mp_android_captions);
    p->vo = vo; p->change = -1;
    atomic_init(&p->updates, 0);
    atomic_init(&p->failed, false);
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->wakeup);
    p->host = (*env)->NewGlobalRef(env, (jobject)(intptr_t)vo->opts->android_native_dolby_surface);
    if (!p->host) goto fail;
    jclass cls = (*env)->GetObjectClass(env, p->host);
    jmethodID get_surface = (*env)->GetMethodID(env, cls, "getCaptionSurface", "()Landroid/view/Surface;");
    if (!get_surface) {
        (*env)->DeleteLocalRef(env, cls);
        goto fail;
    }
    // The native VO owns visibility and places the caption layer with its
    // video geometry. The GPU path needs its captions-only Surface methods.
    if (!native_presentation) {
        p->geometry = (*env)->GetMethodID(env, cls, "setGpuCaptions", "(IIZ)V");
        if (!p->geometry) {
            (*env)->DeleteLocalRef(env, cls);
            goto fail;
        }
        p->scaled_geometry = (*env)->GetMethodID(env, cls, "setGpuCaptionsScaled", "(IIIIZ)V");
        if (!p->scaled_geometry) (*env)->ExceptionClear(env);
        if (p->scaled_geometry) {
            p->max_width = (*env)->GetMethodID(env, cls, "getCaptionMaxWidth", "()I");
            if (!p->max_width) (*env)->ExceptionClear(env);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    if (!captions_java_ok(p, env)) goto fail;
    jobject surface = (*env)->CallObjectMethod(env, p->host, get_surface);
    if (!captions_java_ok(p, env) || !surface) goto fail;
    p->window = ANativeWindow_fromSurface(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    if (!p->window) goto fail;
    if (mp_thread_create(&p->thread, caption_worker, p)) goto fail;
    p->thread_started = true;
    return p;
fail:
    captions_java_ok(p, env);
    mp_android_captions_destroy(&p);
    return NULL;
}

static bool composite_captions(struct mp_android_captions *p,
                              const struct mp_osd_res *res, int display_w, int display_h,
                              struct sub_bitmap_list *list)
{
    if (p->w != res->w || p->h != res->h || p->display_w != display_w || p->display_h != display_h) {
        if (p->geometry) {
            JNIEnv *env = MP_JNI_GET_ENV(p->vo);
            if (!env) return false;
            if (p->scaled_geometry)
                (*env)->CallVoidMethod(env, p->host, p->scaled_geometry, res->w, res->h, display_w, display_h, JNI_TRUE);
            else
                (*env)->CallVoidMethod(env, p->host, p->geometry, res->w, res->h, JNI_TRUE);
            if (!captions_java_ok(p, env)) return false;
        }
        if (ANativeWindow_setBuffersGeometry(p->window, res->w, res->h,
                                             WINDOW_FORMAT_RGBA_8888) < 0)
            return false;
        p->w = res->w; p->h = res->h;
        p->display_w = display_w; p->display_h = display_h;
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(p->window, &buffer, NULL) < 0) return false;
    for (int y = 0; y < buffer.height; y++)
        memset((uint8_t *)buffer.bits + y * buffer.stride * 4, 0, buffer.width * 4);
    int *xmap = talloc_array(list, int, buffer.width);
    uint8_t *rgba = talloc_array(list, uint8_t, buffer.width * 4);
    for (int i = 0; i < list->num_items; i++) {
        struct sub_bitmaps *images = list->items[i];
        for (int j = 0; j < images->num_parts; j++) {
            struct sub_bitmap *b = &images->parts[j];
            mp_android_caption_blend_bgra(buffer.bits, buffer.stride * 4, buffer.width, buffer.height,
                b->bitmap, b->stride, b->w, b->h, b->x, b->y, b->dw, b->dh, xmap, rgba);
        }
    }
    bool ok = ANativeWindow_unlockAndPost(p->window) >= 0;
    if (ok) atomic_fetch_add(&p->updates, 1);
    return ok;
}

static void *caption_worker(void *opaque)
{
    struct mp_android_captions *p = opaque;
    mp_thread_set_name("tv-captions");
    for (;;) {
        mp_mutex_lock(&p->lock);
        while (!p->pending && !p->stopping) mp_cond_wait(&p->wakeup, &p->lock);
        struct mp_osd_res res = p->pending_res;
        double pts = p->pending_pts;
        p->pending = false;
        bool stopping = p->stopping;
        mp_mutex_unlock(&p->lock);
        if (stopping) break;
        int display_w = res.w, display_h = res.h;
        if (p->max_width) {
            JNIEnv *env = MP_JNI_GET_ENV(p->vo);
            int limit = env ? (*env)->CallIntMethod(env, p->host, p->max_width) : 0;
            if (!env || !captions_java_ok(p, env)) {
                atomic_store(&p->failed, true); continue;
            }
            if (limit > 0 && limit < res.w) {
                res.h = MPMAX(1, (int64_t)res.h * limit / res.w);
                res.mt = (int64_t)res.mt * limit / res.w;
                res.mb = (int64_t)res.mb * limit / res.w;
                res.ml = (int64_t)res.ml * limit / res.w;
                res.mr = (int64_t)res.mr * limit / res.w;
                res.w = limit;
            }
        }
        const bool formats[SUBBITMAP_COUNT] = {[SUBBITMAP_BGRA] = true};
        // OSD/PGS preparation can itself be expensive. Its state/decoder
        // locks already support rendering from filter or VO threads.
        struct sub_bitmap_list *list = osd_render(p->vo->osd, res, pts, 0, formats);
        if (list->change_id != p->change || p->w != res.w || p->h != res.h ||
            p->display_w != display_w || p->display_h != display_h) {
            if (composite_captions(p, &res, display_w, display_h, list)) p->change = list->change_id;
            else {
                atomic_store(&p->failed, true);
                MP_ERR(p->vo, "Independent caption worker failed.\n");
            }
        }
        talloc_free(list);
    }
    return NULL;
}

bool mp_android_captions_draw(struct mp_android_captions *p,
                            const struct mp_osd_res *res, double pts)
{
    if (atomic_load(&p->failed)) return false;
    if (res->w <= 0 || res->h <= 0) return true;
    mp_mutex_lock(&p->lock);
    // Only the latest timestamp and geometry are queued. The worker owns
    // bitmap preparation, compositing and posting, without borrowing a frame.
    p->pending = true; p->pending_res = *res; p->pending_pts = pts;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
    return true;
}

int64_t mp_android_captions_updates(const struct mp_android_captions *p)
{
    return p ? atomic_load(&p->updates) : 0;
}

void mp_android_captions_destroy(struct mp_android_captions **ptr)
{
    struct mp_android_captions *p = *ptr;
    if (!p) return;
    mp_mutex_lock(&p->lock);
    p->stopping = true;
    mp_cond_signal(&p->wakeup);
    mp_mutex_unlock(&p->lock);
    if (p->thread_started) mp_thread_join(p->thread);
    JNIEnv *env = MP_JNI_GET_ENV(p->vo);
    if (env && p->host) {
        if (p->geometry) {
            (*env)->CallVoidMethod(env, p->host, p->geometry, 0, 0, JNI_FALSE);
            captions_java_ok(p, env);
        }
        (*env)->DeleteGlobalRef(env, p->host);
    }
    if (p->window) ANativeWindow_release(p->window);
    mp_cond_destroy(&p->wakeup);
    mp_mutex_destroy(&p->lock);
    talloc_free(p); *ptr = NULL;
}

#define SLOTS 4

struct layer_owner {
    atomic_int refs;
    mp_mutex lock;
    mp_cond cond;
    bool closing;
    bool available[SLOTS];
    int fence[SLOTS];
    int64_t submitted, completed, unique_latches, merged_latches, invalid_latches;
    int64_t latch_times[16];
    unsigned latch_cursor;
    JavaVM *vm;
    jobject hdr_pool;
    jmethodID close_pool;
    ASurfaceControl *control;
    void *library;
    // Resolve API 26/29 functions at runtime; the ordinary APK still targets
    // API 23. Avoid referencing unavailable NDK function declarations here.
    ASurfaceControl *(*ASurfaceControl_createFromWindow)(ANativeWindow *, const char *);
    void (*ASurfaceControl_release)(ASurfaceControl *);
    ASurfaceTransaction *(*ASurfaceTransaction_create)(void);
    void (*ASurfaceTransaction_delete)(ASurfaceTransaction *);
    void (*ASurfaceTransaction_apply)(ASurfaceTransaction *);
    void (*ASurfaceTransaction_setVisibility)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*ASurfaceTransaction_setZOrder)(ASurfaceTransaction *, ASurfaceControl *, int32_t);
    void (*ASurfaceTransaction_setGeometry)(ASurfaceTransaction *, ASurfaceControl *, const ARect *, const ARect *, int32_t);
    void (*ASurfaceTransaction_setBuffer)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int);
    void (*ASurfaceTransaction_setBufferDataSpace)(ASurfaceTransaction *, ASurfaceControl *, enum ADataSpace);
    void (*ASurfaceTransaction_setHdrMetadata_smpte2086)(ASurfaceTransaction *, ASurfaceControl *, struct AHdrMetadata_smpte2086 *);
    void (*ASurfaceTransaction_setOnComplete)(ASurfaceTransaction *, void *, ASurfaceTransaction_OnComplete);
    void (*ASurfaceTransaction_setEnableBackPressure)(ASurfaceTransaction *, ASurfaceControl *, bool);
    int (*ASurfaceTransactionStats_getPreviousReleaseFenceFd)(ASurfaceTransactionStats *, ASurfaceControl *);
    int64_t (*ASurfaceTransactionStats_getLatchTime)(ASurfaceTransactionStats *);
    int (*AHardwareBuffer_allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
    void (*AHardwareBuffer_release)(AHardwareBuffer *);
    void (*AHardwareBuffer_acquire)(AHardwareBuffer *);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    AHardwareBuffer *(*AHardwareBuffer_fromHardwareBuffer)(JNIEnv *, jobject);
};

struct mp_android_video_layer {
    struct mp_log *log;
    struct layer_owner *owner;
    GL *gl;
    EGLDisplay display;
    AHardwareBuffer *buffers[SLOTS];
    EGLImageKHR images[SLOTS];
    GLuint textures[SLOTS], fbo;
    GLenum texture_target;
    int current, acquired, next, w, h;
    bool hdr, rgb, finish;
    int finish_samples;
    int slow_acquires, slow_presents;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client_buffer;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    void (GLAPIENTRY *bind_image)(GLenum, void *);
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
};

struct completion {
    struct layer_owner *owner;
    int previous;
};

static void owner_unref(struct layer_owner *o)
{
    if (atomic_fetch_sub(&o->refs, 1) != 1) return;
    for (int i = 0; i < SLOTS; i++)
        if (o->fence[i] >= 0) close(o->fence[i]);
    if (o->control) o->ASurfaceControl_release(o->control);
    if (o->hdr_pool && o->vm) {
        JNIEnv *env = NULL;
        bool attached = (*o->vm)->GetEnv(o->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
        if (attached && (*o->vm)->AttachCurrentThread(o->vm, &env, NULL) != JNI_OK)
            env = NULL;
        if (env) {
            (*env)->CallVoidMethod(env, o->hdr_pool, o->close_pool);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
            }
            (*env)->DeleteGlobalRef(env, o->hdr_pool);
        }
        if (attached && env) (*o->vm)->DetachCurrentThread(o->vm);
    }
    if (o->library) dlclose(o->library);
    mp_mutex_destroy(&o->lock);
    mp_cond_destroy(&o->cond);
    free(o);
}

static void completed(void *opaque, ASurfaceTransactionStats *stats)
{
    struct completion *c = opaque;
    struct layer_owner *o = c->owner;
    int64_t latch = o->ASurfaceTransactionStats_getLatchTime
        ? o->ASurfaceTransactionStats_getLatchTime(stats) : 0;
    mp_mutex_lock(&o->lock);
    o->completed++;
    if (latch > 0) {
        bool duplicate = false;
        for (int i = 0; i < MP_ARRAY_SIZE(o->latch_times); i++)
            duplicate |= o->latch_times[i] == latch;
        if (duplicate) o->merged_latches++;
        else {
            o->unique_latches++;
            o->latch_times[o->latch_cursor++ % MP_ARRAY_SIZE(o->latch_times)] = latch;
        }
    } else if (o->ASurfaceTransactionStats_getLatchTime) {
        o->invalid_latches++;
    }
    mp_mutex_unlock(&o->lock);
    if (c->previous >= 0) {
        int fd = o->ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, o->control);
        mp_mutex_lock(&o->lock);
        if (o->closing) {
            if (fd >= 0) close(fd);
        } else {
            if (o->fence[c->previous] >= 0) close(o->fence[c->previous]);
            o->fence[c->previous] = fd;
            o->available[c->previous] = true;
            mp_cond_signal(&o->cond);
        }
        mp_mutex_unlock(&o->lock);
    }
    free(c);
    owner_unref(o);
}

static bool create_hdr_pool(struct mp_android_video_layer *p, struct ra_ctx *ctx)
{
    struct layer_owner *o = p->owner;
    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    jobject host = (jobject)(intptr_t)ctx->vo->opts->android_native_dolby_surface;
    if (!env || !host) return false;
    bool ok = false;
    jobject local_pool = NULL;
    jmethodID close_pool = NULL;
    if ((*env)->PushLocalFrame(env, 16) < 0) return false;
    if ((*env)->GetJavaVM(env, &o->vm) != JNI_OK) goto done;
    jclass host_class = (*env)->GetObjectClass(env, host);
    if (!host_class) goto done;
    jmethodID create = (*env)->GetMethodID(env, host_class, "createHdrBufferPool", "(III)Ljava/lang/Object;");
    if (!create) goto done;
    local_pool = (*env)->CallObjectMethod(env, host, create, p->w, p->h, SLOTS);
    if ((*env)->ExceptionCheck(env) || !local_pool) goto done;
    jclass pool_class = (*env)->GetObjectClass(env, local_pool);
    if (!pool_class) goto done;
    close_pool = (*env)->GetMethodID(env, pool_class, "close", "()V");
    if (!close_pool) goto done;
    jmethodID get_buffer = (*env)->GetMethodID(env, pool_class, "getBuffer", "(I)Landroid/hardware/HardwareBuffer;");
    if (!get_buffer) goto done;
    o->hdr_pool = (*env)->NewGlobalRef(env, local_pool);
    o->close_pool = close_pool;
    if (!o->hdr_pool) goto done;
#define POOL_LOAD(name) do { \
    *(void **)(&o->name) = dlsym(o->library, #name); \
    if (!o->name) goto done; \
} while (0)
    POOL_LOAD(AHardwareBuffer_acquire); POOL_LOAD(AHardwareBuffer_describe);
    POOL_LOAD(AHardwareBuffer_fromHardwareBuffer);
#undef POOL_LOAD
    for (int i = 0; i < SLOTS; i++) {
        jobject buffer = (*env)->CallObjectMethod(env, local_pool, get_buffer, i);
        if ((*env)->ExceptionCheck(env) || !buffer) goto done;
        AHardwareBuffer *hardware = o->AHardwareBuffer_fromHardwareBuffer(env, buffer);
        if (!hardware) goto done;
        o->AHardwareBuffer_acquire(hardware);
        p->buffers[i] = hardware;
        AHardwareBuffer_Desc desc;
        o->AHardwareBuffer_describe(hardware, &desc);
        if (desc.width != p->w || desc.height != p->h || desc.format != 0x11 ||
            !(desc.usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)) {
            MP_ERR(p, "HDR buffer %d has incompatible geometry, format or GPU usage.\n", i);
            goto done;
        }
        for (int n = 0; n < i; n++) {
            if (p->buffers[n] == hardware) {
                MP_ERR(p, "HDR pool reused a retained buffer.\n");
                goto done;
            }
        }
        (*env)->DeleteLocalRef(env, buffer);
    }
    ok = true;
done:
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (!o->hdr_pool && local_pool && close_pool) {
        (*env)->CallVoidMethod(env, local_pool, close_pool);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    }
    (*env)->PopLocalFrame(env, NULL);
    if (!ok) MP_ERR(p, "Experimental generated HDR buffer pool failed.\n");
    return ok;
}

struct mp_android_video_layer *mp_android_video_layer_create(
    struct ra_ctx *ctx, ANativeWindow *parent, int w, int h, bool hdr)
{
    struct mp_android_video_layer *p = talloc_zero(NULL, struct mp_android_video_layer);
    p->log = ctx->log; p->gl = ra_gl_get(ctx->ra);
    p->w = w; p->h = h; p->hdr = hdr; p->current = p->acquired = -1;
    int format = ctx->vo->opts->android_video_layer_format;
    p->rgb = format != 0;
    p->finish = ctx->vo->opts->android_video_layer_sync != 0;
    p->texture_target = p->rgb ? GL_TEXTURE_2D : GL_TEXTURE_EXTERNAL_OES;
    p->display = eglGetCurrentDisplay();
    struct layer_owner *o = p->owner = calloc(1, sizeof(*o));
    if (!o) goto fail;
    atomic_init(&o->refs, 1);
    mp_mutex_init(&o->lock); mp_cond_init(&o->cond);
    for (int i = 0; i < SLOTS; i++) {o->available[i] = true; o->fence[i] = -1;}
    o->library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!o->library) goto fail;
#define LOAD(name) do { *(void **)(&o->name) = dlsym(o->library, #name); if (!o->name) goto fail; } while (0)
    LOAD(ASurfaceControl_createFromWindow); LOAD(ASurfaceControl_release);
    LOAD(ASurfaceTransaction_create); LOAD(ASurfaceTransaction_delete);
    LOAD(ASurfaceTransaction_apply); LOAD(ASurfaceTransaction_setVisibility);
    LOAD(ASurfaceTransaction_setZOrder); LOAD(ASurfaceTransaction_setGeometry);
    LOAD(ASurfaceTransaction_setBuffer); LOAD(ASurfaceTransaction_setBufferDataSpace);
    LOAD(ASurfaceTransaction_setHdrMetadata_smpte2086);
    LOAD(ASurfaceTransaction_setOnComplete); LOAD(ASurfaceTransactionStats_getPreviousReleaseFenceFd);
    LOAD(AHardwareBuffer_allocate); LOAD(AHardwareBuffer_release);
#undef LOAD
    // Diagnostic only; do not reject an otherwise usable presentation path
    // on firmware that lacks the public latch timestamp entry point.
    *(void **)(&o->ASurfaceTransactionStats_getLatchTime) =
        dlsym(o->library, "ASurfaceTransactionStats_getLatchTime");
    // API 31+: default BufferStateLayer submission may release a buffer
    // without presenting it if another transaction arrives before completion.
    // Let SurfaceFlinger queue each video buffer instead. Resolve optionally
    // so API 29/30 keep their existing compatible path.
    *(void **)(&o->ASurfaceTransaction_setEnableBackPressure) =
        dlsym(o->library, "ASurfaceTransaction_setEnableBackPressure");
    o->control = o->ASurfaceControl_createFromWindow(parent,
        format == 2 ? "TV MPV converted video (RGB10)" :
        format == 1 ? "TV MPV converted video (RGB8)" :
                      "TV MPV converted video (8-bit YUV)");
    if (!o->control) goto fail;
    if (o->ASurfaceTransaction_setEnableBackPressure) {
        ASurfaceTransaction *tx = o->ASurfaceTransaction_create();
        o->ASurfaceTransaction_setEnableBackPressure(tx, o->control, true);
        o->ASurfaceTransaction_apply(tx);
        o->ASurfaceTransaction_delete(tx);
    }
    p->client_buffer = (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p->create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    p->bind_image = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    p->create_sync = (void *)eglGetProcAddress("eglCreateSyncKHR");
    p->destroy_sync = (void *)eglGetProcAddress("eglDestroySyncKHR");
    p->dup_fence = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    if (!p->client_buffer || !p->create_image || !p->destroy_image || !p->bind_image) goto fail;
    GL *gl = p->gl;
    gl->GenFramebuffers(1, &p->fbo);
    gl->GenTextures(SLOTS, p->textures);
    bool hdr_pool = hdr && ctx->vo->opts->android_video_layer_hdr_pool;
    if (hdr_pool && (p->rgb || !create_hdr_pool(p, ctx))) goto fail;
    for (int i = 0; i < SLOTS; i++) {
        AHardwareBuffer_Desc desc = {.width=w, .height=h, .layers=1,
            .format=format == 2 ? AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM :
                    format == 1 ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM :
                                  0x11, // Android HAL NV21
            .usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                   AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                   AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                   // Amlogic am_gralloc_pick_heap(): CAMERA_OUTPUT requests
                   // contiguous memory, which Hwc2Layer can scan out as NV21
                   // DRM_FB_VIDEO_DMABUF. Never tag this as decoder-owned UVM.
                   (ctx->vo->opts->android_video_layer_contiguous == 2 ? 1ULL << 12 :
                    ctx->vo->opts->android_video_layer_contiguous == 1 ? 1ULL << 17 : 0)};
        int error = hdr_pool ? 0 : o->AHardwareBuffer_allocate(&desc, &p->buffers[i]);
        if (error) {
            MP_ERR(p, "Video layer allocation failed: slot=%d %dx%d format=%x usage=%llx error=%d.\n",
                i,w,h,desc.format,(unsigned long long)desc.usage,error);
            goto fail;
        }
        p->images[i] = p->create_image(p->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
            p->client_buffer(p->buffers[i]), (EGLint[]){EGL_NONE});
        if (p->images[i] == EGL_NO_IMAGE_KHR) {
            MP_ERR(p, "Video layer EGL import failed: %x.\n", eglGetError());
            goto fail;
        }
        gl->BindTexture(p->texture_target, p->textures[i]);
        gl->TexParameteri(p->texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(p->texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(p->texture_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(p->texture_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p->bind_image(p->texture_target, p->images[i]);
    }
    gl->BindTexture(p->texture_target, 0);
    if (!mp_android_video_layer_acquire(p)) goto fail;
    MP_INFO(p, "Android independent video layer: %dx%d, %s, %s; contiguous=%d; release fences enabled.\n",
        w, h, format == 2 ? "RGB10" : format == 1 ? "RGB8" : "8-bit YUV",
        hdr ? "BT.2020/PQ HDR compatibility request" : "BT.709 SDR",
        ctx->vo->opts->android_video_layer_contiguous);
    if (hdr_pool) MP_INFO(p, "Experimental HDR pool: four transferred generated IDRs, 8-bit NV21; verify physical HDMI pixels separately.\n");
    return p;
fail:
    MP_ERR(p, "Could not create the opt-in Android video layer.\n");
    mp_android_video_layer_destroy(&p);
    return NULL;
}

bool mp_android_video_layer_acquire(struct mp_android_video_layer *p)
{
    if (p->acquired >= 0) return true;
    int64_t start = mp_time_ns();
    struct layer_owner *o = p->owner;
    int slot = -1, fd = -1;
    mp_mutex_lock(&o->lock);
    for (int attempt = 0; attempt < 3 && slot < 0; attempt++) {
        for (int n = 0; n < SLOTS; n++) {
            int i = (p->next + n) % SLOTS;
            if (!o->available[i]) continue;
            o->available[i] = false; slot = i;
            fd = o->fence[i]; o->fence[i] = -1; break;
        }
        if (slot < 0) mp_cond_timedwait(&o->cond, &o->lock, MP_TIME_MS_TO_NS(100));
    }
    mp_mutex_unlock(&o->lock);
    if (slot < 0) {
        if (p->slow_acquires++ < 8) MP_WARN(p, "No released YUV presentation slot after 300ms.\n");
        return false;
    }
    if (fd >= 0) {
        struct pollfd fence = {.fd=fd, .events=POLLIN};
        if (poll(&fence, 1, 300) != 1) {
            mp_mutex_lock(&o->lock);
            o->fence[slot] = fd; o->available[slot] = true;
            mp_mutex_unlock(&o->lock);
            return false;
        }
        close(fd);
    }
    p->acquired = slot; p->next = (slot + 1) % SLOTS;
    GL *gl = p->gl;
    GLint previous;
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, p->fbo);
    gl->FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             p->texture_target, p->textures[slot], 0);
    GLenum status = gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    bool ok = status == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) MP_ERR(p, "Video layer framebuffer incomplete: %x (GL %x).\n", status,gl->GetError());
    // This swapchain repaints the full video and its letterbox background.
    // Imported YUV buffers otherwise make tile-based GPUs reload the previous
    // frame before drawing over it, including an avoidable YUV conversion.
    if (ok && gl->InvalidateFramebuffer)
        gl->InvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1,
                                  (GLenum[]){GL_COLOR_ATTACHMENT0});
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
    double elapsed_ms = (mp_time_ns() - start) / 1e6;
    if (elapsed_ms > 5 && p->slow_acquires++ < 16)
        MP_VERBOSE(p, "Video presentation slot acquisition took %.2fms.\n", elapsed_ms);
    return ok;
}

void mp_android_video_layer_present(struct mp_android_video_layer *p)
{
    if (p->acquired < 0) return;
    int64_t start = mp_time_ns();
    int fd = -1;
    if (p->finish) {
        p->gl->Finish();
        if (p->finish_samples++ < 48)
            MP_INFO(p, "YUV render completion wait: %.2fms.\n", (mp_time_ns()-start)/1e6);
    } else if (p->create_sync && p->destroy_sync && p->dup_fence) {
        EGLSyncKHR sync = p->create_sync(p->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                                         (EGLint[]){EGL_NONE});
        if (sync != EGL_NO_SYNC_KHR) {
            p->gl->Flush(); fd = p->dup_fence(p->display, sync);
            p->destroy_sync(p->display, sync);
        }
    }
    if (fd < 0 && !p->finish) p->gl->Finish();
    struct layer_owner *o = p->owner;
    struct completion *c = malloc(sizeof(*c));
    if (!c) {if (fd >= 0) close(fd); return;}
    *c = (struct completion){.owner=o, .previous=p->current};
    atomic_fetch_add(&o->refs, 1);
    ASurfaceTransaction *tx = o->ASurfaceTransaction_create();
    o->ASurfaceTransaction_setVisibility(tx, o->control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    o->ASurfaceTransaction_setZOrder(tx, o->control, 1);
    o->ASurfaceTransaction_setGeometry(tx, o->control,
        &(ARect){0,0,p->w,p->h}, &(ARect){0,0,p->w,p->h}, 0);
    o->ASurfaceTransaction_setBufferDataSpace(tx, o->control,
        p->hdr ? (p->rgb ? ADATASPACE_BT2020_PQ : ADATASPACE_BT2020_ITU_PQ) :
                 (p->rgb ? ADATASPACE_SRGB : ADATASPACE_BT709));
    if (p->hdr) {
        struct AHdrMetadata_smpte2086 metadata = {
            .displayPrimaryRed={.708,.292}, .displayPrimaryGreen={.170,.797},
            .displayPrimaryBlue={.131,.046}, .whitePoint={.3127,.3290},
            .maxLuminance=1000, .minLuminance=.0001};
        o->ASurfaceTransaction_setHdrMetadata_smpte2086(tx, o->control, &metadata);
    }
    o->ASurfaceTransaction_setBuffer(tx, o->control, p->buffers[p->acquired], fd);
    o->ASurfaceTransaction_setOnComplete(tx, c, completed);
    mp_mutex_lock(&o->lock); o->submitted++; mp_mutex_unlock(&o->lock);
    o->ASurfaceTransaction_apply(tx); o->ASurfaceTransaction_delete(tx);
    p->current = p->acquired; p->acquired = -1;
    double elapsed_ms = (mp_time_ns() - start) / 1e6;
    if (elapsed_ms > 50 && p->slow_presents++ < 8)
        MP_WARN(p, "YUV presentation submission took %.2fms.\n", elapsed_ms);
}

int mp_android_video_layer_fbo(struct mp_android_video_layer *p) {return p->fbo;}

void mp_android_video_layer_get_stats(struct mp_android_video_layer *p,
                                      struct voctrl_android_video_layer *s)
{
    struct layer_owner *o = p->owner;
    mp_mutex_lock(&o->lock);
    s->latch_stats = o->ASurfaceTransactionStats_getLatchTime != NULL;
    s->hdr_buffer_pool = o->hdr_pool != NULL;
    s->submitted = o->submitted;
    s->completed = o->completed;
    s->unique_latches = o->unique_latches;
    s->merged_latches = o->merged_latches;
    s->invalid_latches = o->invalid_latches;
    mp_mutex_unlock(&o->lock);
}

void mp_android_video_layer_destroy(struct mp_android_video_layer **ptr)
{
    struct mp_android_video_layer *p = *ptr;
    if (!p) return;
    struct layer_owner *o = p->owner;
    if (o) {
        mp_mutex_lock(&o->lock); o->closing = true; mp_mutex_unlock(&o->lock);
        if (o->control && o->ASurfaceTransaction_create) {
            ASurfaceTransaction *tx = o->ASurfaceTransaction_create();
            o->ASurfaceTransaction_setVisibility(tx, o->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
            o->ASurfaceTransaction_apply(tx); o->ASurfaceTransaction_delete(tx);
        }
    }
    if (p->gl) {
        p->gl->Finish();
        p->gl->DeleteFramebuffers(1, &p->fbo);
        p->gl->DeleteTextures(SLOTS, p->textures);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (p->images[i] && p->destroy_image) p->destroy_image(p->display, p->images[i]);
        if (p->buffers[i] && o && o->AHardwareBuffer_release) o->AHardwareBuffer_release(p->buffers[i]);
    }
    if (o) owner_unref(o);
    talloc_free(p); *ptr = NULL;
}
