/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — MeshCore's screens: the chat kit (apps/chatkit) over the
 * kernel's MeshCore node (thistle/mesh.h)
 *
 * - Chats: the Public channel, which every MeshCore node shares and which
 *   is always here, then each node a direct message has gone to or come
 *   from, latest first. Public is a group: its bubbles name their writers.
 * - Nodes: Advertise, so other nodes can add this one, then every chat node
 *   heard advertising, with how far it is; a row opens its conversation.
 * - Settings: this node's name, key and radio; the mesh switched on or off
 *   (as the quick panel's LoRa tile does) and Advertise; then the settings
 *   the SMS app shares.
 *
 * The node queues what it hears while the app is closed; the app takes it
 * as it opens and twice a second while it is open, and keeps the
 * conversations (s_store) until a restart, after which the card's history
 * comes back (thistle/msg_log.h).
 */
#include "meshchat/meshchat_app.h"
#include "meshchat/meshchat_core.h"

#include "chatkit/chatkit.h"
#include "tailchat/tailchat_icons.h"

#include "thistle/mesh.h"
#include "thistle/msg_alert.h"
#include "thistle/msg_log.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "meshchat";

#define PUBLIC      ""        /* the Public channel's conversation */
#define PUBLIC_NAME "Public"
#define POLL_BUDGET 8         /* messages taken a poll: a burst waits for the next */
#define HISTORY_CONVS 12      /* read back from the card, leaving room for new ones */

/* The app's rows. */
enum {
    ROW_ADVERT = 1,
    ROW_PUBLIC,
    ROW_NODE,
    ROW_ENABLED,
};

/* The conversations, kept while the device runs: the app is destroyed
 * whenever another comes to the front. */
static EXT_RAM_BSS_ATTR chatkit_store_t s_store;

/* ------------------------------------------------------------------ */
/* The node                                                             */
/* ------------------------------------------------------------------ */

/* The contact a conversation is with, by its address. */
static bool find_node(const char *conv, mesh_contact_t *out)
{
    uint8_t prefix[MESHCHAT_KEY_PREFIX];
    if (!meshchat_parse_address(conv, prefix)) return false;
    int idx = rs_mesh_find_contact_prefix(prefix, sizeof prefix);
    return idx >= 0 && rs_mesh_get_contact(idx, out) == ESP_OK;
}

static void node_name(const mesh_contact_t *c, char *out, size_t n)
{
    size_t len = c->name_len < n - 1 ? c->name_len : n - 1;
    memcpy(out, c->name, len);
    out[len] = '\0';
}

static chatkit_status_t status_of(uint8_t delivery)
{
    switch (delivery) {
    case MESH_DELIVERY_SENT:      return CHATKIT_SENT;
    case MESH_DELIVERY_DELIVERED: return CHATKIT_DELIVERED;
    case MESH_DELIVERY_FAILED:    return CHATKIT_FAILED;
    default:                      return CHATKIT_NONE;
    }
}

static void poll(void)
{
    if (rs_mesh_available()) chatkit_ensure(&s_store, PUBLIC, PUBLIC_NAME, true);

    /* Too big for the UI task's stack, and only ever used here. */
    static mesh_message_t m;
    for (int n = 0; n < POLL_BUDGET && rs_mesh_inbox_pop(&m); n++) {
        /* Only the Public channel is joined; anything else is not ours. */
        if (m.kind == MESH_MSG_CHANNEL && m.channel != MESH_PUBLIC_CHANNEL) continue;
        char conv[MESHCHAT_ADDR_LEN] = PUBLIC;
        if (m.kind == MESH_MSG_DIRECT) meshchat_address(m.sender_key, conv);
        char sender[sizeof m.sender_name + 1];
        memcpy(sender, m.sender_name, m.sender_name_len);
        sender[m.sender_name_len] = '\0';
        char text[sizeof m.text + 1];
        memcpy(text, m.text, m.text_len);
        text[m.text_len] = '\0';
        const char *who = sender[0] ? sender : "?";
        ESP_LOGI(TAG, "received from %s%s", who, conv[0] ? "" : " in Public");
        chatkit_receive(conv, conv[0] ? who : PUBLIC_NAME, who, text, 0);
    }

    uint32_t id;
    uint8_t delivery;
    while (rs_mesh_delivery_pop(&id, &delivery)) chatkit_sent_status(id, status_of(delivery));
}

/* A message read back from the card: a contact goes by the name it last
 * had, the Public channel by its own. */
static void saved(void *ctx, const char *conv, bool sent, const char *peer, const char *text, uint64_t when)
{
    (void)ctx;
    bool pub = conv[0] == '\0';
    chatkit_keep(conv, pub ? PUBLIC_NAME : peer, sent, sent ? "" : peer, text, when);
}

static void read_history(void)
{
    if (rs_mesh_available()) chatkit_ensure(&s_store, PUBLIC, PUBLIC_NAME, true);
    int n = rs_msg_log_read_back(MSG_LOG_MESH, HISTORY_CONVS, CHATKIT_HISTORY, saved, NULL);
    ESP_LOGI(TAG, "history: %d messages in %d conversations from %s", n, s_store.n, rs_msg_log_dir());
}

static const char *why_not(void)
{
    if (!rs_mesh_available()) return "This device has no LoRa radio for the mesh.";
    if (!rs_mesh_is_enabled()) return "MeshCore is off. Switch it on in Settings here, or the quick panel.";
    if (!rs_mesh_is_running()) return "MeshCore is starting...";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Lists                                                                */
/* ------------------------------------------------------------------ */

static uint32_t contacts_seq(void)
{
    return (uint32_t)rs_mesh_get_contact_count() << 1 | (rs_mesh_is_running() ? 1u : 0u);
}

static int contacts(chatkit_row_t *rows, int max, const char *query)
{
    (void)query;
    int n = 0;
    chatkit_fill_row(&rows[n], ROW_ADVERT, NULL, "Advertise", NULL, "Let nodes add this one");
    chatkit_row_icon(&rows[n], TC_ICON_RADAR);
    rows[n++].avatar = CHATKIT_AVATAR_HOLLOW;
    chatkit_fill_row(&rows[n], ROW_PUBLIC, PUBLIC, PUBLIC_NAME, NULL, "Everyone in range");
    chatkit_row_icon(&rows[n], LV_SYMBOL_LOOP);
    rows[n++].avatar = CHATKIT_AVATAR_SOLID;

    int count = rs_mesh_get_contact_count();
    for (int i = 0; i < count && n < max; i++) {
        mesh_contact_t c;
        if (rs_mesh_get_contact(i, &c) != ESP_OK) continue;
        /* Only a chat node reads direct messages; a repeater or a room
         * server wants a login first. */
        if (c.node_type != MESH_TYPE_CHAT) continue;
        char addr[MESHCHAT_ADDR_LEN], name[40], route[24], right[16] = "";
        meshchat_address(c.pub_key, addr);
        node_name(&c, name, sizeof name);
        meshchat_route(c.path_len, route, sizeof route);
        if (c.last_rssi) snprintf(right, sizeof right, "%d dBm", (int)c.last_rssi);
        chatkit_fill_row(&rows[n], ROW_NODE, addr, name[0] ? name : addr, right, route);
        rows[n].avatar = CHATKIT_AVATAR_SOLID;
        rows[n].online = c.path_len == 0;
        n++;
    }
    return n;
}

static void advertise(void)
{
    esp_err_t err = rs_mesh_send_advert();
    ESP_LOGI(TAG, "advert: %s", esp_err_to_name(err));
    if (err == ESP_OK) toast_info("Advertised: nodes in range can add this one");
    else toast_warn("Could not advertise: MeshCore is not on air");
}

static void contact_chosen(const chatkit_row_t *r)
{
    switch (r->kind) {
    case ROW_ADVERT:
        advertise();
        break;
    case ROW_PUBLIC:
        chatkit_open(PUBLIC, PUBLIC_NAME);
        break;
    case ROW_NODE: {
        mesh_contact_t c;
        char name[40] = "";
        if (find_node(r->key, &c)) node_name(&c, name, sizeof name);
        chatkit_open(r->key, name[0] ? name : r->key);
        break;
    }
    default:
        break;
    }
}

static int settings(chatkit_row_t *rows, int max)
{
    if (max < 2) return 0;
    bool on = rs_mesh_is_enabled();
    chatkit_fill_row(&rows[0], ROW_ENABLED, NULL, "MeshCore", NULL, NULL);
    chatkit_row_icon(&rows[0], LV_SYMBOL_LOOP);
    rows[0].avatar = CHATKIT_AVATAR_HOLLOW;
    chatkit_row_toggle(&rows[0], on, on ? "On air, listening" : "Off: radio asleep");
    chatkit_fill_row(&rows[1], ROW_ADVERT, NULL, "Advertise", NULL, "Let nodes add this one");
    chatkit_row_icon(&rows[1], TC_ICON_RADAR);
    rows[1].avatar = CHATKIT_AVATAR_HOLLOW;
    return 2;
}

static void setting_chosen(const chatkit_row_t *r)
{
    if (r->kind == ROW_ADVERT) {
        advertise();
    } else if (r->kind == ROW_ENABLED) {
        bool on = !rs_mesh_is_enabled();
        esp_err_t err = rs_mesh_set_enabled(on);
        ESP_LOGI(TAG, "mesh %s: %s", on ? "on" : "off", esp_err_to_name(err));
        if (err != ESP_OK) toast_warn(on ? "MeshCore would not start" : "Could not save the choice");
    }
}

static void own_card(char *title, size_t title_n, char *text, size_t text_n)
{
    const char *self = rs_mesh_get_self_name();
    snprintf(title, title_n, "%s", self && self[0] ? self : "This node");
    uint8_t key[32];
    char addr[MESHCHAT_ADDR_LEN] = "";
    if (rs_mesh_get_self_key(key) == ESP_OK) meshchat_address(key, addr);
    mesh_radio_t radio;
    if (rs_mesh_get_radio(&radio) == ESP_OK) {
        snprintf(text, text_n, "Others see this name.\n" TC_ICON_KEY " %s\n%lu.%03lu MHz, %lu kHz, SF%u, CR 4/%u, %d dBm",
                 addr[0] ? addr : "no key yet", (unsigned long)(radio.freq_hz / 1000000),
                 (unsigned long)(radio.freq_hz / 1000 % 1000), (unsigned long)(radio.bw_hz / 1000), (unsigned)radio.sf,
                 (unsigned)radio.cr, (int)radio.tx_power_dbm);
    } else {
        snprintf(text, text_n, "Not on air now.");
    }
}

/* ------------------------------------------------------------------ */
/* A conversation                                                       */
/* ------------------------------------------------------------------ */

static void describe(const chatkit_conv_t *c, char *mono, size_t mono_n, char *sub, size_t sub_n, bool *group)
{
    if (c->conv[0] == '\0') {
        snprintf(mono, mono_n, "%s", LV_SYMBOL_LOOP);
        snprintf(sub, sub_n, "Everyone in range");
        *group = true;
        return;
    }
    mesh_contact_t node;
    char route[24];
    if (find_node(c->conv, &node)) snprintf(sub, sub_n, "%s", meshchat_route(node.path_len, route, sizeof route));
    else snprintf(sub, sub_n, "Not heard advertising");
}

static esp_err_t send(const char *conv, const char *text, uint32_t *ref, chatkit_status_t *status)
{
    *ref = 0;
    esp_err_t err;
    if (conv[0] == '\0') {
        err = rs_mesh_send_channel(MESH_PUBLIC_CHANNEL, text, ref);
    } else {
        mesh_contact_t node;
        if (!find_node(conv, &node)) {
            toast_warn("This node has not been heard; ask it to advertise");
            return ESP_ERR_NOT_FOUND;
        }
        err = rs_mesh_send_direct(node.pub_key, text, ref);
    }
    *status = *ref ? CHATKIT_PENDING : CHATKIT_SENT;
    return err;
}

static uint32_t text_limit(const char *conv)
{
    const char *self = rs_mesh_get_self_name();
    return meshchat_text_limit(conv[0] == '\0', self ? strlen(self) : 0, MESH_TEXT_MAX);
}

static const chatkit_app_t s_app = {
    .tag             = "meshchat",
    .name            = "MeshCore",
    .contacts_tab    = "Nodes",
    .chats_icon      = TC_ICON_CHAT,
    .contacts_icon   = TC_ICON_RADAR,
    .empty_title     = "No conversations yet",
    .empty_text      = "Advertise so other nodes can add this one, and write to a node they heard.",
    .search_hint     = NULL,
    .alert_transport = MSG_ALERT_MESH,
    .store           = &s_store,
    .poll            = poll,
    .read_history    = read_history,
    .why_not         = why_not,
    .contacts_seq    = contacts_seq,
    .contacts        = contacts,
    .contact_chosen  = contact_chosen,
    .settings        = settings,
    .setting_chosen  = setting_chosen,
    .own_card        = own_card,
    .describe        = describe,
    .send            = send,
    .text_limit      = text_limit,
    .call            = NULL,
    .held_unread     = rs_mesh_set_held_unread,
};

esp_err_t meshchat_ui_create(lv_obj_t *parent)
{
    return chatkit_ui_create(parent, &s_app);
}

void meshchat_ui_destroy(void)
{
    chatkit_ui_destroy();
}
