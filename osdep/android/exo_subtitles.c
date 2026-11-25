// Compiled inside the matching libmpv build. The sd drivers and the Exo session
// share mpv's private ABI without exporting it or accessing libplayer's g_mpv.
#include <jni.h>
#include <strings.h>
#include <GLES2/gl2.h>
#include <libavformat/avformat.h>
#include <libavcodec/codec_desc.h>
#include "mpv/client.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "options/m_config_core.h"
#include "options/options.h"
#include "player/client.h"
#include "sub/sd.h"

#define JNI(name) JNIEXPORT JNICALL Java_is_xyz_mpv_ExoSubtitles_##name
extern const struct sd_functions sd_ass, sd_lavc;

struct session {
    mpv_handle *core;
    struct sd *sd;
    bool visible;
};

static char *string_copy(JNIEnv *env, void *parent, jstring value)
{
    const char *str = (*env)->GetStringUTFChars(env, value, NULL);
    if (!str) return NULL;
    char *result = talloc_strdup(parent, str);
    (*env)->ReleaseStringUTFChars(env, value, str);
    return result;
}

static void *bytes_copy(JNIEnv *env, void *parent, jbyteArray arr, int *size)
{
    *size = (*env)->GetArrayLength(env, arr);
    // Both FFmpeg's bit readers and libass can look past the last byte.
    char *bytes = talloc_zero_size(parent, (size_t)*size + AV_INPUT_BUFFER_PADDING_SIZE);
    (*env)->GetByteArrayRegion(env, arr, 0, *size, (jbyte *)bytes);
    return bytes;
}

static void destroy_session(struct session *s)
{
    if (s->sd) s->sd->driver->uninit(s->sd);
    // Decoder option caches unregister from the live core in their talloc destructors.
    mpv_handle *core = s->core;
    talloc_free(s);
    if (core) mpv_terminate_destroy(core);
}

jlong JNI(open)(JNIEnv *env, jclass cls, jstring config_dir, jstring font,
                jstring codec, jobjectArray init, jint width, jint height, jint codec_tag)
{
    struct session *s = talloc_zero(NULL, struct session);
    s->core = mpv_create();
    if (!s->core) goto fail;
    // The directory belongs to the subtitle session and contains its fonts.conf.
    mpv_set_option_string(s->core, "config", "yes");
    mpv_set_option_string(s->core, "config-dir", string_copy(env, s, config_dir));
    mpv_set_option_string(s->core, "load-scripts", "no");
    mpv_set_option_string(s->core, "load-auto-profiles", "no");
    mpv_set_option_string(s->core, "terminal", "no");
    mpv_set_option_string(s->core, "sub-font", string_copy(env, s, font));
    if (mpv_initialize(s->core) < 0) goto fail;
    struct mpv_global *global = mp_client_get_global(s->core);
    struct sd *sd = talloc_zero(s, struct sd);
    sd->global = global;
    sd->log = mp_log_new(sd, global->log, "exo-sub");
    sd->opts = mp_get_config_group(sd, global, &mp_subtitle_sub_opts);
    sd->shared_opts = mp_get_config_group(sd, global, &mp_subtitle_shared_sub_opts);
    sd->attachments = talloc_zero(sd, struct attachment_list);
    sd->codec = talloc_zero(sd, struct mp_codec_params);
    sd->codec->type = STREAM_SUB;
    sd->codec->codec = string_copy(env, sd, codec);
    sd->codec->disp_w = MPMAX(width, 0);
    sd->codec->disp_h = MPMAX(height, 0);
    sd->codec->codec_tag = codec_tag;
    sd->codec->native_tb_num = 1;
    sd->codec->native_tb_den = 1000000;
    int count = (*env)->GetArrayLength(env, init);
    if (count) {
        jbyteArray header = (*env)->GetObjectArrayElement(env, init, 0);
        sd->codec->extradata = bytes_copy(env, sd, header, &sd->codec->extradata_size);
        (*env)->DeleteLocalRef(env, header);
    }
    sd->attachments->num_entries = count > 0 ? (count - 1) / 2 : 0;
    sd->attachments->entries = talloc_zero_array(sd, struct demux_attachment, sd->attachments->num_entries);
    for (int i = 0; i < sd->attachments->num_entries; i++) {
        struct demux_attachment *f = &sd->attachments->entries[i];
        jbyteArray name = (*env)->GetObjectArrayElement(env, init, i * 2 + 1);
        jbyteArray data = (*env)->GetObjectArrayElement(env, init, i * 2 + 2);
        int size;
        f->name = bytes_copy(env, sd, name, &size);
        f->type = "application/octet-stream"; // mpv checks the filename extension.
        f->data = bytes_copy(env, sd, data, &size);
        f->data_size = size;
        (*env)->DeleteLocalRef(env, data);
        (*env)->DeleteLocalRef(env, name);
    }
    const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(sd->codec->codec);
    if (!desc || desc->type != AVMEDIA_TYPE_SUBTITLE) goto fail;
    sd->driver = (desc->props & AV_CODEC_PROP_BITMAP_SUB) ? &sd_lavc : &sd_ass;
    if (sd->driver->init(sd) < 0) goto fail;
    s->sd = sd;
    return (jlong)(intptr_t)s;
fail:
    destroy_session(s);
    return 0;
}

void JNI(close)(JNIEnv *env, jclass cls, jlong handle)
{
    destroy_session((struct session *)(intptr_t)handle);
}

jboolean JNI(settings)(JNIEnv *env, jclass cls, jlong handle, jint mode, jboolean stretch,
                   jfloat position, jfloat scale, jlong delay_us, jboolean scale_with_video)
{
    struct sd *sd = ((struct session *)(intptr_t)handle)->sd;
    bool rebuild = sd->driver == &sd_ass && mode != sd->shared_opts->ass_style_override[0];
    sd->shared_opts->ass_style_override[0] = mode;
    sd->shared_opts->sub_pos[0] = position;
    sd->shared_opts->sub_scale[0] = scale;
    sd->shared_opts->sub_delay[0] = delay_us / 1e6;
    sd->opts->stretch_image_subs = stretch;
    sd->opts->sub_scale_with_window = !scale_with_video;
    sd->opts->ass_use_margins = scale_with_video;
    // mpv marks sub-ass-override UPDATE_SUB_HARD. The shadow track used by
    // strip has different shaping features and needs a fresh libass renderer.
    uint64_t flags = rebuild ? UPDATE_SUB_HARD : 0;
    sd->driver->control(sd, SD_CTRL_UPDATE_OPTS, &flags);
    return rebuild;
}

// Recreate libass's font provider and tracks, including any Matroska attachments.
void JNI(font)(JNIEnv *env, jclass cls, jlong handle, jstring family)
{
    struct sd *sd = ((struct session *)(intptr_t)handle)->sd;
    if (sd->driver != &sd_ass) return;
    const char *name = (*env)->GetStringUTFChars(env, family, NULL);
    if (!name) return;
    talloc_replace(NULL, sd->opts->sub_style->font, name);
    (*env)->ReleaseStringUTFChars(env, family, name);
    uint64_t flags = UPDATE_SUB_HARD;
    sd->driver->control(sd, SD_CTRL_UPDATE_OPTS, &flags);
}

void JNI(video)(JNIEnv *env, jclass cls, jlong handle, jint w, jint h, jfloat par)
{
    struct sd *sd = ((struct session *)(intptr_t)handle)->sd;
    AVRational ratio = av_d2q(par > 0 ? par : 1, 65535);
    struct mp_image_params p = {.w = w, .h = h, .p_w = ratio.num, .p_h = ratio.den};
    sd->driver->control(sd, SD_CTRL_SET_VIDEO_PARAMS, &p);
}

void JNI(packet)(JNIEnv *env, jclass cls, jlong handle, jbyteArray bytes,
                 jint start, jlong pts_us, jlong duration_us)
{
    struct sd *sd = ((struct session *)(intptr_t)handle)->sd;
    int len = (*env)->GetArrayLength(env, bytes) - start;
    struct demux_packet *pkt = new_demux_packet(NULL, len);
    if (!pkt) return;
    (*env)->GetByteArrayRegion(env, bytes, start, len, (jbyte *)pkt->buffer);
    pkt->pts = pts_us / 1e6;
    pkt->duration = duration_us < 0 ? -1 : duration_us / 1e6;
    sd->driver->decode(sd, pkt);
    talloc_free(pkt);
}

jboolean JNI(draw)(JNIEnv *env, jclass cls, jlong handle, jlong pts_us,
                   jint w, jint h, jint ml, jint mt, jint mr, jint mb,
                   jint color_uniform, jint mask_uniform, jboolean force)
{
    struct session *s = (struct session *)(intptr_t)handle;
    struct sd *sd = s->sd;
    bool mask = sd->driver == &sd_ass;
    struct mp_osd_res res = {.w=w, .h=h, .ml=ml, .mt=mt, .mr=mr, .mb=mb, .display_par=1};
    struct sub_bitmaps *b = sd->driver->get_bitmaps(sd, res, mask ? SUBBITMAP_LIBASS : SUBBITMAP_BGRA,
                                                     pts_us / 1e6 - sd->shared_opts->sub_delay[0]);
    bool visible = b && b->num_parts && b->packed;
    bool changed = force || visible != s->visible || (visible && b->change_id);
    s->visible = visible;
    if (changed) glClear(GL_COLOR_BUFFER_BIT);
    if (changed && visible) {
        GLenum format = mask ? GL_ALPHA : GL_RGBA;
        int tex_w = b->packed->stride[0] / (mask ? 1 : 4);
        int tex_h = b->packed_h;
        glTexImage2D(GL_TEXTURE_2D, 0, format, tex_w, tex_h, 0, format, GL_UNSIGNED_BYTE, b->packed->planes[0]);
        glUniform1i(mask_uniform, mask);
        for (int i = 0; i < b->num_parts; i++) {
            struct sub_bitmap *p = &b->parts[i];
            float x0 = 2.f*p->x/w - 1, x1 = 2.f*(p->x+p->dw)/w - 1;
            float y0 = 1 - 2.f*p->y/h, y1 = 1 - 2.f*(p->y+p->dh)/h;
            float u0 = (float)p->src_x/tex_w, u1 = (float)(p->src_x+p->w)/tex_w;
            float v0 = (float)p->src_y/tex_h, v1 = (float)(p->src_y+p->h)/tex_h;
            float vertices[] = {x0,y0,u0,v0, x0,y1,u0,v1, x1,y0,u1,v0, x1,y1,u1,v1};
            if (mask) {
                uint32_t c = p->libass.color;
                glUniform4f(color_uniform, (c>>24)/255.f, ((c>>16)&255)/255.f,
                            ((c>>8)&255)/255.f, (255-(c&255))/255.f);
            }
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, vertices);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    talloc_free(b);
    return changed;
}

struct text_input { uint8_t *bytes; int size, pos; };

static int read_text(void *opaque, uint8_t *dst, int size)
{
    struct text_input *in = opaque;
    int n = MPMIN(size, in->size - in->pos);
    if (!n) return AVERROR_EOF;
    memcpy(dst, in->bytes + in->pos, n);
    in->pos += n;
    return n;
}

static int64_t seek_text(void *opaque, int64_t offset, int whence)
{
    struct text_input *in = opaque;
    if (whence == AVSEEK_SIZE) return in->size;
    if (whence == SEEK_CUR) offset += in->pos;
    else if (whence == SEEK_END) offset += in->size;
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset < 0 || offset > in->size) return AVERROR(EINVAL);
    return in->pos = offset;
}

static jbyteArray java_bytes(JNIEnv *env, const uint8_t *bytes, int size)
{
    jbyteArray result = (*env)->NewByteArray(env, size);
    if (result && size) (*env)->SetByteArrayRegion(env, result, 0, size, (const jbyte *)bytes);
    return result;
}

jboolean JNI(demuxText)(JNIEnv *env, jclass cls, jstring format, jbyteArray bytes, jobject output)
{
    void *tmp = talloc_new(NULL);
    struct text_input in = {0};
    in.bytes = bytes_copy(env, tmp, bytes, &in.size);
    AVFormatContext *ctx = avformat_alloc_context();
    uint8_t *buf = av_malloc(32768);
    AVIOContext *io = buf ? avio_alloc_context(buf, 32768, 0, &in, read_text, NULL, seek_text) : NULL;
    AVPacket *pkt = av_packet_alloc();
    bool ok = false;
    const AVInputFormat *fmt = av_find_input_format(string_copy(env, tmp, format));
    if (!io) av_free(buf);
    if (!fmt || !io || !ctx || !pkt) goto done;
    ctx->pb = io;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&ctx, NULL, fmt, NULL) < 0 || !ctx->nb_streams) goto done;
    AVStream *stream = ctx->streams[0];
    jclass sink = (*env)->GetObjectClass(env, output);
    jmethodID header = (*env)->GetMethodID(env, sink, "header", "([B)V");
    jmethodID sample = (*env)->GetMethodID(env, sink, "sample", "(JJ[B)V");
    if (!header || !sample) goto done;
    jbyteArray extra = java_bytes(env, stream->codecpar->extradata, stream->codecpar->extradata_size);
    if (!extra) goto done;
    (*env)->CallVoidMethod(env, output, header, extra);
    (*env)->DeleteLocalRef(env, extra);
    int r;
    while (!(*env)->ExceptionCheck(env) && (r = av_read_frame(ctx, pkt)) >= 0) {
        if (pkt->stream_index == 0 && pkt->pts != AV_NOPTS_VALUE) {
            jbyteArray data = java_bytes(env, pkt->data, pkt->size);
            if (!data) goto done;
            (*env)->CallVoidMethod(env, output, sample,
                (jlong)av_rescale_q(pkt->pts, stream->time_base, AV_TIME_BASE_Q),
                (jlong)(pkt->duration > 0 ? av_rescale_q(pkt->duration, stream->time_base, AV_TIME_BASE_Q) : -1),
                data);
            (*env)->DeleteLocalRef(env, data);
        }
        av_packet_unref(pkt);
    }
    ok = !(*env)->ExceptionCheck(env) && r == AVERROR_EOF;
done:
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    if (io) av_freep(&io->buffer);
    avio_context_free(&io);
    talloc_free(tmp);
    return ok;
}

// Only self-contained subtitle demuxers and the fixed sibling source.sub of a VobSub index.
jboolean JNI(demuxFile)(JNIEnv *env, jclass cls, jstring path, jobject sink)
{
    void *tmp = talloc_new(NULL);
    char *name = string_copy(env, tmp, path);
    AVFormatContext *ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    // The VobSub demuxer opens its paired MPEG-PS payload with a child demuxer.
    bool idx = name && strlen(name) >= 4 && !strcasecmp(name + strlen(name) - 4, ".idx");
    const AVInputFormat *input = idx ? av_find_input_format("vobsub") : NULL;
    av_dict_set(&opts, "format_whitelist", idx ? "vobsub,mpeg" :
        "aqtitle,ass,dvbsub,jacosub,lrc,matroska,webm,mcc,microdvd,mpl2,mpsub,pjs,rcwt,realtext,sami,scc,srt,stl,subviewer,subviewer1,sup,tedcaptions,vobsub,vplayer,webvtt", 0);
    bool ok = false, seen = false;
    if (!pkt || !name || (idx && !input) || avformat_open_input(&ctx, name, input, &opts) < 0) goto done;
    jclass klass = (*env)->GetObjectClass(env, sink);
    jmethodID font = (*env)->GetMethodID(env, klass, "font", "([B[B)V");
    int font_bytes = 0;
    jmethodID stream = (*env)->GetMethodID(env, klass, "stream", "(ILjava/lang/String;[B[BIII[B)V");
    jmethodID sample = (*env)->GetMethodID(env, klass, "sample", "(IJJ[B)V");
    if (!stream || !sample) goto done;
    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        AVStream *st = ctx->streams[i];
        AVCodecParameters *cp = st->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
            const AVDictionaryEntry *filename = av_dict_get(st->metadata, "filename", NULL, 0);
            const char *ext = filename ? strrchr(filename->value, '.') : NULL;
            if (ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".ttc") ||
                        !strcasecmp(ext, ".otf") || !strcasecmp(ext, ".otc")) &&
                cp->extradata_size > 0 && cp->extradata_size <= 32 * 1024 * 1024 - font_bytes) {
                if ((*env)->PushLocalFrame(env, 2) < 0) goto done;
                jbyteArray fn = java_bytes(env, (const uint8_t *)filename->value, strlen(filename->value));
                jbyteArray data = java_bytes(env, cp->extradata, cp->extradata_size);
                if (!(*env)->ExceptionCheck(env)) (*env)->CallVoidMethod(env, sink, font, fn, data);
                (*env)->PopLocalFrame(env, NULL);
                if ((*env)->ExceptionCheck(env)) goto done;
                font_bytes += cp->extradata_size;
            }
            continue;
        }
        if (cp->codec_type != AVMEDIA_TYPE_SUBTITLE) goto done;
        const AVCodecDescriptor *desc = avcodec_descriptor_get(cp->codec_id);
        if (!desc || !avcodec_find_decoder(cp->codec_id)) continue;
        if ((*env)->PushLocalFrame(env, 6) < 0) goto done;
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        jstring codec = (*env)->NewStringUTF(env, desc->name);
        jbyteArray language = java_bytes(env, lang ? (const uint8_t *)lang->value : NULL, lang ? strlen(lang->value) : 0);
        // FFmpeg metadata is UTF-8. Convert supplementary characters via the Java caller.
        jbyteArray label = java_bytes(env, title ? (const uint8_t *)title->value : NULL, title ? strlen(title->value) : 0);
        jbyteArray extra = java_bytes(env, cp->extradata, cp->extradata_size);
        if (!(*env)->ExceptionCheck(env))
            (*env)->CallVoidMethod(env, sink, stream, (jint)i, codec, language, label,
                (jint)cp->width, (jint)cp->height, (jint)cp->codec_tag, extra);
        (*env)->PopLocalFrame(env, NULL);
        if ((*env)->ExceptionCheck(env)) goto done;
    }
    int result;
    while ((result = av_read_frame(ctx, pkt)) >= 0) {
        AVStream *st = ctx->streams[pkt->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && pkt->pts != AV_NOPTS_VALUE &&
            avcodec_find_decoder(st->codecpar->codec_id) && pkt->size > 0) {
            jbyteArray data = java_bytes(env, pkt->data, pkt->size);
            if (!data) goto done;
            (*env)->CallVoidMethod(env, sink, sample, (jint)pkt->stream_index,
                (jlong)av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q),
                (jlong)(pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : -1), data);
            (*env)->DeleteLocalRef(env, data);
            if ((*env)->ExceptionCheck(env)) goto done;
            seen = true;
        }
        av_packet_unref(pkt);
    }
    ok = seen && result == AVERROR_EOF;
done:
    av_dict_free(&opts);
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    talloc_free(tmp);
    return ok;
}

void JNI(reset)(JNIEnv *env, jclass cls, jlong handle)
{
    struct sd *sd = ((struct session *)(intptr_t)handle)->sd;
    if (sd->driver == &sd_ass) sd->driver->control(sd, SD_CTRL_RESET_SOFT, NULL);
    else sd->driver->reset(sd);
}
