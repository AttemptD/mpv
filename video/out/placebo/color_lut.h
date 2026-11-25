/*
 * Part of mpv, licensed under the GNU Lesser General Public License
 * version 2.1 or later. See the project license for details.
 */

#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/colorspace.h>
#include "video/mp_image.h"

// Render the complete source-code -> display-RGB transform on a small lattice.
bool mp_color_lut_render(pl_gpu gpu, pl_dispatch dispatch, pl_tex atlas,
                         pl_shader_obj *state, const struct mp_image_params *src,
                         const struct pl_color_space *target, int size,
                         bool vertical);
