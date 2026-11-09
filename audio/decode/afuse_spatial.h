// Apple spatial conversion: object/height decode -> rendered 7.1.4 -> APAC.
// The normal FFmpeg PCM decode remains available for every failure.
#pragma once
#include <stdbool.h>
struct mp_log;
struct mp_codec_params;
struct demux_packet;
struct mp_aframe;
struct afuse_spatial;
struct afuse_spatial *afuse_spatial_create(struct mp_log *, struct mp_codec_params *);
void afuse_spatial_destroy(struct afuse_spatial *);
void afuse_spatial_reset(struct afuse_spatial *);
void afuse_spatial_packet(struct afuse_spatial *, struct demux_packet *);
void afuse_spatial_pcm(struct afuse_spatial *, struct mp_aframe *); // takes ownership
struct mp_aframe *afuse_spatial_pop(struct afuse_spatial *); // transfers ownership
void afuse_spatial_finish(struct afuse_spatial *);
