// Tests the portable descriptor printer against a hand-built descriptor shaped like the Mbox's streaming
// interfaces (vendor class 0xFF, isoch async OUT 0x01 and IN 0x81, 156-byte packets, every microframe).
#include "../Sources/mbox3ctl/descdump.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const unsigned char d[] = {
        9, 0x02, 0, 0, 2, 1, 0, 0x80, 50,           // configuration (wTotalLength patched below)
        9, 0x04, 2, 0, 0, 0xFF, 0x02, 0x20, 0,      // interface 2 alt 0, vendor class
        9, 0x04, 2, 1, 1, 0xFF, 0x02, 0x20, 0,      // interface 2 alt 1
        7, 0x05, 0x01, 0x05, 156, 0, 1,             // EP 0x01 OUT isoch async, 156 bytes, bInterval 1
        9, 0x04, 3, 1, 1, 0xFF, 0x02, 0x20, 0,      // interface 3 alt 1
        7, 0x05, 0x81, 0x05, 156, 0, 1,             // EP 0x81 IN isoch async
    };
    unsigned char buf[sizeof d];
    memcpy(buf, d, sizeof d);
    buf[2] = (unsigned char)sizeof d;
    Mbox3ConfigSummary s;
    int rc = mbox3_dump_config(buf, sizeof buf, stdout, &s);
    int ok = rc == 0 && s.totalLength == sizeof d && s.numEndpoints == 2 && !s.malformed;

    buf[9] = 0;                                     // corrupt: zero-length descriptor
    Mbox3ConfigSummary s2;
    int rc2 = mbox3_dump_config(buf, sizeof buf, stdout, &s2);
    ok = ok && rc2 == -1 && s2.malformed;

    printf("descdump: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
