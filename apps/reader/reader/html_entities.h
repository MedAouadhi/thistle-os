/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — the HTML named entities an EPUB chapter may use.
 * See html_entities.c for where the table comes from.
 */
#pragma once

#include <stddef.h>

/* The UTF-8 text of `entity`, written with its '&' and ';' ("&mdash;", len 7),
 * or NULL if it is not one the table knows. */
const char *html_entity_lookup(const char *entity, size_t len);

/* For the test that the table is sorted. */
size_t html_entity_count(void);
const char *html_entity_key(size_t i);
