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
 *   24  the length of the book's path, 2 bytes, and 2 bytes of 0
 *   28  the book's path, which tells apart two books whose paths hash alike
 *    …  the CRC-32 of everything before it, 4 bytes
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
    } else if ((len = HEADER_SIZE + (rec[24] | (size_t)rec[25] << 8)) + 4 > got ||
               get_u32(rec + len) != crc32(rec, len)) {
        ESP_LOGW(TAG, "%s is damaged", path);
    } else if (len - HEADER_SIZE == book_len &&
               strncasecmp((const char *)rec + HEADER_SIZE, book, book_len) == 0) {
        out->book_size = get_u32(rec + 8);
        out->chapter   = get_u32(rec + 12);
        out->offset    = get_u32(rec + 16);
        out->text_len  = get_u32(rec + 20);
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
    rec[24] = (uint8_t)book_len;
    rec[25] = (uint8_t)(book_len >> 8);
    memcpy(rec + HEADER_SIZE, book, book_len);
    size_t len = HEADER_SIZE + book_len;
    put_u32(rec + len, crc32(rec, len));
    len += 4;

    /* "r+" writes over the old record without truncating the file first, so
     * FatFs frees and allocates no cluster: only the record's sector and the
     * directory entry are written. */
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
    epub_free(rec);
    return ok;
}
