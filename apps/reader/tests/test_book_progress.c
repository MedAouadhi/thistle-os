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
           a->offset == b->offset && a->text_len == b->text_len &&
           a->permille == b->permille;
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

/* The progress file whose record names `book`. */
static bool find_file_holding(const char *book, char *out, size_t out_len)
{
    DIR *dir = opendir(s_dir);
    if (!dir) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(out, out_len, "%s/%s", s_dir, de->d_name);
        unsigned char rec[600];
        FILE *f = fopen(out, "rb");
        if (!f) continue;
        size_t n = fread(rec, 1, sizeof(rec), f);
        fclose(f);
        size_t len = strlen(book);
        found = n >= 28 + len && memcmp(rec + 28, book, len) == 0;
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

    book_progress_t left = {.book_size = 1234567, .chapter = 42, .offset = 70000, .text_len = 81234,
                            .permille = 425};
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

    book_progress_t later = {.book_size = 1234567, .chapter = 43, .offset = 0, .text_len = 512,
                             .permille = 1000};
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
    book_progress_t left = {.book_size = 9, .chapter = 1, .offset = 2, .text_len = 3, .permille = 0};
    CHECK(book_progress_save(s_dir, "/books/Moby Dick.epub", &left));
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
    CHECK(same(&got, &left));
}

/* Two books whose names hash alike share a file; each reads only its own. */
static void test_another_books_file(void)
{
    char moby[600], other[600];
    CHECK(find_file(NULL, moby, sizeof(moby)));
    book_progress_t notes = {.book_size = 100, .chapter = 0, .offset = 50, .text_len = 100,
                             .permille = 500};
    CHECK(book_progress_save(s_dir, "/notes.txt", &notes));
    CHECK(find_file(moby, other, sizeof(other)));

    unsigned char rec[600];
    size_t len = read_file(moby, rec, sizeof(rec));
    write_file(other, rec, len);
    book_progress_t got;
    CHECK(!book_progress_load(s_dir, "/notes.txt", &got));
    CHECK(book_progress_load(s_dir, "/books/Moby Dick.epub", &got));
}

/* How far through the book is kept in what was two bytes of 0, as one more
 * than the thousandths: a Reader before it wrote 0 there, which reads as not
 * known, and reads the files this one writes as it always did. */
static void test_how_far_in_former_padding(void)
{
    book_progress_t left = {.book_size = 7, .chapter = 0, .offset = 1, .text_len = 2, .permille = 425};
    CHECK(book_progress_save(s_dir, "/far.txt", &left));
    char path[600];
    CHECK(find_file_holding("/far.txt", path, sizeof(path)));
    unsigned char rec[600];
    CHECK(read_file(path, rec, sizeof(rec)) == 28 + strlen("/far.txt") + 4);
    CHECK(rec[4] == 1);
    CHECK(rec[26] == (426 & 0xff) && rec[27] == 426 >> 8);

    left.permille = BOOK_PROGRESS_UNKNOWN;
    CHECK(book_progress_save(s_dir, "/far.txt", &left));
    CHECK(read_file(path, rec, sizeof(rec)) > 28);
    CHECK(rec[26] == 0 && rec[27] == 0);
    book_progress_t got = {0};
    CHECK(book_progress_load(s_dir, "/far.txt", &got));
    CHECK(got.permille == BOOK_PROGRESS_UNKNOWN);
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

/* ---- The books opened last ---- */

static char s_recent_dir[512];

static void test_recent_newest_first(void)
{
    book_recent_t list[BOOK_RECENT_MAX];
    CHECK(book_recent_load(s_recent_dir, list) == 0);

    CHECK(book_recent_put(s_recent_dir, "/books/a.epub", "Alpha", "Ann"));
    CHECK(book_recent_put(s_recent_dir, "/books/b.txt", "", ""));
    CHECK(book_recent_put(s_recent_dir, "/books/c.epub", "Gamma", "Cy"));
    CHECK(book_recent_load(s_recent_dir, list) == 3);
    CHECK(strcmp(list[0].book, "/books/c.epub") == 0);
    CHECK(strcmp(list[0].title, "Gamma") == 0 && strcmp(list[0].author, "Cy") == 0);
    CHECK(strcmp(list[1].book, "/books/b.txt") == 0);
    CHECK(list[1].title[0] == '\0' && list[1].author[0] == '\0');
    CHECK(strcmp(list[2].book, "/books/a.epub") == 0);

    /* Opened again, a book moves up rather than appearing twice; FAT does
     * not tell case apart, so neither does the list. */
    CHECK(book_recent_put(s_recent_dir, "/BOOKS/A.EPUB", "Alpha", "Ann"));
    CHECK(book_recent_load(s_recent_dir, list) == 3);
    CHECK(strcmp(list[0].book, "/BOOKS/A.EPUB") == 0);
    CHECK(strcmp(list[1].book, "/books/c.epub") == 0);
    CHECK(strcmp(list[2].book, "/books/b.txt") == 0);
}

static void test_recent_drops_the_oldest(void)
{
    for (int i = 0; i < BOOK_RECENT_MAX + 2; i++) {
        char book[32];
        snprintf(book, sizeof(book), "/book%d.txt", i);
        CHECK(book_recent_put(s_recent_dir, book, "", ""));
    }
    book_recent_t list[BOOK_RECENT_MAX];
    CHECK(book_recent_load(s_recent_dir, list) == BOOK_RECENT_MAX);
    char newest[32], oldest[32];
    snprintf(newest, sizeof(newest), "/book%d.txt", BOOK_RECENT_MAX + 1);
    snprintf(oldest, sizeof(oldest), "/book%d.txt", 2);
    CHECK(strcmp(list[0].book, newest) == 0);
    CHECK(strcmp(list[BOOK_RECENT_MAX - 1].book, oldest) == 0);
}

/* A title longer than the list keeps is cut between characters, not inside
 * one. */
static void test_recent_long_title(void)
{
    char title[400] = "";
    for (int i = 0; i < 150; i++) strcat(title, "\xC3\xA9"); /* é */
    CHECK(book_recent_put(s_recent_dir, "/long.epub", title, title));
    book_recent_t list[BOOK_RECENT_MAX];
    CHECK(book_recent_load(s_recent_dir, list) > 0);
    size_t n = strlen(list[0].title);
    CHECK(n > 0 && n < sizeof(list[0].title) && n % 2 == 0);
    CHECK(strncmp(list[0].title, title, n) == 0);
    n = strlen(list[0].author);
    CHECK(n > 0 && n < sizeof(list[0].author) && n % 2 == 0);

    /* A path the list cannot hold is not put in it. */
    char book[600];
    memset(book, 'a', sizeof(book) - 1);
    book[0] = '/';
    book[sizeof(book) - 1] = '\0';
    CHECK(!book_recent_put(s_recent_dir, book, "", ""));
    CHECK(book_recent_load(s_recent_dir, list) > 0);
    CHECK(strcmp(list[0].book, "/long.epub") == 0);
}

static void test_recent_damaged(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/recent.bin", s_recent_dir);
    unsigned char good[8192];
    size_t len = read_file(path, good, sizeof(good));
    CHECK(len > 16);
    book_recent_t list[BOOK_RECENT_MAX];

    unsigned char bad[8192];
    memcpy(bad, good, len);
    bad[len / 2] ^= 0x01;
    write_file(path, bad, len);
    CHECK(book_recent_load(s_recent_dir, list) == 0);
    write_file(path, good, len - 3);
    CHECK(book_recent_load(s_recent_dir, list) == 0);
    write_file(path, good, 0);
    CHECK(book_recent_load(s_recent_dir, list) == 0);

    /* The list starts again from the book opened next. */
    CHECK(book_recent_put(s_recent_dir, "/books/a.epub", "Alpha", "Ann"));
    CHECK(book_recent_load(s_recent_dir, list) == 1);
    CHECK(strcmp(list[0].title, "Alpha") == 0);

    /* Written over in place, a shorter list than the last leaves the end of
     * the longer one behind it, which is not read. */
    write_file(path, good, len);
    CHECK(book_recent_load(s_recent_dir, list) == BOOK_RECENT_MAX);
    CHECK(book_recent_put(s_recent_dir, "/long.epub", "", ""));
    CHECK(file_size(path) == (long)len);
    CHECK(book_recent_load(s_recent_dir, list) == BOOK_RECENT_MAX);
    CHECK(strcmp(list[0].book, "/long.epub") == 0 && list[0].title[0] == '\0');
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
    test_how_far_in_former_padding();
    test_path_too_long();

    /* A directory of its own, which the first put must make. */
    snprintf(s_recent_dir, sizeof(s_recent_dir), "%s/recent/reader", argv[1]);
    test_recent_newest_first();
    test_recent_drops_the_oldest();
    test_recent_long_title();
    test_recent_damaged();

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_book_progress: all checks passed\n");
    return 0;
}
