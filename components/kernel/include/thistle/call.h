// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Voice calls, as the kernel runs them (call_manager.rs, call_log.rs).
 *
 * The kernel notices a call ringing by the modem's RI line, which the modem
 * holds low until the call is answered or given up, and follows every call
 * by asking the modem for its calls. While a call is up it has the device's
 * audio and keeps the device awake: it rings through the speaker and buzzes
 * the motor, the speaker goes over to the modem once it connects, players
 * pause (rs_call_in_progress), and a data session is suspended until it
 * ends. Each call goes into the call log on the SD card.
 *
 * Every function here returns at once: the work with the modem is done on
 * the kernel's call thread, and the UI follows it by rs_call_get_status(). */

#define CALL_NUMBER_MAX 33   /* "+4917612345678", NUL included */
#define CALL_NAME_MAX   64

typedef enum {
    CALL_IDLE = 0,    /* no call */
    CALL_DIALING,     /* outgoing: placed, not yet ringing at the far end */
    CALL_ALERTING,    /* outgoing: ringing at the far end */
    CALL_INCOMING,    /* incoming: ringing here */
    CALL_ACTIVE,      /* connected */
    CALL_ENDED,       /* just ended: kept CALL_ENDED_SHOW_MS for the UI to
                         say how, then CALL_IDLE */
} call_state_t;

#define CALL_ENDED_SHOW_MS 2500

typedef enum {
    CALL_END_NONE = 0,
    CALL_END_LOCAL,     /* hung up here, or an outgoing call cancelled */
    CALL_END_REMOTE,    /* the other side hung up */
    CALL_END_REJECTED,  /* an incoming call declined here */
    CALL_END_MISSED,    /* an incoming call that stopped ringing unanswered */
    CALL_END_FAILED,    /* never connected: no network, no answer, busy, or
                           the modem refused */
} call_end_t;

typedef struct {
    call_state_t state;
    call_end_t   end;                       /* while CALL_ENDED */
    bool         incoming;
    char         number[CALL_NUMBER_MAX];   /* "" when withheld */
    char         name[CALL_NAME_MAX];       /* the phonebook's name for the
                                               number, or "" */
    uint32_t     duration_s;                /* connected time: counting while
                                               CALL_ACTIVE, final once ENDED */
    bool         muted;                     /* the microphone */
    bool         speaker;                   /* loudspeaker on; off, headphones
                                               only. On at the start of each call */
    uint8_t      volume;                    /* the call's volume, percent */
    uint32_t     seq;                       /* changes whenever anything above
                                               but duration_s does */
} call_status_t;

/* Whether this device can place a call now: it has a modem that makes them,
 * on with a SIM ready, and no call is up. The modem stays on for calls and
 * SMS while Settings → Mobile data → "Calls and SMS" is on. */
bool rs_call_available(void);

/* Whether the device has a modem that makes calls at all. */
bool rs_call_supported(void);

/* Place a call to `number`: '+' and digits, '*' and '#'; spaces, dashes,
 * dots and brackets are dropped. ESP_ERR_INVALID_ARG for a number with no
 * digit, or with anything else in it; ESP_ERR_INVALID_STATE with a call
 * already up or the modem not ready; ESP_ERR_NOT_SUPPORTED with no modem
 * that makes calls. */
esp_err_t rs_call_dial(const char *number);

/* Answer the call ringing here. ESP_ERR_INVALID_STATE without one. */
esp_err_t rs_call_answer(void);

/* End the call: hang up, cancel one being placed, or decline one ringing.
 * ESP_ERR_INVALID_STATE without one. */
esp_err_t rs_call_hangup(void);

/* Stop the ringtone and the buzzing of the call ringing here, which goes on
 * ringing for the caller until answered or declined. */
void rs_call_silence(void);

/* A DTMF tone in the connected call: 0-9, '*', '#', 'A'-'D'. */
esp_err_t rs_call_dtmf(char digit);

esp_err_t rs_call_set_mute(bool mute);
esp_err_t rs_call_set_speaker(bool on);
/* The call's volume, 0-100: this call's and the next ones'. */
esp_err_t rs_call_set_volume(uint8_t percent);

void rs_call_get_status(call_status_t *out);

/* Whether a call has the audio: ringing here, being placed, or connected.
 * Anything that plays sound pauses while it does. */
bool rs_call_in_progress(void);

/* ---------------------------------------------------------------------- */
/* The call log: /sdcard/phone/calls.jsonl, one line a call, oldest first.  */
/* ---------------------------------------------------------------------- */

typedef enum {
    CALL_LOG_OUTGOING = 0,
    CALL_LOG_INCOMING = 1,  /* answered */
    CALL_LOG_MISSED   = 2,
    CALL_LOG_REJECTED = 3,
} call_log_kind_t;

typedef struct {
    char     number[CALL_NUMBER_MAX];  /* "" when withheld */
    uint8_t  kind;                     /* call_log_kind_t */
    uint32_t time;                     /* when it began, seconds since 1970,
                                          UTC; 0 if the clock was not set */
    uint32_t duration_s;               /* connected time; 0 if it never was */
} call_log_entry_t;

/* The last CALL_LOG_KEEP calls, newest first: index 0 is the latest. */
#define CALL_LOG_KEEP 100
uint32_t rs_call_log_count(void);
bool rs_call_log_get(uint32_t index, call_log_entry_t *out);

/* Forget every call, on the card too. */
esp_err_t rs_call_log_clear(void);

/* Changes whenever a call is added or the log cleared. */
uint32_t rs_call_log_seq(void);

/* Calls missed since the user last looked at the call log, for the status
 * bar; the Phone app's Recents calls rs_call_missed_seen() when it shows. */
uint32_t rs_call_missed_count(void);
void rs_call_missed_seen(void);
