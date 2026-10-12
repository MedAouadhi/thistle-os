// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Appends JSON text to a buffer. With buffer NULL it only measures, so a
 * caller can run the same writes twice: once to size an allocation, once to
 * fill it. Any write that does not fit clears valid, and every later write
 * fails. */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool valid;
} assistant_json_writer_t;

bool assistant_json_write_raw(assistant_json_writer_t *writer, const char *text);

/* Appends value as a quoted JSON string, escaping what JSON requires. */
bool assistant_json_write_string(assistant_json_writer_t *writer,
                                 const char *value);

/* Finds the value at path in the JSON document json[0..len). A path is keys
 * and array indexes joined by dots, as in "choices.0.message.content".
 * Returns where the value starts, or NULL if it is absent or the document is
 * malformed on the way to it. */
const char *assistant_json_find(const char *json, size_t len, const char *path);

/* Decodes the string at path into out as UTF-8, \uXXXX escapes and
 * surrogate pairs included. A string longer than out is cut at a character
 * boundary and *truncated is set; truncated may be NULL. Returns false if the
 * value is absent or not a well-formed string. */
bool assistant_json_get_string(const char *json, size_t len, const char *path,
                               char *out, size_t out_len, bool *truncated);

/* True if the value at path is a string equal to expected. */
bool assistant_json_string_equals(const char *json, size_t len,
                                  const char *path, const char *expected);
