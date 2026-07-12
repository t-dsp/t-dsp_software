// OpmBank.cpp — see OpmBank.h. A small, allocation-free .opm text parser.

#include "OpmBank.h"
#include <string.h>
#include <stdlib.h>

namespace tdsp {
namespace ymfmopm {

// Read up to maxn decimal integers from [p, end) into out[]. Stops at the line
// boundary (end) — importantly it never lets strtol skip a newline into the next
// line, because it only calls strtol when the next non-space char is a digit/sign
// and it never advances past `end`.
static int readInts(const char *p, const char *end, long *out, int maxn) {
    int n = 0;
    while (n < maxn) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (p >= end) break;
        if (*p != '-' && *p != '+' && !(*p >= '0' && *p <= '9')) break;
        char *e;
        long v = strtol(p, &e, 10);
        if (e == p || (const char *)e > end) break;
        out[n++] = v;
        p = e;
    }
    return n;
}

static inline uint8_t fld(const long *f, int n, int i) {
    return (i < n && f[i] >= 0) ? (uint8_t)f[i] : 0;
}

// Fill one operator from a parsed field list: AR D1R D2R RR D1L TL KS MUL DT1 DT2 AME
static void setOp(OpmOp &o, const long *f, int n) {
    o.ar  = fld(f, n, 0);  o.d1r = fld(f, n, 1);  o.d2r = fld(f, n, 2);  o.rr = fld(f, n, 3);
    o.sl  = fld(f, n, 4);  o.tl  = fld(f, n, 5);  o.ks  = fld(f, n, 6);  o.mul = fld(f, n, 7);
    o.dt1 = fld(f, n, 8);  o.dt2 = fld(f, n, 9);  o.ams = fld(f, n, 10);
}

int parseOpmBank(const char *text, size_t len, OpmVoice *out, int maxVoices) {
    if (!text || !out || maxVoices <= 0) return 0;
    int cur = -1;                          // index of the voice being filled
    const char *p = text;
    const char *end = text + len;

    while (p < end) {
        const char *lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n') lineEnd++;

        const char *ls = p;                                        // line start, skip indent
        while (ls < lineEnd && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;

        const char *colon = ls;                                    // split on first ':'
        while (colon < lineEnd && *colon != ':') colon++;

        if (colon < lineEnd) {
            const char *le = colon;                                // trim trailing ws off label
            while (le > ls && (le[-1] == ' ' || le[-1] == '\t')) le--;
            size_t llen = (size_t)(le - ls);
            const char *rest = colon + 1;

            if (llen == 1 && ls[0] == '@') {
                // "@:<num> <name>" — start a new voice
                if (cur + 1 >= maxVoices) break;                   // array full
                cur++;
                OpmVoice &v = out[cur];
                memset(&v, 0, sizeof(v));
                char *e; strtol(rest, &e, 10);                     // skip the index
                const char *nm = e;
                while (nm < lineEnd && (*nm == ' ' || *nm == '\t')) nm++;
                const char *ne = lineEnd;                          // trim trailing ws/\r
                while (ne > nm && (ne[-1] == ' ' || ne[-1] == '\t' || ne[-1] == '\r')) ne--;
                size_t nlen = (size_t)(ne - nm);
                if (nlen > (size_t)(kVoiceNameLen - 1)) nlen = kVoiceNameLen - 1;
                memcpy(v.name, nm, nlen); v.name[nlen] = 0;
            } else if (cur >= 0) {
                OpmVoice &v = out[cur];
                long f[12];
                if (llen == 2 && ls[0] == 'C' && ls[1] == 'H') {
                    int n = readInts(rest, lineEnd, f, 7);          // PAN FL CON AMS PMS SLOT NE
                    if (n >= 2) v.feedback = (uint8_t)f[1];
                    if (n >= 3) v.alg = (uint8_t)f[2];
                } else if (llen == 2 && (ls[0] == 'M' || ls[0] == 'C') && (ls[1] == '1' || ls[1] == '2')) {
                    int slot = (ls[0] == 'M' && ls[1] == '1') ? 0
                             : (ls[0] == 'M' && ls[1] == '2') ? 1
                             : (ls[0] == 'C' && ls[1] == '1') ? 2
                             : 3;                                   // C2
                    int n = readInts(rest, lineEnd, f, 11);
                    setOp(v.op[slot], f, n);
                }
                // LFO: and anything else are parsed-past.
            }
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }
    return cur + 1;
}

} // namespace ymfmopm
} // namespace tdsp
