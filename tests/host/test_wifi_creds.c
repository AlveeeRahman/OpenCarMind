/* wifi_creds.c on the host (the real file, copied to a scratch dir so its includes find the stubs in stubs/wifi_creds):
   the near-duplicate cleanup after a connect, wifi_creds_dedupe, when its argument points into the list itself. */
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "wifi_creds.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------- the NVS stub: one blob ---------- */
static unsigned char s_blob[1024];
static size_t s_blob_n;
static int s_have_blob;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n)
{
    (void)h; (void)key;
    if (n > sizeof s_blob) return ESP_FAIL;
    memcpy(s_blob, v, n); s_blob_n = n; s_have_blob = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n)
{
    (void)h; (void)key;
    if (!s_have_blob) return ESP_FAIL;
    if (*n < s_blob_n) return ESP_FAIL;
    memcpy(v, s_blob, s_blob_n); *n = s_blob_n;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void clear_all(void)
{
    while (wifi_creds_count()) wifi_creds_remove(wifi_creds_get(0)->ssid);
}

static int has(const char *ssid) { return wifi_creds_find(ssid) != NULL; }
static const char *pw_of(const char *ssid) { wifi_cred_t *c = wifi_creds_find(ssid); return c ? c->password : ""; }

int main(void)
{
    /* the reported case: saved "home ", "Home", "X"; "Home" connects and sync_once calls dedupe(net->ssid), a pointer
       into the list. Removing "home " slides "X" under that pointer; the old code then removed the real "Home". */
    clear_all();
    CHECK(wifi_creds_add("home ", "password1") && wifi_creds_add("Home", "password2") && wifi_creds_add("X", "password3"));
    CHECK(wifi_creds_count() == 3);
    wifi_creds_dedupe(wifi_creds_find("Home")->ssid);
    CHECK(wifi_creds_count() == 2);
    CHECK(has("Home") && has("X") && !has("home "));
    CHECK(!strcmp(pw_of("Home"), "password2"));   /* the network that connected keeps its password */

    /* the joined one last, near-duplicates before it */
    clear_all();
    CHECK(wifi_creds_add("H O M E", "password1") && wifi_creds_add("home", "password2") && wifi_creds_add("Home", "password3"));
    wifi_creds_dedupe(wifi_creds_find("Home")->ssid);
    CHECK(wifi_creds_count() == 1 && has("Home"));

    /* the joined one first: everything after it slides, the pointer itself never moves */
    clear_all();
    CHECK(wifi_creds_add("Cafe", "password1") && wifi_creds_add("cafe ", "password2") && wifi_creds_add("Shop", "password3"));
    wifi_creds_dedupe(wifi_creds_find("Cafe")->ssid);
    CHECK(wifi_creds_count() == 2 && has("Cafe") && has("Shop"));

    /* nothing near: nothing removed; a name that is not saved (a literal) works too */
    clear_all();
    CHECK(wifi_creds_add("Guest", "password1") && wifi_creds_add("Office", "password2"));
    wifi_creds_dedupe(wifi_creds_find("Guest")->ssid);
    CHECK(wifi_creds_count() == 2);
    wifi_creds_dedupe("guest ");
    CHECK(wifi_creds_count() == 1 && has("Office"));

    /* the list survives a reload from the NVS blob after a dedupe */
    clear_all();
    CHECK(wifi_creds_add("home ", "password1") && wifi_creds_add("Home", "password2") && wifi_creds_add("X", "password3"));
    wifi_creds_dedupe(wifi_creds_find("Home")->ssid);
    CHECK(s_blob_n == 2 * (33 + 65));

    printf("wifi_creds: %d checks, %d failed\n", runs, fails);
    return fails != 0;
}
