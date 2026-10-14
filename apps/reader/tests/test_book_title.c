// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// What the library calls a book that has not told it its title.

#include "reader/book_title.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void check(const char *file_name, size_t out_len, const char *expected, int line)
{
    char out[256];
    book_title_from_file(file_name, out, out_len);
    if (strcmp(out, expected) == 0) return;
    fprintf(stderr, "%s:%d: \"%s\"\n  expected: \"%s\"\n  actual:   \"%s\"\n",
            __FILE__, line, file_name, expected, out);
    s_failures++;
}

#define CHECK_TITLE(file_name, expected) check((file_name), 256, (expected), __LINE__)

int main(void)
{
    CHECK_TITLE("sample.txt", "sample");
    CHECK_TITLE("Die Frauen von Janowka.epub", "Die Frauen von Janowka");
    CHECK_TITLE("README.MD", "README");

    /* What some sites add after the title: author, year, publisher, hashes. */
    CHECK_TITLE("Nemesis Games -- James S_ A_ Corey -- 2015.epub", "Nemesis Games");
    CHECK_TITLE("Alles, was wir nicht erinnern Zu Fu\xC3\x9F auf dem Fluchtweg -- Christiane "
                "Hoffmann; -- 2021 -- Verlag C_H_ Beck oHG -- isbn13 9783406784941 -- "
                "612677de7ece5fdefcb152a9e047fd90 -- Anna\xE2\x80\x99s Archive.epub",
                "Alles, was wir nicht erinnern Zu Fu\xC3\x9F auf dem Fluchtweg");
    /* Not a single dash, which titles have. */
    CHECK_TITLE("Star Wars - A New Hope.txt", "Star Wars - A New Hope");
    /* Nothing before it: the name is kept whole. */
    CHECK_TITLE(" -- Anonymous.txt", "-- Anonymous");

    /* Underscores read as the spaces they stand for. */
    CHECK_TITLE("crypto_ar.epub", "crypto ar");
    CHECK_TITLE("\xD8\xA7\xD9\x84\xD9\x86\xD8\xA8\xD8\xA3_\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB8\xD9\x8A\xD9\x85.epub",
                "\xD8\xA7\xD9\x84\xD9\x86\xD8\xA8\xD8\xA3 \xD8\xA7\xD9\x84\xD8\xB9\xD8\xB8\xD9\x8A\xD9\x85");
    CHECK_TITLE("__init__.txt", "init");

    /* No extension, or nothing but one. */
    CHECK_TITLE("notes", "notes");
    CHECK_TITLE(".txt", ".txt");

    /* Cut to fit, between characters. */
    check("\xC3\xA9\xC3\xA9\xC3\xA9.txt", 6, "\xC3\xA9\xC3\xA9", __LINE__);
    check("abc.txt", 1, "", __LINE__);

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_book_title: all checks passed\n");
    return 0;
}
