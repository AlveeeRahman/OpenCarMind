/* Decides whether a Bluetooth LE device is an OBD-II adapter and which of its characteristics carry the serial line.
   Pure C with no Bluetooth stack dependency, so the host tests cover it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GATT characteristic property bits (Bluetooth Core Spec Vol 3 Part G 3.3.1.1). */
#define ADAPTER_PROP_WRITE_NR  0x04
#define ADAPTER_PROP_WRITE     0x08
#define ADAPTER_PROP_NOTIFY    0x10
#define ADAPTER_PROP_INDICATE  0x20

/* A UUID as 16 bytes in the order it is written as text (most significant byte first). */
typedef struct { uint8_t b[16]; } adapter_uuid_t;

/* One characteristic as discovery saw it. props should have NOTIFY/INDICATE cleared when the
   characteristic has no CCCD, since such a characteristic cannot be subscribed to. */
typedef struct {
    adapter_uuid_t svc;
    adapter_uuid_t chr;
    uint8_t        props;
} adapter_chr_t;

/* True when `name` (not NUL-terminated, `len` bytes) starts with any entry of the comma-separated list, ignoring case.
   Entries are trimmed and empty entries are skipped. */
bool adapter_name_matches(const char *name, size_t len, const char *prefixes);

/* True when the name contains a word that OBD-II adapters put in their names ("obd", "elm327", "vgate", "vlink",
   "veepeak", "carista", "kiwi", ...), anywhere and in any case. */
bool adapter_name_looks_obd(const char *name, size_t len);

/* Parses 4 or 8 hex digits (on the Bluetooth base UUID) or a full 32-digit UUID; dashes and spaces are ignored. */
bool adapter_uuid_parse(const char *s, adapter_uuid_t *out);
adapter_uuid_t adapter_uuid16(uint16_t v);
bool adapter_uuid_eq(const adapter_uuid_t *a, const adapter_uuid_t *b);

/* True when an advertised service UUID is one that OBD adapters use for their serial line. FFE0 (the HM-10 module's)
   and the Nordic UART service are left out because many other gadgets carry them; those still match by name. */
bool adapter_service_is_obd(const adapter_uuid_t *u);

/* Picks the notify and write characteristics. Known profiles are tried first (the configured one, then the table),
   then the first non-standard service with a subscribable and a writable characteristic. Returns false when the
   device has no serial line. *profile is the matched table index, or -1 for the generic fallback. */
bool adapter_choose(const adapter_chr_t *c, size_t n, const char *cfg_svc, const char *cfg_ntf, const char *cfg_wr,
                    int *notify_idx, int *write_idx, int *profile);

/* Name of a table profile for logs ("FFF0", "Vgate E7810A71", ...); "configured" for -2, "generic" for -1. */
const char *adapter_profile_name(int profile);
