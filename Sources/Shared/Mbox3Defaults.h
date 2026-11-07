// Mbox3Defaults.h — the known-good start-up state as a list of control writes.
// Pure C, header-only. Shared by mbox3ctl (`init`), the future driver and app, and the unit tests.
//
// Source: Linux mbox3_setup_defaults() [L], matched against GhostBox's verified default [G].
// One deliberate difference: Linux turns the tuner off with bRequest CUR (0x01); GhostBox, verified on
// hardware, addresses every extension-unit register with MEM (0x03). We follow GhostBox. If your unit
// ignores it, try CUR (mbox3ctl raw ...).

#ifndef MBOX3_DEFAULTS_H
#define MBOX3_DEFAULTS_H

#include "Mbox3Protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fills `out` (capacity `cap`) with the writes in order; returns how many were produced
// (MBOX3_DEFAULT_WRITE_COUNT), or -1 if `cap` is too small.
#define MBOX3_DEFAULT_WRITE_COUNT (2 + 36 + 8 + 4 + 4)

static inline int mbox3_default_writes(Mbox3Write *out, int cap) {
    if (cap < MBOX3_DEFAULT_WRITE_COUNT) return -1;
    int n = 0;
    Mbox3Write w;

    // 1. Tuner off (extension unit, MEM).
    w.bRequest = MBOX3_REQ_MEM; w.wValue = MBOX3_EXT_TUNER; w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_EXTENSION);
    w.len = 1; w.data[0] = 0; w.data[1] = 0; out[n++] = w;

    // 2. Clock source = internal. Without this the Mbox waits for S/PDIF and never streams.
    w.bRequest = MBOX3_REQ_CUR; w.wValue = MBOX3_CS_SAM_FREQ; w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_CLOCK_SELECTOR);
    w.len = 1; w.data[0] = MBOX3_CLOCK_SRC_INTERNAL; w.data[1] = 0; out[n++] = w;

    // 3. Monitor mixer: all 36 crosspoints (9 inputs x 4 outputs). SW Return n -> Out n at 0 dB, rest off.
    for (unsigned in = 0; in < MBOX3_MIX_NUM_INPUTS; in++) {
        for (unsigned o = 0; o < MBOX3_MIX_NUM_OUTPUTS; o++) {
            int16_t g = (in < 4 && in == o) ? MBOX3_GAIN_UNITY : MBOX3_GAIN_OFF;
            w.bRequest = MBOX3_REQ_CUR; w.wValue = mbox3_mixer_wvalue(in, o);
            w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_MIXER); w.len = 2; mbox3_put_le16(w.data, g); out[n++] = w;
        }
    }

    // 4. FX sends off (SW Returns 1-4, HW inputs 1-4).
    for (unsigned in = 0; in < 8; in++) {
        w.bRequest = MBOX3_REQ_CUR; w.wValue = mbox3_fxsend_wvalue(in);
        w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_FX_SEND); w.len = 2; mbox3_put_le16(w.data, MBOX3_GAIN_OFF); out[n++] = w;
    }

    // 5. Effect: Room 1, duration 0, feedback 0, return volume 0.
    w.bRequest = MBOX3_REQ_CUR; w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_EFFECT); w.data[0] = 0; w.data[1] = 0;
    w.wValue = MBOX3_FX_PRESET;     w.len = 1; out[n++] = w;
    w.wValue = MBOX3_FX_DURATION;   w.len = 2; out[n++] = w;
    w.wValue = MBOX3_FX_FEEDBACK;   w.len = 1; out[n++] = w;
    w.wValue = MBOX3_FX_RETURN_VOL; w.len = 1; out[n++] = w;

    // 6. Front panel: hold time 500 ms, LEDs off (not dim), mono off, dim off.
    w.bRequest = MBOX3_REQ_MEM; w.wIndex = MBOX3_WINDEX(MBOX3_UNIT_EXTENSION); w.len = 1; w.data[1] = 0;
    w.wValue = MBOX3_EXT_HOLD_TIME; w.data[0] = 0x05; out[n++] = w;
    w.wValue = MBOX3_EXT_LED_DIM;   w.data[0] = 0x00; out[n++] = w;
    w.wValue = MBOX3_EXT_DIM;       w.data[0] = 0x00; out[n++] = w;
    w.wValue = MBOX3_EXT_MONO;      w.data[0] = 0x00; out[n++] = w;

    return n;
}

#ifdef __cplusplus
}
#endif
#endif // MBOX3_DEFAULTS_H
