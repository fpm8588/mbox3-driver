// Unit tests for the hardware-independent code in Sources/Shared.
// Build & run:  ./build.sh test     (or: cc -std=c11 -Wall -Wextra -I Sources/Shared Tests/test_shared.c -lm -o t && ./t)

#include "Mbox3Protocol.h"
#include "Mbox3Codec.h"
#include "Mbox3Defaults.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_addressing(void) {
    CHECK(MBOX3_WINDEX(MBOX3_UNIT_MIXER) == 0x4001);
    CHECK(MBOX3_WINDEX(MBOX3_UNIT_EXTENSION) == 0x2001);
    CHECK(MBOX3_WINDEX(MBOX3_UNIT_CLOCK_INTERNAL) == 0x8101);
    // Values that appear verbatim in Linux mbox3_setup_defaults():
    CHECK(mbox3_mixer_wvalue(MBOX3_MIX_IN_HW_IN1, MBOX3_MIX_OUT_1) == 0x0110);   // "Analogue input 1 left"
    CHECK(mbox3_mixer_wvalue(MBOX3_MIX_IN_HW_IN2, MBOX3_MIX_OUT_2) == 0x0115);   // "Analogue input 2 right"
    CHECK(mbox3_mixer_wvalue(MBOX3_MIX_IN_SW_RET2, MBOX3_MIX_OUT_2) == 0x0105);  // unity in Linux
    CHECK(mbox3_mixer_wvalue(MBOX3_MIX_IN_SW_RET4, MBOX3_MIX_OUT_4) == 0x010f);  // unity in Linux
    CHECK(mbox3_mixer_wvalue(MBOX3_MIX_IN_FX_RET, MBOX3_MIX_OUT_4) == 0x0123);   // "S/PDIF fx returns right"
    CHECK(mbox3_fxsend_wvalue(7) == 0x0107);
}

static void test_gain(void) {
    uint8_t b[2];
    mbox3_put_le16(b, MBOX3_GAIN_OFF);   CHECK(b[0] == 0x00 && b[1] == 0x80);   // Linux: {0x00, 0x80}
    mbox3_put_le16(b, MBOX3_GAIN_UNITY); CHECK(b[0] == 0x00 && b[1] == 0x00);
    CHECK(mbox3_db_to_gain(0.0) == 0);
    CHECK(mbox3_db_to_gain(6.0) == 0);              // clamps at 0 dB
    CHECK(mbox3_db_to_gain(-1.0) == -256);
    CHECK(mbox3_db_to_gain(-0.25) == -64);
    CHECK(mbox3_db_to_gain(-200.0) == MBOX3_GAIN_OFF);
    CHECK(mbox3_db_to_gain(NAN) == MBOX3_GAIN_OFF);
    CHECK(mbox3_db_to_gain(-127.99) == -32765);
    CHECK(fabs(mbox3_gain_to_db(-256) + 1.0) < 1e-9);
    uint8_t r[4]; mbox3_put_le32(r, 48000);
    CHECK(r[0] == 0x80 && r[1] == 0xbb && r[2] == 0 && r[3] == 0);           // Linux: {0x80, 0xbb, 0, 0}
    CHECK(mbox3_get_le32(r) == 48000);
}

static void test_samples(void) {
    uint8_t p[3];
    mbox3_put_s24le(p, -1); CHECK(p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF);
    CHECK(mbox3_get_s24le(p) == -1);
    mbox3_put_s24le(p, 0x123456); CHECK(p[0] == 0x56 && p[1] == 0x34 && p[2] == 0x12);
    CHECK(mbox3_get_s24le(p) == 0x123456);
    CHECK(mbox3_float_to_s24(1.0f) == 0x7FFFFF);
    CHECK(mbox3_float_to_s24(2.0f) == 0x7FFFFF);
    CHECK(mbox3_float_to_s24(-1.0f) == -0x800000);
    CHECK(mbox3_float_to_s24(NAN) == 0);
    CHECK(mbox3_float_to_s24(0.0f) == 0);
    // Round trip within one LSB across the range.
    int ok = 1;
    for (int i = -1000; i <= 1000; i++) {
        float f = (float)i / 1001.0f;
        float g = mbox3_s24_to_float(mbox3_float_to_s24(f));
        if (fabsf(f - g) > 1.0f / 8388608.0f) ok = 0;
    }
    CHECK(ok);
}

static void test_pacer(void) {
    struct { uint32_t rate; uint32_t lo, hi; } cases[] = {
        {44100, 5, 6}, {48000, 6, 6}, {88200, 11, 12}, {96000, 12, 12}};
    for (unsigned c = 0; c < 4; c++) {
        Mbox3PacketPacer p; mbox3_pacer_init(&p, cases[c].rate, 8000);
        uint64_t total = 0; int inRange = 1;
        for (int i = 0; i < 8000 * 10; i++) {           // 10 seconds of microframes
            uint32_t f = mbox3_pacer_next(&p);
            if (f < cases[c].lo || f > cases[c].hi || f * MBOX3_BYTES_PER_FRAME > MBOX3_MAX_PACKET) inRange = 0;
            total += f;
        }
        CHECK(inRange);
        CHECK(total == (uint64_t)cases[c].rate * 10);   // no drift
    }
}

static void test_transfers(void) {
    // 8 packets of mixed sizes, as a 44.1 kHz device would send.
    uint32_t counts[8] = {5, 6, 5, 6, 5, 6, 5, 6};
    uint32_t total = 0; for (int i = 0; i < 8; i++) total += counts[i];
    float src[64 * MBOX3_CHANNELS], back[64 * MBOX3_CHANNELS];
    for (uint32_t i = 0; i < total * MBOX3_CHANNELS; i++) src[i] = (float)((int)i - 100) / 512.0f;

    uint8_t out[8 * MBOX3_MAX_PACKET]; uint16_t lens[8];
    size_t bytes = mbox3_build_out_transfer(out, sizeof out, src, counts, 8, lens);
    CHECK(bytes == total * MBOX3_BYTES_PER_FRAME);       // packed back to back, no slot padding
    CHECK(lens[0] == 60 && lens[1] == 72);
    // The second packet must start right after the first one's 60 bytes.
    CHECK(mbox3_get_s24le(out + 60) == mbox3_float_to_s24(src[5 * MBOX3_CHANNELS]));

    // Simulate the IN side: same samples in fixed 156-byte slots.
    uint8_t in[8 * MBOX3_MAX_PACKET]; memset(in, 0xEE, sizeof in);
    size_t off = 0;
    for (int i = 0; i < 8; i++) { memcpy(in + i * MBOX3_MAX_PACKET, out + off, lens[i]); off += lens[i]; }
    uint32_t got[8];
    int64_t frames = mbox3_parse_in_transfer(back, in, MBOX3_MAX_PACKET, lens, 8, got);
    CHECK(frames == (int64_t)total);
    CHECK(memcmp(got, counts, sizeof counts) == 0);
    int same = 1;
    for (uint32_t i = 0; i < total * MBOX3_CHANNELS; i++)
        if (fabsf(back[i] - src[i]) > 1.0f / 8388608.0f) same = 0;
    CHECK(same);

    // Rejections.
    uint32_t tooBig[1] = {14};                                      // 168 bytes > 156
    CHECK(mbox3_build_out_transfer(out, sizeof out, src, tooBig, 1, NULL) == 0);
    uint16_t badLen[1] = {13};                                      // not a whole frame
    CHECK(mbox3_parse_in_transfer(back, in, MBOX3_MAX_PACKET, badLen, 1, NULL) == -1);
}

static void test_defaults(void) {
    Mbox3Write w[MBOX3_DEFAULT_WRITE_COUNT];
    CHECK(mbox3_default_writes(w, 3) == -1);
    int n = mbox3_default_writes(w, MBOX3_DEFAULT_WRITE_COUNT);
    CHECK(n == MBOX3_DEFAULT_WRITE_COUNT);
    // The clock goes to internal before any audio-related setting.
    CHECK(w[1].wIndex == 0x8001 && w[1].wValue == 0x0100 && w[1].data[0] == 1);
    // Exactly four unity crosspoints, matching Linux: 0x0100, 0x0105, 0x010a, 0x010f.
    int unity = 0, mixer = 0;
    for (int i = 0; i < n; i++) {
        if (w[i].wIndex != 0x4001) continue;
        mixer++;
        if (w[i].data[0] == 0 && w[i].data[1] == 0) {
            unity++;
            CHECK(w[i].wValue == 0x0100 || w[i].wValue == 0x0105 || w[i].wValue == 0x010a || w[i].wValue == 0x010f);
        }
    }
    CHECK(mixer == 36);
    CHECK(unity == 4);
    for (int i = 0; i < n; i++) CHECK(w[i].len >= 1 && w[i].len <= 2);
}

int main(void) {
    test_addressing();
    test_gain();
    test_samples();
    test_pacer();
    test_transfers();
    test_defaults();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
