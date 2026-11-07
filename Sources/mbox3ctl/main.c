// mbox3ctl — talk to an Avid Mbox (3rd gen) from the macOS command line, with no driver installed.
//
// Phase 1 of the Mbox3Driver project: prove that this Mac can reach the hardware from user space
// (IOKit, no kext, no DriverKit, no paid developer account) and that the protocol in
// Sources/Shared/Mbox3Protocol.h is right for YOUR unit, before any Core Audio code is written.
//
//   mbox3ctl info                 device, speed, firmware, full descriptor dump
//   mbox3ctl clock                clock source, internal rate, S/PDIF validity
//   mbox3ctl rate <hz>            set 44100 | 48000 | 88200 | 96000 (selects the internal clock)
//   mbox3ctl init                 write the known-good default state (internal clock, SW returns -> outs)
//   mbox3ctl meters [--watch]     read the 15 hardware meters once, or live
//   mbox3ctl mix <in> <out> <dB|off>   set one monitor-mixer crosspoint (in 0-8, out 0-3)
//   mbox3ctl monitor <on|off>     route In 1 / In 2 to Out 1 / Out 2 at 0 dB (zero-latency monitoring test)
//   mbox3ctl tuner|mono|dim <on|off>
//   mbox3ctl raw get <bRequest> <wValue> <wIndex> <length>
//   mbox3ctl raw set <bRequest> <wValue> <wIndex> [byte ...]
//   mbox3ctl replug               software unplug/replug (USBDeviceReEnumerate)
//
// Build: ./build.sh cli   (needs only the free Xcode Command Line Tools: xcode-select --install)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Mbox3Defaults.h"
#include "Mbox3Protocol.h"
#include "descdump.h"

typedef IOUSBDeviceInterface320 **Dev;

// ---------------------------------------------------------------- Device access

static Dev open_mbox(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return NULL;
    SInt32 vid = MBOX3_VENDOR_ID, pid = MBOX3_PRODUCT_ID;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vid);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), p);
    CFRelease(v);
    CFRelease(p);

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);   // consumes `match`
    if (!svc) return NULL;

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService failed: 0x%08x\n", kr);
        return NULL;
    }
    Dev dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&dev);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || !dev) {
        fprintf(stderr, "QueryInterface(IOUSBDeviceInterface320) failed\n");
        return NULL;
    }
    return dev;
}

// Control request on the default pipe. Exclusive access (USBDeviceOpen) is NOT needed for this, so the
// tool can run alongside a driver that owns the streaming interfaces.
static IOReturn request(Dev dev, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                        void *data, uint16_t length, uint32_t *done) {
    IOUSBDevRequestTO r;
    memset(&r, 0, sizeof r);
    r.bmRequestType = bmRequestType;
    r.bRequest = bRequest;
    r.wValue = wValue;
    r.wIndex = wIndex;
    r.wLength = length;
    r.pData = data;
    r.noDataTimeout = 1000;
    r.completionTimeout = 1000;
    IOReturn kr = (*dev)->DeviceRequestTO(dev, &r);
    if (done) *done = r.wLenDone;
    return kr;
}

static int set_bytes(Dev dev, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, const uint8_t *data, uint16_t len) {
    uint8_t buf[64];
    if (len > sizeof buf) return -1;
    memcpy(buf, data, len);
    IOReturn kr = request(dev, MBOX3_RT_SET, bRequest, wValue, wIndex, buf, len, NULL);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "  SET req=0x%02x wValue=0x%04x wIndex=0x%04x failed: 0x%08x\n", bRequest, wValue, wIndex, kr);
        return -1;
    }
    return 0;
}

static int get_bytes(Dev dev, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint8_t *out, uint16_t len,
                     uint32_t *done) {
    IOReturn kr = request(dev, MBOX3_RT_GET, bRequest, wValue, wIndex, out, len, done);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "  GET req=0x%02x wValue=0x%04x wIndex=0x%04x failed: 0x%08x\n", bRequest, wValue, wIndex, kr);
        return -1;
    }
    return 0;
}

static int set_u8(Dev dev, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint8_t v) {
    return set_bytes(dev, bRequest, wValue, wIndex, &v, 1);
}

// ---------------------------------------------------------------- Commands

static const char *speed_name(UInt8 s) {
    switch (s) {
        case kUSBDeviceSpeedLow: return "low (1.5 Mb/s)";
        case kUSBDeviceSpeedFull: return "full (12 Mb/s)  <-- the Mbox needs high speed; try another port/cable";
        case kUSBDeviceSpeedHigh: return "high (480 Mb/s)";
        default: return "super speed or newer (unexpected for this device)";
    }
}

static int cmd_info(Dev dev) {
    UInt16 vid = 0, pid = 0, rel = 0;
    UInt32 loc = 0;
    UInt8 speed = 0;
    (*dev)->GetDeviceVendor(dev, &vid);
    (*dev)->GetDeviceProduct(dev, &pid);
    (*dev)->GetDeviceReleaseNumber(dev, &rel);
    (*dev)->GetLocationID(dev, &loc);
    (*dev)->GetDeviceSpeed(dev, &speed);
    printf("Mbox 3 found\n");
    printf("  USB ID      %04x:%04x\n", vid, pid);
    printf("  Firmware    %x.%02x (bcdDevice 0x%04x)\n", rel >> 8, rel & 0xFF, rel);
    printf("  Location    0x%08x\n", loc);
    printf("  Speed       %s\n", speed_name(speed));

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    IOReturn kr = (*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg);
    if (kr != kIOReturnSuccess || !cfg) {
        fprintf(stderr, "GetConfigurationDescriptorPtr failed: 0x%08x\n", kr);
        return 1;
    }
    uint16_t total = USBToHostWord(cfg->wTotalLength);
    Mbox3ConfigSummary sum;
    mbox3_dump_config((const uint8_t *)cfg, total, stdout, &sum);
    if (sum.totalLength != MBOX3_CONFIG_DESC_SIZE)
        printf("  NOTE: wTotalLength is %u; the Linux driver expects %u. Report this — your firmware may differ.\n",
               sum.totalLength, MBOX3_CONFIG_DESC_SIZE);
    return sum.malformed ? 1 : 0;
}

static int cmd_clock(Dev dev) {
    uint8_t src = 0, r[4] = {0}, valid = 0;
    int bad = 0;
    bad |= get_bytes(dev, MBOX3_REQ_CUR, MBOX3_CS_SAM_FREQ, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_SELECTOR), &src, 1, NULL);
    bad |= get_bytes(dev, MBOX3_REQ_CUR, MBOX3_CS_SAM_FREQ, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_INTERNAL), r, 4, NULL);
    printf("Clock source   %u (%s)\n", src,
           src == MBOX3_CLOCK_SRC_INTERNAL ? "internal" : src == MBOX3_CLOCK_SRC_SPDIF ? "S/PDIF" : "unknown");
    printf("Internal rate  %u Hz\n", mbox3_get_le32(r));
    if (src == MBOX3_CLOCK_SRC_SPDIF) {
        if (get_bytes(dev, MBOX3_REQ_CUR, MBOX3_CS_CLOCK_VALID, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_SPDIF), &valid, 1, NULL) == 0)
            printf("S/PDIF valid   %s\n", valid ? "yes" : "NO  <-- the Mbox will not stream; run `mbox3ctl init`");
    }
    return bad ? 1 : 0;
}

static int cmd_rate(Dev dev, uint32_t hz) {
    int ok = 0;
    for (int i = 0; i < MBOX3_NUM_SAMPLE_RATES; i++) ok |= (kMbox3SampleRates[i] == hz);
    if (!ok) {
        fprintf(stderr, "Unsupported rate %u (use 44100, 48000, 88200 or 96000)\n", hz);
        return 2;
    }
    // Same order as the Linux driver: write the internal rate, select internal, read back.
    uint8_t b[4];
    mbox3_put_le32(b, hz);
    if (set_bytes(dev, MBOX3_REQ_CUR, MBOX3_CS_SAM_FREQ, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_INTERNAL), b, 4)) return 1;
    if (set_u8(dev, MBOX3_REQ_CUR, MBOX3_CS_SAM_FREQ, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_SELECTOR), MBOX3_CLOCK_SRC_INTERNAL))
        return 1;
    usleep(100 * 1000);   // give the PLL a moment before reading back
    memset(b, 0, sizeof b);
    if (get_bytes(dev, MBOX3_REQ_CUR, MBOX3_CS_SAM_FREQ, MBOX3_WINDEX(MBOX3_UNIT_CLOCK_INTERNAL), b, 4, NULL)) return 1;
    uint32_t now = mbox3_get_le32(b);
    printf("Requested %u Hz, device reports %u Hz: %s\n", hz, now, now == hz ? "OK" : "MISMATCH");
    return now == hz ? 0 : 1;
}

static int cmd_init(Dev dev) {
    Mbox3Write w[MBOX3_DEFAULT_WRITE_COUNT];
    int n = mbox3_default_writes(w, MBOX3_DEFAULT_WRITE_COUNT);
    int failed = 0;
    for (int i = 0; i < n; i++) failed += set_bytes(dev, w[i].bRequest, w[i].wValue, w[i].wIndex, w[i].data, w[i].len) != 0;
    printf("Default state: %d writes, %d failed\n", n, failed);
    printf("  internal clock, SW Return 1-4 -> Out 1-4 at 0 dB, inputs and FX muted, tuner off\n");
    return failed ? 1 : 0;
}

static volatile sig_atomic_t gStop = 0;
static void on_sigint(int s) { (void)s; gStop = 1; }

static int read_meters(Dev dev, uint16_t out[MBOX3_NUM_METERS]) {
    uint8_t b[MBOX3_NUM_METERS * 2];
    uint32_t done = 0;
    memset(b, 0, sizeof b);
    if (get_bytes(dev, MBOX3_REQ_MEM, MBOX3_EXT_METERS, MBOX3_WINDEX(MBOX3_UNIT_EXTENSION), b, sizeof b, &done)) return -1;
    for (int i = 0; i < MBOX3_NUM_METERS; i++) out[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return (int)done;
}

static int cmd_meters(Dev dev, int watch) {
    signal(SIGINT, on_sigint);
    uint16_t m[MBOX3_NUM_METERS];
    do {
        int got = read_meters(dev, m);
        if (got < 0) return 1;
        if (watch) printf("\033[H\033[2J");   // clear screen
        printf("Hardware meters (%d bytes)%s\n", got, watch ? "   — Ctrl-C to stop" : "");
        for (int i = 0; i < MBOX3_NUM_METERS; i++) {
            double db = m[i] ? 20.0 * log10((double)m[i] / 32767.0) : -120.0;
            int bar = db <= -60.0 ? 0 : (int)((db + 60.0) / 60.0 * 40.0);
            printf("  %-12s %6u %7.1f dBFS |", kMbox3MeterNames[i], m[i], db);
            for (int k = 0; k < 40; k++) putchar(k < bar ? '#' : ' ');
            printf("|\n");
        }
        if (watch) usleep(80 * 1000);
    } while (watch && !gStop);
    return 0;
}

static int cmd_mix(Dev dev, unsigned in, unsigned out, const char *level) {
    if (in >= MBOX3_MIX_NUM_INPUTS || out >= MBOX3_MIX_NUM_OUTPUTS) {
        fprintf(stderr, "in must be 0-8 (0-3 SW Ret 1-4, 4 In 1, 5 In 2, 6 S/PDIF L, 7 S/PDIF R, 8 FX ret), out 0-3\n");
        return 2;
    }
    int16_t g = strcmp(level, "off") == 0 ? MBOX3_GAIN_OFF : mbox3_db_to_gain(atof(level));
    uint8_t b[2];
    mbox3_put_le16(b, g);
    if (set_bytes(dev, MBOX3_REQ_CUR, mbox3_mixer_wvalue(in, out), MBOX3_WINDEX(MBOX3_UNIT_MIXER), b, 2)) return 1;
    if (g == MBOX3_GAIN_OFF) printf("Crosspoint in %u -> out %u: off\n", in, out);
    else printf("Crosspoint in %u -> out %u: %.2f dB (raw 0x%04x)\n", in, out, mbox3_gain_to_db(g), (uint16_t)g);
    return 0;
}

static int cmd_monitor(Dev dev, int on) {
    int16_t g = on ? MBOX3_GAIN_UNITY : MBOX3_GAIN_OFF;
    uint8_t b[2];
    mbox3_put_le16(b, g);
    int bad = 0;
    bad |= set_bytes(dev, MBOX3_REQ_CUR, mbox3_mixer_wvalue(MBOX3_MIX_IN_HW_IN1, MBOX3_MIX_OUT_1), MBOX3_WINDEX(MBOX3_UNIT_MIXER), b, 2);
    bad |= set_bytes(dev, MBOX3_REQ_CUR, mbox3_mixer_wvalue(MBOX3_MIX_IN_HW_IN2, MBOX3_MIX_OUT_2), MBOX3_WINDEX(MBOX3_UNIT_MIXER), b, 2);
    printf("Direct monitoring In 1 -> Out 1, In 2 -> Out 2: %s\n", on ? "on (0 dB)" : "off");
    return bad ? 1 : 0;
}

static int cmd_ext_switch(Dev dev, uint16_t reg, const char *name, const char *arg) {
    int on = strcmp(arg, "on") == 0;
    if (!on && strcmp(arg, "off") != 0) { fprintf(stderr, "use on|off\n"); return 2; }
    if (set_u8(dev, MBOX3_REQ_MEM, reg, MBOX3_WINDEX(MBOX3_UNIT_EXTENSION), (uint8_t)on)) return 1;
    printf("%s %s\n", name, on ? "on" : "off");
    return 0;
}

static int cmd_raw(Dev dev, int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "raw get|set <bRequest> <wValue> <wIndex> ...\n"); return 2; }
    uint8_t bRequest = (uint8_t)strtoul(argv[2], NULL, 0);
    uint16_t wValue = (uint16_t)strtoul(argv[3], NULL, 0);
    uint16_t wIndex = (uint16_t)strtoul(argv[4], NULL, 0);
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    if (strcmp(argv[1], "get") == 0) {
        uint16_t len = argc > 5 ? (uint16_t)strtoul(argv[5], NULL, 0) : 1;
        if (len > sizeof buf) len = sizeof buf;
        uint32_t done = 0;
        if (get_bytes(dev, bRequest, wValue, wIndex, buf, len, &done)) return 1;
        printf("%u byte(s):", done);
        for (uint32_t i = 0; i < done; i++) printf(" %02x", buf[i]);
        printf("\n");
        return 0;
    }
    if (strcmp(argv[1], "set") == 0) {
        uint16_t len = 0;
        for (int i = 5; i < argc && len < sizeof buf; i++) buf[len++] = (uint8_t)strtoul(argv[i], NULL, 0);
        if (set_bytes(dev, bRequest, wValue, wIndex, buf, len)) return 1;
        printf("OK (%u byte(s))\n", len);
        return 0;
    }
    fprintf(stderr, "raw get|set ...\n");
    return 2;
}

static int cmd_replug(Dev dev) {
    IOReturn open = (*dev)->USBDeviceOpen(dev);
    if (open != kIOReturnSuccess && open != kIOReturnExclusiveAccess) {
        fprintf(stderr, "USBDeviceOpen failed: 0x%08x\n", open);
        return 1;
    }
    IOReturn kr = (*dev)->USBDeviceReEnumerate(dev, 0);
    if (open == kIOReturnSuccess) (*dev)->USBDeviceClose(dev);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "USBDeviceReEnumerate failed: 0x%08x\n", kr);
        return 1;
    }
    printf("Re-enumeration requested; the Mbox will disappear and come back in a second or two.\n");
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: mbox3ctl <command>\n"
            "  info | clock | rate <hz> | init | meters [--watch]\n"
            "  mix <in 0-8> <out 0-3> <dB|off> | monitor <on|off>\n"
            "  tuner|mono|dim <on|off> | raw get|set ... | replug\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 2; }
    Dev dev = open_mbox();
    if (!dev) {
        fprintf(stderr, "No Mbox 3 (USB 0dba:5000) found. Is it plugged in directly (no hub) and powered?\n");
        return 3;
    }
    const char *c = argv[1];
    int rc = 2;
    if (!strcmp(c, "info")) rc = cmd_info(dev);
    else if (!strcmp(c, "clock")) rc = cmd_clock(dev);
    else if (!strcmp(c, "rate") && argc > 2) rc = cmd_rate(dev, (uint32_t)strtoul(argv[2], NULL, 10));
    else if (!strcmp(c, "init")) rc = cmd_init(dev);
    else if (!strcmp(c, "meters")) rc = cmd_meters(dev, argc > 2 && !strcmp(argv[2], "--watch"));
    else if (!strcmp(c, "mix") && argc > 4)
        rc = cmd_mix(dev, (unsigned)strtoul(argv[2], NULL, 0), (unsigned)strtoul(argv[3], NULL, 0), argv[4]);
    else if (!strcmp(c, "monitor") && argc > 2) rc = cmd_monitor(dev, !strcmp(argv[2], "on"));
    else if (!strcmp(c, "tuner") && argc > 2) rc = cmd_ext_switch(dev, MBOX3_EXT_TUNER, "Tuner", argv[2]);
    else if (!strcmp(c, "mono") && argc > 2) rc = cmd_ext_switch(dev, MBOX3_EXT_MONO, "Mono", argv[2]);
    else if (!strcmp(c, "dim") && argc > 2) rc = cmd_ext_switch(dev, MBOX3_EXT_DIM, "Dim", argv[2]);
    else if (!strcmp(c, "raw")) rc = cmd_raw(dev, argc - 1, argv + 1);
    else if (!strcmp(c, "replug")) rc = cmd_replug(dev);
    else usage();
    (*dev)->Release(dev);
    return rc;
}
