/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS Simulator — a cellular modem
 *
 * A hal_net_driver_t of type HAL_NET_CELLULAR standing in for the A7682E on
 * the boards that carry one, so Settings' mobile data page and the network
 * coordinator can be driven without hardware. It carries no traffic: the
 * host network does that. --sim-card picks what the "SIM" does, and --sms
 * puts messages in its storage for the kernel to fetch.
 *
 * Voice calls are a small state machine, moved on by the clock whenever the
 * kernel asks anything (it asks every second for RI, and every 500 ms while a
 * call is up). --call rings with an incoming call 3 s after the modem first
 * registers, which gives up after 20 s unanswered. A number dialled rings at
 * the far end after 1 s and is answered 2 s later, except one ending in
 * 0000, which is busy: it is gone after 2 s. --call-hangup has the far end
 * hang up that long after a call connects.
 */

#include "hal/network.h"
#include "thistle/net_manager.h"
#include "esp_err.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static hal_sim_state_t s_card = HAL_SIM_READY;

/* SMS in the "modem's" storage, from --sms, until the kernel keeps them. */
#define SIM_MAX_SMS 8
static hal_sms_t s_sms[SIM_MAX_SMS];
static int s_sms_count;
static atomic_int  s_state = HAL_NET_STATE_DISCONNECTED;
static atomic_bool s_abort;
static hal_cell_info_t s_info;

/* Calls: the kernel's call thread and its kworker both ask. */
#define RING_AFTER_REGISTERED_MS 3000
#define RING_GIVES_UP_MS        20000
#define DIALING_MS               1000
#define ALERTING_MS              2000
#define BUSY_AFTER_MS            2000

typedef enum {
    CALL_NONE,
    CALL_RINGING,    /* incoming, ringing here */
    CALL_DIALING,
    CALL_ALERTING,
    CALL_CONNECTED,
} sim_call_phase_t;

static pthread_mutex_t s_call_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    sim_call_phase_t phase;
    bool     incoming;
    bool     busy;            /* dialled a number ending in 0000 */
    char     number[33];
    int64_t  since_ms;        /* when this phase began */
} s_call;
static bool    s_ring_wanted;          /* --call, not yet rung */
static char    s_ring_number[33];
static int64_t s_registered_ms;        /* first registration; 0 before */
static int     s_hangup_after_s = -1;  /* --call-hangup; -1: never */
static atomic_bool s_has_rung;
static atomic_bool s_has_registered;

/* "ready" (the default), "absent", "pin" or "puk". */
void sim_cellular_set_card(const char *card)
{
    if (strcmp(card, "absent") == 0)   s_card = HAL_SIM_ABSENT;
    else if (strcmp(card, "pin") == 0) s_card = HAL_SIM_PIN_REQUIRED;
    else if (strcmp(card, "puk") == 0) s_card = HAL_SIM_PUK_REQUIRED;
    else                               s_card = HAL_SIM_READY;
}

/* A step of a few seconds, as the modem takes, cut short by disconnect. */
static bool wait_unless_aborted(int seconds)
{
    for (int i = 0; i < seconds * 4; i++) {
        if (atomic_load(&s_abort)) return false;
        usleep(250 * 1000);
    }
    return !atomic_load(&s_abort);
}

/* --call: ring with an incoming call from `number` ("" for one withheld) 3 s
 * after the modem first registers. */
void sim_cellular_set_incoming_call(const char *number)
{
    pthread_mutex_lock(&s_call_lock);
    snprintf(s_ring_number, sizeof(s_ring_number), "%s", number);
    s_ring_wanted = true;
    pthread_mutex_unlock(&s_call_lock);
}

/* --call-hangup: the far end hangs up `seconds` after a call connects. */
void sim_cellular_set_remote_hangup(int seconds)
{
    pthread_mutex_lock(&s_call_lock);
    s_hangup_after_s = seconds;
    pthread_mutex_unlock(&s_call_lock);
}

/* For the simulator's --tap-start: whether the modem has registered, and
 * whether the --call has started ringing. */
bool sim_cellular_has_registered(void) { return atomic_load(&s_has_registered); }
bool sim_cellular_has_rung(void)       { return atomic_load(&s_has_rung); }

/* "SENDER: TEXT", held in storage from the start. */
bool sim_cellular_add_sms(const char *spec)
{
    const char *colon = strchr(spec, ':');
    if (!colon || s_sms_count >= SIM_MAX_SMS) return false;
    hal_sms_t *m = &s_sms[s_sms_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->sender, sizeof(m->sender), "%.*s", (int)(colon - spec), spec);
    const char *text = colon + 1;
    while (*text == ' ') text++;
    snprintf(m->text, sizeof(m->text), "%s", text);
    snprintf(m->sent_at, sizeof(m->sent_at), "25/10/13,12:34:56+08");
    return true;
}

/* Power, the SIM and a registration, a few seconds each as on a modem. */
static esp_err_t attach(void)
{
    if (s_info.powered && s_info.sim == HAL_SIM_READY && s_info.reg == HAL_CELL_REG_HOME) {
        return ESP_OK;
    }
    memset(&s_info, 0, sizeof(s_info));
    s_info.powered = true;
    if (!wait_unless_aborted(2)) return ESP_ERR_TIMEOUT;
    s_info.sim = s_card;
    if (s_card != HAL_SIM_READY) return ESP_ERR_NOT_FOUND;

    s_info.reg = HAL_CELL_REG_SEARCHING;
    s_info.rssi_dbm = -97;
    if (!wait_unless_aborted(2)) return ESP_ERR_TIMEOUT;
    s_info.reg = HAL_CELL_REG_HOME;
    s_info.rssi_dbm = -79;
    snprintf(s_info.operator_name, sizeof(s_info.operator_name), "Thistle Mobile");
    return ESP_OK;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void note_registered(void)
{
    pthread_mutex_lock(&s_call_lock);
    if (s_registered_ms == 0) s_registered_ms = now_ms();
    pthread_mutex_unlock(&s_call_lock);
    atomic_store(&s_has_registered, true);
}

static void call_ended_locked(const char *how)
{
    printf("[sim_cellular] %s\n", how);
    printf("[sim_cellular] ended\n");
    s_call.phase = CALL_NONE;
}

/* Move the calls on as far as the clock says, each phase from when the last
 * should have ended, however late the kernel asks. */
static void advance_locked(void)
{
    int64_t now = now_ms();
    for (;;) {
        int64_t in = now - s_call.since_ms;
        switch (s_call.phase) {
        case CALL_NONE:
            if (!s_ring_wanted || s_registered_ms == 0 || !(s_info.powered && s_info.sim == HAL_SIM_READY)) {
                return;
            }
            if (now - s_registered_ms < RING_AFTER_REGISTERED_MS) return;
            s_ring_wanted = false;
            s_call.phase = CALL_RINGING;
            s_call.incoming = true;
            s_call.busy = false;
            snprintf(s_call.number, sizeof(s_call.number), "%s", s_ring_number);
            s_call.since_ms = now;
            atomic_store(&s_has_rung, true);
            printf("[sim_cellular] call ringing from %s\n",
                   s_call.number[0] ? s_call.number : "a withheld number");
            return;
        case CALL_RINGING:
            if (in < RING_GIVES_UP_MS) return;
            call_ended_locked("caller gave up");
            return;
        case CALL_DIALING:
            if (s_call.busy) {
                if (in < BUSY_AFTER_MS) return;
                call_ended_locked("busy");
                return;
            }
            if (in < DIALING_MS) return;
            s_call.phase = CALL_ALERTING;
            s_call.since_ms += DIALING_MS;
            printf("[sim_cellular] alerting\n");
            break;
        case CALL_ALERTING:
            if (in < ALERTING_MS) return;
            s_call.phase = CALL_CONNECTED;
            s_call.since_ms += ALERTING_MS;
            printf("[sim_cellular] active\n");
            break;
        case CALL_CONNECTED:
            if (s_hangup_after_s < 0 || in < (int64_t)s_hangup_after_s * 1000) return;
            call_ended_locked("far end hung up");
            return;
        }
    }
}

static esp_err_t cell_listen(uint32_t timeout_ms)
{
    (void)timeout_ms;
    atomic_store(&s_abort, false);
    printf("[sim_cellular] registering\n");
    esp_err_t ret = attach();
    if (ret == ESP_OK) {
        printf("[sim_cellular] registered\n");
        note_registered();
    }
    return ret;
}

static esp_err_t cell_connect(const char *apn, const char *credential, uint32_t timeout_ms)
{
    (void)credential;
    (void)timeout_ms;
    atomic_store(&s_abort, false);
    atomic_store(&s_state, HAL_NET_STATE_CONNECTING);
    printf("[sim_cellular] connecting, APN \"%s\"\n", apn ? apn : "");

    esp_err_t ret = attach();
    if (ret == ESP_ERR_NOT_FOUND) {
        atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
        return ret;
    }
    if (ret != ESP_OK || !wait_unless_aborted(1)) goto fail;

    atomic_store(&s_state, HAL_NET_STATE_CONNECTED);
    printf("[sim_cellular] connected\n");
    note_registered();
    return ESP_OK;

fail:
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cell_disconnect(void)
{
    atomic_store(&s_abort, true);
    atomic_store(&s_state, HAL_NET_STATE_DISCONNECTED);
    pthread_mutex_lock(&s_call_lock);
    if (s_call.phase != CALL_NONE) call_ended_locked("modem off with a call up");
    memset(&s_info, 0, sizeof(s_info));
    pthread_mutex_unlock(&s_call_lock);
    printf("[sim_cellular] modem off\n");
    return ESP_OK;
}

static hal_net_state_t cell_get_state(void)
{
    return (hal_net_state_t)atomic_load(&s_state);
}

static bool cell_is_connected(void)
{
    return atomic_load(&s_state) == HAL_NET_STATE_CONNECTED;
}

static const char *cell_get_ip(void)
{
    return cell_is_connected() ? "10.64.0.2" : NULL;
}

static int8_t cell_get_rssi(void)
{
    return (int8_t)s_info.rssi_dbm;
}

static esp_err_t cell_get_info(hal_cell_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_info;
    return ESP_OK;
}

static bool cell_ready(void)
{
    return s_info.powered && s_info.sim == HAL_SIM_READY;
}

static esp_err_t cell_sms_send(const char *number, const char *text)
{
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    printf("[sim_cellular] SMS to %s: %s\n", number, text);
    return ESP_OK;
}

static bool cell_sms_due(void)
{
    return cell_ready() && s_sms_count > 0;
}

static esp_err_t cell_sms_fetch(hal_sms_rx_cb_t cb, void *user_data, int *kept)
{
    *kept = 0;
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    int left = 0;
    for (int i = 0; i < s_sms_count; i++) {
        if (cb(&s_sms[i], user_data)) {
            (*kept)++;
        } else {
            s_sms[left++] = s_sms[i];
        }
    }
    s_sms_count = left;
    printf("[sim_cellular] SMS fetched: %d\n", *kept);
    return ESP_OK;
}

/* The network's time, as a network that sends it would give it once the
 * "modem" is registered: the host's clock and the host's offset. */
static esp_err_t cell_get_network_time(hal_net_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!cell_ready()
        || (s_info.reg != HAL_CELL_REG_HOME && s_info.reg != HAL_CELL_REG_ROAMING)) {
        return ESP_ERR_NOT_FOUND;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    out->utc = (int64_t)now;
    out->age_ms = 0;
    out->utc_offset_min = (int16_t)(local.tm_gmtoff / 60);
    out->offset_known = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Voice calls                                                          */
/* ------------------------------------------------------------------ */

/* As the A7682E driver takes them: an optional '+', then digits, '*' and '#',
 * up to HAL_CALL_NUMBER_MAX. */
static bool dial_string_valid(const char *n)
{
    if (!n) return false;
    size_t len = strlen(n);
    if (len == 0 || len > HAL_CALL_NUMBER_MAX) return false;
    if (*n == '+') n++;
    if (!*n) return false;
    for (; *n; n++) {
        if (!((*n >= '0' && *n <= '9') || *n == '*' || *n == '#')) return false;
    }
    return true;
}

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && strcmp(s + n - t, tail) == 0;
}

static esp_err_t cell_call_dial(const char *number)
{
    if (!dial_string_valid(number)) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    if (!cell_ready()) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_call.phase != CALL_NONE) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_call.phase = CALL_DIALING;
        s_call.incoming = false;
        s_call.busy = ends_with(number, "0000");
        snprintf(s_call.number, sizeof(s_call.number), "%s", number);
        s_call.since_ms = now_ms();
        printf("[sim_cellular] dialing %s\n", number);
    }
    pthread_mutex_unlock(&s_call_lock);
    return ret;
}

static esp_err_t cell_call_answer(void)
{
    esp_err_t ret = ESP_OK;
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    if (!cell_ready() || s_call.phase != CALL_RINGING) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_call.phase = CALL_CONNECTED;
        s_call.since_ms = now_ms();
        printf("[sim_cellular] answered\n");
        printf("[sim_cellular] active\n");
    }
    pthread_mutex_unlock(&s_call_lock);
    return ret;
}

/* AT+CHUP answers OK with no call too. */
static esp_err_t cell_call_hangup(void)
{
    esp_err_t ret = ESP_OK;
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    if (!cell_ready()) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_call.phase != CALL_NONE) {
        call_ended_locked(s_call.phase == CALL_RINGING ? "declined" : "hung up");
    }
    pthread_mutex_unlock(&s_call_lock);
    return ret;
}

static esp_err_t cell_call_list(hal_call_t *calls, int max, int *count)
{
    if (!count || (max > 0 && !calls)) return ESP_ERR_INVALID_ARG;
    *count = 0;
    esp_err_t ret = ESP_OK;
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    if (!cell_ready()) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_call.phase != CALL_NONE && max > 0) {
        hal_call_t *c = &calls[0];
        memset(c, 0, sizeof(*c));
        c->id = 1;
        c->incoming = s_call.incoming;
        c->state = s_call.phase == CALL_RINGING  ? HAL_CALL_INCOMING
                 : s_call.phase == CALL_DIALING  ? HAL_CALL_DIALING
                 : s_call.phase == CALL_ALERTING ? HAL_CALL_ALERTING
                                                 : HAL_CALL_ACTIVE;
        snprintf(c->number, sizeof(c->number), "%s", s_call.number);
        *count = 1;
    }
    pthread_mutex_unlock(&s_call_lock);
    return ret;
}

/* RI: low for as long as a call rings here. */
static bool cell_call_ringing(void)
{
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    bool ringing = cell_ready() && s_call.phase == CALL_RINGING;
    pthread_mutex_unlock(&s_call_lock);
    return ringing;
}

static esp_err_t cell_call_dtmf(char digit)
{
    bool valid = (digit >= '0' && digit <= '9') || digit == '*' || digit == '#' ||
                 (digit >= 'A' && digit <= 'D');
    if (!valid) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    pthread_mutex_lock(&s_call_lock);
    advance_locked();
    if (!cell_ready() || s_call.phase != CALL_CONNECTED) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        printf("[sim_cellular] dtmf %c\n", digit);
    }
    pthread_mutex_unlock(&s_call_lock);
    return ret;
}

static esp_err_t cell_call_set_mute(bool mute)
{
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    printf("[sim_cellular] mute %s\n", mute ? "on" : "off");
    return ESP_OK;
}

static esp_err_t cell_call_set_volume(uint8_t percent)
{
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;
    printf("[sim_cellular] volume %u%%\n", (unsigned)percent);
    return ESP_OK;
}

static esp_err_t cell_call_set_speaker(bool speaker)
{
    if (!cell_ready()) return ESP_ERR_INVALID_STATE;
    printf("[sim_cellular] speaker %s\n", speaker ? "on" : "off");
    return ESP_OK;
}

static const hal_net_driver_t s_sim_cellular = {
    .type          = HAL_NET_CELLULAR,
    .name          = "LTE",
    .connect       = cell_connect,
    .disconnect    = cell_disconnect,
    .get_state     = cell_get_state,
    .get_ip        = cell_get_ip,
    .get_rssi      = cell_get_rssi,
    .is_connected  = cell_is_connected,
    .get_cell_info = cell_get_info,
    .listen        = cell_listen,
    .sms_send      = cell_sms_send,
    .sms_due       = cell_sms_due,
    .sms_fetch     = cell_sms_fetch,
    .get_network_time = cell_get_network_time,
    .call_dial        = cell_call_dial,
    .call_answer      = cell_call_answer,
    .call_hangup      = cell_call_hangup,
    .call_list        = cell_call_list,
    .call_ringing     = cell_call_ringing,
    .call_dtmf        = cell_call_dtmf,
    .call_set_mute    = cell_call_set_mute,
    .call_set_volume  = cell_call_set_volume,
    .call_set_speaker = cell_call_set_speaker,
};

esp_err_t sim_cellular_register(void)
{
    return net_manager_register(&s_sim_cellular);
}
