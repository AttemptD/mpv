#ifndef MPV_LIBMPV_APPLE_H
#define MPV_LIBMPV_APPLE_H
#include "mpv/render.h"
#include "sub/osd.h"
#include "video/mp_image.h"
void *mp_apple_native_format(struct mp_image *img);
int mp_apple_p7_prepare(mpv_render_apple_p7_frame *out, struct mp_image *img);
void mp_apple_native_geometry(mpv_render_apple_frame *out, struct mp_image *img,
                             struct mp_rect src, struct mp_rect dst,
                             struct osd_state *osd, struct mp_osd_res osd_res);
int mp_apple_native_render(mpv_render_apple_frame *out, struct mp_image *img,
                          struct mp_rect src, struct mp_rect dst,
                          struct osd_state *osd, struct mp_osd_res osd_res);
#endif
