/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the library calls a book that has not given its title.
 */
#pragma once

#include <stddef.h>

/* The title in a book's file name: the name without its extension or what
 * follows " -- ", where sites that name files "Title -- Author -- Year --
 * Publisher -- …" put the rest, with underscores read as spaces and spaces
 * trimmed from the ends. Cut to fit `out_len` bytes, between characters. */
void book_title_from_file(const char *file_name, char *out, size_t out_len);
