/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the SoC side of the tailnet, for the tailnet manager
 *
 * Every call into MicroLink, the Tailscale client, is here, so that
 * tailnet_manager.rs keeps to deciding and the simulators need none of it.
 * MicroLink is the fork pinned in components/kernel/idf_component.yml; it
 * runs its own four tasks and adds a WireGuard netif that lwIP routes
 * 100.64.0.0/10 through, so any socket reaches a tailnet address.
 *
 * - Starting and stopping hold s_lock for as long as they take: stopping
 *   waits for the client's tasks, up to 30 s. The kernel worker's poll only
 *   tries the lock, and says "busy" while it is held, so the worker never
 *   waits on it; a name lookup waits 50 ms at most.
 *
 * - A client whose stop timed out may still have tasks running, and is
 *   kept as s_zombie rather than freed under them or forgotten: nothing
 *   starts, and nothing erases the identity, until a second stop of it
 *   has found its tasks gone.
 *
 * - The node's identity (its machine, node and disco keys) is MicroLink's,
 *   in its NVS namespaces "microlink" and "ml_peers". Whether this device
 *   has ever joined is kept beside it, in "tailnet".
 *
 * - lwip_hook_netconn_external_resolve() answers getaddrinfo() for the
 *   tailnet's names, "homelab" or "homelab.tail1234.ts.net", from the
 *   client's peer map (CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM).
 *
 * Off the S3 there is no client (THISTLE_HAS_TAILNET unset), and each call
 * answers as a board without one.
 */

#include "esp_err.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t ip;          /* host byte order */
    char     name[64];    /* "homelab.tail1234.ts.net" */
    bool     direct;      /* reached without a DERP relay */
} tailnet_platform_peer_t;

/* What the client is doing, read in one go (tailnet_platform_poll) */
typedef struct {
    int32_t  state;          /* microlink_state_t */
    int32_t  error;          /* microlink_error_t */
    bool     registered;     /* the control plane has accepted the node */
    uint32_t ip;             /* host byte order; 0 until connected */
    int32_t  peer_count;     /* all the client knows; the array holds up to max */
    char     auth_url[192];  /* while a sign-in waits */
    char     error_text[128];
} tailnet_platform_poll_t;

#define POLL_RUNNING      0
#define POLL_NOT_RUNNING  (-1)
#define POLL_BUSY         (-2)

#ifdef THISTLE_HAS_TAILNET

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "microlink.h"
#include "nvs.h"

static const char *TAG = "tailnet";

#define NVS_NAMESPACE "tailnet"
#define NVS_JOINED    "joined"
/* How long a name lookup waits for the lock before going on to DNS */
#define RESOLVE_WAIT_MS 50

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_init = portMUX_INITIALIZER_UNLOCKED;

static microlink_t *s_ml;
static microlink_t *s_zombie;
/* The client keeps a pointer to this, not a copy. */
static char s_hostname[64];
static bool s_joined_known;
static bool s_joined;

static SemaphoreHandle_t lock(void)
{
    taskENTER_CRITICAL(&s_lock_init);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    taskEXIT_CRITICAL(&s_lock_init);
    return s_lock;
}

static void joined_store(bool joined)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (joined) {
        nvs_set_u8(nvs, NVS_JOINED, 1);
    } else {
        nvs_erase_key(nvs, NVS_JOINED);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    s_joined = joined;
    s_joined_known = true;
}

/* A client whose stop timed out: stop it again, waiting for its tasks once
 * more, and free it if they have gone. Under the lock. */
static bool bury_zombie(void)
{
    if (!s_zombie) {
        return true;
    }
    if (microlink_stop(s_zombie) != ESP_OK) {
        ESP_LOGE(TAG, "a client that did not stop still has tasks running");
        return false;
    }
    microlink_destroy(s_zombie);
    s_zombie = NULL;
    ESP_LOGI(TAG, "a client that did not stop has now");
    return true;
}

bool tailnet_platform_available(void)
{
    return true;
}

void tailnet_platform_default_name(char *buf, size_t len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "thistle-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* MicroLink narrates: its WireGuard task logs every 400 ms, its DERP task a
 * heartbeat warning every 5 s, and both packet paths at INFO. What the
 * kernel needs is the control plane's, and the tailnet manager's own. */
static void quiet_the_client(void)
{
    static const char *const chatty[] = {
        "ml_wg_mgr", "ml_net_io", "ml_stun", "ml_peer_nvs", "ml_tcp", "ml_noise", "ml_h2",
    };
    for (size_t i = 0; i < sizeof chatty / sizeof chatty[0]; i++) {
        esp_log_level_set(chatty[i], ESP_LOG_WARN);
    }
    esp_log_level_set("ml_derp", ESP_LOG_ERROR);
}

/* Without an auth key: a device that has not joined is given a URL to sign
 * in at (ML_STATE_NEEDS_LOGIN), and one that has rejoins with its own
 * identity. */
esp_err_t tailnet_platform_start(const char *hostname)
{
    quiet_the_client();
    xSemaphoreTake(lock(), portMAX_DELAY);
    if (s_ml || !bury_zombie()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_hostname, hostname ? hostname : "", sizeof(s_hostname));

    microlink_config_t config = {
        .device_name = s_hostname,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
    };
    microlink_t *ml = microlink_init(&config);
    if (!ml) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        if (microlink_stop(ml) == ESP_OK) {
            microlink_destroy(ml);
        } else {
            s_zombie = ml;
        }
        xSemaphoreGive(s_lock);
        return err;
    }
    s_ml = ml;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "started as %s", s_hostname);
    return ESP_OK;
}

esp_err_t tailnet_platform_stop(void)
{
    xSemaphoreTake(lock(), portMAX_DELAY);
    microlink_t *ml = s_ml;
    s_ml = NULL;
    esp_err_t err = ESP_OK;
    if (ml) {
        /* Waits for the client's tasks. If one does not exit, the client
         * is kept, not freed under it, and stopped again before the next
         * start. */
        err = microlink_stop(ml);
        if (err == ESP_OK) {
            microlink_destroy(ml);
        } else {
            ESP_LOGE(TAG, "the client did not stop (%s)", esp_err_to_name(err));
            s_zombie = ml;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

int tailnet_platform_poll(tailnet_platform_poll_t *out, tailnet_platform_peer_t *peers,
                          int max_peers)
{
    if (!out) {
        return POLL_NOT_RUNNING;
    }
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(lock(), 0) != pdTRUE) {
        return POLL_BUSY;
    }
    microlink_t *ml = s_ml;
    if (!ml) {
        xSemaphoreGive(s_lock);
        return POLL_NOT_RUNNING;
    }
    out->state = (int32_t)microlink_get_state(ml);
    out->error = (int32_t)microlink_get_error(ml, out->error_text, sizeof(out->error_text));
    out->registered = microlink_is_registered(ml);
    out->ip = microlink_get_vpn_ip(ml);
    microlink_get_auth_url(ml, out->auth_url, sizeof(out->auth_url));
    out->peer_count = microlink_get_peer_count(ml);
    for (int i = 0; peers && i < max_peers && i < out->peer_count; i++) {
        microlink_peer_info_t info;
        memset(&info, 0, sizeof(info));
        memset(&peers[i], 0, sizeof(peers[i]));
        if (microlink_get_peer_info(ml, i, &info) == ESP_OK) {
            peers[i].ip = info.vpn_ip;
            strlcpy(peers[i].name, info.hostname, sizeof(peers[i].name));
            peers[i].direct = info.direct_path;
        }
    }
    xSemaphoreGive(s_lock);
    if (out->registered && (!s_joined_known || !s_joined)) {
        joined_store(true);
    }
    return POLL_RUNNING;
}

bool tailnet_platform_joined(void)
{
    if (!s_joined_known) {
        nvs_handle_t nvs;
        uint8_t v = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u8(nvs, NVS_JOINED, &v);
            nvs_close(nvs);
        }
        s_joined = v != 0;
        s_joined_known = true;
    }
    return s_joined;
}

uint32_t tailnet_platform_resolve(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    if (xSemaphoreTake(lock(), pdMS_TO_TICKS(RESOLVE_WAIT_MS)) != pdTRUE) {
        return 0;
    }
    uint32_t ip = s_ml && microlink_is_connected(s_ml) ? microlink_resolve(s_ml, name) : 0;
    xSemaphoreGive(s_lock);
    return ip;
}

esp_err_t tailnet_platform_forget(void)
{
    xSemaphoreTake(lock(), portMAX_DELAY);
    if (s_ml || !bury_zombie()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = microlink_factory_reset();
    joined_store(false);
    xSemaphoreGive(s_lock);
    return err;
}

/* lwIP asks this before DNS for every name getaddrinfo() is given. A name
 * the tailnet holds is answered from the peer map; anything else, 0, goes
 * on to DNS. MicroLink matches a whole name or its first label, so
 * "homelab" finds "homelab.tail1234.ts.net" but "example.com" finds
 * nothing. */
int lwip_hook_netconn_external_resolve(const char *name, ip_addr_t *addr, u8_t addrtype,
                                       err_t *err)
{
    if (addrtype == NETCONN_DNS_IPV6 || !name || !addr || !err) {
        return 0;
    }
    uint32_t ip = tailnet_platform_resolve(name);
    if (!ip) {
        return 0;
    }
    ip_addr_set_ip4_u32_val(*addr, lwip_htonl(ip));
    *err = ERR_OK;
    return 1;
}

#else /* !THISTLE_HAS_TAILNET */

bool tailnet_platform_available(void) { return false; }
void tailnet_platform_default_name(char *buf, size_t len)
{
    if (buf && len) {
        strlcpy(buf, "thistle", len);
    }
}
esp_err_t tailnet_platform_start(const char *hostname)
{
    (void)hostname;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t tailnet_platform_stop(void) { return ESP_OK; }
int tailnet_platform_poll(tailnet_platform_poll_t *out, tailnet_platform_peer_t *peers,
                          int max_peers)
{
    (void)peers;
    (void)max_peers;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return POLL_NOT_RUNNING;
}
bool tailnet_platform_joined(void) { return false; }
uint32_t tailnet_platform_resolve(const char *name)
{
    (void)name;
    return 0;
}
esp_err_t tailnet_platform_forget(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* THISTLE_HAS_TAILNET */
