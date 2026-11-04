/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — background checks
 *
 * A background check is a short, periodic piece of work a kernel module
 * registers to look for news while nothing in front asks for it (ADR-0014).
 * Once its interval has passed, with a network up and the clock set, the
 * kernel runs it on a FreeRTOS task made for it and gone after, holding the
 * device awake meanwhile. A light-sleeping device wakes on a timer for it,
 * brings the network back with the screen left asleep, and sleeps again
 * after; deep sleep runs none.
 *
 * A due check waits up to 90 s for a network and the clock, then is skipped;
 * one that finds less internal RAM free than it declared is skipped at once.
 * A skipped or failed check is tried again an interval later, never sooner.
 * Each run is logged with its name, its outcome, how long it took and the
 * internal RAM free before and after.
 *
 * Kernel modules only: packaged apps have no syscall for it, as their code
 * would have to stay loaded and be called off the UI task.
 * Implemented in components/kernel_rs/src/background_check.rs.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKGROUND_CHECK_DONE = 0,     /* it did its work; detail says what it found */
    BACKGROUND_CHECK_SKIPPED = 1,  /* it did not run; detail says why */
    BACKGROUND_CHECK_FAILED = 2,   /* it ran and failed; detail says why */
    BACKGROUND_CHECK_STOPPED = 3,  /* not to run before stopped_until */
} background_check_result_t;

typedef struct {
    background_check_result_t result;
    int64_t stopped_until;  /* BACKGROUND_CHECK_STOPPED: seconds since 1970 */
    char detail[96];        /* NUL-terminated, for the log */
} background_check_outcome_t;

/* Runs on the check's own task. `out` comes zeroed, which is DONE. */
typedef void (*background_check_fn_t)(void *arg, background_check_outcome_t *out);

/* Register a check: `name` for the log, every `interval_s` seconds (0 is
 * off), on a task with `stack` bytes of stack, once `ram_needed` bytes of
 * internal RAM, the stack among them, are free. First due one interval from
 * now. Returns its id, or 0 if name or fn is NULL. */
uint32_t background_check_register(const char *name, uint32_t interval_s,
                                   uint32_t ram_needed, uint32_t stack,
                                   background_check_fn_t fn, void *arg);

/* How often it runs, counted from its last run; 0 is off.
 * ESP_ERR_NOT_FOUND for no such check. */
esp_err_t background_check_set_interval(uint32_t id, uint32_t interval_s);

/* Hold it back, as while its module holds a live connection, or let it run
 * again; once let go, it runs if its interval has passed. */
esp_err_t background_check_set_paused(uint32_t id, bool paused);

#ifdef __cplusplus
}
#endif
