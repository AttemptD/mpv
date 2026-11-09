/* Original compressed E-AC-3/JOC -> fragmented MP4 -> AVPlayer on tvOS.
 * Included once by ao_avfoundation.m; LGPL-2.1-or-later, like mpv.
 * The listener is loopback-only. No source URL, credentials or media file is
 * exposed, and the bounded in-memory segments disappear on reset/uninit.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <math.h>
#include <sys/socket.h>
#include <unistd.h>

@interface AFEAC3HLSBridge : NSObject {
    AVFormatContext *_mux;
    AVIOContext *_io;
    NSMutableData *_output;
    NSMutableDictionary *_segments;
    NSData *_initialization;
    NSLock *_lock;
    NSString *_token, *_channels;
    int _fragmentPacketLimit, _bandwidth;
    AVPlayer *_player;
    CMAudioFormatDescriptionRef _format;
    int _listener, _port, _nextSequence, _firstSequence, _fragmentPackets;
    int64_t _origin;
    double _fragmentStart, _mediaEnd;
    BOOL _hasOrigin, _finished, _paused, _muted, _serving;
    float _volume;
    atomic_bool _closing, _playbackEnded, _playbackFailed;
    int _muxError;
}
- (instancetype)initWithFormat:(CMAudioFormatDescriptionRef)format;
- (BOOL)appendPacket:(const uint8_t *)bytes length:(size_t)size pts:(int64_t)pts;
- (void)finish;
- (void)setPaused:(BOOL)paused;
- (void)setVolume:(float)volume;
- (float)volume;
- (void)setMuted:(BOOL)muted;
- (BOOL)muted;
- (int64_t)currentTimeNanoseconds;
- (BOOL)failed;
- (NSError *)error;
- (BOOL)ended;
- (void)stop;
@end

static int af_eac3_hls_write(void *opaque, const uint8_t *bytes, int size)
{
    [(NSMutableData *)opaque appendBytes:bytes length:size];
    return size;
}

static bool af_eac3_hls_send(int fd, const void *bytes, size_t length)
{
    while (length) {
        ssize_t sent = send(fd, bytes, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes = (const uint8_t *)bytes + sent;
        length -= sent;
    }
    return true;
}

@implementation AFEAC3HLSBridge
- (instancetype)initWithFormat:(CMAudioFormatDescriptionRef)format
{
    if (!(self = [super init])) return nil;
    _listener = -1;
    _volume = 1;
    _format = (CMAudioFormatDescriptionRef)CFRetain(format);
    _lock = [[NSLock alloc] init];
    _output = [[NSMutableData alloc] init];
    _segments = [[NSMutableDictionary alloc] init];
    _token = [NSUUID.UUID.UUIDString copy];
    atomic_init(&_closing, false);
    atomic_init(&_playbackEnded, false);
    atomic_init(&_playbackFailed, false);

    const AudioStreamBasicDescription *asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format);
    if (!asbd || asbd->mSampleRate < 1536 ||
        avformat_alloc_output_context2(&_mux, NULL, "mp4", NULL) < 0 || !_mux)
        goto error;
    // Whole six-block bursts keep each segment at or below one second at
    // every supported sample rate. AO permits six seconds of startup prefill.
    _fragmentPacketLimit = (int)asbd->mSampleRate / 1536;
    _bandwidth = 640000;
    _channels = [[NSString stringWithFormat:@"%u", (unsigned)asbd->mChannelsPerFrame] copy];
    size_t cookieSize = 0;
    const uint8_t *cookie = CMAudioFormatDescriptionGetMagicCookie(format, &cookieSize);
    if (cookie && cookieSize >= 13 && !memcmp(cookie + 4, "dec3", 4)) {
        _bandwidth = MAX(64000, (AV_RB16(cookie + 8) >> 3) * 1100 + 16000);
        size_t position = 10;
        unsigned independent = (cookie[9] & 7) + 1;
        for (unsigned i = 0; i < independent; i++) {
            if (position + 3 > cookieSize) { position = cookieSize; break; }
            position += ((cookie[position + 2] >> 1) & 15) ? 4 : 3;
        }
        // HLS uses ec-3 plus CHANNELS="complexity/JOC", not ec+3. Read
        // complexity_index_type_a from the original dec3; never invent Atmos.
        if (position + 2 <= cookieSize && (cookie[position] & 1) && cookie[position + 1]) {
            [_channels release];
            _channels = [[NSString stringWithFormat:@"%u/JOC", cookie[position + 1]] copy];
        }
    }
    AVStream *stream = avformat_new_stream(_mux, NULL);
    uint8_t *buffer = av_malloc(32768);
    if (!stream || !buffer) { av_free(buffer); goto error; }
    _io = avio_alloc_context(buffer, 32768, 1, _output, NULL, af_eac3_hls_write, NULL);
    if (!_io) { av_free(buffer); goto error; }
    _mux->pb = _io;
    _mux->flags |= AVFMT_FLAG_CUSTOM_IO;
    stream->time_base = (AVRational){1, (int)asbd->mSampleRate};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_EAC3;
    stream->codecpar->sample_rate = asbd->mSampleRate;
    av_channel_layout_default(&stream->codecpar->ch_layout, asbd->mChannelsPerFrame);
    AVDictionary *options = NULL;
    // Delay moov until the muxer has parsed the original JOC complexity data.
    av_dict_set(&options, "movflags", "delay_moov+empty_moov+frag_custom+default_base_moof", 0);
    int result = avformat_write_header(_mux, &options);
    av_dict_free(&options);
    if (result < 0) goto error;

    _listener = socket(AF_INET, SOCK_STREAM, 0);
    if (_listener < 0) goto error;
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    if (bind(_listener, (struct sockaddr *)&address, sizeof(address)) || listen(_listener, 8))
        goto error;
    socklen_t addressSize = sizeof(address);
    if (getsockname(_listener, (struct sockaddr *)&address, &addressSize)) goto error;
    _port = ntohs(address.sin_port);
    // NSThread retains its target until this method exits. The bridge does not
    // retain the thread, so closing the listener cannot form a retain cycle.
    _serving = YES;
    [NSThread detachNewThreadSelector:@selector(serve) toTarget:self withObject:nil];
    return self;
error:
    [self release];
    return nil;
}

- (void)publishFragment
{
    if (!_fragmentPackets || _muxError < 0) return;
    size_t fragment = 0;
    // delay_moov's first explicit flush may contain only ftyp/moov. Flush
    // twice in that case so a short EOF tail also produces its moof/mdat.
    for (int attempt = 0; attempt < 2; attempt++) {
        _muxError = av_write_frame(_mux, NULL);
        avio_flush(_io);
        if (_muxError < 0 || _io->error < 0) {
            if (_muxError >= 0) _muxError = _io->error;
            return;
        }
        const uint8_t *bytes = _output.bytes;
        size_t length = _output.length, offset = 0;
        fragment = length;
        while (offset + 8 <= length) {
            uint64_t size = AV_RB32(bytes + offset);
            if (size == 1) {
                if (length - offset < 16) { _muxError = AVERROR_INVALIDDATA; return; }
                size = AV_RB64(bytes + offset + 8);
            }
            if (size < 8 || size > length - offset) { _muxError = AVERROR_INVALIDDATA; return; }
            if (!memcmp(bytes + offset + 4, "moof", 4)) { fragment = offset; break; }
            offset += size;
        }
        if (fragment < length) break;
    }
    if (fragment == _output.length) { _muxError = AVERROR_INVALIDDATA; return; }
    const uint8_t *bytes = _output.bytes;
    [_lock lock];
    if (!_initialization && fragment)
        _initialization = [[NSData alloc] initWithBytes:bytes length:fragment];
    if (!_initialization) {
        [_lock unlock]; _muxError = AVERROR_INVALIDDATA; return;
    }
    NSData *data = [NSData dataWithBytes:bytes + fragment length:_output.length - fragment];
    _segments[@(_nextSequence)] = @{ @"data":data, @"duration":@(_mediaEnd - _fragmentStart) };
    _nextSequence++;
    while (_nextSequence - _firstSequence > 24) [_segments removeObjectForKey:@(_firstSequence++)];
    [_lock unlock];
    [_output setLength:0];
    _fragmentPackets = 0;
    _fragmentStart = _mediaEnd;
}

- (void)preparePlayerIfReady
{
    if (_player || !_initialization || (!_finished && _nextSequence < 4)) return;
    NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%d/%@/master.m3u8", _port, _token]];
    _player = [[AVPlayer alloc] initWithURL:url];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(didReachEnd:) name:AVPlayerItemDidPlayToEndTimeNotification object:_player.currentItem];
    [center addObserver:self selector:@selector(didFailToEnd:) name:AVPlayerItemFailedToPlayToEndTimeNotification object:_player.currentItem];
    _player.automaticallyWaitsToMinimizeStalling = YES;
    _player.currentItem.preferredForwardBufferDuration = 2;
    _player.volume = _volume;
    _player.muted = _muted;
    if (!_paused) [_player play];
}

- (BOOL)appendPacket:(const uint8_t *)bytes length:(size_t)size pts:(int64_t)pts
{
    if (_finished || atomic_load(&_closing) || !size || size > INT_MAX || _muxError < 0) return NO;
    if (!_hasOrigin) { _origin = pts; _hasOrigin = YES; }
    AVPacket *packet = av_packet_alloc();
    if (!packet) { _muxError = AVERROR(ENOMEM); return NO; }
    int rate = _mux->streams[0]->codecpar->sample_rate;
    int64_t timestamp = av_rescale_q(pts - _origin, (AVRational){1,1000000000}, (AVRational){1,rate});
    int totalDuration = 0, result = 0;
    size_t offset = 0;
    while (offset < size) {
        size_t group = offset;
        uint8_t bsid;
        uint16_t frameSize;
        if (size - offset < 7 || af_eac3_dependent(bytes + offset, (int)(size - offset)) ||
            av_ac3_parse_header(bytes + offset, (int)(size - offset), &bsid, &frameSize) < 0) {
            result = AVERROR_INVALIDDATA; break;
        }
        const int blocks[] = {1, 2, 3, 6};
        int duration = bsid <= 10 || (bytes[offset + 4] >> 6) == 3
            ? 1536 : blocks[(bytes[offset + 4] >> 4) & 3] * 256;
        do {
            if (size - offset < 7 ||
                av_ac3_parse_header(bytes + offset, (int)(size - offset), &bsid, &frameSize) < 0 ||
                frameSize < 7 || frameSize > size - offset) {
                result = AVERROR_INVALIDDATA; break;
            }
            offset += frameSize;
        } while (af_eac3_dependent(bytes + offset, (int)(size - offset)));
        if (result < 0) break;
        result = av_new_packet(packet, (int)(offset - group));
        if (result < 0) break;
        // An IEC burst can contain several small independent groups. Keep
        // each group's dependent frames/JOC byte-identical when remuxing.
        memcpy(packet->data, bytes + group, packet->size);
        packet->pts = packet->dts = timestamp + totalDuration;
        packet->duration = duration;
        packet->flags = AV_PKT_FLAG_KEY;
        result = av_write_frame(_mux, packet);
        av_packet_unref(packet);
        if (result < 0) break;
        totalDuration += duration;
    }
    av_packet_free(&packet);
    if (result < 0 || totalDuration != 1536) {
        _muxError = result < 0 ? result : AVERROR_INVALIDDATA;
        return NO;
    }
    _mediaEnd = (timestamp + totalDuration) / (double)rate;
    _fragmentPackets++;
    if (_fragmentPackets >= _fragmentPacketLimit) [self publishFragment];
    [self preparePlayerIfReady];
    return _muxError >= 0;
}

- (void)finish
{
    if (_finished || atomic_load(&_closing)) return;
    [self publishFragment];
    [_lock lock]; _finished = YES; [_lock unlock];
    [self preparePlayerIfReady];
}
- (void)setPaused:(BOOL)paused { _paused = paused; if (paused) [_player pause]; else [_player play]; }
- (void)setVolume:(float)volume { _volume = volume; _player.volume = volume; }
- (float)volume { return _volume; }
- (void)setMuted:(BOOL)muted { _muted = muted; _player.muted = muted; }
- (BOOL)muted { return _muted; }
- (int64_t)currentTimeNanoseconds
{
    double seconds = _player ? CMTimeGetSeconds(_player.currentTime) : 0;
    return _origin + (isfinite(seconds) && seconds > 0 ? llround(seconds * 1e9) : 0);
}
- (void)didReachEnd:(NSNotification *)note { atomic_store(&_playbackEnded, true); }
- (void)didFailToEnd:(NSNotification *)note { atomic_store(&_playbackFailed, true); }
- (NSError *)error { return _player.currentItem.error ?: _player.error; }
- (BOOL)failed { return atomic_load(&_playbackFailed) || _muxError < 0 || _player.status == AVPlayerStatusFailed || _player.currentItem.status == AVPlayerItemStatusFailed; }
- (BOOL)ended
{
    if (!_finished) return NO;
    if (atomic_load(&_playbackEnded)) return YES;
    if (!_hasOrigin) return YES;
    double seconds = _player ? CMTimeGetSeconds(_player.currentTime) : 0;
    return isfinite(seconds) && seconds >= _mediaEnd - 0.001;
}

- (NSData *)bodyForPath:(NSString *)path type:(NSString **)type
{
    NSString *prefix = [NSString stringWithFormat:@"/%@/", _token];
    if (![path hasPrefix:prefix]) return nil;
    NSString *name = [path substringFromIndex:prefix.length];
    [_lock lock];
    NSData *result = nil;
    if ([name isEqualToString:@"master.m3u8"]) {
        NSString *master = [NSString stringWithFormat:@"#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-START:TIME-OFFSET=0,PRECISE=YES\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"original\",NAME=\"Original\",DEFAULT=YES,AUTOSELECT=YES,CHANNELS=\"%@\",URI=\"stream.m3u8\"\n#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"ec-3\",AUDIO=\"original\"\nstream.m3u8\n", _channels, _bandwidth];
        result = [[master dataUsingEncoding:NSUTF8StringEncoding] retain];
        *type = @"application/vnd.apple.mpegurl";
    } else if ([name isEqualToString:@"stream.m3u8"]) {
        NSMutableString *playlist = [NSMutableString stringWithFormat:@"#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:%d\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-START:TIME-OFFSET=0,PRECISE=YES\n#EXT-X-MAP:URI=\"init.mp4\"\n", _firstSequence];
        for (int i = _firstSequence; i < _nextSequence; i++)
            [playlist appendFormat:@"#EXTINF:%.6f,\nseg%d.m4s\n", [_segments[@(i)][@"duration"] doubleValue], i];
        if (_finished) [playlist appendString:@"#EXT-X-ENDLIST\n"];
        result = [[playlist dataUsingEncoding:NSUTF8StringEncoding] retain];
        *type = @"application/vnd.apple.mpegurl";
    } else if ([name isEqualToString:@"init.mp4"]) {
        result = [_initialization retain]; *type = @"audio/mp4";
    } else if ([name hasPrefix:@"seg"] && [name hasSuffix:@".m4s"]) {
        NSScanner *scanner = [NSScanner scannerWithString:[name substringFromIndex:3]];
        int sequence;
        if ([scanner scanInt:&sequence] && [[scanner.string substringFromIndex:scanner.scanLocation] isEqualToString:@".m4s"])
            result = [_segments[@(sequence)][@"data"] retain];
        *type = @"audio/mp4";
    }
    [_lock unlock];
    return [result autorelease];
}

- (void)serve
{
    @autoreleasepool {
        [_lock lock];
        int listener = _listener;
        [_lock unlock];
        while (!atomic_load(&_closing)) {
            struct pollfd ready = {.fd = listener, .events = POLLIN};
            int polled = poll(&ready, 1, 250);
            if (atomic_load(&_closing)) break;
            if (!polled || (polled < 0 && errno == EINTR)) continue;
            if (polled < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                atomic_store(&_playbackFailed, true);
                break;
            }
            int client = accept(listener, NULL, NULL);
            if (client < 0) {
                if (errno == EINTR) continue;
                if (!atomic_load(&_closing)) atomic_store(&_playbackFailed, true);
                break;
            }
            @autoreleasepool {
                struct timeval timeout = {.tv_sec=1};
                int noSignal = 1;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
                char request[8193]; size_t used = 0;
                while (used < sizeof(request) - 1 && !atomic_load(&_closing)) {
                    ssize_t size = recv(client, request + used, sizeof(request) - 1 - used, 0);
                    if (size <= 0) break;
                    used += size; request[used] = 0;
                    if (strstr(request, "\r\n\r\n")) break;
                }
                request[used] = 0;
                NSString *head = [[[NSString alloc] initWithBytes:request length:used encoding:NSUTF8StringEncoding] autorelease];
                NSArray *first = [[head componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
                NSString *type = @"application/octet-stream";
                BOOL method = [first.firstObject isEqualToString:@"GET"] || [first.firstObject isEqualToString:@"HEAD"];
                NSData *body = method && first.count >= 2 ? [self bodyForPath:first[1] type:&type] : nil;
                NSUInteger start = 0, count = body.length;
                BOOL partial = NO;
                for (NSString *line in [head componentsSeparatedByString:@"\r\n"]) {
                    if (![[line lowercaseString] hasPrefix:@"range: bytes="]) continue;
                    NSArray *range = [[line substringFromIndex:13] componentsSeparatedByString:@"-"];
                    if (range.count != 2 || ![range[0] length]) continue;
                    unsigned long long from = [range[0] longLongValue];
                    unsigned long long to = [range[1] length] ? [range[1] longLongValue] : body.length - 1;
                    if (from < body.length && to >= from) {
                        start = (NSUInteger)from; count = MIN(to, body.length - 1) - start + 1; partial = YES;
                    }
                }
                NSMutableString *header = [NSMutableString stringWithFormat:@"HTTP/1.1 %s\r\nContent-Type: %@\r\nContent-Length: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n", !body ? "404 Not Found" : partial ? "206 Partial Content" : "200 OK", type, (unsigned long)count];
                if (partial) [header appendFormat:@"Content-Range: bytes %lu-%lu/%lu\r\n", (unsigned long)start, (unsigned long)(start+count-1), (unsigned long)body.length];
                [header appendString:@"\r\n"];
                NSData *headerBytes = [header dataUsingEncoding:NSUTF8StringEncoding];
                if (af_eac3_hls_send(client, headerBytes.bytes, headerBytes.length) && body && ![first.firstObject isEqualToString:@"HEAD"])
                    af_eac3_hls_send(client, (const uint8_t *)body.bytes + start, count);
                close(client);
            }
        }
        [_lock lock];
        if (_listener >= 0) { close(_listener); _listener = -1; }
        _serving = NO;
        [_lock unlock];
    }
}
- (void)stop
{
    if (atomic_exchange(&_closing, true)) return;
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_player pause];
    [_player replaceCurrentItemWithPlayerItem:nil];
    [_lock lock];
    // Let the server close its own descriptor. Closing here could allow a new
    // bridge to reuse it before this server has left accept/poll after a seek.
    if (_listener >= 0) {
        shutdown(_listener, SHUT_RDWR);
        if (!_serving) { close(_listener); _listener = -1; }
    }
    [_segments removeAllObjects];
    [_initialization release]; _initialization = nil;
    [_lock unlock];
    [_output setLength:0];
}
- (void)dealloc
{
    [self stop];
    [_player release];
    if (_mux) { _mux->pb = NULL; avformat_free_context(_mux); }
    if (_io) { av_freep(&_io->buffer); avio_context_free(&_io); }
    if (_format) CFRelease(_format);
    [_output release]; [_segments release]; [_initialization release]; [_lock release]; [_token release]; [_channels release];
    [super dealloc];
}
@end
