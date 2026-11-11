/* log_store.c: which store a file goes to, the session counter over two stores, the move rules and the drives.csv merge
   (see main/log_store.h). Plus a simulated move over two directories with resets at every step. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "log_store.h"
#include "session_next.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------- a two-directory model of the move (log_move.c's order of steps), with a reset after any step ---------- */
static char dflash[64], dnand[64];

static void put(const char *dir, const char *name, const char *text)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static long get(const char *dir, const char *name, char *buf, size_t n)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return (long)r;
}

static uint32_t crc(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static void wipe(const char *dir)
{
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[160];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
}

/* one move pass; stops (as a reset would) after `budget` file-system steps; returns the steps it used */
static int move_pass(int budget)
{
    int steps = 0;
#define STEP() do { if (++steps > budget) return steps; } while (0)
    DIR *d = opendir(dnand);                              /* copies a reset cut short */
    for (struct dirent *e; d && (e = readdir(d));)
        if (log_store_is_move_tmp(e->d_name)) { STEP(); char p[160]; snprintf(p, sizeof p, "%s/%s", dnand, e->d_name); unlink(p); }
    if (d) closedir(d);
    char names[32][24];
    int n = 0;
    d = opendir(dflash);
    for (struct dirent *e; d && (e = readdir(d)) && n < 32;)
        if (log_store_is_drive_file(e->d_name) && !log_store_is_move_tmp(e->d_name)) snprintf(names[n++], 24, "%s", e->d_name);
    if (d) closedir(d);
    for (int i = 0; i < n; i++) {
        char a[512], b[512], from[160], to[160], tmp[170];
        snprintf(from, sizeof from, "%s/%s", dflash, names[i]);
        snprintf(to, sizeof to, "%s/%s", dnand, names[i]);
        snprintf(tmp, sizeof tmp, "%s" LOG_STORE_MOVE_SUFFIX, to);
        long fl = get(dflash, names[i], a, sizeof a), nl = get(dnand, names[i], b, sizeof b);
        ls_move_t m = log_store_move_step(nl >= 0, nl < 0 ? 0 : (uint64_t)nl, nl < 0 ? 0 : crc(b), (uint64_t)fl, crc(a));
        if (m == LS_DROP_FLASH) { STEP(); unlink(from); }
        else if (m == LS_COPY) {
            STEP();
            FILE *f = fopen(tmp, "wb");
            size_t half = strlen(a) / 2;
            fwrite(a, 1, half, f);
            fclose(f);
            STEP();                                       /* a reset here leaves a half copy */
            f = fopen(tmp, "ab");
            fputs(a + half, f);
            fclose(f);
            char t[512];
            FILE *g = fopen(tmp, "rb");
            size_t r = fread(t, 1, sizeof t - 1, g);
            fclose(g);
            t[r] = 0;
            if (!log_store_copy_verified((uint64_t)fl, crc(a), r, crc(t))) { unlink(tmp); continue; }
            STEP();
            rename(tmp, to);
            STEP();
            unlink(from);
        }
    }
    return 0;
#undef STEP
}

static int count_files(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static void check_move_with_resets(void)
{
    static const char *const NAMES[] = {"s000001.csv", "s000001.json", "s000002.csv", "s000002.ml.csv", "s000002.sum.json"};
    for (int budget = 0; budget < 40; budget++) {
        wipe(dflash);
        wipe(dnand);
        for (int i = 0; i < 5; i++) { char t[64]; snprintf(t, sizeof t, "content of %s, %d\n", NAMES[i], i * 7); put(dflash, NAMES[i], t); }
        put(dflash, "notes.txt", "a state file, never moved");
        int r = move_pass(budget);                        /* cut by a reset after `budget` steps */
        /* after the cut: every drive file is on one store or both, never lost or changed */
        for (int i = 0; i < 5; i++) {
            char t[64], a[512], b[512];
            snprintf(t, sizeof t, "content of %s, %d\n", NAMES[i], i * 7);
            long fl = get(dflash, NAMES[i], a, sizeof a), nl = get(dnand, NAMES[i], b, sizeof b);
            CHECK(fl >= 0 || nl >= 0);
            if (fl >= 0) CHECK(!strcmp(a, t));
            if (nl >= 0) CHECK(!strcmp(b, t));
        }
        move_pass(1000);                                  /* the next boot finishes it */
        for (int i = 0; i < 5; i++) {
            char t[64], b[512];
            snprintf(t, sizeof t, "content of %s, %d\n", NAMES[i], i * 7);
            CHECK(get(dnand, NAMES[i], b, sizeof b) >= 0 && !strcmp(b, t));
        }
        CHECK(count_files(dnand) == 5);                   /* no .mv left */
        CHECK(count_files(dflash) == 1);                  /* only the state file */
        if (!r && budget > 0) break;                      /* the pass finished inside the budget: every cut point seen */
    }
}

int main(void)
{
    /* routing */
    CHECK(log_store_is_drive_file("s000001.csv"));
    CHECK(log_store_is_drive_file("s000062.sum.json"));
    CHECK(log_store_is_drive_file("s000004.json.tmp"));
    CHECK(log_store_is_drive_file("s000062.sys.txt"));
    CHECK(log_store_is_drive_file("drives.csv"));
    CHECK(log_store_is_drive_file("DRIVES.CSV"));
    CHECK(!log_store_is_drive_file("rev_state.bin"));
    CHECK(!log_store_is_drive_file("clock.txt"));
    CHECK(!log_store_is_drive_file("config.json"));
    CHECK(!log_store_is_drive_file("wifi.csv"));
    CHECK(!log_store_is_drive_file("patterns.json"));
    CHECK(!log_store_is_drive_file("custom_pids.csv"));
    CHECK(!log_store_is_drive_file(NULL));
    CHECK(log_store_on_nand("s000001.csv", true));
    CHECK(!log_store_on_nand("s000001.csv", false));      /* no NAND: drive logs stay on the flash FAT */
    CHECK(!log_store_on_nand("rev_state.bin", true));     /* state files never move */
    CHECK(!log_store_on_nand("clock.txt", true));

    /* move temporaries */
    CHECK(log_store_is_move_tmp("s000001.csv.mv"));
    CHECK(log_store_is_move_tmp("s000001.sum.json.mv"));
    CHECK(!log_store_is_move_tmp("s000001.csv"));
    CHECK(!log_store_is_move_tmp("drives.csv.mv"));       /* handled apart (log_move_drives_csv) */
    CHECK(!log_store_is_move_tmp(".mv"));
    CHECK(session_id_of_name("s000007.csv.mv") == 7);     /* counted by the session counter */

    /* counter over both stores */
    CHECK(log_store_next_session(0, 0, 0) == 1);
    CHECK(log_store_next_session(9, 4, 3) == 10);         /* NVS ahead */
    CHECK(log_store_next_session(0, 62, 10) == 63);       /* NVS erased, NAND ahead */
    CHECK(log_store_next_session(0, 5, 62) == 63);        /* NVS erased, old drives still on the flash FAT */
    CHECK(log_store_next_session(62, 62, 62) == 63);
    CHECK(log_store_next_session(70, 62, 65) == 71);

    /* move decisions */
    CHECK(log_store_move_step(false, 0, 0, 100, 0xAB) == LS_COPY);
    CHECK(log_store_move_step(true, 100, 0xAB, 100, 0xAB) == LS_DROP_FLASH);
    CHECK(log_store_move_step(true, 100, 0xAC, 100, 0xAB) == LS_CONFLICT);
    CHECK(log_store_move_step(true, 99, 0xAB, 100, 0xAB) == LS_CONFLICT);
    CHECK(log_store_move_step(true, 0, 0, 0, 0) == LS_DROP_FLASH);       /* two empty files */
    CHECK(log_store_copy_verified(100, 7, 100, 7));
    CHECK(!log_store_copy_verified(100, 7, 50, 7));       /* short copy (a reset or a full NAND) */
    CHECK(!log_store_copy_verified(100, 7, 100, 8));

    /* drives.csv merge */
    static const char H[] = "session,start_epoch_ms,duration_s,km,fuel_l,l_per_100km,avg_kph,max_kph,idle_pct,hard_brakes\n";
    char out[2048];
    char a[512], b[512];
    snprintf(a, sizeof a, "%s3,1,60,1.00,0.100,10.00,30.0,50.0,5.0,0\n1,1,60,1.00,0.100,10.00,30.0,50.0,5.0,0\n", H);
    snprintf(b, sizeof b, "%s2,1,60,2.00,0.200,10.00,30.0,50.0,5.0,1\n3,1,60,1.00,0.100,10.00,30.0,50.0,5.0,0\n", H);
    long n = log_store_merge_drives_csv(a, b, out, sizeof out);
    CHECK(n > 0 && (size_t)n == strlen(out));
    CHECK(!strncmp(out, H, strlen(H)));
    const char *r1 = out + strlen(H);
    CHECK(!strncmp(r1, "1,", 2));
    CHECK(strstr(r1, "\n2,") && strstr(r1, "\n3,"));
    CHECK(strstr(r1, "\n2,") < strstr(r1, "\n3,"));
    CHECK(strstr(strstr(r1, "\n3,") + 1, "\n3,") == NULL);   /* the row on both stores once */
    int lines = 0; for (const char *p = out; *p; p++) lines += *p == '\n';
    CHECK(lines == 4);                                    /* header + 1, 2, 3 */
    n = log_store_merge_drives_csv(a, NULL, out, sizeof out);
    CHECK(n > 0 && !strncmp(out, H, strlen(H)) && strstr(out, "\n1,") && strstr(out, "\n3,"));
    CHECK(strstr(out, "\n1,") < strstr(out, "\n3,"));     /* sorted */
    n = log_store_merge_drives_csv("", "", out, sizeof out);
    CHECK(n == 0 && out[0] == 0);
    n = log_store_merge_drives_csv("4,a\r\n", H, out, sizeof out);   /* CRLF, the header only in the second file */
    CHECK(n > 0 && !strncmp(out, H, strlen(H)) && !strcmp(out + strlen(H), "4,a\n"));
    n = log_store_merge_drives_csv(a, b, out, 40);        /* does not fit: nothing half written */
    CHECK(n == -1 && out[0] == 0);
    n = log_store_merge_drives_csv("x,y\nbroken line\n5,z\n", "", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "x,y\n5,z\nbroken line\n"));   /* a row with no number kept, at the end */

    /* the move over two real directories, with a reset after every step */
    snprintf(dflash, sizeof dflash, "/tmp/ocm_lsflash_XXXXXX");
    snprintf(dnand, sizeof dnand, "/tmp/ocm_lsnand_XXXXXX");
    if (!mkdtemp(dflash) || !mkdtemp(dnand)) { printf("FAIL mkdtemp\n"); return 1; }
    check_move_with_resets();
    /* a conflict keeps both copies */
    wipe(dflash); wipe(dnand);
    put(dflash, "s000009.csv", "old flash bytes\n");
    put(dnand, "s000009.csv", "different NAND bytes\n");
    move_pass(1000);
    char t[512];
    CHECK(get(dflash, "s000009.csv", t, sizeof t) > 0 && get(dnand, "s000009.csv", t, sizeof t) > 0);
    wipe(dflash); wipe(dnand);
    rmdir(dflash);
    rmdir(dnand);

    printf("log_store: %d checks, %d failed\n", runs, fails);
    return fails != 0;
}
