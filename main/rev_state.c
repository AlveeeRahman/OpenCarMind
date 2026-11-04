#include "rev_state.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAGIC   0x31564552u              /* "REV1" */
#define VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version, nf;
    uint32_t prior_id;
    uint32_t fuel_size, phys_size, pick_size, cal_size;
} hdr_t;

typedef struct { uint32_t commits, resets, kept, failed, learned; } fuel_counts_t;

#define FUEL_SIZE ((uint32_t)(sizeof(float) * (REV_NF + REV_NF * REV_NF + REV_NF) + sizeof(fuel_counts_t)))

uint32_t rev_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool put(FILE *f, uint32_t *crc, const void *p, size_t n)
{
    *crc = rev_crc32(*crc, p, n);
    return fwrite(p, 1, n, f) == n;
}

bool rev_state_save(const char *path, const rev_fuel_t *fuel, const rev_phys_t *phys, const rev_pick_t *pick,
                    const rev_cal_t *cal, uint32_t prior_id)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    hdr_t h = {MAGIC, VERSION, REV_NF, prior_id, fuel ? FUEL_SIZE : 0, sizeof *phys, sizeof *pick, sizeof *cal};
    uint32_t crc = 0;
    bool ok = put(f, &crc, &h, sizeof h);
    if (fuel) {
        fuel_counts_t c = {fuel->commits, fuel->resets, fuel->kept, fuel->failed, fuel->learned};
        ok = ok && put(f, &crc, fuel->w[fuel->live], sizeof fuel->w[0]) && put(f, &crc, fuel->A, sizeof fuel->A) &&
             put(f, &crc, fuel->b, sizeof fuel->b) && put(f, &crc, &c, sizeof c);
    }
    ok = ok && put(f, &crc, phys, sizeof *phys) && put(f, &crc, pick, sizeof *pick) && put(f, &crc, cal, sizeof *cal);
    ok = ok && fwrite(&crc, sizeof crc, 1, f) == 1;
    ok = fflush(f) == 0 && ok;
    fsync(fileno(f));
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

/* Header and CRC of one file; the file is left positioned after the header. */
static FILE *open_checked(const char *path, hdr_t *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fread(h, sizeof *h, 1, f) != 1 || h->magic != MAGIC || h->version != VERSION || h->nf != REV_NF ||
        (h->fuel_size != 0 && h->fuel_size != FUEL_SIZE) || h->phys_size != sizeof(rev_phys_t) ||
        h->pick_size != sizeof(rev_pick_t) || h->cal_size != sizeof(rev_cal_t)) { fclose(f); return NULL; }
    uint32_t crc = rev_crc32(0, h, sizeof *h);
    long left = (long)h->fuel_size + h->phys_size + h->pick_size + h->cal_size;
    uint8_t buf[256];
    while (left > 0) {
        size_t want = left < (long)sizeof buf ? (size_t)left : sizeof buf;
        if (fread(buf, 1, want, f) != want) { fclose(f); return NULL; }
        crc = rev_crc32(crc, buf, want);
        left -= (long)want;
    }
    uint32_t stored;
    if (fread(&stored, sizeof stored, 1, f) != 1 || stored != crc || fgetc(f) != EOF) { fclose(f); return NULL; }
    fseek(f, sizeof *h, SEEK_SET);
    return f;
}

static bool finite_f(const float *v, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!isfinite(v[i])) return false;
    return true;
}

static bool phys_ok(const rev_phys_t *p)
{
    if (!isfinite(p->abs.sxx) || !isfinite(p->abs.sxy) || !isfinite(p->abs.k)) return false;
    for (int i = 0; i < REV_PHYS_BINS; i++) if (!isfinite(p->calc[i].sxx) || !isfinite(p->calc[i].sxy) || !isfinite(p->calc[i].k)) return false;
    return true;
}

static bool cal_ok(const rev_cal_t *c)
{
    return isfinite(c->c) && c->c >= REV_CAL_MIN && c->c <= REV_CAL_MAX && c->ledger_n >= 0 && c->ledger_n <= REV_CAL_SESSIONS &&
           c->n_fills >= 0 && c->n_fills <= REV_CAL_FILLUPS && c->n_iv >= 0 && c->n_iv <= REV_CAL_FILLUPS &&
           isfinite(c->life_l) && isfinite(c->life_km);
}

rev_load_t rev_state_load(const char *path, rev_fuel_t *fuel, rev_phys_t *phys, rev_pick_t *pick, rev_cal_t *cal,
                          uint32_t prior_id)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    hdr_t h;
    rev_load_t how = REV_LOAD_OK;
    FILE *f = open_checked(path, &h);
    if (!f) { f = open_checked(tmp, &h); how = REV_LOAD_TMP; }
    if (!f) return REV_LOAD_NONE;
    bool same_model = h.prior_id == prior_id && h.fuel_size == FUEL_SIZE;
    bool ok = true;
    if (h.fuel_size) {
        if (fuel && same_model) {
            fuel_counts_t c;
            float *live = fuel->w[0];
            ok = fread(live, sizeof fuel->w[0], 1, f) == 1 && fread(fuel->A, sizeof fuel->A, 1, f) == 1 &&
                 fread(fuel->b, sizeof fuel->b, 1, f) == 1 && fread(&c, sizeof c, 1, f) == 1;
            if (ok && finite_f(live, REV_NF) && finite_f(&fuel->A[0][0], REV_NF * REV_NF) && finite_f(fuel->b, REV_NF)) {
                memcpy(fuel->w[1], live, sizeof fuel->w[1]);
                fuel->live = 0;
                fuel->ring_head = fuel->ring_n = 0;
                fuel->n = 0;
                fuel->commits = c.commits; fuel->resets = c.resets; fuel->kept = c.kept; fuel->failed = c.failed; fuel->learned = c.learned;
            } else {
                rev_fuel_reset(fuel);                                 /* damaged in a way the CRC cannot see: back to the prior */
            }
        } else {
            fseek(f, (long)h.fuel_size, SEEK_CUR);
        }
    }
    rev_phys_t p; rev_pick_t k; rev_cal_t c;
    ok = fread(&p, sizeof p, 1, f) == 1 && fread(&k, sizeof k, 1, f) == 1 && fread(&c, sizeof c, 1, f) == 1;
    fclose(f);
    if (!ok) return REV_LOAD_NONE;
    if (phys_ok(&p)) { *phys = p; rev_phys_new_drive(phys); }
    if (finite_f(k.e, 3)) *pick = k;
    if (cal_ok(&c)) *cal = c;
    if (!same_model && h.fuel_size) return REV_LOAD_OTHER_MODEL;
    return how;
}
