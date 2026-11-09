// VideoToolbox IOSurface -> libplacebo, without CPU pixel readback/upload.
// Uses the host GL context so frame import, subtitles and Dolby reshaping stay
// in the same ordered rendering pipeline. Decoder surfaces live until a fence.
#include "config.h"
#include "afuse_vt.h"
#include "ra.h"
#include "common/msg.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "ta/ta_talloc.h"
#include <libplacebo/opengl.h>
#include <libplacebo/renderer.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLIOSurface.h>
#include <OpenGLES/ES3/gl.h>
#else
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/CGLIOSurface.h>
#endif

struct surface {
    CVPixelBufferRef pixels;
    GLuint textures[2];
    GLsync fence;
    struct surface *next;
};

struct vt_state {
    struct mp_hwdec_ctx device;
    struct mp_hwdec_devices *devices;
    struct surface *retired;
    bool failed;
    bool announced;
};

static void free_surface(struct surface *s)
{
    if (s->fence) glDeleteSync(s->fence);
    glDeleteTextures(2, s->textures);
    CVPixelBufferRelease(s->pixels);
    talloc_free(s);
}

static void collect(struct vt_state *p, bool finish)
{
    if (finish && p->retired) glFinish(); // teardown only
    struct surface **entry = &p->retired;
    while (*entry) {
        struct surface *s = *entry;
        GLenum status = finish ? GL_ALREADY_SIGNALED :
            glClientWaitSync(s->fence, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            entry = &s->next;
            continue;
        }
        *entry = s->next;
        free_surface(s);
    }
}

void afuse_vt_init(struct ra_next *ra, struct mp_hwdec_devices *devices)
{
    if (ra->apple_vt || !pl_opengl_get(ra->gpu)) return;
#if TARGET_OS_IPHONE
    if (EAGLContext.currentContext.API != kEAGLRenderingAPIOpenGLES3) return;
#else
    if (!CGLGetCurrentContext()) return;
#endif
    struct vt_state *p = talloc_zero(NULL, struct vt_state);
    p->devices = devices;
    p->device = (struct mp_hwdec_ctx){
        .driver_name = "afuse-videotoolbox-iosurface",
        .hw_imgfmt = IMGFMT_VIDEOTOOLBOX,
    };
    if (av_hwdevice_ctx_create(&p->device.av_device_ref,
                               AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) < 0) {
        talloc_free(p);
        return;
    }
    ra->apple_vt = p;
    hwdec_devices_add(devices, &p->device);
}

bool afuse_vt_map(struct ra_next *ra, struct pl_frame *frame,
                  const struct mp_image *image)
{
    struct vt_state *p = ra->apple_vt;
    if (!p || p->failed || image->imgfmt != IMGFMT_VIDEOTOOLBOX) return false;
    collect(p, false);
    int outstanding = 0;
    for (struct surface *s = p->retired; s; s = s->next) outstanding++;
    // A stalled GPU must not retain an unbounded number of decoder surfaces.
    if (outstanding >= 12) goto error;
    CVPixelBufferRef pixels = (CVPixelBufferRef)image->planes[3];
    IOSurfaceRef io = pixels ? CVPixelBufferGetIOSurface(pixels) : NULL;
    if (!io || CVPixelBufferGetPlaneCount(pixels) != 2) goto error;
    OSType type = CVPixelBufferGetPixelFormatType(pixels);
    bool ten = type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
               type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    bool eight = type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                 type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    if (!ten && !eight) goto error;

    struct surface *s = talloc_zero(NULL, struct surface);
    s->pixels = CVPixelBufferRetain(pixels);
    GLint binding = 0;
#if TARGET_OS_IPHONE
    GLenum target = GL_TEXTURE_2D;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
#else
    GLenum target = GL_TEXTURE_RECTANGLE;
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &binding);
#endif
    glGenTextures(2, s->textures);
    struct pl_frame mapped = {.num_planes = 2, .user_data = s};
    for (int n = 0; n < 2; n++) {
        int w = CVPixelBufferGetWidthOfPlane(pixels, n);
        int h = CVPixelBufferGetHeightOfPlane(pixels, n);
        GLenum internal = n ? GL_RG8 : GL_R8;
        GLenum format = n ? GL_RG : GL_RED;
        GLenum data_type = GL_UNSIGNED_BYTE;
        if (ten) {
#if TARGET_OS_IPHONE
            // ES has no normalized 16-bit texture requirement. Integer textures
            // preserve every P010 bit; libplacebo performs normalization.
            internal = n ? GL_RG16UI : GL_R16UI;
            format = n ? GL_RG_INTEGER : GL_RED_INTEGER;
#else
            internal = n ? GL_RG16 : GL_R16;
#endif
            data_type = GL_UNSIGNED_SHORT;
        }
        glBindTexture(target, s->textures[n]);
#if TARGET_OS_IPHONE
        bool bound = [EAGLContext.currentContext texImageIOSurface:io target:target
            internalFormat:internal width:w height:h format:format type:data_type plane:n];
#else
        bool bound = CGLTexImageIOSurface2D(CGLGetCurrentContext(), target,
            internal, w, h, format, data_type, io, n) == kCGLNoError;
#endif
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (bound) mapped.planes[n].texture = pl_opengl_wrap(ra->gpu,
            pl_opengl_wrap_params(.texture = s->textures[n], .target = target,
                .iformat = internal, .width = w, .height = h));
        if (!mapped.planes[n].texture) {
            glBindTexture(target, binding);
            for (int j = 0; j < 2; j++) pl_tex_destroy(ra->gpu, &mapped.planes[j].texture);
            free_surface(s);
            goto error;
        }
        mapped.planes[n].components = n ? 2 : 1;
        mapped.planes[n].component_mapping[0] = n ? 1 : 0;
        mapped.planes[n].component_mapping[1] = n ? 2 : -1;
    }
    glBindTexture(target, binding);
    mapped.color = frame->color;
    mapped.repr = frame->repr;
    mapped.repr.bits = (struct pl_bit_encoding){
        .sample_depth = ten ? 16 : 8, .color_depth = ten ? 10 : 8,
        .bit_shift = ten ? 6 : 0,
    };
    mapped.crop = frame->crop;
    mapped.enhancement_layer = frame->enhancement_layer;
    *frame = mapped;
    if (!p->announced) {
        mp_msg(ra->log, MSGL_INFO, "AfuseHWPlus: VideoToolbox IOSurface direct YUV active (%s).\n",
               ten ? "P010 10-bit" : "NV12 8-bit");
        p->announced = true;
    }
    return true;
error:
    if (p) p->failed = true;
    mp_msg(ra->log, MSGL_WARN, "AfuseHWPlusFallback: IOSurface import unavailable; use videotoolbox-copy.\n");
    return false;
}

bool afuse_vt_release(struct ra_next *ra, struct pl_frame *frame)
{
    struct vt_state *p = ra->apple_vt;
    struct surface *s = frame->user_data;
    if (!p || !s) return false;
    for (int n = 0; n < frame->num_planes; n++) pl_tex_destroy(ra->gpu, &frame->planes[n].texture);
    frame->user_data = NULL;
    s->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!s->fence) {
        glFinish(); // failed synchronization; never return a surface too early
        free_surface(s);
        return true;
    }
    glFlush();
    s->next = p->retired;
    p->retired = s;
    collect(p, false);
    return true;
}

void afuse_vt_uninit(struct ra_next *ra)
{
    struct vt_state *p = ra->apple_vt;
    if (!p) return;
    collect(p, true);
    hwdec_devices_remove(p->devices, &p->device);
    av_buffer_unref(&p->device.av_device_ref);
    talloc_free(p);
    ra->apple_vt = NULL;
}
