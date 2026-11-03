/* Decides whether a Bluetooth LE advertiser looks like an OBD-II adapter, from its name. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* True when `name` (not NUL-terminated, `len` bytes) starts with any entry of the comma-separated list, ignoring case.
   Entries are trimmed and empty entries are skipped. */
bool adapter_name_matches(const char *name, size_t len, const char *prefixes);
