/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Map Pack reader
 *
 * docs/maps/map-pack-format.md is the format; map_pack.h says how it is
 * read. Everything read from the file is checked before it is used: the
 * file is a user's copy on a card, and may be cut short, damaged, or made
 * by a newer tool.
 */
#include "map_engine/map_pack.h"

#include "esp_heap_caps.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_LEN        32
#define DIR_ENTRY_LEN     16
#define MAX_SECTIONS      32
#define INDEX_ENTRY_LEN   24
#define MAX_CREDIT_LEN    1024
#define MAX_REGIONS_LEN   (64 * 1024)
/* Raw DEFLATE of 8192 bytes is at most a little over 8192 (stored blocks);
 * anything past this is damage. */
#define MAX_PAYLOAD       (16 * 1024)

static const uint8_t MAGIC[8] = {'T', 'H', 'S', 'T', 'M', 'A', 'P', 0};

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t crc;
    bool     present;
} section_t;

struct map_pack {
    FILE        *file;
    unsigned     major;
    unsigned     minor;
    uint32_t     tile_count;
    section_t    index;
    section_t    tiles;
    char        *credit;
    map_region_t regions[MAP_PACK_MAX_REGIONS];
    int          region_count;
    uint8_t     *payload;  /* MAX_PAYLOAD, in PSRAM */
};

/* ------------------------------------------------------------------ */
/* Bytes                                                                */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t map_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    /* A nibble at a time: 64 bytes of table, and a tile in about a
     * millisecond on an S3. */
    static const uint32_t T[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t c = ~crc;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        c = (c >> 4) ^ T[c & 15];
        c = (c >> 4) ^ T[c & 15];
    }
    return ~c;
}

static bool read_at(FILE *f, uint32_t offset, void *buf, size_t len)
{
    if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, f) == len;
}

static void *alloc_psram(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(len);
}

/* Copies at most cap - 1 bytes of UTF-8, ending at a character's end, and
 * turns control characters into spaces: the name is the file's, and goes
 * into a log and a label. */
static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (len > cap - 1) {
        len = cap - 1;
        while (len > 0 && (src[len] & 0xC0) == 0x80) len--;
    }
    for (size_t i = 0; i < len; i++) dst[i] = src[i] < 0x20 || src[i] == 0x7f ? ' ' : (char)src[i];
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Opening                                                              */
/* ------------------------------------------------------------------ */

static bool section_fits(const section_t *s, uint32_t first, uint64_t file_size)
{
    return s->offset >= first && (uint64_t)s->offset + s->length <= file_size;
}

static bool read_section(FILE *f, const section_t *s, uint8_t **out)
{
    uint8_t *buf = malloc(s->length ? s->length : 1);
    if (!buf) return false;
    if (s->length && !read_at(f, s->offset, buf, s->length)) {
        free(buf);
        return false;
    }
    if (map_crc32(0, buf, s->length) != s->crc) {
        free(buf);
        return false;
    }
    *out = buf;
    return true;
}

static bool parse_regions(map_pack_t *pack, const uint8_t *p, uint32_t len)
{
    if (len < 2) return false;
    unsigned count = rd16(p);
    uint32_t at = 2;
    for (unsigned i = 0; i < count; i++) {
        if (len - at < 4) return false;
        unsigned kind = p[at], deepest = p[at + 1], name_len = rd16(p + at + 2);
        at += 4;
        if (kind > 1 || deepest < MAP_MIN_ZOOM || deepest > MAP_MAX_ZOOM) return false;
        if (len - at < name_len) return false;
        const uint8_t *name = p + at;
        at += name_len;

        double lat, lon;
        if (kind == 0) {
            if (len - at < 12) return false;
            lat = (int32_t)rd32(p + at) / 1e7;
            lon = (int32_t)rd32(p + at + 4) / 1e7;
            at += 12;
        } else {
            if (len - at < 2) return false;
            unsigned n = rd16(p + at);
            at += 2;
            if (n < 3 || (len - at) / 8 < n) return false;
            double s = 90, w = 180, nn = -90, e = -180;
            for (unsigned k = 0; k < n; k++) {
                double plat = (int32_t)rd32(p + at + 8 * k) / 1e7;
                double plon = (int32_t)rd32(p + at + 8 * k + 4) / 1e7;
                if (plat < s) s = plat;
                if (plat > nn) nn = plat;
                if (plon < w) w = plon;
                if (plon > e) e = plon;
            }
            at += 8 * n;
            lat = (s + nn) / 2;
            lon = (w + e) / 2;
        }
        if (lat < -85.0511 || lat > 85.0511 || lon < -180 || lon > 180) return false;
        if (pack->region_count < MAP_PACK_MAX_REGIONS) {
            map_region_t *r = &pack->regions[pack->region_count++];
            copy_text(r->name, sizeof r->name, name, name_len);
            r->circle = kind == 0;
            r->deepest_zoom = (int)deepest;
            r->centre_lat = lat;
            r->centre_lon = lon;
        }
    }
    return at == len && count > 0;
}

static map_pack_err_t open_file(map_pack_t *pack, unsigned *newer_major)
{
    FILE *f = pack->file;
    if (fseek(f, 0, SEEK_END) != 0) return MAP_PACK_ERR_IO;
    long end = ftell(f);
    if (end < 0) return MAP_PACK_ERR_IO;
    uint64_t file_size = (uint64_t)end;

    uint8_t header[HEADER_LEN];
    if (file_size < HEADER_LEN || !read_at(f, 0, header, HEADER_LEN)) return MAP_PACK_ERR_NOT_PACK;
    if (memcmp(header, MAGIC, sizeof MAGIC) != 0) return MAP_PACK_ERR_NOT_PACK;

    pack->major = rd16(header + 8);
    pack->minor = rd16(header + 10);
    if (pack->major == 0) return MAP_PACK_ERR_NOT_PACK;
    if (pack->major > MAP_PACK_MAJOR) {
        if (newer_major) *newer_major = pack->major;
        return MAP_PACK_ERR_NEWER;
    }
    unsigned header_len = rd16(header + 12);
    unsigned variant = header[14];
    unsigned min_zoom = header[16], max_zoom = header[17];
    unsigned sections = rd16(header + 18);
    pack->tile_count = rd32(header + 20);
    if (header_len != HEADER_LEN || sections == 0 || sections > MAX_SECTIONS) return MAP_PACK_ERR_DAMAGED;
    if (min_zoom < MAP_MIN_ZOOM || max_zoom > MAP_MAX_ZOOM || min_zoom > max_zoom) return MAP_PACK_ERR_DAMAGED;

    uint8_t directory[MAX_SECTIONS * DIR_ENTRY_LEN];
    size_t dir_len = (size_t)sections * DIR_ENTRY_LEN;
    if (!read_at(f, HEADER_LEN, directory, dir_len)) return MAP_PACK_ERR_DAMAGED;
    uint32_t crc = map_crc32(0, header, 28);
    crc = map_crc32(crc, directory, dir_len);
    if (crc != rd32(header + 28)) return MAP_PACK_ERR_DAMAGED;
    /* Checked after the CRC, so a damaged byte is not taken for colour. */
    if (variant != 0) return MAP_PACK_ERR_VARIANT;

    section_t attr = {0}, regn = {0};
    uint32_t first = HEADER_LEN + (uint32_t)dir_len;
    for (unsigned i = 0; i < sections; i++) {
        const uint8_t *d = directory + i * DIR_ENTRY_LEN;
        section_t s = {.offset = rd32(d + 4), .length = rd32(d + 8), .crc = rd32(d + 12), .present = true};
        section_t *slot = NULL;
        if (memcmp(d, "ATTR", 4) == 0) slot = &attr;
        else if (memcmp(d, "REGN", 4) == 0) slot = &regn;
        else if (memcmp(d, "TIDX", 4) == 0) slot = &pack->index;
        else if (memcmp(d, "TILE", 4) == 0) slot = &pack->tiles;
        /* Any other section (a name index, overlays) is a later minor
         * version's, and read past. */
        if (!slot) continue;
        if (slot->present || !section_fits(&s, first, file_size)) return MAP_PACK_ERR_DAMAGED;
        *slot = s;
    }
    if (!attr.present || !regn.present || !pack->index.present || !pack->tiles.present) {
        return MAP_PACK_ERR_DAMAGED;
    }
    if ((uint64_t)pack->index.length != (uint64_t)pack->tile_count * INDEX_ENTRY_LEN) {
        return MAP_PACK_ERR_DAMAGED;
    }
    if (attr.length > MAX_CREDIT_LEN || regn.length > MAX_REGIONS_LEN) return MAP_PACK_ERR_DAMAGED;
    /* fseek takes a long. */
    if (file_size > (uint64_t)LONG_MAX) return MAP_PACK_ERR_DAMAGED;

    uint8_t *buf = NULL;
    if (!read_section(f, &attr, &buf)) return MAP_PACK_ERR_DAMAGED;
    pack->credit = malloc(attr.length + 1);
    if (!pack->credit) {
        free(buf);
        return MAP_PACK_ERR_NO_MEMORY;
    }
    copy_text(pack->credit, attr.length + 1, buf, attr.length);
    free(buf);

    buf = NULL;
    if (!read_section(f, &regn, &buf)) return MAP_PACK_ERR_DAMAGED;
    bool ok = parse_regions(pack, buf, regn.length);
    free(buf);
    if (!ok) return MAP_PACK_ERR_DAMAGED;

    pack->payload = alloc_psram(MAX_PAYLOAD);
    if (!pack->payload) return MAP_PACK_ERR_NO_MEMORY;
    return MAP_PACK_OK;
}

map_pack_err_t map_pack_open(const char *path, map_pack_t **out, unsigned *newer_major)
{
    *out = NULL;
    map_pack_t *pack = calloc(1, sizeof *pack);
    if (!pack) return MAP_PACK_ERR_NO_MEMORY;
    pack->file = fopen(path, "rb");
    if (!pack->file) {
        free(pack);
        return MAP_PACK_ERR_IO;
    }
    map_pack_err_t err = open_file(pack, newer_major);
    if (err != MAP_PACK_OK) {
        map_pack_close(pack);
        return err;
    }
    *out = pack;
    return MAP_PACK_OK;
}

void map_pack_close(map_pack_t *pack)
{
    if (!pack) return;
    if (pack->file) fclose(pack->file);
    free(pack->credit);
    free(pack->payload);  /* heap_caps memory is free()d too */
    free(pack);
}

const char *map_pack_err_text(map_pack_err_t err)
{
    switch (err) {
    case MAP_PACK_OK:            return "opened";
    case MAP_PACK_ERR_IO:        return "cannot be read";
    case MAP_PACK_ERR_NOT_PACK:  return "not a Map Pack";
    case MAP_PACK_ERR_NEWER:     return "made by a newer Map Pack tool";
    case MAP_PACK_ERR_VARIANT:   return "made for a colour screen";
    case MAP_PACK_ERR_DAMAGED:   return "damaged";
    case MAP_PACK_ERR_NO_MEMORY: return "not enough memory";
    }
    return "unknown error";
}

unsigned map_pack_version_major(const map_pack_t *pack) { return pack->major; }
unsigned map_pack_version_minor(const map_pack_t *pack) { return pack->minor; }
uint32_t map_pack_tile_count(const map_pack_t *pack) { return pack->tile_count; }
int map_pack_region_count(const map_pack_t *pack) { return pack->region_count; }
const char *map_pack_credit(const map_pack_t *pack) { return pack->credit; }

const map_region_t *map_pack_region(const map_pack_t *pack, int i)
{
    return i >= 0 && i < pack->region_count ? &pack->regions[i] : NULL;
}

/* ------------------------------------------------------------------ */
/* Tiles                                                                */
/* ------------------------------------------------------------------ */

static int compare_key(int z, uint32_t x, uint32_t y, const uint8_t *entry)
{
    int ez = entry[8];
    uint32_t ex = rd32(entry), ey = rd32(entry + 4);
    if (z != ez) return z < ez ? -1 : 1;
    if (x != ex) return x < ex ? -1 : 1;
    if (y != ey) return y < ey ? -1 : 1;
    return 0;
}

map_tile_result_t map_pack_read_tile(map_pack_t *pack, int z, uint32_t x, uint32_t y,
                                     uint8_t out[MAP_TILE_BYTES])
{
    if (!pack || z < MAP_MIN_ZOOM || z > MAP_MAX_ZOOM) return MAP_TILE_NONE;
    uint32_t lo = 0, hi = pack->tile_count;
    uint8_t entry[INDEX_ENTRY_LEN];
    bool found = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!read_at(pack->file, pack->index.offset + mid * INDEX_ENTRY_LEN, entry, sizeof entry)) {
            return MAP_TILE_BAD;
        }
        int c = compare_key(z, x, y, entry);
        if (c == 0) {
            found = true;
            break;
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    if (!found) return MAP_TILE_NONE;

    uint32_t offset = rd32(entry + 12), length = rd32(entry + 16), crc = rd32(entry + 20);
    const section_t *t = &pack->tiles;
    if (length == 0 || length > MAX_PAYLOAD || offset < t->offset ||
        (uint64_t)offset + length > (uint64_t)t->offset + t->length) {
        return MAP_TILE_BAD;
    }
    if (!read_at(pack->file, offset, pack->payload, length)) return MAP_TILE_BAD;
    if (!map_inflate(pack->payload, length, out, MAP_TILE_BYTES)) return MAP_TILE_BAD;
    if (map_crc32(0, out, MAP_TILE_BYTES) != crc) return MAP_TILE_BAD;
    return MAP_TILE_OK;
}
