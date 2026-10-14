// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — voice calls on the A7682E (the call functions of hal_net_driver_t)
//
// The module places, answers and ends calls on AT commands (A76XX Series AT
// Command Manual V1.09, 2.2 and chapter 7) and carries their audio itself:
// its own microphone, and a speaker output the board's audio switch hands to
// the loudspeaker. That switch is the audio driver's (set_route); nothing here
// touches the speaker.
//
// A call needs the module answering commands, which it does not do during a
// data session. The first call command leaves data mode, as an SMS exchange
// does (ADR-0004), and from then on a call "holds the line": connect() dials
// no session and the module is not put to sleep. Only call_list() lets the
// line go, once AT+CLCC lists no call, and it then resumes the session a call
// interrupted. The kernel polls it while a call is up.
//
// What happens to a call is learned from AT+CLCC alone. The module's
// unsolicited lines (RING, +CLIP, VOICE CALL: BEGIN, NO CARRIER) are never
// read: esp_modem's URC hook is compiled out, a data session would hide them,
// and they are thrown away before each call command (a7682e_call_exchange_
// locked()). AT+CLIP is left alone for the same reason: it only adds +CLIP
// lines after each RING (7.2.10), and AT+CLCC names the caller without it.

#include "a7682e_call_validation.h"
#include "a7682e_internal.h"
#include "a7682e_lifecycle.h"
#include "a7682e_parse.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "a7682e_call";

/* ATD and ATA answer OK once the module has taken them, and the call follows
 * (2.2.1, 2.2.2); the manual gives no bound, so this is generous. */
#define CALL_DIAL_MS      10000
/* AT+CHUP, AT+CLCC, AT+VTS, AT+CMUT, AT+CSDVC, AT+COUTGAIN: "Max Response
 * Time 9000ms" (7.2). */
#define CALL_COMMAND_MS   9000

/* RI held low this long is a call ringing: an SMS pulls it low for 120 ms, a
 * URC for 60 ms (A7682E Hardware Design V1.05, 3.3.1). */
#define RING_LOW_US       (250 * 1000)
/* A fall the interrupt saw is taken as the start of the low level only this
 * long after it. The pin's interrupt is off while the power manager has it as
 * a wake source, and a fall recorded before that may be long stale. */
#define RING_EDGE_TRUST_US (3000 * 1000)

static struct {
    atomic_bool holds_line;  // from a call command until AT+CLCC lists none
    bool        suspended;   // a call command left a data session, to resume
    int         listed;      // calls the last AT+CLCC listed, for the log
} s_call;

bool a7682e_call_holds_line(void)
{
    return atomic_load(&s_call.holds_line);
}

void a7682e_call_forget_locked(void)
{
    if (atomic_exchange(&s_call.holds_line, false) || s_call.suspended) {
        ESP_LOGI(TAG, "call: forgotten as the modem powers off");
    }
    s_call.suspended = false;
    s_call.listed = 0;
}

/* An answer on one line, for the log. */
static void one_line(char *s)
{
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') *s = ' ';
    }
}

/* Take the driver for a call command: refused while the module is off or
 * asleep. The line is held from here until call_list() finds no call, and a
 * data session is left, to be resumed then. */
static esp_err_t begin_call_command(void)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!a7682e_dce_locked() || a7682e_asleep_locked()) {
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_call.holds_line, true);
    if (a7682e_enter_command_locked()) {
        s_call.suspended = true;
        ESP_LOGI(TAG, "call: data session suspended");
    }
    return ESP_OK;
}

/* ATD, ATA or AT+CHUP: one exchange, the driver taken for it. */
static esp_err_t call_command(const char *cmd, uint32_t timeout_ms, bool dial)
{
    esp_err_t ret = begin_call_command();
    if (ret != ESP_OK) {
        return ret;
    }
    char resp[A7682E_EXCHANGE_BUF];
    ret = a7682e_call_exchange_locked(cmd, resp, sizeof(resp), timeout_ms, dial);
    a7682e_lifecycle_finish_operation();
    if (ret != ESP_OK) {
        one_line(resp);
        ESP_LOGW(TAG, "call: %.*s failed (%s): %s", (int)strcspn(cmd, "\r"), cmd,
                 esp_err_to_name(ret), resp);
    }
    return ret;
}

/* A setting of the call's audio, which the module takes in a call or out of
 * one. A data session is left for it and resumed at once, as an SMS is sent:
 * in a call there is none. The line is not held for it. */
static esp_err_t call_setting(const char *cmd)
{
    if (!a7682e_lifecycle_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!a7682e_dce_locked() || a7682e_asleep_locked()) {
        a7682e_lifecycle_finish_operation();
        return ESP_ERR_INVALID_STATE;
    }
    bool was_data = a7682e_enter_command_locked();
    char resp[A7682E_EXCHANGE_BUF];
    esp_err_t ret = a7682e_call_exchange_locked(cmd, resp, sizeof(resp), CALL_COMMAND_MS, false);
    a7682e_restore_data_locked(was_data);
    a7682e_lifecycle_finish_operation();
    if (ret != ESP_OK) {
        one_line(resp);
        ESP_LOGW(TAG, "call: %.*s failed (%s): %s", (int)strcspn(cmd, "\r"), cmd,
                 esp_err_to_name(ret), resp);
    }
    return ret;
}

/* -------------------------------------------------------------------------
 * hal_net_driver_t
 * ---------------------------------------------------------------------- */

esp_err_t a7682e_call_dial(const char *number)
{
    if (!a7682e_call_number_is_valid(number)) {
        ESP_LOGE(TAG, "call: refusing a number ATD cannot carry");
        return ESP_ERR_INVALID_ARG;
    }
    /* The ';' makes it a voice call (2.2.1). */
    char cmd[HAL_CALL_NUMBER_MAX + 8];
    snprintf(cmd, sizeof(cmd), "ATD%s;\r", number);
    ESP_LOGI(TAG, "call: dialling %s", number);
    return call_command(cmd, CALL_DIAL_MS, true);
}

esp_err_t a7682e_call_answer(void)
{
    ESP_LOGI(TAG, "call: answering");
    return call_command("ATA\r", CALL_DIAL_MS, true);
}

/* AT+CHUP ends every call whatever AT+CVHU says, which ATH does not (7.2.2,
 * 2.2.3), and answers OK with no call to end. */
esp_err_t a7682e_call_hangup(void)
{
    ESP_LOGI(TAG, "call: hanging up");
    return call_command("AT+CHUP\r", CALL_COMMAND_MS, false);
}

esp_err_t a7682e_call_list(hal_call_t *calls, int max, int *count)
{
    if (!count || max < 0 || (max > 0 && !calls)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    esp_err_t ret = begin_call_command();
    if (ret != ESP_OK) {
        return ret;
    }

    char resp[A7682E_EXCHANGE_BUF];
    ret = a7682e_call_exchange_locked("AT+CLCC\r", resp, sizeof(resp), CALL_COMMAND_MS, false);
    int listed = ret == ESP_OK ? a7682e_parse_clcc(resp, calls, max) : -1;
    if (ret == ESP_OK && listed < 0) {
        one_line(resp);
        ESP_LOGW(TAG, "call: AT+CLCC answer not understood: %s", resp);
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    /* An answer that could not be read keeps the line: it says nothing about
     * whether a call is up. */
    bool resumed = false;
    if (listed == 0) {
        atomic_store(&s_call.holds_line, false);
        if (s_call.suspended) {
            s_call.suspended = false;
            a7682e_restore_data_locked(true);
            resumed = true;
        }
    }
    if (listed >= 0) {
        *count = listed < max ? listed : max;
        if (listed != s_call.listed || resumed) {
            if (listed > 0) {
                ESP_LOGI(TAG, "call: %d call(s) listed", listed);
            } else {
                ESP_LOGI(TAG, "call: no calls%s", resumed ? "; data session resumed" : "");
            }
        }
        s_call.listed = listed;
    }
    a7682e_lifecycle_finish_operation();
    return ret;
}

/* RI is read, nothing else: no driver, no UART. It is low for every SMS and
 * URC too, but only briefly, so a call is RI low across two looks a quarter
 * second apart, or since a fall the interrupt saw at least that long ago.
 * The interrupt is not re-armed here: the power manager turns it off to have
 * the pin wake the SoC, and re-arming it under the power manager's feet
 * would break that wake. */
bool a7682e_call_ringing(void)
{
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    static int64_t low_since;  // the first look of an unbroken run that found RI low, or 0
    static int64_t high_seen;  // the last look that found it high

    int ri = a7682e_ri_pin();
    if (ri < 0) {
        return false;
    }
    bool low = gpio_get_level((gpio_num_t)ri) == 0;
    int64_t fell = 0;
    bool fell_seen = a7682e_ri_fell_at(&fell);
    int64_t now = esp_timer_get_time();

    bool ringing = false;
    taskENTER_CRITICAL(&lock);
    if (!low) {
        low_since = 0;
        high_seen = now;
    } else {
        if (low_since == 0) {
            low_since = now;
        }
        if (now - low_since >= RING_LOW_US) {
            ringing = true;
        } else if (fell_seen && fell > high_seen) {
            /* No look has found the pin high since this fall, so it has been
             * low since then, unless it rose and fell again unseen; a fall
             * only seconds old leaves no time for that to matter. */
            int64_t low_for = now - fell;
            ringing = low_for >= RING_LOW_US && low_for <= RING_EDGE_TRUST_US;
        }
    }
    taskEXIT_CRITICAL(&lock);
    return ringing;
}

esp_err_t a7682e_call_dtmf(char digit)
{
    if (!a7682e_call_dtmf_is_valid(digit)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The tone lasts as long as AT+VTD says, 100 ms by default (7.2.15). */
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "AT+VTS=%c\r", digit);
    ESP_LOGI(TAG, "call: DTMF %c", digit);
    return call_setting(cmd);
}

/* Only in a connected call; the module unmutes by itself once every call has
 * ended (7.2.19). */
esp_err_t a7682e_call_set_mute(bool mute)
{
    ESP_LOGI(TAG, "call: microphone %s", mute ? "muted" : "on");
    return call_setting(mute ? "AT+CMUT=1\r" : "AT+CMUT=0\r");
}

/* Takes effect at once in a call, otherwise at the next, and the module
 * keeps it across a power-off (AUTO_SAVE, 7.2.22). */
esp_err_t a7682e_call_set_volume(uint8_t percent)
{
    int gain = a7682e_call_gain_for_volume(percent);
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+COUTGAIN=%d\r", gain);
    ESP_LOGI(TAG, "call: volume %u%%, gain %d of 7", (unsigned)(percent > 100 ? 100 : percent),
             gain);
    return call_setting(cmd);
}

/* Speaker phone (3) or handset (1), the only two the module lists (7.2.20).
 * The board has no earpiece: handset is for headphones. */
esp_err_t a7682e_call_set_speaker(bool speaker)
{
    ESP_LOGI(TAG, "call: audio set for %s", speaker ? "the loudspeaker" : "headphones");
    return call_setting(speaker ? "AT+CSDVC=3\r" : "AT+CSDVC=1\r");
}
