/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the PodcastIndex key and secret.
 */
#include "podcasts/pod_cred.h"
#include "podcasts/pod_api.h"
#include "podcasts/pod_json.h"
#include "podcasts/pod_text.h"

#include "hal/sdcard_path.h"

/* POD_CRED_DEFAULT_KEY and POD_CRED_DEFAULT_SECRET, when the build had a
 * .env with them; generated into the build directory by credentials.cmake.
 * A build that makes none, the browser simulator's, has no key of its own. */
#if defined(__has_include)
#if __has_include("pod_cred_default.h")
#include "pod_cred_default.h"
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR  THISTLE_SDCARD "/config"
#define CONFIG_PATH CONFIG_DIR "/podcasts.json"
#define CONFIG_MAX  1024

/* Reads the card's file: its key and secret, true when it has both, and its
 * API URL into `url` when it gives one (and `url` is not NULL). */
static bool read_card(char *key, size_t key_cap, char *secret, size_t secret_cap,
                      char *url, size_t url_cap)
{
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) return false;
    char text[CONFIG_MAX];
    size_t len = fread(text, 1, sizeof text, f);
    fclose(f);

    char k[POD_CRED_KEY_MAX] = "", s[POD_CRED_SECRET_MAX] = "", name[24];
    pod_json_t j;
    pod_json_init(&j, text, len);
    if (pod_json_object(&j)) {
        while (pod_json_key(&j, name, sizeof name)) {
            if (strcmp(name, "api_key") == 0) pod_json_string(&j, k, sizeof k);
            else if (strcmp(name, "api_secret") == 0) pod_json_string(&j, s, sizeof s);
            else if (strcmp(name, "api_url") == 0 && url) pod_json_string(&j, url, url_cap);
            else pod_json_skip(&j);
        }
    }
    memset(text, 0, sizeof text);
    bool ok = !j.error && k[0] && s[0];
    if (ok && key) {
        pod_text_copy(key, key_cap, k);
        pod_text_copy(secret, secret_cap, s);
    }
    memset(s, 0, sizeof s);
    return ok;
}

void pod_cred_api_base(char *out, size_t cap)
{
    out[0] = '\0';
    read_card(NULL, 0, NULL, 0, out, cap);
    size_t n = strlen(out);
    while (n && out[n - 1] == '/') out[--n] = '\0';
    if (!n) pod_text_copy(out, cap, POD_API_BASE);
}

bool pod_cred_on_card(void)
{
    char k[POD_CRED_KEY_MAX], s[POD_CRED_SECRET_MAX];
    bool ok = read_card(k, sizeof k, s, sizeof s, NULL, 0);
    memset(s, 0, sizeof s);
    return ok;
}

bool pod_cred_built_in(void)
{
#if defined(POD_CRED_DEFAULT_KEY) && defined(POD_CRED_DEFAULT_SECRET)
    return POD_CRED_DEFAULT_KEY[0] && POD_CRED_DEFAULT_SECRET[0];
#else
    return false;
#endif
}

bool pod_cred_get(char *key, size_t key_cap, char *secret, size_t secret_cap)
{
    if (read_card(key, key_cap, secret, secret_cap, NULL, 0)) return true;
#if defined(POD_CRED_DEFAULT_KEY) && defined(POD_CRED_DEFAULT_SECRET)
    if (pod_cred_built_in()) {
        pod_text_copy(key, key_cap, POD_CRED_DEFAULT_KEY);
        pod_text_copy(secret, secret_cap, POD_CRED_DEFAULT_SECRET);
        return true;
    }
#endif
    return false;
}

/* Whether `s` can go into the JSON as it is: the API's keys and secrets are
 * printable ASCII, and none has a quote or a backslash. */
static bool plain(const char *s)
{
    for (; *s; s++) {
        if (*s < 0x20 || *s > 0x7e || *s == '"' || *s == '\\') return false;
    }
    return true;
}

bool pod_cred_save(const char *key, const char *secret)
{
    if (!key[0] && !secret[0]) {
        return remove(CONFIG_PATH) == 0 || errno == ENOENT;
    }
    if (!key[0] || !secret[0] || !plain(key) || !plain(secret)) return false;
    mkdir(CONFIG_DIR, 0755);

    /* As the Assistant's: written aside, then put in the old one's place. */
    const char *tmp = CONFIG_PATH ".tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    char url[192] = "";
    read_card(NULL, 0, NULL, 0, url, sizeof url);
    int n = url[0] && plain(url)
                ? fprintf(f, "{\"api_key\": \"%s\", \"api_secret\": \"%s\", \"api_url\": \"%s\"}\n",
                          key, secret, url)
                : fprintf(f, "{\"api_key\": \"%s\", \"api_secret\": \"%s\"}\n", key, secret);
    bool ok = n > 0 && fclose(f) == 0;
    if (ok) {
        remove(CONFIG_PATH);
        ok = rename(tmp, CONFIG_PATH) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}
