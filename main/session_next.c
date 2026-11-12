/* Session numbers after an NVS erase: see session_next.h. */
#include <dirent.h>
#include <stddef.h>
#include "session_next.h"

uint32_t session_id_of_name(const char *n)
{
    if (!n || (n[0] != 's' && n[0] != 'S')) return 0;          /* FatFs may hand back an 8.3 name in upper case */
    uint32_t id = 0;
    for (int i = 1; i <= 6; i++) {
        if (n[i] < '0' || n[i] > '9') return 0;
        id = id * 10 + (uint32_t)(n[i] - '0');
    }
    return n[7] == '.' ? id : 0;
}

uint32_t session_highest_in_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint32_t hi = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t id = session_id_of_name(e->d_name);
        if (id > hi) hi = id;
    }
    closedir(d);
    return hi;
}

uint32_t session_next_id(uint32_t nvs_last, uint32_t highest_on_storage)
{
    return (nvs_last > highest_on_storage ? nvs_last : highest_on_storage) + 1;
}
