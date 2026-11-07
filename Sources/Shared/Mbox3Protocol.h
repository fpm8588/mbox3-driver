// Mbox3Protocol.h — everything this project knows about how the Avid Mbox (3rd gen) talks over USB.
//
// Pure C, no OS headers, so it builds on macOS (driver, CLI, app) and on Linux (unit tests).
//
// Every value here is a hardware fact cross-checked against two independent public sources:
//   [L] Linux kernel, sound/usb/quirks.c + quirks-table.h (mbox3_setup_defaults, mbox3_set_format_quirk,
//       snd_usb_mbox3_boot_quirk), by Conner Knox and Manuel Barrio Linares (mainline since 6.1).
//   [G] GhostBox Revenant docs/PROTOCOL.md (github.com/isaacmateosv/ghostbox-revenant), verified on
//       firmware 1.08 hardware on macOS 27.
// No code was copied from either project; only the protocol facts (which are not copyrightable) are used.
// Items marked UNVERIFIED have only one source — confirm them on your own unit with mbox3ctl first.

#ifndef MBOX3_PROTOCOL_H
#define MBOX3_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- Device identity
#define MBOX3_VENDOR_ID            0x0DBA   // Digidesign / Avid            [L][G] + your System Information
#define MBOX3_PRODUCT_ID           0x5000   // Mbox (3rd generation)        [L][G] + your System Information
#define MBOX3_CONFIG_DESC_SIZE     464      // wTotalLength the Linux driver expects  [L]

// ---------------------------------------------------------------- Interfaces and endpoints
// The device carries USB Audio Class 2 descriptors behind vendor class codes (0xFF), which is
// exactly why macOS's built-in AppleUSBAudio driver never claims it.                     [G]
#define MBOX3_IF_CONTROL           1        // audio control: all units below       [L][G]
#define MBOX3_IF_PLAYBACK          2        // alt 1, EP 0x01 isoch OUT, 4ch x 24-bit [L][G]
#define MBOX3_IF_CAPTURE           3        // alt 1, EP 0x81 isoch IN,  4ch x 24-bit [L][G]
#define MBOX3_IF_MIDI              4        // class-compliant USB MIDI (macOS handles it) [L][G]
#define MBOX3_STREAM_ALT           1
#define MBOX3_EP_OUT               0x01
#define MBOX3_EP_IN                0x81
#define MBOX3_CHANNELS             4
#define MBOX3_BYTES_PER_SAMPLE     3        // S24_3LE                               [L][G]
#define MBOX3_BYTES_PER_FRAME      (MBOX3_CHANNELS * MBOX3_BYTES_PER_SAMPLE)   // 12
#define MBOX3_MAX_PACKET           156      // 13 frames x 12 bytes (enough for 96 kHz)  [L: maxpacksize 0x9c][G]

// Streaming is asynchronous with IMPLICIT FEEDBACK: each OUT packet must carry exactly as many
// frames as the matching IN packet. There is no explicit feedback endpoint.       [L: implicit_fb][G]
// OUT packets inside one transfer are packed back to back (NOT in fixed 156-byte slots).   [G]

// Capture channel order (interface 3): In 1, In 2, S/PDIF L, S/PDIF R                        [G]
// Playback channel order (interface 2): Software Returns 1-4 (routed by the mixer below)      [L][G]

static const uint32_t kMbox3SampleRates[] = { 44100, 48000, 88200, 96000 };   // [L][G]
#define MBOX3_NUM_SAMPLE_RATES 4

// ---------------------------------------------------------------- Control requests
// All controls are UAC2-style class requests to interface 1.  wIndex = (unit << 8) | interface.
#define MBOX3_RT_SET               0x21     // host->device, class, interface
#define MBOX3_RT_GET               0xA1     // device->host, class, interface
#define MBOX3_REQ_CUR              0x01
#define MBOX3_REQ_RANGE            0x02
#define MBOX3_REQ_MEM              0x03     // used by the extension unit

#define MBOX3_WINDEX(unit)         ((uint16_t)(((unit) << 8) | MBOX3_IF_CONTROL))

#define MBOX3_UNIT_EXTENSION       0x20     // front panel functions + meters (MEM requests)   [L][G]
#define MBOX3_UNIT_MIXER           0x40     // monitor mixer, two stereo mixes                  [L][G]
#define MBOX3_UNIT_FX_SEND         0x42     // FX send levels                                   [L][G]
#define MBOX3_UNIT_EFFECT          0x43     // reverb/delay                                     [L][G]
#define MBOX3_UNIT_CLOCK_SELECTOR  0x80     // 1 = internal, 2 = S/PDIF                         [L][G]
#define MBOX3_UNIT_CLOCK_INTERNAL  0x81     // internal rate, 4 bytes LE Hz                     [L][G]
#define MBOX3_UNIT_CLOCK_SPDIF     0x82     // S/PDIF rate + validity                           [G] UNVERIFIED on your unit

// Clock (wValue for CUR requests)
#define MBOX3_CS_SAM_FREQ          0x0100   // rate (on 0x81 / 0x82) or source (on 0x80)        [L][G]
#define MBOX3_CS_CLOCK_VALID       0x0200   // S/PDIF clock valid (on 0x82)                     [G] UNVERIFIED
#define MBOX3_CLOCK_SRC_INTERNAL   1
#define MBOX3_CLOCK_SRC_SPDIF      2
// IMPORTANT: the Mbox powers up on the S/PDIF clock and will not stream without an S/PDIF
// signal. Selecting the internal clock is the first thing any driver must do.           [L][G]

// Extension unit registers (wIndex 0x2001, bRequest MEM 0x03, wValue = register)
#define MBOX3_EXT_LOW_LATENCY      0x00     // keep 0 (Pro Tools mode; inputs mute)  [G]
#define MBOX3_EXT_MONO             0x01     // 0/1                                    [L][G]
#define MBOX3_EXT_DIM              0x02     // 0/1                                    [L][G]
#define MBOX3_EXT_TUNER            0x03     // 0/1                                    [L][G] (Linux sends it as CUR)
#define MBOX3_EXT_LED_DIM          0x04     // 0/1 button LEDs dim instead of off     [L][G]
#define MBOX3_EXT_HOLD_TIME        0x05     // 0x03 250ms, 0x05 500ms, 0x08 750ms, 0x0a 1s  [L][G]
#define MBOX3_EXT_METERS           0x10     // read: 15 x uint16 LE, full scale 32767 [G] UNVERIFIED
#define MBOX3_NUM_METERS           15
static const char *const kMbox3MeterNames[MBOX3_NUM_METERS] = {
    "In 1", "In 2", "S/PDIF In L", "S/PDIF In R",
    "SW Ret 1", "SW Ret 2", "SW Ret 3", "SW Ret 4",
    "Out 1", "Out 2", "Out 3", "Out 4",
    "FX In", "FX Out L", "FX Out R",
};

// ---------------------------------------------------------------- Mixer (unit 0x40)
// Crosspoint gain: wValue = 0x0100 | (input * 4 + output), data = int16 LE in 1/256 dB.
// 0x0000 = 0 dB (the maximum used), 0x8000 = off. Gains are WRITE-ONLY (reads return 0),
// so software must remember what it wrote. Power-up state is SILENT.                       [L][G]
enum {
    MBOX3_MIX_IN_SW_RET1 = 0, MBOX3_MIX_IN_SW_RET2, MBOX3_MIX_IN_SW_RET3, MBOX3_MIX_IN_SW_RET4,
    MBOX3_MIX_IN_HW_IN1,      MBOX3_MIX_IN_HW_IN2,  MBOX3_MIX_IN_SPDIF_L, MBOX3_MIX_IN_SPDIF_R,
    MBOX3_MIX_IN_FX_RET,      // FX return is stereo: outputs use 0x0120..0x0123     [L]
    MBOX3_MIX_NUM_INPUTS
};
enum {
    MBOX3_MIX_OUT_1 = 0,  // Out 1 + headphones L  ("Analog 1/2" mix)
    MBOX3_MIX_OUT_2,      // Out 2 + headphones R
    MBOX3_MIX_OUT_3,      // S/PDIF out L          ("S/PDIF L/R" mix)
    MBOX3_MIX_OUT_4,      // S/PDIF out R
    MBOX3_MIX_NUM_OUTPUTS
};
#define MBOX3_GAIN_OFF             ((int16_t)(-32768))   // 0x8000
#define MBOX3_GAIN_UNITY           ((int16_t)0)

static inline uint16_t mbox3_mixer_wvalue(unsigned input, unsigned output) {
    return (uint16_t)(0x0100 | ((input * 4u + output) & 0xFF));
}
static inline uint16_t mbox3_fxsend_wvalue(unsigned input /* 0-7 */) {
    return (uint16_t)(0x0100 | (input & 0x07));
}

// dB -> hardware int16 (1/256 dB). Values <= -128 dB (or NaN) become OFF; values > 0 dB clamp to 0.
// The hardware itself quantizes to 0.25 dB steps.                                          [G]
static inline int16_t mbox3_db_to_gain(double db) {
    if (!(db > -128.0)) return MBOX3_GAIN_OFF;   // also catches NaN
    if (db > 0.0) db = 0.0;
    double v = db * 256.0;
    long r = (long)(v < 0 ? v - 0.5 : v + 0.5);
    if (r <= -32768) return MBOX3_GAIN_OFF;
    return (int16_t)r;
}
static inline double mbox3_gain_to_db(int16_t g) {
    return g == MBOX3_GAIN_OFF ? -1.0e9 : (double)g / 256.0;
}
static inline void mbox3_put_le16(uint8_t out[2], int16_t v) {
    uint16_t u = (uint16_t)v;
    out[0] = (uint8_t)(u & 0xFF);
    out[1] = (uint8_t)(u >> 8);
}
static inline void mbox3_put_le32(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16); out[3] = (uint8_t)(v >> 24);
}
static inline uint32_t mbox3_get_le32(const uint8_t in[4]) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

// ---------------------------------------------------------------- Effect unit (0x43)
#define MBOX3_FX_PRESET            0x0200   // 1 byte: 0-5 Room1,Room2,Room3,Hall1,Hall2,Plate; 6 Delay; 7 Echo [L][G]
#define MBOX3_FX_RETURN_VOL        0x0300   // 1 byte 0-255                                       [L][G]
#define MBOX3_FX_DURATION          0x0400   // 2 bytes LE 0-65535                                 [L][G]
#define MBOX3_FX_FEEDBACK          0x0500   // 1 byte 0-255 (Delay/Echo only)                     [L][G]

// ---------------------------------------------------------------- Default state
// The known-good start-up state: tuner off, internal clock, every crosspoint off except
// SW Return n -> Out n at unity, all FX sends off, FX return volume/feedback 0.  [L: mbox3_setup_defaults][G]
typedef struct { uint8_t bRequest; uint16_t wValue; uint16_t wIndex; uint8_t len; uint8_t data[2]; } Mbox3Write;

#ifdef __cplusplus
}
#endif
#endif // MBOX3_PROTOCOL_H
