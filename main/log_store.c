/* Drive-log store rules: see log_store.h. */
#include "log_store.h"
#include <stdlib.h>
#include <string.h>
#include "session_next.h"

bool log_store_is_drive_file(const char *name)
{
    return name && (session_id_of_name(name) != 0 || !strcmp(name, "drives.csv") || !strcmp(name, "DRIVES.CSV"));
}

bool log_store_on_nand(const char *name, bool nand_mounted)
{
    return nand_mounted && log_store_is_drive_file(name);
}

uint32_t log_store_next_session(uint32_t nvs_last, uint32_t highest_nand, uint32_t highest_flash)
{
    return session_next_id(nvs_last, highest_nand > highest_flash ? highest_nand : highest_flash);
}

bool log_store_is_move_tmp(const char *name)
{
    size_t n = name ? strlen(name) : 0, k = strlen(LOG_STORE_MOVE_SUFFIX);
    return n > k && session_id_of_name(name) != 0 && !strcmp(name + n - k, LOG_STORE_MOVE_SUFFIX);
}

ls_move_t log_store_move_step(bool on_nand, uint64_t nand_size, uint32_t nand_crc, uint64_t flash_size, uint32_t flash_crc)
{
    if (!on_nand) return LS_COPY;
    return nand_size == flash_size && nand_crc == flash_crc ? LS_DROP_FLASH : LS_CONFLICT;
}

bool log_store_copy_verified(uint64_t flash_size, uint32_t flash_crc, uint64_t nand_size, uint32_t nand_crc)
{
    return flash_size == nand_size && flash_crc == nand_crc;
}

/* ---------- drives.csv merge ---------- */
typedef struct { const char *p; size_t len; uint32_t id; size_t ord; } row_t;

static bool is_number_field(const char *p, size_t len, uint32_t *id)
{
    size_t i = 0;
    uint32_t v = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') { v = v * 10 + (uint32_t)(p[i] - '0'); i++; }
    if (i == 0 || (i < len && p[i] != ',')) return false;
    *id = v;
    return true;
}

static int row_cmp(const void *x, const void *y)
{
    const row_t *a = x, *b = y;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return a->ord < b->ord ? -1 : a->ord > b->ord ? 1 : 0;
}

static size_t count_lines(const char *s) { size_t n = 0; for (; s && *s; s++) n += *s == '\n'; return n + 1; }

/* splits s into rows; the first non-numeric line is the header (kept in *hdr when none was found yet) */
static size_t split(const char *s, row_t *rows, size_t n, size_t *ord, const char **hdr, size_t *hdr_len)
{
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        size_t L = len;
        if (L && s[L - 1] == '\r') L--;
        uint32_t id = 0;
        if (L) {
            if (is_number_field(s, L, &id)) rows[n++] = (row_t){s, L, id, (*ord)++};
            else if (!*hdr) { *hdr = s; *hdr_len = L; }
            else if (L != *hdr_len || memcmp(s, *hdr, L)) rows[n++] = (row_t){s, L, UINT32_MAX, (*ord)++};
        }
        s = e ? e + 1 : s + len;
    }
    return n;
}

long log_store_merge_drives_csv(const char *a, const char *b, char *out, size_t out_n)
{
    if (!out || !out_n) return -1;
    out[0] = 0;
    size_t cap = count_lines(a) + count_lines(b);
    row_t *rows = malloc(cap * sizeof *rows);
    if (!rows) return -1;
    const char *hdr = NULL;
    size_t hdr_len = 0, ord = 0, n = 0;
    n = split(a, rows, n, &ord, &hdr, &hdr_len);
    n = split(b, rows, n, &ord, &hdr, &hdr_len);
    qsort(rows, n, sizeof *rows, row_cmp);
    size_t w = 0;
    bool fits = true;
    if (hdr) {
        if (hdr_len + 1 >= out_n) fits = false;
        else { memcpy(out, hdr, hdr_len); out[hdr_len] = '\n'; w = hdr_len + 1; }
    }
    for (size_t i = 0; fits && i < n; i++) {
        bool dup = false;                                    /* the same row in both files: once */
        for (size_t k = i; k-- > 0 && rows[k].id == rows[i].id && !dup;)
            dup = rows[k].len == rows[i].len && !memcmp(rows[k].p, rows[i].p, rows[i].len);
        if (dup) continue;
        if (w + rows[i].len + 1 >= out_n) { fits = false; break; }
        memcpy(out + w, rows[i].p, rows[i].len);
        w += rows[i].len;
        out[w++] = '\n';
    }
    free(rows);
    if (!fits) { out[0] = 0; return -1; }
    out[w] = 0;
    return (long)w;
}

bool log_store_format_allowed(bool session_open, uint32_t rows)
{
    return !session_open || rows == 0;
}

bool log_store_clock_due(int64_t now_ms, int64_t last_ms, bool force)
{
    return force || last_ms < 0 || now_ms - last_ms >= LOG_STORE_CLOCK_PERIOD_MS || now_ms < last_ms;
}

int64_t log_store_clock_pick(int64_t nand_epoch_s, int64_t flash_epoch_s)
{
    if (nand_epoch_s < 0) nand_epoch_s = 0;
    if (flash_epoch_s < 0) flash_epoch_s = 0;
    return nand_epoch_s > flash_epoch_s ? nand_epoch_s : flash_epoch_s;
}
