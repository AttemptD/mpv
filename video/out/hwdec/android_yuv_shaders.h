/* Experimental MediaCodec YUV bridge. Shared with the on-device GPU probe. */
#pragma once

static const char android_yuv_vertex_shader[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char android_yuv_fragment_shader[] =
    "#version 300 es\n"
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "uniform highp __samplerExternal2DY2YEXT source;\n"
    "uniform highp float source_max;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out float plane_y;\n"
    "layout(location = 1) out float plane_u;\n"
    "layout(location = 2) out float plane_v;\n"
    "void main() {\n"
    "  vec3 yuv = texture(source, uv).xyz;\n"
    "  if (source_max > 0.0)\n"
    "    yuv = floor(yuv * source_max + 0.5) / 65535.0;\n"
    "  plane_y = yuv.x;\n"
    "  plane_u = yuv.y;\n"
    "  plane_v = yuv.z;\n"
    "}\n";
