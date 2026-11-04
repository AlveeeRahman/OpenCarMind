/* Display units for distance, speed and fuel economy (metric, US or UK), decided once and kept.
 *
 * The board starts in metric. When it first joins a Wi-Fi network it reads the country code the access point broadcasts
 * (802.11d) and, if that names the US or the UK, switches to miles and mpg; the time zone set by the desktop app is the
 * fallback, and only the UK is unambiguous there. The first answer is stored and never re-detected, so units do not
 * change between drives. "units" in config.json ("metric", "us", "uk") overrides it; "auto" returns to the stored choice. */
#pragma once
#include "econ.h"

void    units_init(void);                         /* load the stored choice; call after nvs_flash_init */
units_t units_current(void);
void    units_set_override(int u);                /* from config.json: UNITS_* or -1 for auto */
void    units_detect(const char *country, const char *tz);   /* on Wi-Fi connect: decide once if nothing is stored */
