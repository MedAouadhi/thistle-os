/*
 * Matching a board's fingerprint — the set of I2C addresses whose presence on
 * a given pin pair identifies a board — against the embedded fallback profiles.
 *
 * Kept apart from the probing in board_fallback.c so the rules deciding which
 * board a freshly flashed device believes it is can be exercised without
 * hardware.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdbool.h>

/* Addresses probed when fingerprinting. */
#define BOARD_ADDR_CST816    0x15   /* CST816 capacitive touch (T-Display-S3) */
#define BOARD_ADDR_CST328    0x1A   /* CST328 capacitive touch */
#define BOARD_ADDR_XL9555    0x20   /* XL9555 IO expander (T-Deck Max only) */
#define BOARD_ADDR_BHI260AP  0x28   /* BHI260AP IMU (T-Deck Pro and Max) */
#define BOARD_ADDR_TCA8418   0x34   /* TCA8418 keyboard controller */
#define BOARD_ADDR_SSD1306   0x3C   /* SSD1306 OLED display */
#define BOARD_ADDR_CARDKB    0x5F   /* CardKB keyboard (Cardputer) */

/* What answered on one I2C pin pair. */
typedef struct {
    int  sda;
    int  scl;
    bool has_tca8418;
    bool has_cst328;
    bool has_bhi260ap;
    bool has_cst816;
    bool has_ssd1306;
    bool has_cardkb;
    bool has_xl9555;
} board_scan_t;

/* An embedded fallback profile: a board profile duplicated as a string literal
 * so a board with no profile present has something to be given. */
typedef struct {
    const char *board_id;
    const char *json;
} board_profile_t;

/* Whether anything at all answered on this pin pair. */
bool board_scan_found_anything(const board_scan_t *scan);

/* The profile for the board this scan identifies, or NULL when the scan
 * matches nothing or is ambiguous. Ambiguity resolves to NULL deliberately:
 * a board that comes up in safe mode is recoverable, one that comes up
 * confidently as the wrong board is not. */
const board_profile_t *board_fingerprint_match(const board_scan_t *scan);

/* The profile to assume when nothing answered on any pin pair. */
const board_profile_t *board_fingerprint_headless(void);
