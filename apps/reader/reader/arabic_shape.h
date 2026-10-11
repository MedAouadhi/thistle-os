/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — Arabic letters in the forms they take joined to their
 * neighbours. See arabic_shape.c for where the rules come from.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Whether `text` has any character from the Arabic block. */
bool arabic_present(const char *text, size_t len);

/* `text` with each Arabic letter replaced by its presentation form (initial,
 * medial, final or isolated) and lam-alef pairs by their ligature, which is
 * how a font draws joined script; everything else is copied as it is. A new
 * string the caller frees with free(), or NULL when out of memory.
 *
 * `offsets`, `offset_count` of them, are positions in `text`, each moved to
 * the same place in the shaped text. */
char *arabic_shape(const char *text, size_t len, size_t *offsets, size_t offset_count,
                   size_t *len_out);
