#pragma once

#include <stdbool.h>

struct ra_next;
struct pl_frame;
struct mp_image;
struct mp_hwdec_devices;

// All operations run with the host's GL context current, on the render thread.
void afuse_vt_init(struct ra_next *ra, struct mp_hwdec_devices *devices);
bool afuse_vt_map(struct ra_next *ra, struct pl_frame *frame,
                  const struct mp_image *image);
bool afuse_vt_release(struct ra_next *ra, struct pl_frame *frame);
void afuse_vt_uninit(struct ra_next *ra);
