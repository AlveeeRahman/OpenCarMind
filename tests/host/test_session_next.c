/* session_next.c: the session number after a re-flash erased the NVS counter (see main/session_next.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "session_next.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static char dir[64];
static void touch(const char *name)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs("x", f); fclose(f); }
}

int main(void)
{
    CHECK(session_id_of_name("s000001.csv") == 1);
    CHECK(session_id_of_name("s000062.sys.txt") == 62);
    CHECK(session_id_of_name("s123456.ml.csv") == 123456);
    CHECK(session_id_of_name("s000004.json.tmp") == 4);
    CHECK(session_id_of_name("S000007.CSV") == 7);
    CHECK(session_id_of_name("s00001.csv") == 0);            /* 5 digits */
    CHECK(session_id_of_name("s0000012.csv") == 0);          /* 7 digits */
    CHECK(session_id_of_name("s000001") == 0);               /* no suffix */
    CHECK(session_id_of_name("x000001.csv") == 0);
    CHECK(session_id_of_name("rev_state.bin") == 0);
    CHECK(session_id_of_name("") == 0);
    CHECK(session_id_of_name(NULL) == 0);

    CHECK(session_next_id(0, 0) == 1);                       /* new board */
    CHECK(session_next_id(9, 4) == 10);                      /* the counter is ahead: as before */
    CHECK(session_next_id(0, 62) == 63);                     /* NVS erased by a re-flash */
    CHECK(session_next_id(3, 62) == 63);                     /* NVS erased, then a few boots */
    CHECK(session_next_id(62, 62) == 63);

    snprintf(dir, sizeof dir, "/tmp/ocm_session_XXXXXX");
    if (!mkdtemp(dir)) { printf("FAIL mkdtemp\n"); return 1; }
    CHECK(session_highest_in_dir(dir) == 0);                 /* empty */
    char missing[96];
    snprintf(missing, sizeof missing, "%s/none", dir);
    CHECK(session_highest_in_dir(missing) == 0);             /* no such directory */
    /* after a re-flash: s000001..s000004 were written since, s000062 is from before it */
    const char *names[] = {"s000004.csv", "s000001.sys.txt", "s000002.sys.txt", "s000062.sys.txt", "s000003.sys.txt",
                           "s000004.json", "wifi.example.csv", "s9.csv", "notes.txt"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) touch(names[i]);
    CHECK(session_highest_in_dir(dir) == 62);
    CHECK(session_next_id(4, session_highest_in_dir(dir)) == 63);
    touch("s000100.ml.csv");
    CHECK(session_highest_in_dir(dir) == 100);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) printf("note: could not remove %s\n", dir);
    printf("session_next: %d checks, %d failures\n", runs, fails);
    return fails != 0;
}
