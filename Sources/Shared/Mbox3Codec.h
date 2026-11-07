// Mbox3Codec.h — sample conversion and isochronous packet layout for the Mbox 3.
// Pure C, header-only, no OS dependencies, unit tested in Tests/test_codec.c (runs on macOS or Linux).
//
// Layout facts (see Mbox3Protocol.h): 4 channels, 24-bit little-endian, interleaved, 12 bytes per frame.
// OUT packets are packed back to back inside a transfer. Each OUT packet carries as many frames as the
// IN packet it answers (implicit feedback).

#ifndef MBOX3_CODEC_H
#define MBOX3_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "Mbox3Protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- Samples
// float [-1, 1) <-> signed 24-bit. Out-of-range input clamps; NaN becomes 0.
static inline int32_t mbox3_float_to_s24(float f) {
    if (!(f == f)) return 0;                       // NaN
    if (f >= 1.0f) return 0x7FFFFF;
    if (f <= -1.0f) return -0x800000;
    float s = f * 8388608.0f;
    int32_t v = (int32_t)(s < 0 ? s - 0.5f : s + 0.5f);
    if (v > 0x7FFFFF) v = 0x7FFFFF;
    if (v < -0x800000) v = -0x800000;
    return v;
}
static inline float mbox3_s24_to_float(int32_t v) { return (float)v / 8388608.0f; }

static inline void mbox3_put_s24le(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}
static inline int32_t mbox3_get_s24le(const uint8_t *p) {
    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x800000) v -= 0x1000000;             // sign-extend
    return v;
}

// Encode `frames` interleaved float frames (MBOX3_CHANNELS each) into S24_3LE bytes. Returns bytes written.
static inline size_t mbox3_encode_frames(uint8_t *dst, const float *src, uint32_t frames) {
    size_t n = (size_t)frames * MBOX3_CHANNELS;
    for (size_t i = 0; i < n; i++) mbox3_put_s24le(dst + i * 3, mbox3_float_to_s24(src[i]));
    return n * 3;
}
// Decode `frames` S24_3LE frames into interleaved floats.
static inline void mbox3_decode_frames(float *dst, const uint8_t *src, uint32_t frames) {
    size_t n = (size_t)frames * MBOX3_CHANNELS;
    for (size_t i = 0; i < n; i++) dst[i] = mbox3_s24_to_float(mbox3_get_s24le(src + i * 3));
}

// ---------------------------------------------------------------- Packet sizing
// Nominal frame-count generator for when no IN packet is available yet (stream start-up).
// Fractional accumulator: after N packets, total frames == floor(N * rate / packetsPerSecond)
// (exactly, with no drift). For USB high speed with one packet per microframe, packetsPerSecond = 8000.
typedef struct { uint32_t rate; uint32_t packetsPerSecond; uint64_t acc; } Mbox3PacketPacer;

static inline void mbox3_pacer_init(Mbox3PacketPacer *p, uint32_t rate, uint32_t packetsPerSecond) {
    p->rate = rate; p->packetsPerSecond = packetsPerSecond; p->acc = 0;
}
static inline uint32_t mbox3_pacer_next(Mbox3PacketPacer *p) {
    p->acc += p->rate;
    uint32_t frames = (uint32_t)(p->acc / p->packetsPerSecond);
    p->acc -= (uint64_t)frames * p->packetsPerSecond;
    return frames;
}

// ---------------------------------------------------------------- Transfers
// Build one OUT transfer: `npackets` packets, packet i carries frameCounts[i] frames, packed back to back.
// `src` holds sum(frameCounts) interleaved float frames. `outLens[i]` receives each packet's byte length
// (this is what goes into the isochronous frame list's request count).
// Returns total bytes, or 0 if a packet would exceed MBOX3_MAX_PACKET or `cap`.
static inline size_t mbox3_build_out_transfer(uint8_t *dst, size_t cap, const float *src,
                                              const uint32_t *frameCounts, uint32_t npackets,
                                              uint16_t *outLens) {
    size_t off = 0;
    for (uint32_t i = 0; i < npackets; i++) {
        size_t len = (size_t)frameCounts[i] * MBOX3_BYTES_PER_FRAME;
        if (len > MBOX3_MAX_PACKET || off + len > cap) return 0;
        mbox3_encode_frames(dst + off, src, frameCounts[i]);
        src += (size_t)frameCounts[i] * MBOX3_CHANNELS;
        if (outLens) outLens[i] = (uint16_t)len;
        off += len;
    }
    return off;
}

// Parse one completed IN transfer. IN packets land in fixed slots of `slotStride` bytes (the request
// size per frame given to the host controller); actLens[i] is the byte count the device actually sent.
// Writes decoded floats to `dst`, stores each packet's frame count in frameCounts[i] (feed these to the
// OUT side: implicit feedback). Returns total frames, or -1 if a length is not a whole number of frames.
static inline int64_t mbox3_parse_in_transfer(float *dst, const uint8_t *src, uint32_t slotStride,
                                              const uint16_t *actLens, uint32_t npackets,
                                              uint32_t *frameCounts) {
    int64_t total = 0;
    for (uint32_t i = 0; i < npackets; i++) {
        uint32_t len = actLens[i];
        if (len % MBOX3_BYTES_PER_FRAME != 0 || len > slotStride) return -1;
        uint32_t frames = len / MBOX3_BYTES_PER_FRAME;
        mbox3_decode_frames(dst, src + (size_t)i * slotStride, frames);
        dst += (size_t)frames * MBOX3_CHANNELS;
        if (frameCounts) frameCounts[i] = frames;
        total += frames;
    }
    return total;
}

#ifdef __cplusplus
}
#endif
#endif // MBOX3_CODEC_H
