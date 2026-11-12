/* host renderer: which firmware state the stubs (fw_stubs.c) publish for the screen being checked */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    bool demo;                 /* realistic example values for the README and the manual (not worst case): a car
                                  connected and logging, R.E.V in use, no faults, the clock at 17:31 */
    bool idle;                 /* no car link: values show "--", the adapter searches */
    bool no_clock;             /* the clock was never set */
    bool no_drive;             /* no drive saved yet: with no_faults, Home shows ECONOMY + WAITING */
    bool no_faults;            /* no trouble codes: Home shows the economy card (ECONOMY + WAITING / NO SOURCE) */
    bool alarm;                /* a critical fault banner */
    const char *alarm_desc;    /* its text after the code, NULL = the fault engine's */
    bool pairing;              /* the desktop pairing code banner */
    const char *obd_pair;      /* the OBD adapter pairing sheet: the adapter's name, NULL = no question */
    int model;                 /* ml_model_id_t in use */
    uint8_t health, runlevel, bat_state;
} uishot_case_t;
extern uishot_case_t uishot_case;
/* the screens' time(NULL): 17:31 local in the demo case, the Mac's clock otherwise */
time_t uishot_time(time_t *t);
void uishot_fill(void);
