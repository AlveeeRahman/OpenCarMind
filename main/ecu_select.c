#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ecu_select.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int hexbyte(const char *s)
{
    int hi = hexval(s[0]), lo = hi < 0 ? -1 : hexval(s[1]);
    return hi < 0 || lo < 0 ? -1 : hi << 4 | lo;
}

int ecu_parse_replies(const char *resp, bool ext29, ecu_t *out, int max)
{
    int n = 0;
    size_t idl = ext29 ? 8 : 3;
    const char *line = resp;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t L = eol ? (size_t)(eol - line) : strlen(line);
        char buf[80];
        size_t k = 0;
        bool hexline = true;
        for (size_t i = 0; i < L && k + 1 < sizeof buf; i++) {
            unsigned char c = (unsigned char)line[i];
            if (isspace(c)) continue;
            if (!isxdigit(c)) { hexline = false; break; }
            buf[k++] = (char)toupper(c);
        }
        buf[k] = 0;
        line = eol ? eol + 1 : NULL;
        if (!hexline || k < idl + 2 + 4 + 8) continue;
        int pci = hexbyte(buf + idl);
        if (pci < 0 || (pci >> 4) != 0 || (pci & 0x0F) < 6 || strncmp(buf + idl + 2, "4100", 4) != 0) continue;

        ecu_t e;
        memset(&e, 0, sizeof e);
        memcpy(e.resp_id, buf, idl);
        e.resp_id[idl] = 0;
        if (!ext29) {
            unsigned v = (unsigned)strtoul(e.resp_id, NULL, 16);
            if (v < 0x7E8 || v > 0x7EF) continue;
            snprintf(e.req_hdr, sizeof e.req_hdr, "%03X", v - 8);
        } else {
            if (strncmp(e.resp_id, "18DAF1", 6) != 0) continue;
            snprintf(e.req_hdr, sizeof e.req_hdr, "18DA%.2sF1", e.resp_id + 6);
        }
        bool bad = false;
        for (int i = 0; i < 4; i++) {
            int b = hexbyte(buf + idl + 6 + 2 * i);
            if (b < 0) { bad = true; break; }
            e.sup01[i] = (uint8_t)b;
        }
        if (bad) continue;

        int pos = n;
        bool dup = false;
        for (int i = 0; i < n; i++) {
            int c = strcmp(out[i].resp_id, e.resp_id);
            if (c == 0) { dup = true; break; }
            if (c > 0 && pos == n) pos = i;
        }
        if (dup || n >= max) continue;
        memmove(&out[pos + 1], &out[pos], (size_t)(n - pos) * sizeof out[0]);
        out[pos] = e;
        n++;
    }
    return n;
}

static bool sup01_bit(const uint8_t b[4], uint8_t pid) { return pid >= 1 && pid <= 32 && ((b[(pid - 1) / 8] >> (7 - (pid - 1) % 8)) & 1); }

int ecu_pick_engine(const ecu_t *e, int n)
{
    for (int i = 0; i < n; i++)
        if (strcmp(e[i].resp_id, "7E8") == 0 || strcmp(e[i].resp_id, "18DAF110") == 0) return i;
    int best = -1, best_pop = -1;
    for (int i = 0; i < n; i++) {
        if (!sup01_bit(e[i].sup01, 0x0C)) continue;
        int pop = 0;
        for (uint8_t pid = 1; pid <= 32; pid++) pop += sup01_bit(e[i].sup01, pid);
        if (pop > best_pop) { best_pop = pop; best = i; }
    }
    return best;
}

size_t ecu_search_summary(const char *proto, const ecu_t *e, const bool *rpm_ok, int n, int pick, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t k = (size_t)snprintf(out, cap, "%s:", proto && proto[0] ? proto : "?");
    if (n <= 0 && k < cap) k += (size_t)snprintf(out + k, cap - k, " one computer");
    for (int i = 0; i < n && k < cap; i++)
        k += (size_t)snprintf(out + k, cap - k, "%s %s %s", i ? "," : "", e[i].resp_id, rpm_ok && rpm_ok[i] ? "rpm" : "-");
    if (k < cap) k += (size_t)snprintf(out + k, cap - k, "; use %s", pick >= 0 && pick < n ? e[pick].req_hdr : "all");
    return k < cap ? k : cap - 1;
}
