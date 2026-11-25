#include <EGL/egl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "android_gl_trace.h"
#include "common/common.h"
#include "common/msg.h"
#include "osdep/compiler.h"
#include "utils.h"

#define TRACED_GL(F) \
    F(TexImage2D, (GLenum target, GLint level, GLint format, GLsizei w, GLsizei h, \
                   GLint border, GLenum data_format, GLenum type, const void *pixels), \
      (target, level, format, w, h, border, data_format, type, pixels), \
      "target=0x%x level=%d size=%dx%d internal=0x%x format=0x%x type=0x%x", \
      target, level, w, h, (unsigned)format, data_format, type) \
    F(TexImage3D, (GLenum target, GLint level, GLint format, GLsizei w, GLsizei h, GLsizei d, \
                   GLint border, GLenum data_format, GLenum type, const void *pixels), \
      (target, level, format, w, h, d, border, data_format, type, pixels), \
      "target=0x%x level=%d size=%dx%dx%d internal=0x%x format=0x%x type=0x%x", \
      target, level, w, h, d, (unsigned)format, data_format, type) \
    F(TexStorage2D, (GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h), \
      (target, levels, format, w, h), "target=0x%x levels=%d size=%dx%d internal=0x%x", \
      target, levels, w, h, format) \
    F(TexStorage3D, (GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h, GLsizei d), \
      (target, levels, format, w, h, d), "target=0x%x levels=%d size=%dx%dx%d internal=0x%x", \
      target, levels, w, h, d, format) \
    F(TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, \
                      GLenum format, GLenum type, const void *pixels), \
      (target, level, x, y, w, h, format, type, pixels), \
      "target=0x%x level=%d offset=%d,%d size=%dx%d format=0x%x type=0x%x", \
      target, level, x, y, w, h, format, type) \
    F(FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum tex_target, \
                             GLuint texture, GLint level), \
      (target, attachment, tex_target, texture, level), \
      "target=0x%x attachment=0x%x texture_target=0x%x texture=%u level=%d", \
      target, attachment, tex_target, texture, level) \
    F(BlitFramebuffer, (GLint x0, GLint y0, GLint x1, GLint y1, GLint u0, GLint v0, \
                        GLint u1, GLint v1, GLbitfield mask, GLenum filter), \
      (x0, y0, x1, y1, u0, v0, u1, v1, mask, filter), \
      "src=%d,%d:%d,%d dst=%d,%d:%d,%d mask=0x%x filter=0x%x", \
      x0, y0, x1, y1, u0, v0, u1, v1, mask, filter) \
    F(BufferData, (GLenum target, intptr_t size, const void *data, GLenum usage), \
      (target, size, data, usage), "target=0x%x bytes=%jd usage=0x%x", \
      target, (intmax_t)size, usage) \
    F(BufferStorage, (GLenum target, intptr_t size, const void *data, GLbitfield flags), \
      (target, size, data, flags), "target=0x%x bytes=%jd flags=0x%x", \
      target, (intmax_t)size, flags) \
    F(BufferStorageEXT, (GLenum target, intptr_t size, const void *data, GLbitfield flags), \
      (target, size, data, flags), "target=0x%x bytes=%jd flags=0x%x", \
      target, (intmax_t)size, flags) \
    F(BufferSubData, (GLenum target, intptr_t offset, intptr_t size, const void *data), \
      (target, offset, size, data), "target=0x%x offset=%jd bytes=%jd", \
      target, (intmax_t)offset, (intmax_t)size) \
    F(Clear, (GLbitfield mask), (mask), "mask=0x%x", mask) \
    F(DrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count), \
      "mode=0x%x first=%d count=%d", mode, first, count) \
    F(DrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices), \
      (mode, count, type, indices), "mode=0x%x count=%d index_type=0x%x", mode, count, type) \
    F(DispatchCompute, (GLuint x, GLuint y, GLuint z), (x, y, z), "groups=%ux%ux%u", x, y, z) \
    F(EGLImageTargetTexture2DOES, (GLenum target, void *image), (target, image), "target=0x%x", target) \
    F(Flush, (void), (), "%s", "") \
    F(Finish, (void), (), "%s", "")

struct android_gl_trace {
    struct mp_log *log;
    GLenum (GLAPIENTRY *get_error)(void);
    // Preserve GL errors for mpv and libplacebo's own glGetError calls.
    GLenum pending[16];
    int num_pending;
#define DECLARE_PROC(name, params, args, fmt, ...) void (GLAPIENTRY *name) params;
    TRACED_GL(DECLARE_PROC)
#undef DECLARE_PROC
};

// Android EGL makes the renderer's context current on its VO thread.
static thread_local struct android_gl_trace *current;

static void save_error(struct android_gl_trace *trace, GLenum error)
{
    for (int i = 0; i < trace->num_pending; i++) {
        if (trace->pending[i] == error)
            return;
    }
    if (trace->num_pending < MP_ARRAY_SIZE(trace->pending))
        trace->pending[trace->num_pending++] = error;
}

static void pending_before(struct android_gl_trace *trace, const char *operation)
{
    for (int i = 0; i < MP_ARRAY_SIZE(trace->pending); i++) {
        GLenum error = trace->get_error();
        if (error == GL_NO_ERROR)
            break;
        save_error(trace, error);
        mp_err(trace->log, "OpenGL trace: pending %s (0x%04x) before %s.\n",
               gl_error_to_string(error), error, operation);
    }
}

static GLenum GLAPIENTRY trace_GetError(void)
{
    struct android_gl_trace *trace = current;
    if (!trace) {
        GLenum (GLAPIENTRY *get_error)(void) = (void *)eglGetProcAddress("glGetError");
        return get_error ? get_error() : GL_NO_ERROR;
    }
    if (!trace->num_pending)
        return trace->get_error();

    GLenum error = trace->pending[0];
    trace->num_pending--;
    memmove(trace->pending, trace->pending + 1,
            trace->num_pending * sizeof(trace->pending[0]));
    return error;
}

#define TRACE_PROC(name, params, args, fmt, ...) \
    static void GLAPIENTRY trace_##name params \
    { \
        struct android_gl_trace *trace = current; \
        if (!trace) { \
            void (GLAPIENTRY *proc) params = (void *)eglGetProcAddress("gl" #name); \
            if (proc) \
                proc args; \
            return; \
        } \
        pending_before(trace, "gl" #name); \
        trace->name args; \
        for (int i = 0; i < MP_ARRAY_SIZE(trace->pending); i++) { \
            GLenum error = trace->get_error(); \
            if (error == GL_NO_ERROR) \
                break; \
            save_error(trace, error); \
            mp_err(trace->log, "OpenGL trace: gl" #name " reported %s (0x%04x); " fmt "\n", \
                   gl_error_to_string(error), error, __VA_ARGS__); \
        } \
    }
TRACED_GL(TRACE_PROC)
#undef TRACE_PROC

struct android_gl_trace *android_gl_trace_create(void *parent, struct mp_log *log)
{
    GLenum (GLAPIENTRY *get_error)(void) = (void *)eglGetProcAddress("glGetError");
    if (!get_error || current) {
        mp_warn(log, "OpenGL command tracing unavailable in this context.\n");
        return NULL;
    }

    current = talloc_zero(parent, struct android_gl_trace);
    current->log = log;
    current->get_error = get_error;
    mp_info(log, "OpenGL command tracing enabled.\n");
    return current;
}

void android_gl_trace_destroy(struct android_gl_trace *trace)
{
    if (current == trace)
        current = NULL;
    talloc_free(trace);
}

void *android_gl_trace_get_proc_address(void *ctx, const char *name)
{
    struct android_gl_trace *trace = ctx ? ctx : current;
    void *proc = eglGetProcAddress(name);
    if (!trace || !proc)
        return proc;
    if (strcmp(name, "glGetError") == 0)
        return (void *)trace_GetError;

#define WRAP_PROC(cmd, params, args, fmt, ...) \
    if (strcmp(name, "gl" #cmd) == 0) { \
        trace->cmd = proc; \
        return (void *)trace_##cmd; \
    }
    TRACED_GL(WRAP_PROC)
#undef WRAP_PROC

    return proc;
}
