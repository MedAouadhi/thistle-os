/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — where the Hermes app finds the bridge. See hermes_config.h.
 */
#include "hermes/hermes_config.h"
#include "hermes/hermes_proto.h"

#include "podcasts/pod_json.h"

#include "hal/sdcard_path.h"

/* HERMES_URL_DEFAULT, when the build had a .env with one; generated into the
 * build directory by bridge_url.cmake. */
#if defined(__has_include)
#if __has_include("hermes_url_default.h")
#include "hermes_url_default.h"
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR  THISTLE_SDCARD "/config"
#define CONFIG_PATH CONFIG_DIR "/hermes.json"

static bool read_card(char *out, size_t cap)
{
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) return false;
    char text[512];
    size_t len = fread(text, 1, sizeof text, f);
    fclose(f);

    out[0] = '\0';
    char key[24];
    pod_json_t j;
    pod_json_init(&j, text, len);
    if (pod_json_object(&j)) {
        while (pod_json_key(&j, key, sizeof key)) {
            if (strcmp(key, "bridge_url") == 0) pod_json_string(&j, out, cap);
            else pod_json_skip(&j);
        }
    }
    return !j.error && out[0];
}

const char *hermes_config_built_in(void)
{
#ifdef HERMES_URL_DEFAULT
    return HERMES_URL_DEFAULT;
#else
    return "";
#endif
}

bool hermes_config_url(char *out, size_t cap)
{
    if (read_card(out, cap)) return true;
    snprintf(out, cap, "%s", hermes_config_built_in());
    return out[0] != '\0';
}

bool hermes_config_save(const char *url)
{
    if (!url[0]) return remove(CONFIG_PATH) == 0 || errno == ENOENT;
    char quoted[256];
    if (hermes_json_string(quoted, sizeof quoted, url) < 0) return false;
    mkdir(CONFIG_DIR, 0755);

    /* Written aside, then put in the old one's place. */
    const char *tmp = CONFIG_PATH ".tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fprintf(f, "{\"bridge_url\": %s}\n", quoted) > 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(CONFIG_PATH);
        ok = rename(tmp, CONFIG_PATH) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}
