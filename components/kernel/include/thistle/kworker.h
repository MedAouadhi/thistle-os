/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the kernel worker
 *
 * One thread, with a 16 KB stack in internal RAM, that runs short jobs one
 * after another, for code that would otherwise keep a thread of its own
 * waiting most of the time. Jobs run one at a time, so its stack has to fit
 * the deepest job rather than all of them together.
 *
 * A job must return promptly: nothing else runs on the worker meanwhile.
 * Anything that waits on a network or a modem for long belongs elsewhere.
 * None of these may be called from an interrupt. Implemented in
 * components/kernel_rs/src/kworker.rs.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*kworker_fn_t)(void *arg);

/* Start the worker. kernel_init() does this before any driver starts; jobs
 * added earlier wait for it. */
esp_err_t kworker_start(void);

/* Run fn(arg) once on the worker, as soon as it is free. Returns the job's
 * id, or 0 if fn is NULL. */
uint32_t kworker_post(kworker_fn_t fn, void *arg);

/* Run fn(arg) on the worker now, and then period_ms after each run ends,
 * until cancelled. Returns the job's id, or 0 if fn is NULL or period_ms 0. */
uint32_t kworker_every(kworker_fn_t fn, void *arg, uint32_t period_ms);

/* Stop a job. Once this returns the job is not running and will not run
 * again; called from a job, which may be the one stopped, it only stops it
 * running again. ESP_ERR_NOT_FOUND if there is no such job. */
esp_err_t kworker_cancel(uint32_t id);

/* Whether the caller is running on the worker. */
bool kworker_is_current(void);

#ifdef __cplusplus
}
#endif
