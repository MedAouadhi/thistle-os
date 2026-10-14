/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — what the library calls a book that has not given its title.
 */
#include "reader/book_title.h"

#include <string.h>

void book_title_from_file(const char *file_name, char *out, size_t out_len)
{
    if (out_len == 0) return;

    size_t len = strlen(file_name);
    const char *dot = strrchr(file_name, '.');
    if (dot && dot != file_name) len = (size_t)(dot - file_name);

    /* What follows the title, unless nothing comes before it. */
    for (const char *sep = file_name; (sep = strstr(sep, " -- ")) != NULL && sep < file_name + len; sep++) {
        const char *p = file_name;
        while (p < sep && (*p == ' ' || *p == '_')) p++;
        if (p < sep) {
            len = (size_t)(sep - file_name);
            break;
        }
    }

    /* Underscores as spaces, then the spaces at either end dropped. */
    size_t start = 0;
    while (start < len && (file_name[start] == ' ' || file_name[start] == '_')) start++;
    while (len > start && (file_name[len - 1] == ' ' || file_name[len - 1] == '_')) len--;

    size_t n = len - start;
    if (n >= out_len) {
        n = out_len - 1;
        while (n > 0 && ((unsigned char)file_name[start + n] & 0xc0) == 0x80) n--;
    }
    for (size_t i = 0; i < n; i++) {
        char c = file_name[start + i];
        out[i] = c == '_' ? ' ' : c;
    }
    out[n] = '\0';
}
