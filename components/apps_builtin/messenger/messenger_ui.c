/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Messenger UI (multi-transport)
 *
 * Screen flow:
 *
 *   SCREEN_CONV_LIST
 *     Scrollable list of conversations: the mesh's Public channel, one per
 *     contact a direct message has gone to or come from, and one per phone
 *     number an SMS has. Each entry shows the transport icon, the
 *     conversation's name, how many messages in it are unread, and its last
 *     message. A message is read once its conversation has been on screen;
 *     the status bar counts the unread mesh messages and SMS, here or still
 *     waiting in the kernel.
 *     A [+] button in the header opens SCREEN_NEW_CONV, a gear SCREEN_SETTINGS.
 *
 *   SCREEN_NEW_CONV
 *     What a conversation can be started with: the mesh's Public channel and
 *     every contact it has heard advertise, a phone number to text, and each
 *     other transport that is ready. Also where this node advertises itself
 *     to the mesh.
 *
 *   SCREEN_SETTINGS
 *     Whether every message is saved to the SD card (thistle/msg_log.h).
 *     The kernel saves them as they come and go, Messenger open or not.
 *
 *   SCREEN_CHAT
 *     Classic message-bubble chat view.  The header shows the conversation
 *     and its transport: "Public [MeshCore]".  Messages the user sent show
 *     what became of them: sending, sent, delivered, failed.
 *     Input bar + send button at the bottom.
 */

#include "messenger/messenger_app.h"
#include "esp_attr.h"
#include "messenger/messenger_transport.h"
#include "messenger/messenger_kernel.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#include "thistle/mesh.h"
#include "thistle/msg_log.h"
#include "thistle/sms.h"
#include "thistle/msg_alert.h"
#include "thistle/wifi_manager.h"
#include "thistle/time_manager.h"
#include "ui/theme.h"

#include <stdatomic.h>

static const char *TAG = "messenger_ui";

static atomic_int s_pending_rx = 0;
#define MAX_PENDING_RX 10

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MSG_MAX_TEXT     200
#define MSG_HISTORY      50
#define MAX_CONVS        16
#define CONV_NAME_MAX         32
#define MAX_PICKER       32   /* mesh contacts offered on the new-conversation screen */

/* Fixed pixel dimensions for 240×296 display */
#define DISP_W           240
#define DISP_H           296
#define HEADER_H         30
#define INPUT_BAR_H      40
#define CHAT_LIST_H      (DISP_H - HEADER_H - INPUT_BAR_H)   /* 146 */
#define CONV_ITEM_H      48

/* ------------------------------------------------------------------ */
/* Data types                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    SCREEN_CONV_LIST,
    SCREEN_NEW_CONV,
    SCREEN_CHAT,
    SCREEN_SETTINGS,
} app_screen_t;

typedef struct {
    char         sender[CONV_NAME_MAX];
    char         text[MSG_MAX_TEXT];
    char         time_str[12];  /* "14:05", or "2:05 PM" */
    bool         is_self;
    uint32_t     ref;           /* the transport's reference for it, 0 if none */
    msg_status_t status;
} chat_message_t;

typedef struct {
    msg_transport_t      transport;
    char                 dest[32];          /* "" = the transport's own conversation */
    char                 title[CONV_NAME_MAX];   /* who it is with, for any other */
    chat_message_t       messages[MSG_HISTORY];
    int                  msg_count;
    char                 last_preview[48];  /* shown in conversation list */
    char                 last_time[8];
    int                  unread;            /* received since it was last on screen */
} conversation_t;

/* A contact offered on the new-conversation screen. */
typedef struct {
    char addr[MSG_MESH_ADDR_LEN];
    char name[CONV_NAME_MAX];
} picker_entry_t;

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static EXT_RAM_BSS_ATTR struct {
    /* LVGL root */
    lv_obj_t   *root;

    /* Active screen, and whether the Messenger is in front to show it */
    app_screen_t screen;
    bool         visible;

    /* --- Conversation list screen --- */
    lv_obj_t   *conv_list_screen;
    lv_obj_t   *conv_list;          /* scrollable column of items */

    /* --- New conversation screen --- */
    lv_obj_t   *new_conv_screen;
    lv_obj_t   *new_conv_list;
    lv_obj_t   *advert_label;
    lv_obj_t   *sms_number_ta;
    lv_obj_t   *sms_note_label;

    /* --- Settings screen --- */
    lv_obj_t   *settings_screen;
    lv_obj_t   *settings_list;
    lv_obj_t   *save_on_btn;
    lv_obj_t   *save_off_btn;
    lv_obj_t   *save_note;
    lv_obj_t   *alert_on_btn;
    lv_obj_t   *alert_off_btn;
    lv_obj_t   *alert_note;

    /* --- Chat screen --- */
    lv_obj_t   *chat_screen;
    lv_obj_t   *chat_header_label;
    lv_obj_t   *msg_list;
    lv_obj_t   *input_ta;
    lv_obj_t   *send_btn;

    int             active_conv;         /* index into s_convs[] */

    picker_entry_t  picker[MAX_PICKER];
} s_msg;

/* Conversations, allocated as they start: each holds its history. They
 * outlive the widgets above. The app manager destroys the Messenger whenever
 * another app comes to the front, and messages taken from the mesh, read or
 * not, must still be here when it is opened again. A reboot loses them. */
static conversation_t *s_convs[MAX_CONVS];
static int             s_conv_count;

/* LVGL periodic timer for kernel tick integration */
static lv_timer_t *s_tick_timer;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void show_screen(app_screen_t screen);
static void build_conv_list_screen(void);
static void build_new_conv_screen(void);
static void refresh_new_conv_list(void);
static void build_settings_screen(void);
static void refresh_settings(void);
static void build_chat_screen(void);
static void refresh_conv_list(void);
static void open_conversation(int conv_idx);
static void rebuild_bubbles(void);
static void create_message_bubble(const chat_message_t *msg);
static void add_message_to_conv(int conv_idx, const char *sender, const char *text,
                                bool is_self, uint32_t ref, msg_status_t status);
static void send_message(void);
static void poll_transports(void);
static void kernel_tick_cb(lv_timer_t *timer);
static int ensure_conversation(msg_transport_t transport, const char *dest, const char *title);

/* ------------------------------------------------------------------ */
/* Conversations                                                        */
/* ------------------------------------------------------------------ */

static const char *conv_title(const conversation_t *cv)
{
    if (cv->dest[0] != '\0') return cv->title;
    const msg_transport_driver_t *drv = messenger_get_transport(cv->transport);
    return (drv && drv->default_conv) ? drv->default_conv : "?";
}

static int find_conversation_index(msg_transport_t transport, const char *dest)
{
    if (!dest) dest = "";
    for (int i = 0; i < s_conv_count; i++) {
        if (s_convs[i]->transport == transport && strcmp(s_convs[i]->dest, dest) == 0) {
            return i;
        }
    }
    return -1;
}

/* The conversation with `dest` on `transport`, started if there is none.
 * `title` names who it is with; a newer one replaces the old, so a contact
 * that renames itself is shown by its new name. */
static int ensure_conversation(msg_transport_t transport, const char *dest, const char *title)
{
    if (!dest) dest = "";
    int ci = find_conversation_index(transport, dest);
    if (ci < 0) {
        if (s_conv_count >= MAX_CONVS) {
            ESP_LOGW(TAG, "no free conversation slots for transport %d", (int)transport);
            return -1;
        }
        conversation_t *cv = calloc(1, sizeof(conversation_t));
        if (!cv) {
            ESP_LOGW(TAG, "no memory for a conversation");
            return -1;
        }
        cv->transport = transport;
        strncpy(cv->dest, dest, sizeof(cv->dest) - 1);
        ci = s_conv_count++;
        s_convs[ci] = cv;
    }
    if (title && title[0] != '\0' && dest[0] != '\0') {
        conversation_t *cv = s_convs[ci];
        strncpy(cv->title, title, sizeof(cv->title) - 1);
        cv->title[sizeof(cv->title) - 1] = '\0';
    }
    return ci;
}

/* Whether the user is looking at the conversation. */
static bool conversation_on_screen(int ci)
{
    return s_msg.visible && s_msg.screen == SCREEN_CHAT && s_msg.active_conv == ci;
}

/* Tell the kernel which conversation is on screen, so that a message
 * arriving in it neither buzzes nor pops up: the user is reading it. */
static void report_viewing(void)
{
    int ci = s_msg.active_conv;
    if (s_msg.visible && s_msg.screen == SCREEN_CHAT && ci >= 0 && ci < s_conv_count) {
        const conversation_t *cv = s_convs[ci];
        if (cv->transport == MSG_TRANSPORT_SMS) {
            rs_msg_alert_set_viewing(MSG_ALERT_SMS, cv->dest);
            return;
        }
        if (cv->transport == MSG_TRANSPORT_LORA) {
            rs_msg_alert_set_viewing(MSG_ALERT_MESH, cv->dest);
            return;
        }
    }
    rs_msg_alert_set_viewing(-1, NULL);
}

/* Tell the kernel how many mesh messages and SMS the Messenger holds unread.
 * The status bar adds those still waiting in the kernel's inboxes. */
static void report_unread(void)
{
    uint32_t mesh = 0, sms = 0;
    for (int i = 0; i < s_conv_count; i++) {
        if (s_convs[i]->transport == MSG_TRANSPORT_LORA) {
            mesh += (uint32_t)s_convs[i]->unread;
        } else if (s_convs[i]->transport == MSG_TRANSPORT_SMS) {
            sms += (uint32_t)s_convs[i]->unread;
        }
    }
    rs_mesh_set_held_unread(mesh);
    rs_sms_set_held_unread(sms);
}

/* The user has seen everything in the conversation. */
static void mark_read(int ci)
{
    if (ci < 0 || ci >= s_conv_count || s_convs[ci]->unread == 0) return;
    s_convs[ci]->unread = 0;
    report_unread();
}

/* ------------------------------------------------------------------ */
/* Receiving                                                            */
/* ------------------------------------------------------------------ */

/* A message for the UI, on the LVGL task. */
static void deliver_message(msg_transport_t transport, const char *conv,
                            const char *sender, const char *text)
{
    if (!conv) conv = "";
    if (!sender) sender = "?";
    if (!text) text = "";

    /* Contact resolution: an SMS sender is a phone number, which the
     * kernel contact manager may know a name for. */
    const char *display_name = sender;
    char resolved_name[64];
    CContactInfo contact;
    memset(&contact, 0, sizeof(contact));
    bool have_contact = false;

    if (transport == MSG_TRANSPORT_SMS) {
        have_contact = (rs_contact_find_by_phone(sender, &contact) == 0);
        if (have_contact && contact.name[0] != '\0') {
            strncpy(resolved_name, (const char *)contact.name, sizeof(resolved_name) - 1);
            resolved_name[sizeof(resolved_name) - 1] = '\0';
            display_name = resolved_name;
        }
    }

    int ci = ensure_conversation(transport, conv, display_name);
    if (ci < 0) return;

    /* Decrypt if an encrypted channel is active for this contact. A mesh
     * direct message is already end-to-end encrypted by MeshCore. */
    const char *final_text = text;
    char decrypted[MSG_MAX_TEXT];
    if (have_contact && rs_msg_crypto_is_active(contact.id)) {
        int pt_len = rs_msg_crypto_decrypt(
            contact.id,
            (const uint8_t *)text, strlen(text),
            (uint8_t *)decrypted, sizeof(decrypted) - 1);
        if (pt_len > 0) {
            decrypted[pt_len] = '\0';
            final_text = decrypted;
        }
        /* If decryption fails, fall through and show raw text */
    }

    add_message_to_conv(ci, display_name, final_text, false, 0, MSG_STATUS_NONE);

    /* Unread unless it arrived in front of the user. No more can be unread
     * than the history keeps. */
    if (!conversation_on_screen(ci)) {
        conversation_t *cv = s_convs[ci];
        if (cv->unread < MSG_HISTORY) cv->unread++;
        report_unread();
    }

    if (s_msg.screen == SCREEN_CONV_LIST) {
        refresh_conv_list();
    }
}

/*
 * Transports that call back do so from their own task. LVGL is not
 * thread-safe, so the message is marshalled to the LVGL task with
 * lv_async_call.
 */
typedef struct {
    msg_transport_t transport;
    char            conv[32];
    char            sender[CONV_NAME_MAX];
    char            text[MSG_MAX_TEXT];
} rx_async_arg_t;

static void rx_async_handler(void *arg)
{
    rx_async_arg_t *rx = (rx_async_arg_t *)arg;
    if (!rx) return;
    atomic_fetch_sub(&s_pending_rx, 1);
    deliver_message(rx->transport, rx->conv, rx->sender, rx->text);

    /* lv_async_call does NOT free the arg — we must do it ourselves.
     * The arg was heap-allocated in transport_rx_cb below. */
    free(rx);
}

static void transport_rx_cb(msg_transport_t transport, const char *conv,
                            const char *sender, const char *text)
{
    if (atomic_load(&s_pending_rx) >= MAX_PENDING_RX) {
        ESP_LOGW(TAG, "RX queue full, dropping message");
        return;
    }
    atomic_fetch_add(&s_pending_rx, 1);
    rx_async_arg_t *arg = malloc(sizeof(rx_async_arg_t));
    if (!arg) { atomic_fetch_sub(&s_pending_rx, 1); return; }

    arg->transport = transport;
    strncpy(arg->conv, conv ? conv : "", sizeof(arg->conv) - 1);
    arg->conv[sizeof(arg->conv) - 1] = '\0';
    strncpy(arg->sender, sender ? sender : "?", sizeof(arg->sender) - 1);
    arg->sender[sizeof(arg->sender) - 1] = '\0';
    strncpy(arg->text, text ? text : "", sizeof(arg->text) - 1);
    arg->text[sizeof(arg->text) - 1] = '\0';

    lv_async_call(rx_async_handler, arg);
}

/* News of a message the user sent, on the LVGL task. */
static void transport_status_cb(msg_transport_t transport, uint32_t ref, msg_status_t status)
{
    if (ref == 0) return;
    for (int ci = 0; ci < s_conv_count; ci++) {
        conversation_t *cv = s_convs[ci];
        if (cv->transport != transport) continue;
        int count = cv->msg_count < MSG_HISTORY ? cv->msg_count : MSG_HISTORY;
        for (int i = 0; i < count; i++) {
            chat_message_t *m = &cv->messages[i];
            if (!m->is_self || m->ref != ref) continue;
            m->status = status;
            if (s_msg.screen == SCREEN_CHAT && s_msg.active_conv == ci) {
                rebuild_bubbles();
            }
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Screen switching                                                     */
/* ------------------------------------------------------------------ */

static void show_screen(app_screen_t screen)
{
    /* Hide all sub-screens first */
    if (s_msg.conv_list_screen)
        lv_obj_add_flag(s_msg.conv_list_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_msg.new_conv_screen)
        lv_obj_add_flag(s_msg.new_conv_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_msg.chat_screen)
        lv_obj_add_flag(s_msg.chat_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_msg.settings_screen)
        lv_obj_add_flag(s_msg.settings_screen, LV_OBJ_FLAG_HIDDEN);

    switch (screen) {
        case SCREEN_CONV_LIST:
            if (s_msg.conv_list_screen) {
                lv_obj_clear_flag(s_msg.conv_list_screen, LV_OBJ_FLAG_HIDDEN);
                refresh_conv_list();
            }
            break;
        case SCREEN_NEW_CONV:
            if (s_msg.new_conv_screen) {
                refresh_new_conv_list();
                lv_obj_clear_flag(s_msg.new_conv_screen, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case SCREEN_CHAT:
            if (s_msg.chat_screen)
                lv_obj_clear_flag(s_msg.chat_screen, LV_OBJ_FLAG_HIDDEN);
            break;
        case SCREEN_SETTINGS:
            if (s_msg.settings_screen) {
                refresh_settings();
                lv_obj_clear_flag(s_msg.settings_screen, LV_OBJ_FLAG_HIDDEN);
            }
            break;
    }
    s_msg.screen = screen;
    report_viewing();
}

/* A screen-sized panel with a header bar, as every screen here has. */
static lv_obj_t *create_screen_with_header(lv_obj_t **header_out)
{
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *scr = lv_obj_create(s_msg.root);
    lv_obj_set_size(scr, DISP_W, DISP_H);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, DISP_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    *header_out = header;
    return scr;
}

/* ------------------------------------------------------------------ */
/* Conversation list screen                                             */
/* ------------------------------------------------------------------ */

static void conv_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_conversation(idx);
}

static void new_conv_btn_cb(lv_event_t *e)
{
    (void)e;
    show_screen(SCREEN_NEW_CONV);
}

static void settings_btn_cb(lv_event_t *e)
{
    (void)e;
    show_screen(SCREEN_SETTINGS);
}

/* A small square button at the right of a header, `right` from its edge. */
static void add_header_button(lv_obj_t *header, const char *text, int right,
                              lv_event_cb_t cb)
{
    const theme_colors_t *colors = theme_get_colors();
    lv_obj_t *btn = lv_button_create(header);
    lv_obj_set_size(btn, 22, 22);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -right, 0);
    lv_obj_set_style_bg_color(btn, colors->primary, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 2, LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

static void build_conv_list_screen(void)
{
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *header;
    lv_obj_t *scr = create_screen_with_header(&header);
    s_msg.conv_list_screen = scr;

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Messenger");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, colors->text, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    add_header_button(header, "+", 0, new_conv_btn_cb);
    add_header_button(header, LV_SYMBOL_SETTINGS, 28, settings_btn_cb);

    /* Scrollable list body */
    s_msg.conv_list = lv_obj_create(scr);
    lv_obj_set_size(s_msg.conv_list, DISP_W, DISP_H - HEADER_H);
    lv_obj_set_pos(s_msg.conv_list, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_msg.conv_list, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.conv_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg.conv_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.conv_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.conv_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg.conv_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_msg.conv_list, LV_DIR_VER);
}

/*
 * refresh_conv_list — rebuild the conversation list items.
 * Called after init and whenever a new message arrives while the list is shown.
 */
static void refresh_conv_list(void)
{
    if (!s_msg.conv_list) return;
    const theme_colors_t *colors = theme_get_colors();

    /* Remove all existing items */
    lv_obj_clean(s_msg.conv_list);

    for (int i = 0; i < s_conv_count; i++) {
        conversation_t *cv = s_convs[i];
        const msg_transport_driver_t *drv = messenger_get_transport(cv->transport);
        if (!drv) continue;

        bool avail = drv->is_available();

        /* Item row */
        lv_obj_t *item = lv_obj_create(s_msg.conv_list);
        lv_obj_set_size(item, DISP_W, CONV_ITEM_H);
        lv_obj_set_style_bg_color(item, colors->surface, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(item, colors->text_secondary, LV_PART_MAIN);
        lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(item, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(item, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(item, 4, LV_PART_MAIN);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, conv_item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* First line: icon + who the conversation is with */
        char top_str[64];
        snprintf(top_str, sizeof(top_str), "%s %s%s",
                 drv->icon, conv_title(cv),
                 avail ? "" : " (unavailable)");

        /* How many are unread, at the right of that line */
        int name_w = DISP_W - 16;
        if (cv->unread > 0) {
            char count[12];
            snprintf(count, sizeof(count), "%d", cv->unread);
            lv_obj_t *badge = lv_label_create(item);
            lv_label_set_text(badge, count);
            lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(badge, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(badge, colors->primary, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(badge, 5, LV_PART_MAIN);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, 0);
            name_w -= 32;
        }

        lv_obj_t *name_lbl = lv_label_create(item);
        lv_label_set_text(name_lbl, top_str);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, name_w);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(name_lbl,
            avail ? colors->text : colors->text_secondary,
            LV_PART_MAIN);
        lv_obj_set_pos(name_lbl, 0, 0);

        /* Second line: last message preview */
        if (cv->last_preview[0] != '\0') {
            char preview[64];
            snprintf(preview, sizeof(preview), "%s (%s)",
                     cv->last_preview, cv->last_time);

            lv_obj_t *prev_lbl = lv_label_create(item);
            lv_label_set_text(prev_lbl, preview);
            lv_label_set_long_mode(prev_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(prev_lbl, DISP_W - 16);
            lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(prev_lbl, colors->text_secondary, LV_PART_MAIN);
            lv_obj_set_pos(prev_lbl, 0, 18);
        }
    }
}

/* ------------------------------------------------------------------ */
/* New conversation screen                                              */
/* ------------------------------------------------------------------ */

static void start_conversation(msg_transport_t transport, const char *dest, const char *title)
{
    int ci = ensure_conversation(transport, dest, title);
    if (ci >= 0) open_conversation(ci);
}

static void transport_pick_cb(lv_event_t *e)
{
    msg_transport_t type = (msg_transport_t)(intptr_t)lv_event_get_user_data(e);
    const msg_transport_driver_t *drv = messenger_get_transport(type);
    if (!drv || !drv->is_available()) {
        ESP_LOGI(TAG, "transport %d is not available", (int)type);
        return;
    }
    start_conversation(type, "", NULL);
}

static void mesh_contact_pick_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_PICKER) return;
    start_conversation(MSG_TRANSPORT_LORA, s_msg.picker[idx].addr, s_msg.picker[idx].name);
}

static void mesh_advert_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = rs_mesh_send_advert();
    if (s_msg.advert_label) {
        lv_label_set_text(s_msg.advert_label,
                          err == ESP_OK ? "Advert sent" : "Could not advertise");
    }
}

static void new_conv_back_cb(lv_event_t *e)
{
    (void)e;
    show_screen(SCREEN_CONV_LIST);
}

static void build_new_conv_screen(void)
{
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *header;
    lv_obj_t *scr = create_screen_with_header(&header);
    s_msg.new_conv_screen = scr;

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "< New Conversation");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, colors->text, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, new_conv_back_cb, LV_EVENT_CLICKED, NULL);

    s_msg.new_conv_list = lv_obj_create(scr);
    lv_obj_set_size(s_msg.new_conv_list, DISP_W, DISP_H - HEADER_H);
    lv_obj_set_pos(s_msg.new_conv_list, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_msg.new_conv_list, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.new_conv_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg.new_conv_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.new_conv_list, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.new_conv_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg.new_conv_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_msg.new_conv_list, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_msg.new_conv_list, LV_DIR_VER);
}

static lv_obj_t *add_list_label(lv_obj_t *list, const char *text, bool secondary)
{
    const theme_colors_t *colors = theme_get_colors();
    lv_obj_t *lbl = lv_label_create(list);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, DISP_W - 16);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, secondary ? colors->text_secondary : colors->text,
                                LV_PART_MAIN);
    return lbl;
}

static lv_obj_t *add_list_button(lv_obj_t *list, const char *text, lv_event_cb_t cb,
                                 intptr_t user_data, bool primary)
{
    const theme_colors_t *colors = theme_get_colors();
    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_size(btn, DISP_W - 16, 36);
    lv_obj_set_style_bg_color(btn, primary ? colors->primary : colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, primary ? 0 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, colors->text_secondary, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, DISP_W - 32);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, primary ? lv_color_white() : colors->text, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)user_data);
    return btn;
}

static lv_obj_t *add_picker_label(const char *text, bool secondary)
{
    return add_list_label(s_msg.new_conv_list, text, secondary);
}

static void add_picker_button(const char *text, lv_event_cb_t cb, intptr_t user_data,
                              bool primary)
{
    add_list_button(s_msg.new_conv_list, text, cb, user_data, primary);
}

/* How far a contact is, as MeshCore knows it. */
static void describe_route(const mesh_contact_t *c, char *out, size_t size)
{
    if (c->path_len == MESH_PATH_UNKNOWN) snprintf(out, size, "route unknown");
    else if (c->path_len == 0)            snprintf(out, size, "in range");
    else                                  snprintf(out, size, "%u hop%s", (unsigned)c->path_len,
                                                   c->path_len == 1 ? "" : "s");
}

static int add_mesh_choices(void)
{
    const msg_transport_driver_t *mesh = messenger_get_transport(MSG_TRANSPORT_LORA);
    if (!mesh || !mesh->is_available()) return 0;

    char line[96];
    snprintf(line, sizeof(line), "MeshCore - you are %s", rs_mesh_get_self_name());
    add_picker_label(line, false);

    snprintf(line, sizeof(line), "%s Public channel", mesh->icon);
    add_picker_button(line, transport_pick_cb, MSG_TRANSPORT_LORA, true);
    add_picker_button("Advertise this device", mesh_advert_cb, 0, true);
    s_msg.advert_label = add_picker_label(
        "Advertise so other nodes can add you, and advertise from them to see them here.",
        true);

    int shown = 0;
    int count = rs_mesh_get_contact_count();
    for (int i = 0; i < count && shown < MAX_PICKER; i++) {
        mesh_contact_t c;
        if (rs_mesh_get_contact(i, &c) != ESP_OK) continue;
        /* Only a chat node reads direct messages; a repeater or room server
         * wants a login first. */
        if (c.node_type != MESH_TYPE_CHAT) continue;

        picker_entry_t *entry = &s_msg.picker[shown];
        messenger_mesh_address(c.pub_key, entry->addr);
        size_t n = c.name_len < sizeof(entry->name) - 1 ? c.name_len : sizeof(entry->name) - 1;
        memcpy(entry->name, c.name, n);
        entry->name[n] = '\0';

        char route[24];
        describe_route(&c, route, sizeof(route));
        snprintf(line, sizeof(line), "%s (%s)", entry->name[0] ? entry->name : "?", route);
        if (shown == 0) add_picker_label("Contacts", false);
        add_picker_button(line, mesh_contact_pick_cb, shown, false);
        shown++;
    }
    if (shown == 0) add_picker_label("No contacts heard yet.", true);
    return 1;
}

/* A phone number as the modem takes it: an optional '+', then digits. */
static bool sms_number_valid(const char *n)
{
    if (*n == '+') n++;
    if (!*n) return false;
    for (; *n; n++) {
        if (*n < '0' || *n > '9') return false;
    }
    return true;
}

static void sms_start_cb(lv_event_t *e)
{
    (void)e;
    if (!s_msg.sms_number_ta) return;

    /* Spaces, dashes and brackets as people write numbers are dropped. */
    const char *typed = lv_textarea_get_text(s_msg.sms_number_ta);
    char number[24];
    size_t n = 0;
    for (; *typed && n < sizeof(number) - 1; typed++) {
        if (*typed == ' ' || *typed == '-' || *typed == '(' || *typed == ')') continue;
        number[n++] = *typed;
    }
    number[n] = '\0';
    if (*typed || !sms_number_valid(number)) {
        if (s_msg.sms_note_label) {
            lv_label_set_text(s_msg.sms_note_label,
                              "A number is digits, with + in front if international.");
        }
        return;
    }

    char title[CONV_NAME_MAX];
    CContactInfo contact;
    if (rs_contact_find_by_phone(number, &contact) == 0 && contact.name[0] != '\0') {
        snprintf(title, sizeof(title), "%.*s", (int)sizeof(title) - 1,
                 (const char *)contact.name);
    } else {
        snprintf(title, sizeof(title), "%s", number);
    }
    start_conversation(MSG_TRANSPORT_SMS, number, title);
}

static void sms_number_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ENTER) sms_start_cb(e);
}

static int add_sms_choices(void)
{
    const msg_transport_driver_t *sms = messenger_get_transport(MSG_TRANSPORT_SMS);
    if (!sms || !sms->is_available()) return 0;

    add_picker_label("SMS - a phone number", false);
    s_msg.sms_number_ta = lv_textarea_create(s_msg.new_conv_list);
    lv_textarea_set_one_line(s_msg.sms_number_ta, true);
    lv_textarea_set_max_length(s_msg.sms_number_ta, 23);
    lv_textarea_set_accepted_chars(s_msg.sms_number_ta, "+0123456789 -()");
    lv_textarea_set_placeholder_text(s_msg.sms_number_ta, "+49...");
    lv_obj_set_width(s_msg.sms_number_ta, DISP_W - 16);
    lv_obj_set_style_border_width(s_msg.sms_number_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_msg.sms_number_ta, theme_get_colors()->text_secondary,
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.sms_number_ta, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.sms_number_ta, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_msg.sms_number_ta, sms_number_key_cb, LV_EVENT_KEY, NULL);

    char line[48];
    snprintf(line, sizeof(line), "%s Text this number", sms->icon);
    add_picker_button(line, sms_start_cb, 0, true);
    s_msg.sms_note_label = add_picker_label("", true);

    /* The physical keyboard types the number. */
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, s_msg.sms_number_ta);
        lv_group_focus_obj(s_msg.sms_number_ta);
    }
    return 1;
}

static void refresh_new_conv_list(void)
{
    if (!s_msg.new_conv_list) return;
    lv_obj_clean(s_msg.new_conv_list);
    s_msg.advert_label = NULL;
    s_msg.sms_number_ta = NULL;
    s_msg.sms_note_label = NULL;

    int visible_count = add_mesh_choices();
    visible_count += add_sms_choices();

    for (int t = 0; t < MSG_TRANSPORT_COUNT; t++) {
        /* Every SMS conversation is with a number. */
        if (t == MSG_TRANSPORT_LORA || t == MSG_TRANSPORT_SMS) continue;
        const msg_transport_driver_t *drv = messenger_get_transport((msg_transport_t)t);
        if (!drv || !drv->is_available()) continue;

        char btn_label[48];
        snprintf(btn_label, sizeof(btn_label), "%s %s", drv->icon, drv->name);
        add_picker_button(btn_label, transport_pick_cb, t, true);
        visible_count++;
    }

    if (visible_count == 0) {
        add_picker_label("No message transports are ready yet.", true);
    }
}

/* ------------------------------------------------------------------ */
/* Settings screen                                                      */
/* ------------------------------------------------------------------ */

static void update_settings(esp_err_t saved);
static void update_alerts(esp_err_t saved);

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    show_screen(SCREEN_CONV_LIST);
}

static void save_history_cb(lv_event_t *e)
{
    bool on = (bool)(intptr_t)lv_event_get_user_data(e);
    update_settings(rs_msg_log_set_enabled(on));
}

static void alerts_cb(lv_event_t *e)
{
    bool on = (bool)(intptr_t)lv_event_get_user_data(e);
    update_alerts(rs_msg_alert_set_enabled(on));
}

static void build_settings_screen(void)
{
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *header;
    lv_obj_t *scr = create_screen_with_header(&header);
    s_msg.settings_screen = scr;

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "< Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, colors->text, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, settings_back_cb, LV_EVENT_CLICKED, NULL);

    s_msg.settings_list = lv_obj_create(scr);
    lv_obj_set_size(s_msg.settings_list, DISP_W, DISP_H - HEADER_H);
    lv_obj_set_pos(s_msg.settings_list, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_msg.settings_list, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.settings_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg.settings_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.settings_list, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.settings_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg.settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_msg.settings_list, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_msg.settings_list, LV_DIR_VER);
}

/* Mark which of an On/Off pair stands. The buttons' own handlers call this
 * through update_settings() and update_alerts(), so it changes them rather
 * than building them again. */
static void show_choice(lv_obj_t *on_btn, lv_obj_t *off_btn, bool on)
{
    const theme_colors_t *colors = theme_get_colors();
    lv_obj_t *btns[2] = { on_btn, off_btn };
    const char *names[2] = { "On", "Off" };
    for (int i = 0; i < 2; i++) {
        bool chosen = (i == 0) == on;
        lv_obj_set_style_bg_color(btns[i], chosen ? colors->primary : colors->surface,
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(btns[i], chosen ? 0 : 1, LV_PART_MAIN);
        lv_obj_t *lbl = lv_obj_get_child(btns[i], 0);
        lv_label_set_text_fmt(lbl, "%s%s", chosen ? LV_SYMBOL_OK " " : "", names[i]);
        lv_obj_set_style_text_color(lbl, chosen ? lv_color_white() : colors->text,
                                    LV_PART_MAIN);
    }
}

/* Show whether messages are saved, as it stands. */
static void update_settings(esp_err_t saved)
{
    if (!s_msg.save_on_btn) return;
    bool on = rs_msg_log_enabled();
    show_choice(s_msg.save_on_btn, s_msg.save_off_btn, on);

    char note[160];
    if (saved != ESP_OK) {
        snprintf(note, sizeof(note), "Could not save the choice; it lasts until a restart.");
    } else if (!on) {
        snprintf(note, sizeof(note), "Messages are not saved: a restart loses them.");
    } else if (!rs_msg_log_writable()) {
        snprintf(note, sizeof(note), "No SD card: messages are not being saved.");
    } else {
        snprintf(note, sizeof(note),
                 "Every SMS and MeshCore message, sent or received, goes to %s, "
                 "a file per conversation, unencrypted.", rs_msg_log_dir());
    }
    lv_label_set_text(s_msg.save_note, note);
}

/* Show whether new messages buzz and pop up, as it stands. */
static void update_alerts(esp_err_t saved)
{
    if (!s_msg.alert_on_btn) return;
    bool on = rs_msg_alert_enabled();
    show_choice(s_msg.alert_on_btn, s_msg.alert_off_btn, on);
    lv_label_set_text(s_msg.alert_note,
        saved != ESP_OK ? "Could not save the choice; it lasts until a restart." :
        on ? "A new SMS or MeshCore message vibrates and pops up, unless its "
             "conversation is on screen." :
             "New messages are only counted in the status bar.");
}

static void refresh_settings(void)
{
    if (!s_msg.settings_list) return;
    lv_obj_t *list = s_msg.settings_list;
    lv_obj_clean(list);

    add_list_label(list, "Save messages to the SD card", false);
    s_msg.save_on_btn = add_list_button(list, "On", save_history_cb, 1, true);
    s_msg.save_off_btn = add_list_button(list, "Off", save_history_cb, 0, false);
    s_msg.save_note = add_list_label(list, "", true);
    update_settings(ESP_OK);

    add_list_label(list, "Vibrate and pop up for new messages", false);
    s_msg.alert_on_btn = add_list_button(list, "On", alerts_cb, 1, true);
    s_msg.alert_off_btn = add_list_button(list, "Off", alerts_cb, 0, false);
    s_msg.alert_note = add_list_label(list, "", true);
    update_alerts(ESP_OK);
}

/* ------------------------------------------------------------------ */
/* Chat screen                                                          */
/* ------------------------------------------------------------------ */

static void chat_back_cb(lv_event_t *e)
{
    (void)e;

    /* Stop receiving on the current transport */
    if (s_msg.active_conv >= 0) {
        const msg_transport_driver_t *drv =
            messenger_get_transport(s_convs[s_msg.active_conv]->transport);
        if (drv && drv->stop_receive) drv->stop_receive();
    }
    s_msg.active_conv = -1;
    show_screen(SCREEN_CONV_LIST);
}

static void send_btn_cb(lv_event_t *e)
{
    (void)e;
    send_message();
}

static void ta_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) {
        send_message();
    }
}

static void build_chat_screen(void)
{
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *header;
    lv_obj_t *scr = create_screen_with_header(&header);
    s_msg.chat_screen = scr;

    s_msg.chat_header_label = lv_label_create(header);
    lv_label_set_text(s_msg.chat_header_label, "Messenger");
    lv_label_set_long_mode(s_msg.chat_header_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_msg.chat_header_label, DISP_W - 48);
    lv_obj_set_style_text_font(s_msg.chat_header_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_msg.chat_header_label, colors->text, LV_PART_MAIN);
    lv_obj_align(s_msg.chat_header_label, LV_ALIGN_LEFT_MID, 0, 0);

    /* Back button */
    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_set_size(back_btn, 28, 22);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_btn, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(back_btn, 2, LV_PART_MAIN);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "<<");
    lv_obj_set_style_text_color(back_lbl, colors->text, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, chat_back_cb, LV_EVENT_CLICKED, NULL);

    /* Message list */
    s_msg.msg_list = lv_obj_create(scr);
    lv_obj_set_size(s_msg.msg_list, DISP_W, CHAT_LIST_H);
    lv_obj_set_pos(s_msg.msg_list, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_msg.msg_list, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.msg_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg.msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.msg_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_msg.msg_list, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg.msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_msg.msg_list, LV_DIR_VER);

    /* Input bar */
    lv_obj_t *input_bar = lv_obj_create(scr);
    lv_obj_set_size(input_bar, DISP_W, INPUT_BAR_H);
    lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(input_bar, colors->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(input_bar, colors->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(input_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_bar, 4, LV_PART_MAIN);

    s_msg.input_ta = lv_textarea_create(input_bar);
    lv_textarea_set_one_line(s_msg.input_ta, true);
    lv_textarea_set_placeholder_text(s_msg.input_ta, "Type a message...");
    lv_obj_set_flex_grow(s_msg.input_ta, 1);
    lv_obj_set_height(s_msg.input_ta, 30);
    lv_obj_add_event_cb(s_msg.input_ta, ta_key_cb, LV_EVENT_KEY, NULL);

    s_msg.send_btn = lv_button_create(input_bar);
    lv_obj_set_size(s_msg.send_btn, 40, 30);
    lv_obj_set_style_bg_color(s_msg.send_btn, colors->primary, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.send_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.send_btn, 4, LV_PART_MAIN);
    lv_obj_t *send_lbl = lv_label_create(s_msg.send_btn);
    lv_label_set_text(send_lbl, ">");
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(s_msg.send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* How much text one message on this conversation can carry. A mesh channel
 * message goes out as "<our name>: <text>", inside MeshCore's limit. */
static uint32_t conv_text_limit(const conversation_t *cv)
{
    /* One SMS: the modem sends no longer message. */
    if (cv->transport == MSG_TRANSPORT_SMS) return 160;
    if (cv->transport != MSG_TRANSPORT_LORA) return MSG_MAX_TEXT - 1;
    if (cv->dest[0] != '\0') return MESH_TEXT_MAX;
    size_t prefix = strlen(rs_mesh_get_self_name()) + 2;
    return prefix < MESH_TEXT_MAX ? (uint32_t)(MESH_TEXT_MAX - prefix) : 0;
}

/* ------------------------------------------------------------------ */
/* Open a conversation (switch to chat screen and start receiving)     */
/* ------------------------------------------------------------------ */

static void open_conversation(int conv_idx)
{
    if (conv_idx < 0 || conv_idx >= s_conv_count) return;

    s_msg.active_conv = conv_idx;
    conversation_t *cv = s_convs[conv_idx];
    const msg_transport_driver_t *drv = messenger_get_transport(cv->transport);
    mark_read(conv_idx);

    /* Update header label: "Public [MeshCore]" */
    if (s_msg.chat_header_label && drv) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s [%s]", conv_title(cv), drv->name);
        lv_label_set_text(s_msg.chat_header_label, hdr);
    }

    /* Dim send controls if transport unavailable */
    const theme_colors_t *colors = theme_get_colors();
    bool avail = (drv && drv->is_available());

    if (s_msg.send_btn) {
        lv_obj_set_style_bg_color(s_msg.send_btn,
            avail ? colors->primary : colors->text_secondary,
            LV_PART_MAIN);
    }
    if (s_msg.input_ta) {
        lv_obj_set_style_text_color(s_msg.input_ta,
            avail ? colors->text : colors->text_secondary,
            LV_PART_MAIN);
        lv_textarea_set_max_length(s_msg.input_ta, conv_text_limit(cv));
    }

    /* Start receiving on this transport */
    if (drv && drv->start_receive) {
        drv->start_receive(transport_rx_cb);
    }

    show_screen(SCREEN_CHAT);
    rebuild_bubbles();

    /* The physical keyboard types into the message straight away. */
    lv_group_t *grp = lv_group_get_default();
    if (grp && s_msg.input_ta) {
        lv_group_add_obj(grp, s_msg.input_ta);
        lv_group_focus_obj(s_msg.input_ta);
    }
}

/* ------------------------------------------------------------------ */
/* Message bubbles                                                      */
/* ------------------------------------------------------------------ */

static const char *status_text(msg_status_t status)
{
    switch (status) {
        case MSG_STATUS_PENDING:   return " sending";
        case MSG_STATUS_SENT:      return " sent";
        case MSG_STATUS_DELIVERED: return " " LV_SYMBOL_OK;
        case MSG_STATUS_FAILED:    return " failed";
        default:                   return "";
    }
}

static void create_message_bubble(const chat_message_t *msg)
{
    if (!s_msg.msg_list) return;
    const theme_colors_t *colors = theme_get_colors();

    lv_obj_t *bubble = lv_obj_create(s_msg.msg_list);
    lv_obj_set_width(bubble, LV_PCT(90));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, colors->text_secondary, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (msg->is_self) {
        lv_obj_set_style_bg_color(bubble, colors->primary, LV_PART_MAIN);
        lv_obj_set_align(bubble, LV_ALIGN_RIGHT_MID);
    } else {
        lv_obj_set_style_bg_color(bubble, colors->surface, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 2, LV_PART_MAIN);

    /* Sender + timestamp, and for the user's own what became of it */
    lv_obj_t *hdr_lbl = lv_label_create(bubble);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%s] %s%s", msg->sender, msg->time_str,
             msg->is_self ? status_text(msg->status) : "");
    lv_label_set_text(hdr_lbl, hdr);
    lv_label_set_long_mode(hdr_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hdr_lbl, LV_PCT(100));
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr_lbl,
        msg->is_self ? lv_color_white() : colors->text_secondary,
        LV_PART_MAIN);

    /* Message body */
    lv_obj_t *body = lv_label_create(bubble);
    lv_label_set_text(body, msg->text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(body,
        msg->is_self ? lv_color_white() : colors->text,
        LV_PART_MAIN);
}

/* Redraw the open conversation from its history. */
static void rebuild_bubbles(void)
{
    if (!s_msg.msg_list || s_msg.active_conv < 0) return;
    conversation_t *cv = s_convs[s_msg.active_conv];

    lv_obj_clean(s_msg.msg_list);
    int count = cv->msg_count;
    int start = (count > MSG_HISTORY) ? count - MSG_HISTORY : 0;
    for (int i = start; i < count; i++) {
        create_message_bubble(&cv->messages[i % MSG_HISTORY]);
    }
    lv_obj_scroll_to_y(s_msg.msg_list, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ------------------------------------------------------------------ */
/* Add message to a conversation's history and (optionally) the UI     */
/* ------------------------------------------------------------------ */

static void add_message_to_conv(int conv_idx, const char *sender, const char *text,
                                bool is_self, uint32_t ref, msg_status_t status)
{
    if (conv_idx < 0 || conv_idx >= s_conv_count) return;

    conversation_t *cv = s_convs[conv_idx];
    chat_message_t *m = &cv->messages[cv->msg_count % MSG_HISTORY];

    strncpy(m->sender, sender, sizeof(m->sender) - 1);
    m->sender[sizeof(m->sender) - 1] = '\0';
    strncpy(m->text, text, MSG_MAX_TEXT - 1);
    m->text[MSG_MAX_TEXT - 1] = '\0';
    m->is_self = is_self;
    m->ref = ref;
    m->status = status;

    char time_buf[12];
    time_manager_format_time(time_buf, sizeof(time_buf));
    strncpy(m->time_str, time_buf, sizeof(m->time_str) - 1);
    m->time_str[sizeof(m->time_str) - 1] = '\0';

    cv->msg_count++;

    /* Update conversation list preview */
    strncpy(cv->last_preview, text, sizeof(cv->last_preview) - 1);
    cv->last_preview[sizeof(cv->last_preview) - 1] = '\0';
    strncpy(cv->last_time, time_buf, sizeof(cv->last_time) - 1);
    cv->last_time[sizeof(cv->last_time) - 1] = '\0';

    /* If this conversation is the active chat, add a bubble immediately */
    if (s_msg.screen == SCREEN_CHAT && s_msg.active_conv == conv_idx) {
        create_message_bubble(m);
        if (s_msg.msg_list) {
            lv_obj_scroll_to_y(s_msg.msg_list, LV_COORD_MAX, LV_ANIM_ON);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Send a message on the active conversation's transport               */
/* ------------------------------------------------------------------ */

static void send_message(void)
{
    if (!s_msg.input_ta) return;
    if (s_msg.active_conv < 0) return;

    const char *text = lv_textarea_get_text(s_msg.input_ta);
    if (!text || text[0] == '\0') return;

    conversation_t *cv = s_convs[s_msg.active_conv];
    const msg_transport_driver_t *drv = messenger_get_transport(cv->transport);
    const char *dest = cv->dest[0] ? cv->dest : NULL;

    /*
     * Encrypt if an encrypted channel is active for the destination contact.
     * Look up the contact by destination (phone for SMS).
     */
    const uint8_t *send_payload = (const uint8_t *)text;
    uint16_t send_len = (uint16_t)strlen(text);
    uint8_t encrypted_buf[512];
    bool encrypted = false;

    CContactInfo dest_contact;
    memset(&dest_contact, 0, sizeof(dest_contact));
    bool have_dest_contact = false;

    if (dest && cv->transport == MSG_TRANSPORT_SMS) {
        have_dest_contact = (rs_contact_find_by_phone(dest, &dest_contact) == 0);
    }

    if (have_dest_contact && rs_msg_crypto_is_active(dest_contact.id)) {
        int ct_len = rs_msg_crypto_encrypt(
            dest_contact.id,
            (const uint8_t *)text, strlen(text),
            encrypted_buf, sizeof(encrypted_buf));
        if (ct_len > 0) {
            send_payload = encrypted_buf;
            send_len = (uint16_t)ct_len;
            encrypted = true;
            ESP_LOGI(TAG, "message encrypted for contact %u", (unsigned)dest_contact.id);
        } else {
            ESP_LOGW(TAG, "encryption failed (rc=%d), sending plaintext", ct_len);
        }
    }

    /*
     * Enqueue through the kernel message queue for retry/persistence,
     * then also attempt direct send for immediate delivery.
     */
    int msg_id = rs_msg_queue_enqueue(
        (uint8_t)cv->transport,
        (const uint8_t *)dest, dest ? (uint8_t)strlen(dest) : 0,
        send_payload, send_len,
        0 /* MSG_PRIORITY_NORMAL */);

    if (msg_id > 0) {
        ESP_LOGI(TAG, "queued message id=%d on %s%s",
                 msg_id, drv ? drv->name : "?",
                 encrypted ? " (encrypted)" : "");
    }

    /* Also attempt direct send for immediate delivery */
    uint32_t ref = 0;
    msg_status_t status = msg_id > 0 ? MSG_STATUS_PENDING : MSG_STATUS_FAILED;
    if (drv && drv->send && drv->is_available()) {
        const char *send_text = encrypted ? (const char *)encrypted_buf : text;
        esp_err_t ret = drv->send(dest, send_text, &ref);
        if (ret == ESP_OK) {
            /* Mark as sent in the queue so it won't retry */
            if (msg_id > 0) rs_msg_queue_mark_sent((uint32_t)msg_id);
            status = ref ? MSG_STATUS_PENDING : MSG_STATUS_NONE;
        } else {
            ESP_LOGW(TAG, "direct send failed on %s: %s%s",
                     drv->name, esp_err_to_name(ret),
                     msg_id > 0 ? " (queued for retry)" : "");
        }
    } else {
        ESP_LOGW(TAG, "transport %s unavailable%s",
                 drv ? drv->name : "?", msg_id > 0 ? " - queued for later" : "");
    }

    /* Always show the message locally (original plaintext) */
    add_message_to_conv(s_msg.active_conv, "You", text, true, ref, status);

    lv_textarea_set_text(s_msg.input_ta, "");
}

/* ------------------------------------------------------------------ */
/* Kernel tick — transports, burn timer, message queue                 */
/* ------------------------------------------------------------------ */

static void poll_transports(void)
{
    for (int t = 0; t < MSG_TRANSPORT_COUNT; t++) {
        const msg_transport_driver_t *drv = messenger_get_transport((msg_transport_t)t);
        if (drv && drv->poll) drv->poll(deliver_message, transport_status_cb);
    }
}

static void kernel_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

    poll_transports();

    /* Advance burn timers and check for expired messages */
    rs_burn_timer_tick(now_ms);

    CBurnExpired expired[8];
    int n_expired = rs_burn_timer_get_expired(expired, 8);
    for (int i = 0; i < n_expired; i++) {
        uint8_t conv_id  = expired[i].conversation_id;
        uint8_t msg_idx  = expired[i].message_index;

        if (conv_id < s_conv_count) {
            conversation_t *cv = s_convs[conv_id];
            int slot = msg_idx % MSG_HISTORY;
            if (msg_idx < cv->msg_count) {
                /* Wipe the burned message content */
                strncpy(cv->messages[slot].text, "[burned]", MSG_MAX_TEXT - 1);
                cv->messages[slot].text[MSG_MAX_TEXT - 1] = '\0';
            }
        }
        ESP_LOGI(TAG, "burn expired: conv=%u msg=%u", conv_id, msg_idx);
    }

    /* If any messages were burned and we're viewing that conversation, refresh */
    if (n_expired > 0 && s_msg.screen == SCREEN_CHAT && s_msg.active_conv >= 0) {
        rebuild_bubbles();
    }

    /* Advance the message queue and dispatch ready-to-send messages */
    rs_msg_queue_tick(now_ms);

    CQueuedMsgInfo ready[4];
    int n_ready = rs_msg_queue_get_ready(ready, 4);
    for (int i = 0; i < n_ready; i++) {
        uint8_t transport_type = ready[i].transport;
        const msg_transport_driver_t *drv = messenger_get_transport((msg_transport_t)transport_type);
        if (!drv || !drv->send || !drv->is_available()) {
            /* Transport not available — leave in queue for next tick */
            continue;
        }

        /* Retrieve the payload from the queue */
        uint8_t payload[512];
        int plen = rs_msg_queue_get_payload(ready[i].id, payload, sizeof(payload) - 1);
        if (plen <= 0) {
            rs_msg_queue_mark_failed(ready[i].id);
            continue;
        }
        payload[plen] = '\0';

        /* Build destination string from queue entry */
        char dest[33];
        if (ready[i].dest_len > 0) {
            int dlen = ready[i].dest_len < 32 ? ready[i].dest_len : 32;
            memcpy(dest, ready[i].dest, dlen);
            dest[dlen] = '\0';
        } else {
            dest[0] = '\0';
        }

        esp_err_t ret = drv->send(dest[0] ? dest : NULL, (const char *)payload, NULL);
        if (ret == ESP_OK) {
            rs_msg_queue_mark_sent(ready[i].id);
            ESP_LOGI(TAG, "queue dispatch: msg %u sent via %s",
                     (unsigned)ready[i].id, drv->name);
        } else {
            rs_msg_queue_mark_failed(ready[i].id);
            ESP_LOGW(TAG, "queue dispatch: msg %u failed on %s: %s",
                     (unsigned)ready[i].id, drv->name, esp_err_to_name(ret));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t messenger_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "creating messenger UI (multi-transport)");

    if (parent == NULL) {
        parent = lv_scr_act();
    }

    /* Reset state */
    memset(&s_msg, 0, sizeof(s_msg));
    s_msg.active_conv = -1;

    /* Initialise transport registry */
    messenger_transport_init();

    /* Root container — fills the app panel */
    const theme_colors_t *colors = theme_get_colors();
    s_msg.root = lv_obj_create(parent);
    lv_obj_set_size(s_msg.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_msg.root, 0, 0);
    lv_obj_set_style_bg_color(s_msg.root, colors->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg.root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_msg.root, LV_OBJ_FLAG_SCROLLABLE);

    /* Build all sub-screens (each starts hidden) */
    build_conv_list_screen();
    build_new_conv_screen();
    build_chat_screen();
    build_settings_screen();

    /* Each ready transport's own conversation, then anything that arrived
     * while the messenger was closed. SMS have none of their own: each is
     * with a number. */
    for (int t = 0; t < MSG_TRANSPORT_COUNT; t++) {
        if (t == MSG_TRANSPORT_SMS) continue;
        const msg_transport_driver_t *drv = messenger_get_transport((msg_transport_t)t);
        if (!drv || !drv->is_available()) continue;
        (void)ensure_conversation((msg_transport_t)t, "", NULL);
    }
    poll_transports();

    /* Start the kernel tick timer (1 second interval) */
    s_tick_timer = lv_timer_create(kernel_tick_cb, 1000, NULL);

    /* Start on the conversation list */
    show_screen(SCREEN_CONV_LIST);

    ESP_LOGI(TAG, "messenger ready");
    return ESP_OK;
}

void messenger_ui_show(void)
{
    if (s_msg.root) {
        lv_obj_clear_flag(s_msg.root, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tick_timer) {
        lv_timer_resume(s_tick_timer);
    }
    s_msg.visible = true;
    report_viewing();
    if (s_msg.screen == SCREEN_CHAT) mark_read(s_msg.active_conv);
    /* What arrived while hidden, before the first tick. */
    poll_transports();
    if (s_msg.screen == SCREEN_CONV_LIST) refresh_conv_list();
}

void messenger_ui_hide(void)
{
    s_msg.visible = false;
    report_viewing();
    if (s_msg.root) {
        lv_obj_add_flag(s_msg.root, LV_OBJ_FLAG_HIDDEN);

        /* Stop any active receive when hidden */
        if (s_msg.active_conv >= 0) {
            const msg_transport_driver_t *drv =
                messenger_get_transport(s_convs[s_msg.active_conv]->transport);
            if (drv && drv->stop_receive) drv->stop_receive();
        }
    }
    /* Pause the tick timer while hidden to avoid unnecessary work */
    if (s_tick_timer) {
        lv_timer_pause(s_tick_timer);
    }
    /* Persist the message queue to disk on hide */
    rs_msg_queue_save();
}

/* The widgets go; the conversations stay for the next time it opens. */
void messenger_ui_destroy(void)
{
    s_msg.visible = false;
    if (s_tick_timer) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }
    /* Persist before destroying */
    rs_msg_queue_save();
    if (s_msg.root) {
        lv_obj_delete(s_msg.root);
    }
    /* Nothing may reach a deleted widget: a message still on its way from
     * lv_async_call finds none and lands in its conversation unread. */
    memset(&s_msg, 0, sizeof(s_msg));
    s_msg.active_conv = -1;
    report_viewing();
}
