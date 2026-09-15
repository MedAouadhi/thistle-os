// SPDX-License-Identifier: BSD-3-Clause
//
// The active MIA-M10Q driver implementation lives in
// components/kernel_rs/src/drv_gps_mia_m10q.rs (Rust). It is the one that
// wins the link, because kernel_rs is built with WHOLE_ARCHIVE so its
// strong `drv_gps_mia_m10q_get` symbol overrides any C definition in this
// component. This file is intentionally empty — the C implementation it
// held was a duplicate of the Rust port, and editing it had no runtime
// effect. Emptying it follows drv_kbd_tca8418, which was resolved the same
// way and for the same reason.
//
// The header `drv_gps_mia_m10q.h` is still useful: it defines the
// `gps_mia_m10q_config_t` struct that the legacy compiled-in board
// initialisers (board_tdeck.c, board_tdeck_pro.c) and the JSON-driven
// dispatcher (kernel/src/board_builtin_drivers.c) populate before
// calling drv_gps_mia_m10q_get(). That header stays.
