#pragma once
#include <stdbool.h>
#include <stdint.h>

struct ra_ctx;
struct ANativeWindow;
struct mp_android_video_layer;
struct mp_android_captions;
struct mp_osd_res;
struct vo;
struct voctrl_android_video_layer;

struct mp_android_video_layer *mp_android_video_layer_create(
    struct ra_ctx *ctx, struct ANativeWindow *parent, int w, int h, bool hdr);
bool mp_android_video_layer_acquire(struct mp_android_video_layer *p);
void mp_android_video_layer_present(struct mp_android_video_layer *p);
int mp_android_video_layer_fbo(struct mp_android_video_layer *p);
void mp_android_video_layer_get_stats(struct mp_android_video_layer *p,
                                      struct voctrl_android_video_layer *stats);
void mp_android_video_layer_destroy(struct mp_android_video_layer **p);

struct mp_android_captions *mp_android_captions_create(struct vo *vo, bool native_presentation);
bool mp_android_captions_draw(struct mp_android_captions *p,
                            const struct mp_osd_res *res, double pts);
void mp_android_captions_destroy(struct mp_android_captions **p);
int64_t mp_android_captions_updates(const struct mp_android_captions *p);
