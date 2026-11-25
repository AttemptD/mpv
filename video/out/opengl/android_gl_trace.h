#ifndef MP_ANDROID_GL_TRACE_H
#define MP_ANDROID_GL_TRACE_H

struct android_gl_trace;
struct mp_log;

// The Android EGL renderer owns the trace for the lifetime of its context.
struct android_gl_trace *android_gl_trace_create(void *parent, struct mp_log *log);
void android_gl_trace_destroy(struct android_gl_trace *trace);
void *android_gl_trace_get_proc_address(void *ctx, const char *name);

#endif
