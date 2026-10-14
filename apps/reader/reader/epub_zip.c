/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — the ZIP container an EPUB is.
 *
 * A C port of ZipFile and InflateReader from CrossPoint Reader,
 * Copyright (c) 2025 Dave Allie. See THIRD_PARTY_LICENSES.md.
 *
 * CrossPoint scans the central directory on the card for every lookup, since
 * the X4 has no PSRAM. Here the directory is read once, into PSRAM, and every
 * size and offset in it is checked against the file before it is used: a book
 * can come from anywhere.
 */
#include "reader/epub_zip.h"
#include "reader/epub_port.h"

#include "third_party/uzlib/uzlib.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "epub_zip";

#define ZIP_SIG_LOCAL     0x04034b50u
#define ZIP_SIG_CENTRAL   0x02014b50u
#define ZIP_SIG_EOCD      0x06054b50u
#define ZIP_LOCAL_SIZE    30
#define ZIP_CENTRAL_SIZE  46
#define ZIP_EOCD_SIZE     22
/* The end-of-directory record is followed by a comment of up to 64 KB. */
#define ZIP_MAX_COMMENT   0xFFFF

#define ZIP_METHOD_STORED   0
#define ZIP_METHOD_DEFLATED 8
#define ZIP_FLAG_ENCRYPTED  0x0001

/* No chapter or index of a real book comes near this, and a larger claim is
 * a corrupt or hostile archive asking for more PSRAM than the board has. */
#define ZIP_MAX_ENTRY_SIZE  (16u * 1024u * 1024u)

typedef struct {
    const char *name;   /* into directory, not NUL-terminated */
    uint16_t    name_len;
    uint16_t    method;
    uint16_t    flags;
    uint32_t    compressed_size;
    uint32_t    size;
    uint32_t    local_offset;
} zip_entry_t;

struct epub_zip {
    FILE        *file;
    long         file_size;
    uint8_t     *directory;   /* the central directory, as read */
    zip_entry_t *entries;
    int          count;
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_at(FILE *f, long offset, void *buf, size_t len)
{
    return fseek(f, offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

/* Finds the end-of-directory record, searching back from the end of the file
 * over the longest comment there can be, and returns the directory's offset
 * and size. */
static bool find_directory(epub_zip_t *zip, uint32_t *offset, uint32_t *size)
{
    if (zip->file_size < ZIP_EOCD_SIZE) return false;

    long tail_len = zip->file_size < ZIP_EOCD_SIZE + ZIP_MAX_COMMENT
                        ? zip->file_size
                        : ZIP_EOCD_SIZE + ZIP_MAX_COMMENT;
    long tail_start = zip->file_size - tail_len;
    uint8_t *tail = epub_malloc((size_t)tail_len);
    if (!tail) return false;

    bool found = false;
    if (read_at(zip->file, tail_start, tail, (size_t)tail_len)) {
        for (long i = tail_len - ZIP_EOCD_SIZE; i >= 0; i--) {
            if (le32(tail + i) != ZIP_SIG_EOCD) continue;
            *size   = le32(tail + i + 12);
            *offset = le32(tail + i + 16);
            /* The directory sits between the entries and this record. */
            found = (uint64_t)*offset + *size <= (uint64_t)(tail_start + i);
            break;
        }
    }
    epub_free(tail);
    return found;
}

static bool load_directory(epub_zip_t *zip)
{
    uint32_t dir_offset = 0, dir_size = 0;
    if (!find_directory(zip, &dir_offset, &dir_size)) {
        ESP_LOGW(TAG, "no end-of-directory record");
        return false;
    }

    zip->directory = epub_malloc(dir_size ? dir_size : 1);
    if (!zip->directory) return false;
    if (!read_at(zip->file, (long)dir_offset, zip->directory, dir_size)) return false;

    /* Every record is at least ZIP_CENTRAL_SIZE bytes, which bounds the count
     * whatever the end record claims. */
    int capacity = (int)(dir_size / ZIP_CENTRAL_SIZE);
    zip->entries = epub_malloc(sizeof(zip_entry_t) * (size_t)(capacity ? capacity : 1));
    if (!zip->entries) return false;

    uint32_t pos = 0;
    while (zip->count < capacity && pos + ZIP_CENTRAL_SIZE <= dir_size) {
        const uint8_t *rec = zip->directory + pos;
        if (le32(rec) != ZIP_SIG_CENTRAL) break;

        uint16_t name_len    = le16(rec + 28);
        uint16_t extra_len   = le16(rec + 30);
        uint16_t comment_len = le16(rec + 32);
        uint32_t rec_len = ZIP_CENTRAL_SIZE + (uint32_t)name_len + extra_len + comment_len;
        if (pos + rec_len > dir_size) break;

        zip_entry_t *e = &zip->entries[zip->count++];
        e->name            = (const char *)rec + ZIP_CENTRAL_SIZE;
        e->name_len        = name_len;
        e->flags           = le16(rec + 8);
        e->method          = le16(rec + 10);
        e->compressed_size = le32(rec + 20);
        e->size            = le32(rec + 24);
        e->local_offset    = le32(rec + 42);
        pos += rec_len;
    }
    return zip->count > 0;
}

epub_zip_t *epub_zip_open(const char *path)
{
    epub_zip_t *zip = epub_malloc(sizeof(*zip));
    if (!zip) return NULL;
    memset(zip, 0, sizeof(*zip));

    zip->file = fopen(path, "rb");
    if (!zip->file) {
        ESP_LOGW(TAG, "cannot open %s", path);
        epub_free(zip);
        return NULL;
    }
    if (fseek(zip->file, 0, SEEK_END) == 0) zip->file_size = ftell(zip->file);

    uzlib_init();
    if (zip->file_size <= 0 || !load_directory(zip)) {
        ESP_LOGW(TAG, "%s is not a ZIP archive", path);
        epub_zip_close(zip);
        return NULL;
    }
    return zip;
}

void epub_zip_close(epub_zip_t *zip)
{
    if (!zip) return;
    if (zip->file) fclose(zip->file);
    epub_free(zip->entries);
    epub_free(zip->directory);
    epub_free(zip);
}

static const zip_entry_t *find_entry(const epub_zip_t *zip, const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; i < zip->count; i++) {
        const zip_entry_t *e = &zip->entries[i];
        if (e->name_len == len && memcmp(e->name, name, len) == 0) return e;
    }
    return NULL;
}

size_t epub_zip_size(const epub_zip_t *zip, const char *name)
{
    const zip_entry_t *e = zip && name ? find_entry(zip, name) : NULL;
    return e ? e->size : 0;
}

static bool inflate_into(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    /* Its two Huffman trees make this 1.2 KB: off the caller's stack, which
     * is the LVGL task's. */
    struct uzlib_uncomp *d = epub_malloc(sizeof(*d));
    if (!d) return false;
    memset(d, 0, sizeof(*d));

    uzlib_uncompress_init(d, NULL, 0);
    d->source       = src;
    d->source_limit = src + src_len;
    d->dest_start   = dst;
    d->dest         = dst;
    d->dest_limit   = dst + dst_len;

    int res = uzlib_uncompress(d);
    /* eof: the stream wanted more input than the entry holds. */
    bool ok = res >= 0 && d->dest == d->dest_limit && !d->eof;
    epub_free(d);
    return ok;
}

char *epub_zip_read(epub_zip_t *zip, const char *name, size_t *len_out)
{
    if (!zip || !name) return NULL;

    const zip_entry_t *e = find_entry(zip, name);
    if (!e) return NULL;

    if (e->flags & ZIP_FLAG_ENCRYPTED) {
        ESP_LOGW(TAG, "%s is encrypted", name);
        return NULL;
    }
    if (e->size > ZIP_MAX_ENTRY_SIZE || e->compressed_size > ZIP_MAX_ENTRY_SIZE) {
        ESP_LOGW(TAG, "%s is too large (%u bytes)", name, (unsigned)e->size);
        return NULL;
    }
    if (e->method != ZIP_METHOD_STORED && e->method != ZIP_METHOD_DEFLATED) {
        ESP_LOGW(TAG, "%s uses compression method %u", name, e->method);
        return NULL;
    }

    /* The data follows the local header, whose own name and extra field may
     * differ in length from the directory's. */
    uint8_t local[ZIP_LOCAL_SIZE];
    if (!read_at(zip->file, (long)e->local_offset, local, sizeof(local)) ||
        le32(local) != ZIP_SIG_LOCAL) {
        ESP_LOGW(TAG, "%s has no local header", name);
        return NULL;
    }
    uint64_t data_offset = (uint64_t)e->local_offset + ZIP_LOCAL_SIZE +
                           le16(local + 26) + le16(local + 28);
    if (data_offset + e->compressed_size > (uint64_t)zip->file_size) {
        ESP_LOGW(TAG, "%s runs past the end of the archive", name);
        return NULL;
    }

    char *out = epub_malloc((size_t)e->size + 1);
    if (!out) return NULL;

    bool ok;
    if (e->method == ZIP_METHOD_STORED) {
        ok = e->compressed_size == e->size &&
             read_at(zip->file, (long)data_offset, out, e->size);
    } else if (e->size == 0) {
        ok = true;
    } else {
        uint8_t *packed = epub_malloc(e->compressed_size ? e->compressed_size : 1);
        ok = packed &&
             read_at(zip->file, (long)data_offset, packed, e->compressed_size) &&
             inflate_into(packed, e->compressed_size, (uint8_t *)out, e->size);
        epub_free(packed);
    }

    if (!ok) {
        ESP_LOGW(TAG, "%s is corrupt", name);
        epub_free(out);
        return NULL;
    }
    out[e->size] = '\0';
    if (len_out) *len_out = e->size;
    return out;
}
