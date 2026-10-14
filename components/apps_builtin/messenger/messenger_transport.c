/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Messenger transport backend implementations
 *
 * Four backends are registered at init time:
 *
 *  MSG_TRANSPORT_LORA     — the kernel's MeshCore node: Public channel and direct messages
 *  MSG_TRANSPORT_SMS      — the kernel's SMS: a conversation per phone number
 *  MSG_TRANSPORT_BLE      — ble_manager relay stub (logs TODO, returns NOT_SUPPORTED)
 *  MSG_TRANSPORT_INTERNET — HTTP/WebSocket stub    (logs TODO, returns NOT_SUPPORTED)
 *
 * Adding a new backend: implement the five function pointers, fill a
 * static msg_transport_driver_t, and call messenger_register_transport()
 * from messenger_transport_init() below.
 */

#include "messenger/messenger_transport.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "thistle/ble_manager.h"
#include "thistle/mesh.h"
#include "thistle/sms.h"

static const char *TAG = "msg_transport";

/* ------------------------------------------------------------------ */
/* Registry storage                                                     */
/* ------------------------------------------------------------------ */

static const msg_transport_driver_t *s_registry[MSG_TRANSPORT_COUNT];
static int                           s_count;

/* ------------------------------------------------------------------ */
/* LoRa transport: MeshCore                                             */
/* ------------------------------------------------------------------ */

/*
 * The kernel runs a MeshCore node from boot and queues what it hears, so
 * nothing is lost while the messenger is closed; this hands it over when
 * polled. The transport's own conversation is the Public channel every
 * MeshCore node shares. Any other conversation is a direct message to a
 * contact, addressed by the start of its key.
 */

/* Messages handed over per poll: a burst waits for the next tick rather than
 * holding up the LVGL task. */
#define MESH_POLL_BUDGET 8

void messenger_mesh_address(const uint8_t *pub_key, char out[MSG_MESH_ADDR_LEN])
{
    for (int i = 0; i < MSG_MESH_KEY_PREFIX; i++) {
        snprintf(out + 2 * i, 3, "%02x", pub_key[i]);
    }
}

static bool mesh_parse_address(const char *addr, uint8_t prefix[MSG_MESH_KEY_PREFIX])
{
    if (!addr || strlen(addr) != MSG_MESH_KEY_PREFIX * 2) return false;
    for (int i = 0; i < MSG_MESH_KEY_PREFIX; i++) {
        unsigned int byte;
        if (sscanf(addr + 2 * i, "%2x", &byte) != 1) return false;
        prefix[i] = (uint8_t)byte;
    }
    return true;
}

static bool mesh_is_available(void)
{
    return rs_mesh_is_running();
}

static esp_err_t mesh_send(const char *dest, const char *text, uint32_t *out_ref)
{
    if (out_ref) *out_ref = 0;
    if (!text || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (!dest || dest[0] == '\0') {
        return rs_mesh_send_channel(MESH_PUBLIC_CHANNEL, text, out_ref);
    }

    uint8_t prefix[MSG_MESH_KEY_PREFIX];
    if (!mesh_parse_address(dest, prefix)) return ESP_ERR_INVALID_ARG;
    int idx = rs_mesh_find_contact_prefix(prefix, sizeof(prefix));
    if (idx < 0) return ESP_ERR_NOT_FOUND;

    mesh_contact_t contact;
    esp_err_t err = rs_mesh_get_contact(idx, &contact);
    if (err != ESP_OK) return err;
    return rs_mesh_send_direct(contact.pub_key, text, out_ref);
}

/* The node is always listening; there is nothing to start or stop. */
static esp_err_t mesh_start_receive(msg_rx_cb_t cb)
{
    (void)cb;
    return mesh_is_available() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void mesh_stop_receive(void)
{
}

static msg_status_t mesh_status(uint8_t delivery)
{
    switch (delivery) {
        case MESH_DELIVERY_SENT:      return MSG_STATUS_SENT;
        case MESH_DELIVERY_DELIVERED: return MSG_STATUS_DELIVERED;
        case MESH_DELIVERY_FAILED:    return MSG_STATUS_FAILED;
        default:                      return MSG_STATUS_NONE;
    }
}

static void mesh_poll(msg_rx_cb_t rx, msg_status_cb_t status)
{
    mesh_message_t m;
    for (int n = 0; n < MESH_POLL_BUDGET && rs_mesh_inbox_pop(&m); n++) {
        /* Only the Public channel is joined; anything else is not ours. */
        if (m.kind == MESH_MSG_CHANNEL && m.channel != MESH_PUBLIC_CHANNEL) continue;

        char conv[MSG_MESH_ADDR_LEN] = "";
        if (m.kind == MESH_MSG_DIRECT) messenger_mesh_address(m.sender_key, conv);

        char sender[sizeof(m.sender_name) + 1];
        memcpy(sender, m.sender_name, m.sender_name_len);
        sender[m.sender_name_len] = '\0';

        char text[sizeof(m.text) + 1];
        memcpy(text, m.text, m.text_len);
        text[m.text_len] = '\0';

        if (rx) rx(MSG_TRANSPORT_LORA, conv, sender[0] ? sender : "?", text);
    }

    uint32_t id;
    uint8_t delivery;
    while (rs_mesh_delivery_pop(&id, &delivery)) {
        if (status) status(MSG_TRANSPORT_LORA, id, mesh_status(delivery));
    }
}

static const msg_transport_driver_t s_lora_driver = {
    .type           = MSG_TRANSPORT_LORA,
    .name           = "MeshCore",
    .icon           = "[~]",
    .default_conv   = "Public",
    .is_available   = mesh_is_available,
    .send           = mesh_send,
    .start_receive  = mesh_start_receive,
    .stop_receive   = mesh_stop_receive,
    .poll           = mesh_poll,
};

/* ------------------------------------------------------------------ */
/* SMS transport                                                        */
/* ------------------------------------------------------------------ */

/*
 * The kernel fetches each SMS from the modem as it arrives and queues it,
 * as it does the mesh's; this hands them over when polled. Every
 * conversation is with a phone number, or with the name a sender such as a
 * bank or a parcel service gives instead; there is no "" conversation.
 */

/* Messages handed over per poll, as for the mesh. */
#define SMS_POLL_BUDGET 4

static bool sms_is_available(void)
{
    return rs_sms_ready();
}

static esp_err_t sms_send(const char *dest, const char *text, uint32_t *out_ref)
{
    if (out_ref) *out_ref = 0;
    if (!dest || !dest[0] || !text || !text[0]) return ESP_ERR_INVALID_ARG;
    return rs_sms_send(dest, text);
}

/* The kernel is always listening; there is nothing to start or stop. */
static esp_err_t sms_start_receive(msg_rx_cb_t cb)
{
    (void)cb;
    return ESP_OK;
}

static void sms_stop_receive(void)
{
}

static void sms_poll(msg_rx_cb_t rx, msg_status_cb_t status)
{
    (void)status;
    /* Too big for the LVGL task's stack, and only ever used here. */
    static sms_message_t m;
    for (int n = 0; n < SMS_POLL_BUDGET && rs_sms_inbox_pop(&m); n++) {
        if (rx) rx(MSG_TRANSPORT_SMS, m.sender, m.sender, m.text);
    }
}

static const msg_transport_driver_t s_sms_driver = {
    .type           = MSG_TRANSPORT_SMS,
    .name           = "SMS",
    .icon           = "[M]",
    .default_conv   = "SMS",
    .is_available   = sms_is_available,
    .send           = sms_send,
    .start_receive  = sms_start_receive,
    .stop_receive   = sms_stop_receive,
    .poll           = sms_poll,
};

/* ------------------------------------------------------------------ */
/* BLE relay transport (stub)                                           */
/* ------------------------------------------------------------------ */

/*
 * BLE framing for messenger traffic:
 *   M1|<sender>|<text>
 * where <sender> is "Node-XXXXXXXX" and <text> is UTF-8 payload.
 */
#define BLE_MAX_TEXT 220

static msg_rx_cb_t s_ble_rx_cb;
static uint32_t    s_ble_device_id;

static void ble_build_sender(char *out, size_t out_len)
{
    if (s_ble_device_id == 0) {
        s_ble_device_id = (uint32_t)esp_timer_get_time() ^ 0xB1E01234;
        if (s_ble_device_id == 0) s_ble_device_id = 0xB1E00001;
    }
    snprintf(out, out_len, "Node-%08X", (unsigned)s_ble_device_id);
}

static void ble_rx_handler(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    if (!s_ble_rx_cb || !data || len < 6) return;

    char frame[BLE_MAX_TEXT + 48];
    size_t copy_len = len < (sizeof(frame) - 1) ? len : (sizeof(frame) - 1);
    memcpy(frame, data, copy_len);
    frame[copy_len] = '\0';

    if (strncmp(frame, "M1|", 3) != 0) return;

    char *sender = frame + 3;
    char *sep = strchr(sender, '|');
    if (!sep) return;
    *sep = '\0';

    char *text = sep + 1;
    if (text[0] == '\0') return;

    if (strncmp(sender, "Node-", 5) == 0) {
        unsigned long sender_id = strtoul(sender + 5, NULL, 16);
        if ((uint32_t)sender_id == s_ble_device_id) return;
    }

    s_ble_rx_cb(MSG_TRANSPORT_BLE, "", sender, text);
}

static bool ble_is_available(void)
{
    return (ble_manager_get_state() == BLE_STATE_CONNECTED);
}

static esp_err_t ble_send(const char *dest, const char *text, uint32_t *out_ref)
{
    (void)dest;
    if (out_ref) *out_ref = 0;
    if (!ble_is_available()) return ESP_ERR_NOT_SUPPORTED;
    if (!text || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    char sender[16];
    ble_build_sender(sender, sizeof(sender));

    size_t text_len = strlen(text);
    if (text_len > BLE_MAX_TEXT) text_len = BLE_MAX_TEXT;

    char frame[BLE_MAX_TEXT + 48];
    int written = snprintf(frame, sizeof(frame), "M1|%s|%.*s",
                           sender, (int)text_len, text);
    if (written <= 0 || written >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ble_manager_send((const uint8_t *)frame, (size_t)written);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ble_manager_send failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ble_start_receive(msg_rx_cb_t cb)
{
    if (!ble_is_available()) return ESP_ERR_NOT_SUPPORTED;
    s_ble_rx_cb = cb;
    esp_err_t err = ble_manager_register_rx_cb(ble_rx_handler, NULL);
    if (err != ESP_OK) {
        s_ble_rx_cb = NULL;
    }
    return err;
}

static void ble_stop_receive(void)
{
    s_ble_rx_cb = NULL;
    ble_manager_register_rx_cb(NULL, NULL);
}

static const msg_transport_driver_t s_ble_driver = {
    .type           = MSG_TRANSPORT_BLE,
    .name           = "BLE",
    .icon           = "[B]",
    .default_conv   = "BLE",
    .is_available   = ble_is_available,
    .send           = ble_send,
    .start_receive  = ble_start_receive,
    .stop_receive   = ble_stop_receive,
};

/* ------------------------------------------------------------------ */
/* Internet transport (stub)                                            */
/* ------------------------------------------------------------------ */

static bool internet_is_available(void)
{
    /* Hidden until readiness/send/rx are implemented end-to-end. */
    return false;
}

static esp_err_t internet_send(const char *dest, const char *text, uint32_t *out_ref)
{
    (void)dest;
    (void)text;
    if (out_ref) *out_ref = 0;
    /*
     * TODO: POST to messaging API endpoint using esp_http_client.
     * The endpoint URL and auth token should be read from NVS.
     */
    ESP_LOGW(TAG, "Internet send: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t internet_start_receive(msg_rx_cb_t cb)
{
    (void)cb;
    /*
     * TODO: open a WebSocket connection and dispatch received frames
     * to cb() on the LVGL task via lv_async_call.
     */
    ESP_LOGW(TAG, "Internet start_receive: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

static void internet_stop_receive(void)
{
    /* TODO: close WebSocket / unregister polling timer */
}

static const msg_transport_driver_t s_internet_driver = {
    .type           = MSG_TRANSPORT_INTERNET,
    .name           = "Internet",
    .icon           = "[W]",
    .default_conv   = "Internet",
    .is_available   = internet_is_available,
    .send           = internet_send,
    .start_receive  = internet_start_receive,
    .stop_receive   = internet_stop_receive,
};

/* ------------------------------------------------------------------ */
/* Registry implementation                                              */
/* ------------------------------------------------------------------ */

void messenger_register_transport(const msg_transport_driver_t *driver)
{
    if (!driver) return;
    if (driver->type >= MSG_TRANSPORT_COUNT) {
        ESP_LOGE(TAG, "invalid transport type %d", (int)driver->type);
        return;
    }
    s_registry[driver->type] = driver;
    s_count++;
    ESP_LOGI(TAG, "registered transport: %s", driver->name);
}

const msg_transport_driver_t *messenger_get_transport(msg_transport_t type)
{
    if (type >= MSG_TRANSPORT_COUNT) return NULL;
    return s_registry[type];
}

int messenger_get_available_transports(const msg_transport_driver_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < MSG_TRANSPORT_COUNT && n < max; i++) {
        if (s_registry[i] && s_registry[i]->is_available()) {
            out[n++] = s_registry[i];
        }
    }
    return n;
}

void messenger_transport_init(void)
{
    memset(s_registry, 0, sizeof(s_registry));
    s_count = 0;

    messenger_register_transport(&s_lora_driver);
    messenger_register_transport(&s_sms_driver);
    messenger_register_transport(&s_ble_driver);
    messenger_register_transport(&s_internet_driver);

    ESP_LOGI(TAG, "transport layer ready (%d backends)", MSG_TRANSPORT_COUNT);
}
