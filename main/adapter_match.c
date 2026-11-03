#include "adapter_match.h"
#include <ctype.h>
#include <string.h>

static bool starts_with_nocase(const char *name, size_t len, const char *prefix, size_t plen)
{
    if (plen == 0 || len < plen) return false;
    for (size_t i = 0; i < plen; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

bool adapter_name_matches(const char *name, size_t len, const char *prefixes)
{
    if (!name || !prefixes) return false;
    const char *p = prefixes;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *start = p;
        while (n && isspace((unsigned char)*start)) { start++; n--; }
        while (n && isspace((unsigned char)start[n - 1])) n--;
        if (starts_with_nocase(name, len, start, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
