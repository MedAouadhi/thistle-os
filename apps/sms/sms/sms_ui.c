/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — SMS's screens: the chat kit (apps/chatkit) over the kernel's
 * SMS (thistle/sms.h)
 *
 * - Chats: a conversation with each number, or with the name a sender such
 *   as a bank gives, latest first, named by the phonebook. A conversation
 *   has Call in its header.
 * - Contacts: a field to search the phonebook by name or number; a number
 *   typed whole is offered first, as "Text +49...". A row opens the
 *   conversation with that number, the one already there if any, however
 *   its messages wrote the number.
 * - Settings: those the MeshCore app shares.
 *
 * The kernel fetches each SMS from the modem as it arrives and queues it;
 * the app takes it as it opens and twice a second while it is open, and
 * keeps the conversations (s_store) until a restart, after which the card's
 * history comes back (thistle/msg_log.h). Sending takes the modem seconds,
 * so it runs on a task of its own (s_out), which the app polls for how it
 * went: each message shows a clock until then, a tick or a warning after.
 *
 * The Phone app's Message asks for a conversation (rs_sms_request_compose)
 * and brings this app forward; each poll takes such a request.
 */
#include "sms/sms_app.h"
#include "sms/sms_core.h"

#include "chatkit/chatkit.h"
#include "tailchat/tailchat_icons.h"

#include "thistle/call.h"
#include "thistle/msg_alert.h"
#include "thistle/msg_log.h"
#include "thistle/phonebook.h"
#include "thistle/power_manager.h"
#include "thistle/sms.h"
#include "ui/toast.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sms";

#define POLL_BUDGET    4     /* messages taken a poll */
#define HISTORY_CONVS  12
#define SMS_MAX        160   /* one SMS: the modem sends no longer */
#define BOOK_ROWS      32
#define OUTBOX         4
#define TASK_STACK     8192
#define TASK_PRIORITY  4

enum {
    ROW_NUMBER = 1,   /* the number typed, "Text +49..." */
    ROW_PERSON,       /* a phonebook contact's number */
    ROW_NOTE,         /* a line that says why the list is short */
};

/* The older contact manager (contact_manager.rs), whose contacts may carry
 * a phone number and an encrypted chat (msg_crypto.rs). */
typedef struct {
    uint32_t id;
    uint8_t  name[64];
    uint8_t  callsign[16];
    uint32_t device_id;
    uint8_t  phone[24];
    uint8_t  ble_addr[24];
    uint8_t  public_key[32];
    bool     is_emergency;
} CContactInfo;
extern int  rs_contact_manager_init(void);
extern int  rs_contact_find_by_phone(const char *phone, CContactInfo *out);
extern int  rs_msg_crypto_init(void);
extern bool rs_msg_crypto_is_active(uint32_t contact_id);
extern int  rs_msg_crypto_encrypt(uint32_t contact_id, const uint8_t *pt, size_t pt_len, uint8_t *ct,
                                  size_t ct_max);
extern int  rs_msg_crypto_decrypt(uint32_t contact_id, const uint8_t *ct, size_t ct_len, uint8_t *pt,
                                  size_t pt_max);

/* The conversations, kept while the device runs. */
static EXT_RAM_BSS_ATTR chatkit_store_t s_store;
static uint32_t s_book_seq;
static bool     s_started;

/* ------------------------------------------------------------------ */
/* Names and text                                                       */
/* ------------------------------------------------------------------ */

/* Who a number is: the phonebook's name, the contact manager's, or the
 * number itself. */
static void name_for(const char *number, char *out, size_t n)
{
    char name[PB_NAME_MAX];
    CContactInfo contact;
    if (rs_phonebook_name_for(number, name, sizeof name) && name[0]) {
        chatkit_copy(out, n, name);
    } else if (rs_contact_find_by_phone(number, &contact) == 0 && contact.name[0]) {
        char older[sizeof contact.name + 1];
        memcpy(older, contact.name, sizeof contact.name);
        older[sizeof contact.name] = '\0';
        chatkit_copy(out, n, older);
    } else {
        chatkit_copy(out, n, number);
    }
}

/* An SMS's text as the user reads it: decrypted if the contact manager has
 * an encrypted chat with `number`, else as it came, which is also what
 * shows if decryption fails. */
static const char *plain_text(const char *number, const char *text, char *buf, size_t n)
{
    CContactInfo contact;
    if (rs_contact_find_by_phone(number, &contact) != 0 || !rs_msg_crypto_is_active(contact.id)) return text;
    int len = rs_msg_crypto_decrypt(contact.id, (const uint8_t *)text, strlen(text), (uint8_t *)buf, n - 1);
    if (len <= 0) return text;
    buf[len] = '\0';
    return buf;
}

/* The conversation already there with `number`, written any way; NULL for
 * none. */
static chatkit_conv_t *conv_with(const char *number)
{
    for (int i = 0; i < s_store.n; i++) {
        if (sms_numbers_match(s_store.convs[i]->conv, number)) return s_store.convs[i];
    }
    return NULL;
}

/* Open the conversation with `number`: the one there, or a new one under
 * the number as given. */
static void open_number(const char *number, const char *name)
{
    chatkit_conv_t *c = conv_with(number);
    char title[CHATKIT_NAME];
    if (name && name[0]) chatkit_copy(title, sizeof title, name);
    else name_for(number, title, sizeof title);
    ESP_LOGI(TAG, "compose %s: %s%s", c ? c->conv : number, title, c ? " (existing)" : "");
    chatkit_open(c ? c->conv : number, title);
}

/* ------------------------------------------------------------------ */
/* Sending, on a task of its own                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t ref;
    char     number[CHATKIT_ADDR];
    char     text[CHATKIT_TEXT];
} out_job_t;

typedef struct {
    uint32_t ref;
    bool     ok;
} out_result_t;

/* The outbox and what became of each, under s_lock: the UI task adds and
 * takes, the sender takes and adds. In PSRAM: only tasks touch them. */
static EXT_RAM_BSS_ATTR struct {
    out_job_t    jobs[OUTBOX];
    int          jobs_n;
    out_result_t done[OUTBOX * 2];
    int          done_n;
    bool         running;
    uint32_t     next_ref;
} s_out;
static SemaphoreHandle_t s_lock;

static void send_one(const out_job_t *job)
{
    uint32_t hold = power_manager_hold("sending an SMS");
    esp_err_t err = rs_sms_send(job->number, job->text);
    power_manager_release(hold);
    ESP_LOGI(TAG, "sent %u: %s", (unsigned)job->ref, esp_err_to_name(err));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_out.done_n < (int)(sizeof s_out.done / sizeof s_out.done[0])) {
        s_out.done[s_out.done_n].ref = job->ref;
        s_out.done[s_out.done_n].ok = err == ESP_OK;
        s_out.done_n++;
    }
    xSemaphoreGive(s_lock);
}

/* Send what the outbox holds, one at a time, until it is empty. */
static void drain(void)
{
    /* Too big for the stack: one at a time, taken from the outbox. */
    static EXT_RAM_BSS_ATTR out_job_t job;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_out.jobs_n == 0) {
            s_out.running = false;
            xSemaphoreGive(s_lock);
            break;
        }
        job = s_out.jobs[0];
        memmove(&s_out.jobs[0], &s_out.jobs[1], sizeof s_out.jobs[0] * (size_t)(s_out.jobs_n - 1));
        s_out.jobs_n--;
        xSemaphoreGive(s_lock);
        send_one(&job);
    }
}

static void sender_task(void *arg)
{
    (void)arg;
    drain();
    vTaskDelete(NULL);
}

/* What the sender made of each message, for its tick. */
static void take_results(void)
{
    if (!s_lock) return;
    out_result_t done[OUTBOX * 2];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_out.done_n;
    memcpy(done, s_out.done, sizeof done[0] * (size_t)n);
    s_out.done_n = 0;
    xSemaphoreGive(s_lock);
    for (int i = 0; i < n; i++) chatkit_sent_status(done[i].ref, done[i].ok ? CHATKIT_SENT : CHATKIT_FAILED);
}

static esp_err_t send(const char *conv, const char *text, uint32_t *ref, chatkit_status_t *status)
{
    char number[CHATKIT_ADDR];
    if (!sms_number(conv, number, sizeof number)) {
        toast_warn("This sender takes no replies");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_out.jobs_n >= OUTBOX) {
        xSemaphoreGive(s_lock);
        toast_warn("Still sending the last messages");
        return ESP_ERR_INVALID_STATE;
    }
    out_job_t *job = &s_out.jobs[s_out.jobs_n];
    memset(job, 0, sizeof *job);
    if (++s_out.next_ref == 0) ++s_out.next_ref;
    job->ref = s_out.next_ref;
    chatkit_copy(job->number, sizeof job->number, number);

    /* Encrypted if the contact manager has an encrypted chat with them. */
    CContactInfo contact;
    int ct = 0;
    if (rs_contact_find_by_phone(number, &contact) == 0 && rs_msg_crypto_is_active(contact.id)) {
        ct = rs_msg_crypto_encrypt(contact.id, (const uint8_t *)text, strlen(text), (uint8_t *)job->text,
                                   sizeof job->text - 1);
        if (ct > 0) job->text[ct] = '\0';
        else ESP_LOGW(TAG, "encryption failed (%d): sending it as it is", ct);
    }
    if (ct <= 0) chatkit_copy(job->text, sizeof job->text, text);
    s_out.jobs_n++;
    *ref = job->ref;
    bool start = !s_out.running;
    s_out.running = true;
    xSemaphoreGive(s_lock);

    *status = CHATKIT_PENDING;
    if (!start) return ESP_OK;
#ifdef __EMSCRIPTEN__
    /* The browser build's xTaskCreate runs nothing: the send runs here. */
    drain();
#else
    if (xTaskCreate(sender_task, "sms_send", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_out.jobs_n--;
        s_out.running = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* What the kernel brings                                               */
/* ------------------------------------------------------------------ */

/* Another app asked for a conversation (rs_sms_request_compose). */
static void take_compose(void)
{
    char asked[PB_NUMBER_MAX], name[PB_NAME_MAX], number[CHATKIT_ADDR];
    if (!rs_sms_take_compose(asked, sizeof asked, name, sizeof name)) return;
    /* As the card writes it, "+49 176 1234567": the modem takes it, and an
     * SMS from it arrives, without the spaces. */
    if (!sms_number(asked, number, sizeof number)) chatkit_copy(number, sizeof number, asked);
    if (number[0]) open_number(number, name);
}

/* Name conversations by the phonebook once it has changed. The first
 * look-up starts the card's reading and finds no name until it ends, which
 * a conversation read back from the card at the first opening always
 * meets. */
static void name_conversations(void)
{
    uint32_t seq = rs_phonebook_seq();
    if (seq == s_book_seq) return;
    s_book_seq = seq;
    for (int i = 0; i < s_store.n; i++) {
        chatkit_conv_t *c = s_store.convs[i];
        char name[PB_NAME_MAX], title[CHATKIT_NAME];
        if (!rs_phonebook_name_for(c->conv, name, sizeof name) || !name[0]) continue;
        chatkit_copy(title, sizeof title, name);
        chatkit_retitle(&s_store, c, title);
    }
}

/* The older contact manager and its encrypted chats, which name and
 * decrypt some numbers' messages. */
static void start(void)
{
    if (s_started) return;
    s_started = true;
    rs_contact_manager_init();
    rs_msg_crypto_init();
}

static void poll(void)
{
    start();
    take_results();
    /* Too big for the UI task's stack, and only ever used here. */
    static EXT_RAM_BSS_ATTR sms_message_t m;
    static EXT_RAM_BSS_ATTR char plain[CHATKIT_TEXT];
    for (int n = 0; n < POLL_BUDGET && rs_sms_inbox_pop(&m); n++) {
        chatkit_conv_t *c = conv_with(m.sender);
        char name[CHATKIT_NAME];
        name_for(m.sender, name, sizeof name);
        const char *text = plain_text(m.sender, m.text, plain, sizeof plain);
        ESP_LOGI(TAG, "received from %s", name);
        chatkit_receive(c ? c->conv : m.sender, name, name, text, 0);
    }
    name_conversations();
    take_compose();
}

static void saved(void *ctx, const char *conv, bool sent, const char *peer, const char *text, uint64_t when)
{
    (void)ctx;
    (void)peer;
    static EXT_RAM_BSS_ATTR char plain[CHATKIT_TEXT];
    char name[CHATKIT_NAME];
    name_for(conv, name, sizeof name);
    chatkit_keep(conv, name, sent, sent ? "" : name, plain_text(conv, text, plain, sizeof plain), when);
}

static void read_history(void)
{
    start();
    int n = rs_msg_log_read_back(MSG_LOG_SMS, HISTORY_CONVS, CHATKIT_HISTORY, saved, NULL);
    ESP_LOGI(TAG, "history: %d messages in %d conversations from %s", n, s_store.n, rs_msg_log_dir());
}

static const char *why_not(void)
{
    if (rs_sms_ready()) return NULL;
    if (!rs_call_supported()) return "This device has no modem for SMS.";
    return "The modem is not ready: check the SIM, and Calls and SMS in Settings.";
}

/* ------------------------------------------------------------------ */
/* Contacts                                                             */
/* ------------------------------------------------------------------ */

static uint32_t contacts_seq(void)
{
    return rs_phonebook_seq() << 1 | (rs_phonebook_loading() ? 1u : 0u);
}

static int contacts(chatkit_row_t *rows, int max, const char *query)
{
    int n = 0;
    char number[CHATKIT_ADDR];
    if (query[0] && sms_number(query, number, sizeof number) && n < max) {
        char title[64];
        snprintf(title, sizeof title, "Text %s", number);
        char name[CHATKIT_NAME];
        name_for(number, name, sizeof name);
        chatkit_fill_row(&rows[n], ROW_NUMBER, number, title, NULL,
                         strcmp(name, number) != 0 ? name : "A new conversation");
        chatkit_row_icon(&rows[n], LV_SYMBOL_EDIT);
        rows[n++].avatar = CHATKIT_AVATAR_HOLLOW;
    }

    static EXT_RAM_BSS_ATTR uint32_t found[BOOK_ROWS];
    static EXT_RAM_BSS_ATTR pb_contact_t person;
    uint32_t matched = rs_phonebook_search(query, found, BOOK_ROWS);
    if (matched > BOOK_ROWS) matched = BOOK_ROWS;
    for (uint32_t i = 0; i < matched && n < max; i++) {
        if (!rs_phonebook_get(found[i], &person)) continue;
        for (int k = 0; k < person.number_count && n < max; k++) {
            char detail[64];
            snprintf(detail, sizeof detail, "%s  %s", person.numbers[k].label, person.numbers[k].number);
            if (!sms_number(person.numbers[k].number, number, sizeof number)) continue;
            chatkit_fill_row(&rows[n], ROW_PERSON, number, person.name, person.starred ? LV_SYMBOL_OK : NULL,
                             detail);
            rows[n++].avatar = CHATKIT_AVATAR_SOLID;
        }
    }

    if (n == 0 && n < max) {
        const char *why = rs_phonebook_loading() ? "Reading the contacts on the card..."
                          : query[0]             ? "No contact matches"
                                                 : "No contacts.vcf on the SD card";
        chatkit_fill_row(&rows[n], ROW_NOTE, NULL, why, NULL, "Type a number above to text it");
        chatkit_row_icon(&rows[n], TC_ICON_PERSON);
        rows[n++].avatar = CHATKIT_AVATAR_HOLLOW;
    }
    return n;
}

static void contact_chosen(const chatkit_row_t *r)
{
    if (r->kind != ROW_NUMBER && r->kind != ROW_PERSON) return;
    open_number(r->key, r->kind == ROW_PERSON ? r->title : NULL);
}

/* ------------------------------------------------------------------ */
/* A conversation                                                       */
/* ------------------------------------------------------------------ */

static void describe(const chatkit_conv_t *c, char *mono, size_t mono_n, char *sub, size_t sub_n, bool *group)
{
    *group = false;
    /* A number with no name is a person not yet known. */
    if (!c->title[0] || strcmp(c->title, c->conv) == 0) snprintf(mono, mono_n, "%s", TC_ICON_PERSON);
    char number[CHATKIT_ADDR];
    if (!sms_number(c->conv, number, sizeof number)) snprintf(sub, sub_n, "Takes no replies");
    else if (strcmp(c->title, c->conv) != 0 && c->title[0]) snprintf(sub, sub_n, "%s", c->conv);
    else snprintf(sub, sub_n, "Not in your contacts");
}

static uint32_t text_limit(const char *conv)
{
    (void)conv;
    return SMS_MAX;
}

static esp_err_t call(const char *conv)
{
    char number[CHATKIT_ADDR];
    if (!sms_number(conv, number, sizeof number)) return ESP_ERR_INVALID_ARG;
    if (!rs_call_available()) {
        toast_warn("Calls need the modem on");
        return ESP_ERR_INVALID_STATE;
    }
    return rs_call_dial(number);
}

static const chatkit_app_t s_app = {
    .tag             = "sms",
    .name            = "SMS",
    .contacts_tab    = "Contacts",
    .chats_icon      = LV_SYMBOL_ENVELOPE,
    .contacts_icon   = TC_ICON_PERSON,
    .empty_title     = "No messages yet",
    .empty_text      = "Text a contact or a number. SMS that arrive while the app is closed wait here.",
    .search_hint     = "Name or number",
    .alert_transport = MSG_ALERT_SMS,
    .store           = &s_store,
    .poll            = poll,
    .read_history    = read_history,
    .why_not         = why_not,
    .contacts_seq    = contacts_seq,
    .contacts        = contacts,
    .contact_chosen  = contact_chosen,
    .settings        = NULL,
    .setting_chosen  = NULL,
    .own_card        = NULL,
    .describe        = describe,
    .send            = send,
    .text_limit      = text_limit,
    .call            = call,
    .held_unread     = rs_sms_set_held_unread,
};

esp_err_t sms_ui_create(lv_obj_t *parent)
{
    return chatkit_ui_create(parent, &s_app);
}

void sms_ui_destroy(void)
{
    chatkit_ui_destroy();
}
