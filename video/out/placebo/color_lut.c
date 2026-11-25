/*
 * Part of mpv, licensed under the GNU Lesser General Public License
 * version 2.1 or later. See the project license for details.
 */

#include <stdio.h>

#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/colorspace.h>
#include "color_lut.h"

bool mp_color_lut_render(pl_gpu gpu, pl_dispatch dispatch, pl_tex atlas,
                         pl_shader_obj *state, const struct mp_image_params *src,
                         const struct pl_color_space *target, int size,
                         bool vertical)
{
    struct pl_color_repr repr = src->repr;
    repr.bits.sample_depth = repr.bits.color_depth;
    repr.bits.bit_shift = 0;
    char body[320];
    if (vertical) {
        snprintf(body, sizeof(body),
            "ivec2 q = ivec2(gl_FragCoord.xy);\n"
            "color = vec4(vec3(q.x, q.y %% %d, q.y / %d) / %d.0, 1.0);\n",
            size, size, size - 1);
    } else {
        snprintf(body, sizeof(body),
            "ivec2 q = ivec2(gl_FragCoord.xy);\n"
            "color = vec4(vec3(q.x %% %d, q.x / %d, q.y) / %d.0, 1.0);\n",
            size, size, size - 1);
    }
    pl_dispatch_reset_frame(dispatch);
    pl_shader sh = pl_dispatch_begin(dispatch);
    pl_shader_reset(sh, pl_shader_params(.gpu = gpu, .dynamic_constants = true));
    bool ok = pl_shader_custom(sh, &(struct pl_custom_shader){
        .description = "Android color LUT generation",
        .body = body, .input = PL_SHADER_SIG_NONE, .output = PL_SHADER_SIG_COLOR,
    });
    pl_shader_decode_color_ex(sh, pl_color_decode_args(.repr = &repr));
    struct pl_color_map_params params = pl_color_map_default_params;
    params.tone_mapping_function = &pl_tone_map_bt2390;
    params.metadata = PL_HDR_METADATA_HDR10;
    params.gamut_mapping = &pl_gamut_map_clip;
    pl_shader_color_map_ex(sh, &params, pl_color_map_args(
        .src = src->color, .dst = *target, .state = state));
    return pl_dispatch_finish(dispatch, pl_dispatch_params(
        .shader = &sh, .target = atlas)) && ok;
}
