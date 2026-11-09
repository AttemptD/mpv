#ifndef MPV_STREAM_DVDNAV_H
#define MPV_STREAM_DVDNAV_H

#include <stdint.h>

struct stream;

struct mp_dvdnav_stream_cb {
    int (*seek)(void *opaque, uint64_t position);
    int (*read)(void *opaque, void *buffer, int size);
    void (*close)(struct stream *stream, void *opaque);
};

// Open a DVD image through an already-open mpv stream adapter. Ownership of
// opaque is transferred regardless of success and released through close().
int mp_dvdnav_open_external(struct stream *stream, void *opaque,
                            const struct mp_dvdnav_stream_cb *callbacks,
                            int title);

#endif
