/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's text, in characters this device can draw.
 *
 * The UI's fonts are Montserrat's ASCII, the degree sign and the bullet,
 * LVGL's symbols, and Arabic (components/ui/src/arabic_text.c). Hermes
 * writes for screens with every font, and a character no font has is drawn
 * as a box. So, as hermes_esp32's Markdown view does:
 *
 * - punctuation with a faithful ASCII spelling is written that way: dashes,
 *   quotes, the ellipsis, odd spaces, arrows, a few signs;
 * - accented Latin letters lose their accents: "Größe" is "Grosse";
 * - an emoji with an LVGL symbol becomes it (check, cross, warning, up and
 *   down triangles); other emoji, and the joiners, selectors and skin
 *   tones that make them up, are dropped;
 * - anything else is '?', an honest "this screen cannot show that".
 *
 * LVGL's own symbols, which the app draws its marks with, pass as they are.
 *
 * For drawing only: what is sent and stored is left as it was.
 */
#pragma once

#include <stddef.h>

/* Writes `in` (len bytes of UTF-8) into `out` as drawable text, cut between
 * characters to fit `cap` with its NUL. Returns the length written. */
size_t hermes_glyphs(const char *in, size_t len, char *out, size_t cap);
