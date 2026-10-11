// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// Where the Reader left each book, saved under a directory that does not
// exist yet:
//     test_book_progress DIR

#define _POSIX_C_SOURCE 200809L

#include "reader/book_progress.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char s_dir[512];
static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static bool same(const book_progress_t *a, const book_progress_t *b)
{
    return a->book_size == b->book_size && a->chapter == b->chapter &&
           a->offset == b->offset && a->text_len == b->text_len;
}

/* The one file in the directory not named `other`, into `out`. */
static bool find_file(const char *other, char *out, size_t out_len)
{
    DIR *dir = opendir(s_dir);
    if (!dir) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(out, out_len, "%s/%s", s_dir, de->d_name);
        found = !other || strcmp(out, other) != 0;
    }
    closedir(dir);
    return found;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static size_t read_file(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void write_file(const char *path, const unsigned char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(buf, 1, len, f);
    fclose(f);
}

static void test_round_trip(void)
{
    book_progress_t got = {0};
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));

    book_progress_t left = {.book_size = 1234567, .chapter = 42, .offset = 70000, .text_len = 81234};
    CHECK(book_progress_save(s_dir, "/books/Moby Dick.epub", &left));
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    CHECK(same(&got, &left));

    /* FAT does not tell case apart, so neither does a book's key. */
    got = (book_progress_t){0};
    CHECK(book_progress_load(s_dir, "/BOOKS/moby dick.EPUB", &got));
    CHECK(same(&got, &left));

    CHECK(!book_progress_load(s_dir, "/books/Ulysses.epub", &got));
}

static void test_written_over_in_place(void)
{
    char path[600];
    CHECK(find_file(NULL, path, sizeof(path)));
    long size = file_size(path);
    CHECK(size == 28 + (long)strlen("/books/Moby Dick.epub") + 4);

    book_progress_t later = {.book_size = 1234567, .chapter = 43, .offset = 0, .text_len = 512};
    CHECK(book_progress_save(s_dir, "/books/Moby Dick.epub", &later));
    book_progress_t got = {0};
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    CHECK(same(&got, &later));
    CHECK(file_size(path) == size);
}

static void test_damaged_files(void)
{
    char path[600];
    CHECK(find_file(NULL, path, sizeof(path)));
    unsigned char good[600];
    size_t len = read_file(path, good, sizeof(good));
    CHECK(len > 32);
    book_progress_t got;

    /* A byte changed, as by a write the power cut. */
    unsigned char bad[600];
    memcpy(bad, good, len);
    bad[17] ^= 0x01;
    write_file(path, bad, len);
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));

    /* Cut short, before and after its path's length. */
    write_file(path, good, 20);
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    write_file(path, good, len - 1);
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));

    /* Someone else's file, and an empty one. */
    write_file(path, (const unsigned char *)"{\"spine\": 3, \"page\": 7}", 23);
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    write_file(path, good, 0);
    CHECK(!book_progress_load(s_dir, "/books/Moby Dick.epub", &got));

    /* Saving again mends it. */
    book_progress_t left = {.book_size = 9, .chapter = 1, .offset = 2, .text_len = 3};
    CHECK(book_progress_save(s_dir, "/books/Moby Dick.epub", &left));
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    CHECK(same(&got, &left));
}

/* Two books whose names hash alike share a file; each reads only its own. */
static void test_another_books_file(void)
{
    char moby[600], other[600];
    CHECK(find_file(NULL, moby, sizeof(moby)));
    book_progress_t notes = {.book_size = 100, .chapter = 0, .offset = 50, .text_len = 100};
    CHECK(book_progress_save(s_dir, "/notes.txt", &notes));
    CHECK(find_file(moby, other, sizeof(other)));

    unsigned char rec[600];
    size_t len = read_file(moby, rec, sizeof(rec));
    write_file(other, rec, len);
    book_progress_t got;
    CHECK(!book_progress_load(s_dir, "/notes.txt", &got));
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
}

static void test_path_too_long(void)
{
    char book[600];
    memset(book, 'a', sizeof(book) - 1);
    book[0] = '/';
    book[sizeof(book) - 1] = '\0';
    book_progress_t left = {0};
    CHECK(!book_progress_save(s_dir, book, &left));
    CHECK(!book_progress_load(s_dir, book, &left));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    /* Two levels the save must make. */
    snprintf(s_dir, sizeof(s_dir), "%s/data/reader", argv[1]);

    test_round_trip();
    test_written_over_in_place();
    test_damaged_files();
    test_another_books_file();
    test_path_too_long();

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_book_progress: all checks passed\n");
    return 0;
}
