/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — where the Reader left each book.
 *
 * A book's file is <dir>/<hash>.bin, the hash a 64-bit FNV-1a of its path
 * with A-Z folded to a-z. It holds, little-endian:
 *
 *    0  "THRP"
 *    4  the version, 1, and 3 bytes of 0
 *    8  book_size, chapter, offset and text_len, 4 bytes each
 *   24  the length of the book's path, 2 bytes
 *   26  permille + 1, 2 bytes: 0, as a Reader before it wrote, for unknown
 *   28  the book's path, which tells apart two books whose paths hash alike
 *    …  the CRC-32 of everything before it, 4 bytes
 *
 * The books opened last are <dir>/recent.bin:
 *
 *    0  "THRR"
 *    4  the version, 1, and 3 bytes of 0
 *    8  how many books, 2 bytes, and 2 bytes of 0
 *   12  for each, the lengths of its path, title and author, 2 bytes each,
 *       then those
 *    …  the CRC-32 of everything before it, 4 bytes
 *
 * Both are written over in place, so whatever a longer record left after the
 * CRC is not read.
 */
#include "reader/book_progress.h"
#include "reader/epub_port.h"

#include "third_party/uzlib/uzlib.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "book_progress";

#define MAGIC        "THRP"
#define VERSION      1
#define HEADER_SIZE  28
#define MAX_BOOK_LEN 512
/* The record is on the heap: the LVGL task, which saves, has 8 KB of stack
 * and FatFs and the SPI driver run beneath this. */
#define MAX_RECORD   (HEADER_SIZE + MAX_BOOK_LEN + 4)

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
    return uzlib_crc32(data, (unsigned int)len, 0xffffffffu) ^ 0xffffffffu;
}

static bool file_path(const char *dir, const char *book, char *buf, size_t buf_len)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char *c = book; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
        hash = (hash ^ ch) * 0x100000001b3ull;
    }
    int n = snprintf(buf, buf_len, "%s/%016llx.bin", dir, (unsigned long long)hash);
    return n > 0 && (size_t)n < buf_len;
}

/* Makes `dir`, and the directories above it that are missing. */
static void make_dirs(const char *dir)
{
    char path[128];
    if (snprintf(path, sizeof(path), "%s", dir) >= (int)sizeof(path)) return;
    for (char *p = path + 1;; p++) {
        if (*p != '/' && *p != '\0') continue;
        char c = *p;
        *p = '\0';
        struct stat st;
        if (stat(path, &st) != 0) {
#ifdef _WIN32
            mkdir(path);
#else
            mkdir(path, 0755);
#endif
        }
        if (c == '\0') break;
        *p = c;
    }
}

/* Writes `rec` over the start of the file at `path`, in `dir`, making them if
 * they are missing. "r+" writes over the old record without truncating the
 * file first, so FatFs frees and allocates no cluster: only the record's
 * sectors and the directory entry are written. */
static bool write_over(const char *dir, const char *path, const uint8_t *rec, size_t len)
{
    FILE *f = fopen(path, "r+b");
    if (!f) {
        make_dirs(dir);
        f = fopen(path, "wb");
    }
    bool ok = false;
    if (f) {
        ok = fwrite(rec, 1, len, f) == len;
        ok = fclose(f) == 0 && ok;
    }
    if (!ok) ESP_LOGW(TAG, "Cannot write %s", path);
    return ok;
}

bool book_progress_load(const char *dir, const char *book, book_progress_t *out)
{
    char path[160];
    size_t book_len = strlen(book);
    if (book_len > MAX_BOOK_LEN || !file_path(dir, book, path, sizeof(path))) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t *rec = epub_malloc(MAX_RECORD);
    if (!rec) {
        fclose(f);
        return false;
    }
    size_t got = fread(rec, 1, MAX_RECORD, f);
    fclose(f);

    bool ok = false;
    size_t len = 0;
    if (got < HEADER_SIZE + 4 || memcmp(rec, MAGIC, 4) != 0 || rec[4] != VERSION) {
        ESP_LOGW(TAG, "%s is not a progress file this Reader knows", path);
    } else if ((len = HEADER_SIZE + get_u16(rec + 24)) + 4 > got ||
               get_u32(rec + len) != crc32(rec, len)) {
        ESP_LOGW(TAG, "%s is damaged", path);
    } else if (len - HEADER_SIZE == book_len &&
               strncasecmp((const char *)rec + HEADER_SIZE, book, book_len) == 0) {
        out->book_size = get_u32(rec + 8);
        out->chapter   = get_u32(rec + 12);
        out->offset    = get_u32(rec + 16);
        out->text_len  = get_u32(rec + 20);
        uint16_t far   = get_u16(rec + 26);
        out->permille  = far == 0 || far > 1001 ? BOOK_PROGRESS_UNKNOWN : (uint16_t)(far - 1);
        ok = true;
    }
    epub_free(rec);
    return ok;
}

bool book_progress_save(const char *dir, const char *book, const book_progress_t *progress)
{
    char path[160];
    size_t book_len = strlen(book);
    if (book_len > MAX_BOOK_LEN || !file_path(dir, book, path, sizeof(path))) return false;

    uint8_t *rec = epub_malloc(MAX_RECORD);
    if (!rec) return false;
    memset(rec, 0, HEADER_SIZE);
    memcpy(rec, MAGIC, 4);
    rec[4] = VERSION;
    put_u32(rec + 8, progress->book_size);
    put_u32(rec + 12, progress->chapter);
    put_u32(rec + 16, progress->offset);
    put_u32(rec + 20, progress->text_len);
    put_u16(rec + 24, (uint16_t)book_len);
    put_u16(rec + 26, progress->permille <= 1000 ? (uint16_t)(progress->permille + 1) : 0);
    memcpy(rec + HEADER_SIZE, book, book_len);
    size_t len = HEADER_SIZE + book_len;
    put_u32(rec + len, crc32(rec, len));
    len += 4;

    bool ok = write_over(dir, path, rec, len);
    epub_free(rec);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The books opened last                                                */
/* ------------------------------------------------------------------ */

#define RECENT_MAGIC  "THRR"
#define RECENT_HEADER 12
#define RECENT_MAX_RECORD \
    (RECENT_HEADER + BOOK_RECENT_MAX * (6 + sizeof(((book_recent_t *)0)->book) + \
                                        sizeof(((book_recent_t *)0)->title) + \
                                        sizeof(((book_recent_t *)0)->author)) + 4)

static bool recent_path(const char *dir, char *buf, size_t buf_len)
{
    int n = snprintf(buf, buf_len, "%s/recent.bin", dir);
    return n > 0 && (size_t)n < buf_len;
}

/* `src`, cut to fit `cap` bytes with its NUL, and not inside a character. */
static size_t copy_cut(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

int book_recent_load(const char *dir, book_recent_t list[BOOK_RECENT_MAX])
{
    char path[160];
    if (!recent_path(dir, path, sizeof(path))) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *rec = epub_malloc(RECENT_MAX_RECORD);
    if (!rec) {
        fclose(f);
        return 0;
    }
    size_t got = fread(rec, 1, RECENT_MAX_RECORD, f);
    fclose(f);

    int count = 0;
    bool ok = got >= RECENT_HEADER + 4 && memcmp(rec, RECENT_MAGIC, 4) == 0 && rec[4] == VERSION;
    int kept = ok ? get_u16(rec + 8) : 0;
    ok = ok && kept <= BOOK_RECENT_MAX;
    size_t pos = RECENT_HEADER;
    for (int i = 0; ok && i < kept; i++) {
        book_recent_t *b = &list[i];
        size_t book_len, title_len, author_len;
        ok = pos + 6 <= got &&
             (book_len = get_u16(rec + pos)) < sizeof(b->book) &&
             (title_len = get_u16(rec + pos + 2)) < sizeof(b->title) &&
             (author_len = get_u16(rec + pos + 4)) < sizeof(b->author) &&
             pos + 6 + book_len + title_len + author_len <= got;
        if (!ok) break;
        pos += 6;
        pos += copy_cut(b->book, sizeof(b->book), (const char *)rec + pos, book_len);
        pos += copy_cut(b->title, sizeof(b->title), (const char *)rec + pos, title_len);
        pos += copy_cut(b->author, sizeof(b->author), (const char *)rec + pos, author_len);
    }
    if (ok && pos + 4 <= got && get_u32(rec + pos) == crc32(rec, pos)) {
        count = kept;
    } else if (got > 0) {
        ESP_LOGW(TAG, "%s is damaged", path);
    }
    epub_free(rec);
    return count;
}

/* Puts `book` first in the list kept in `dir`, with `list` and `rec` to work
 * in, and writes the list to `path`. */
static bool put_first(const char *dir, const char *path, book_recent_t *list, uint8_t *rec,
                      const char *book, const char *title, const char *author)
{
    /* The book first, then the others as they were, without it. */
    int count = book_recent_load(dir, list);
    int at = count;
    for (int i = 0; i < count; i++) {
        if (strcasecmp(list[i].book, book) == 0) {
            at = i;
            break;
        }
    }
    if (at == count && count == BOOK_RECENT_MAX) at = --count;
    memmove(&list[1], &list[0], sizeof(book_recent_t) * (size_t)at);
    if (at == count) count++;
    copy_cut(list[0].book, sizeof(list[0].book), book, strlen(book));
    copy_cut(list[0].title, sizeof(list[0].title), title, strlen(title));
    copy_cut(list[0].author, sizeof(list[0].author), author, strlen(author));

    memset(rec, 0, RECENT_HEADER);
    memcpy(rec, RECENT_MAGIC, 4);
    rec[4] = VERSION;
    put_u16(rec + 8, (uint16_t)count);
    size_t len = RECENT_HEADER;
    for (int i = 0; i < count; i++) {
        const char *field[3] = {list[i].book, list[i].title, list[i].author};
        size_t field_len[3];
        for (int k = 0; k < 3; k++) {
            field_len[k] = strlen(field[k]);
            put_u16(rec + len + 2 * k, (uint16_t)field_len[k]);
        }
        len += 6;
        for (int k = 0; k < 3; k++) {
            memcpy(rec + len, field[k], field_len[k]);
            len += field_len[k];
        }
    }
    put_u32(rec + len, crc32(rec, len));
    return write_over(dir, path, rec, len + 4);
}

bool book_recent_put(const char *dir, const char *book, const char *title, const char *author)
{
    char path[160];
    if (strlen(book) >= sizeof(((book_recent_t *)0)->book) || !recent_path(dir, path, sizeof(path))) {
        return false;
    }
    book_recent_t *list = epub_malloc(sizeof(book_recent_t) * BOOK_RECENT_MAX);
    uint8_t *rec = epub_malloc(RECENT_MAX_RECORD);
    bool ok = list && rec && put_first(dir, path, list, rec, book, title, author);
    epub_free(list);
    epub_free(rec);
    return ok;
}
