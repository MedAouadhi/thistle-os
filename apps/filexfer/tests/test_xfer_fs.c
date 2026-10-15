// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// Listing, reading, uploading, removing, making and renaming, in a scratch
// folder ($XFER_TEST_DIR) standing in for both roots; and how a flat root's
// file names become folders.

#include "filexfer/xfer_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_failures;
static char s_dir[512];

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_ERR(expr, want)                                                  \
    do {                                                                       \
        int _got = (expr);                                                     \
        if (_got != (want)) {                                                  \
            fprintf(stderr, "%s:%d: %s gave %d (%s), not %d (%s)\n", __FILE__, \
                    __LINE__, #expr, _got, strerror(_got), (want),              \
                    strerror(want));                                           \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static xfer_path_t P(const char *s)
{
    xfer_path_t p;
    if (!xfer_path_parse(s, &p)) {
        fprintf(stderr, "bad test path %s\n", s);
        exit(2);
    }
    return p;
}

static void put(const char *rel, const char *content)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/sd%s", s_dir, rel);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static char *slurp(const char *rel)
{
    static char buf[256];
    char path[1024];
    snprintf(path, sizeof path, "%s/sd%s", s_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool exists(const char *rel)
{
    char path[1024];
    struct stat st;
    snprintf(path, sizeof path, "%s/sd%s", s_dir, rel);
    return stat(path, &st) == 0;
}

typedef struct {
    char names[16][64];
    bool dirs[16];
    int64_t sizes[16];
    int count;
} listing_t;

static bool collect(const xfer_entry_t *e, void *ctx)
{
    listing_t *l = ctx;
    if (l->count == 16) return false;
    snprintf(l->names[l->count], sizeof l->names[0], "%s", e->name);
    l->dirs[l->count] = e->dir;
    l->sizes[l->count] = e->size;
    l->count++;
    return true;
}

static int find(const listing_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->names[i], name) == 0) return i;
    }
    return -1;
}

static void test_list(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/sd/notes", s_dir);
    mkdir(path, 0755);
    put("/notes/a.txt", "hello");
    put("/top.json", "{}");
    put("/" XFER_UPLOAD_TMP, "half an upload");

    listing_t l = {0};
    xfer_path_t root = P("/sdcard");
    CHECK_ERR(xfer_fs_list(&root, collect, &l), 0);
    CHECK(l.count == 2);
    int i = find(&l, "notes");
    CHECK(i >= 0 && l.dirs[i]);
    i = find(&l, "top.json");
    CHECK(i >= 0 && !l.dirs[i] && l.sizes[i] == 2);
    CHECK(find(&l, XFER_UPLOAD_TMP) < 0);

    memset(&l, 0, sizeof l);
    xfer_path_t notes = P("/sdcard/notes");
    CHECK_ERR(xfer_fs_list(&notes, collect, &l), 0);
    CHECK(l.count == 1 && strcmp(l.names[0], "a.txt") == 0 && l.sizes[0] == 5);

    xfer_path_t missing = P("/sdcard/nothing");
    CHECK_ERR(xfer_fs_list(&missing, collect, &l), ENOENT);
    xfer_path_t file = P("/sdcard/top.json");
    CHECK_ERR(xfer_fs_list(&file, collect, &l), ENOTDIR);
}

static void test_read(void)
{
    FILE *f;
    int64_t size = -1;
    xfer_path_t a = P("/sdcard/notes/a.txt");
    CHECK_ERR(xfer_fs_open_read(&a, &f, &size), 0);
    CHECK(f && size == 5);
    if (f) fclose(f);

    xfer_path_t dir = P("/sdcard/notes");
    CHECK_ERR(xfer_fs_open_read(&dir, &f, &size), EISDIR);
    xfer_path_t root = P("/sdcard");
    CHECK_ERR(xfer_fs_open_read(&root, &f, &size), EISDIR);
    xfer_path_t none = P("/sdcard/none.txt");
    CHECK_ERR(xfer_fs_open_read(&none, &f, &size), ENOENT);
}

static void test_upload(void)
{
    xfer_upload_t up;
    xfer_path_t a = P("/sdcard/notes/a.txt");

    /* There already, and not to be replaced */
    CHECK_ERR(xfer_fs_upload_begin(&up, &a, false), EEXIST);

    /* Replaced, but cut short: the old one stays */
    CHECK_ERR(xfer_fs_upload_begin(&up, &a, true), 0);
    CHECK_ERR(xfer_fs_upload_write(&up, "half", 4), 0);
    xfer_fs_upload_abort(&up);
    CHECK(strcmp(slurp("/notes/a.txt"), "hello") == 0);
    CHECK(!exists("/" XFER_UPLOAD_TMP));

    /* Replaced whole */
    CHECK_ERR(xfer_fs_upload_begin(&up, &a, true), 0);
    CHECK_ERR(xfer_fs_upload_write(&up, "new ", 4), 0);
    CHECK_ERR(xfer_fs_upload_write(&up, "text", 4), 0);
    CHECK_ERR(xfer_fs_upload_commit(&up), 0);
    CHECK(strcmp(slurp("/notes/a.txt"), "new text") == 0);
    CHECK(!exists("/" XFER_UPLOAD_TMP));

    /* New, and empty */
    xfer_path_t b = P("/sdcard/notes/b.txt");
    CHECK_ERR(xfer_fs_upload_begin(&up, &b, false), 0);
    CHECK_ERR(xfer_fs_upload_commit(&up), 0);
    CHECK(exists("/notes/b.txt") && strcmp(slurp("/notes/b.txt"), "") == 0);

    xfer_path_t nofolder = P("/sdcard/nofolder/c.txt");
    CHECK_ERR(xfer_fs_upload_begin(&up, &nofolder, false), ENOENT);
    xfer_path_t dir = P("/sdcard/notes");
    CHECK_ERR(xfer_fs_upload_begin(&up, &dir, true), EISDIR);
    xfer_path_t root = P("/sdcard");
    CHECK_ERR(xfer_fs_upload_begin(&up, &root, true), EINVAL);
    xfer_path_t tmp = P("/sdcard/" XFER_UPLOAD_TMP);
    CHECK_ERR(xfer_fs_upload_begin(&up, &tmp, true), EINVAL);
}

static void test_mkdir_rename_delete(void)
{
    xfer_path_t books = P("/sdcard/books");
    CHECK_ERR(xfer_fs_mkdir(&books), 0);
    CHECK_ERR(xfer_fs_mkdir(&books), EEXIST);
    xfer_path_t deep = P("/sdcard/x/y");
    CHECK_ERR(xfer_fs_mkdir(&deep), ENOENT);

    xfer_path_t to;
    xfer_path_t b = P("/sdcard/notes/b.txt");
    CHECK_ERR(xfer_fs_rename(&b, "c.txt", &to), 0);
    CHECK(strcmp(to.path, "/sdcard/notes/c.txt") == 0);
    CHECK(exists("/notes/c.txt") && !exists("/notes/b.txt"));
    xfer_path_t c = P("/sdcard/notes/c.txt");
    CHECK_ERR(xfer_fs_rename(&c, "a.txt", &to), EEXIST);
    CHECK_ERR(xfer_fs_rename(&c, "../escape.txt", &to), EINVAL);
    CHECK_ERR(xfer_fs_rename(&c, "", &to), EINVAL);
    CHECK_ERR(xfer_fs_rename(&c, XFER_UPLOAD_TMP, &to), EINVAL);
    CHECK_ERR(xfer_fs_rename(&books, "library", &to), 0);
    CHECK(exists("/library"));
    xfer_path_t gone = P("/sdcard/gone.txt");
    CHECK_ERR(xfer_fs_rename(&gone, "here.txt", &to), ENOENT);
    xfer_path_t root = P("/sdcard");
    CHECK_ERR(xfer_fs_rename(&root, "x", &to), EINVAL);

    xfer_path_t notes = P("/sdcard/notes");
    CHECK_ERR(xfer_fs_delete(&notes), ENOTEMPTY);
    CHECK_ERR(xfer_fs_delete(&c), 0);
    xfer_path_t a = P("/sdcard/notes/a.txt");
    CHECK_ERR(xfer_fs_delete(&a), 0);
    CHECK_ERR(xfer_fs_delete(&notes), 0);
    CHECK(!exists("/notes"));
    CHECK_ERR(xfer_fs_delete(&notes), ENOENT);
    CHECK_ERR(xfer_fs_delete(&root), EINVAL);
}

static void test_flat_entries(void)
{
    xfer_seen_t seen = {.count = 0};
    char part[64];
    bool dir;

    CHECK(xfer_flat_entry("system.json", &seen, part, sizeof part, &dir));
    CHECK(!dir && strcmp(part, "system.json") == 0);
    CHECK(xfer_flat_entry("config/board.json", &seen, part, sizeof part, &dir));
    CHECK(dir && strcmp(part, "config") == 0);
    CHECK(!xfer_flat_entry("config/system.json", &seen, part, sizeof part, &dir));
    CHECK(xfer_flat_entry("drivers/a/b.elf", &seen, part, sizeof part, &dir));
    CHECK(dir && strcmp(part, "drivers") == 0);
    CHECK(xfer_flat_entry("/leading.txt", &seen, part, sizeof part, &dir));
    CHECK(!dir && strcmp(part, "leading.txt") == 0);
    CHECK(!xfer_flat_entry("", &seen, part, sizeof part, &dir));
    CHECK(!xfer_flat_entry("toolong/x", &seen, part, 4, &dir));
    CHECK(seen.count == 2);
}

int main(void)
{
    const char *base = getenv("XFER_TEST_DIR");
    if (!base) {
        fprintf(stderr, "test_xfer_fs: XFER_TEST_DIR is not set\n");
        return 2;
    }
    snprintf(s_dir, sizeof s_dir, "%s", base);
    char sd[600], spiffs[600];
    snprintf(sd, sizeof sd, "%s/sd", s_dir);
    snprintf(spiffs, sizeof spiffs, "%s/spiffs", s_dir);
    mkdir(sd, 0755);
    mkdir(spiffs, 0755);
    xfer_fs_set_mount(XFER_ROOT_SDCARD, sd, false);
    xfer_fs_set_mount(XFER_ROOT_SPIFFS, spiffs, false);

    test_list();
    test_read();
    test_upload();
    test_mkdir_rename_delete();
    test_flat_entries();
    if (s_failures) {
        fprintf(stderr, "test_xfer_fs: %d failed\n", s_failures);
        return 1;
    }
    printf("test_xfer_fs: all passed\n");
    return 0;
}
