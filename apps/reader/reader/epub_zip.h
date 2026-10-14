/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — the ZIP container an EPUB is.
 *
 * A C port of ZipFile and InflateReader from CrossPoint Reader,
 * Copyright (c) 2025 Dave Allie. See THIRD_PARTY_LICENSES.md.
 */
#pragma once

#include <stddef.h>

typedef struct epub_zip epub_zip_t;

/* Opens the archive at `path` and reads its central directory. NULL if the
 * file cannot be opened or is not a ZIP archive. */
epub_zip_t *epub_zip_open(const char *path);

void epub_zip_close(epub_zip_t *zip);

/* The contents of the entry named `name`, inflated and NUL-terminated, which
 * the caller frees with free(). NULL if there is no such entry, or it is
 * corrupt, larger than the Reader will load, or compressed with anything but
 * deflate. `len_out`, when given, receives the length without the NUL. */
char *epub_zip_read(epub_zip_t *zip, const char *name, size_t *len_out);

/* The size of the entry named `name` once inflated, as the archive's
 * directory gives it, without reading it; 0 if there is no such entry. */
size_t epub_zip_size(const epub_zip_t *zip, const char *name);
