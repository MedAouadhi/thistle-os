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
/* Baked overlays: the format's limits make about 5 MB possible; a real
 * network is a few hundred KB, and the section is kept in PSRAM. */
#define MAX_OVERLAYS_LEN  (2 * 1024 * 1024)
#define OVERLAY_NAME_MAX  32

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

    /* OVLY, checked whole at opening, kept in PSRAM. */
    uint8_t     *ovly;
    uint32_t     ovly_len;
    bool         ovly_damaged;
    int          overlay_count;
    struct {
        char     name[OVERLAY_NAME_MAX];
        int      items;
        int      first;  /* its first item's place in item_at */
    } overlays[MAP_PACK_MAX_OVERLAYS];
    uint32_t    *item_at;  /* each item's offset in ovly, PSRAM */
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

/* OVLY, docs/maps/map-pack-format.md: checked byte by byte before any of
 * it is used, each item's offset kept. False if it does not hold. */
static bool parse_overlays(map_pack_t *pack, const uint8_t *p, uint32_t len)
{
    static const int least[4] = {1, 2, 3, 1};  /* point, line, polygon, text */
    if (len < 2) return false;
    unsigned count = rd16(p);
    if (count > MAP_PACK_MAX_OVERLAYS) return false;
    uint32_t at = 2;
    int total = 0;
    for (unsigned o = 0; o < count; o++) {
        if (len - at < 1) return false;
        unsigned name_len = p[at++];
        if (name_len == 0 || name_len >= OVERLAY_NAME_MAX || len - at < name_len + 2) return false;
        copy_text(pack->overlays[o].name, OVERLAY_NAME_MAX, p + at, name_len);
        at += name_len;
        unsigned items = rd16(p + at);
        at += 2;
        if (items > MAP_PACK_ITEMS_MAX) return false;
        pack->overlays[o].items = (int)items;
        pack->overlays[o].first = total;
        uint32_t vertices = 0;
        for (unsigned i = 0; i < items; i++) {
            if (len - at < 4) return false;
            pack->item_at[total++] = at;
            unsigned kind = p[at], icon = p[at + 1], n = rd16(p + at + 2);
            at += 4;
            if (kind > 3 || icon > 4 || (kind != 0 && icon != 0)) return false;
            if ((int)n < least[kind] || ((kind == 0 || kind == 3) && n != 1)) return false;
            vertices += n;
            if (vertices > MAP_PACK_VERTICES_MAX || (len - at) / 8 < n) return false;
            for (unsigned v = 0; v < n; v++) {
                int32_t lat = (int32_t)rd32(p + at), lon = (int32_t)rd32(p + at + 4);
                if (lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000) {
                    return false;
                }
                at += 8;
            }
            if (len - at < 1) return false;
            unsigned label_len = p[at++];
            if (label_len >= MAP_PACK_LABEL_MAX || len - at < label_len + 1) return false;
            at += label_len;
            unsigned props = p[at++];
            if (props > MAP_PACK_ITEM_PROPS) return false;
            for (unsigned k = 0; k < props; k++) {
                if (len - at < 1) return false;
                unsigned key_len = p[at++];
                if (key_len == 0 || key_len >= MAP_PACK_KEY_MAX || len - at < key_len + 1) {
                    return false;
                }
                at += key_len;
                unsigned value_len = p[at++];
                if (value_len >= MAP_PACK_VALUE_MAX || len - at < value_len) return false;
                at += value_len;
            }
        }
    }
    if (at != len) return false;
    pack->overlay_count = (int)count;
    return true;
}

/* Reads and checks OVLY; one that does not hold is left out, the Pack
 * opening without overlays. False only without the memory. */
static bool load_overlays(map_pack_t *pack, const section_t *s)
{
    if (s->length > MAX_OVERLAYS_LEN) {
        pack->ovly_damaged = true;
        return true;
    }
    pack->ovly = alloc_psram(s->length ? s->length : 1);
    pack->item_at = alloc_psram(sizeof(uint32_t) * MAP_PACK_MAX_OVERLAYS * MAP_PACK_ITEMS_MAX);
    if (!pack->ovly || !pack->item_at) return false;
    if ((s->length && !read_at(pack->file, s->offset, pack->ovly, s->length)) ||
        map_crc32(0, pack->ovly, s->length) != s->crc ||
        !parse_overlays(pack, pack->ovly, s->length)) {
        pack->ovly_damaged = true;
        pack->overlay_count = 0;
        free(pack->ovly);
        free(pack->item_at);
        pack->ovly = NULL;
        pack->item_at = NULL;
        return true;
    }
    pack->ovly_len = s->length;
    return true;
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

    section_t attr = {0}, regn = {0}, ovly = {0};
    uint32_t first = HEADER_LEN + (uint32_t)dir_len;
    for (unsigned i = 0; i < sections; i++) {
        const uint8_t *d = directory + i * DIR_ENTRY_LEN;
        section_t s = {.offset = rd32(d + 4), .length = rd32(d + 8), .crc = rd32(d + 12), .present = true};
        section_t *slot = NULL;
        if (memcmp(d, "ATTR", 4) == 0) slot = &attr;
        else if (memcmp(d, "REGN", 4) == 0) slot = &regn;
        else if (memcmp(d, "TIDX", 4) == 0) slot = &pack->index;
        else if (memcmp(d, "TILE", 4) == 0) slot = &pack->tiles;
        else if (memcmp(d, "OVLY", 4) == 0) slot = &ovly;  /* 1.1 */
        /* Any other section (a name index) is a later minor version's,
         * and read past. */
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

    if (ovly.present && !load_overlays(pack, &ovly)) return MAP_PACK_ERR_NO_MEMORY;

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
    free(pack->ovly);
    free(pack->item_at);
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

/* Finds tile (z, x, y)'s index entry: MAP_TILE_OK with it in entry. */
static map_tile_result_t find_entry(map_pack_t *pack, int z, uint32_t x, uint32_t y,
                                    uint8_t entry[INDEX_ENTRY_LEN])
{
    if (!pack || z < MAP_MIN_ZOOM || z > MAP_MAX_ZOOM) return MAP_TILE_NONE;
    uint32_t lo = 0, hi = pack->tile_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!read_at(pack->file, pack->index.offset + mid * INDEX_ENTRY_LEN, entry,
                     INDEX_ENTRY_LEN)) {
            return MAP_TILE_BAD;
        }
        int c = compare_key(z, x, y, entry);
        if (c == 0) return MAP_TILE_OK;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return MAP_TILE_NONE;
}

bool map_pack_has_tile(map_pack_t *pack, int z, uint32_t x, uint32_t y)
{
    uint8_t entry[INDEX_ENTRY_LEN];
    return find_entry(pack, z, x, y, entry) == MAP_TILE_OK;
}

map_tile_result_t map_pack_read_tile(map_pack_t *pack, int z, uint32_t x, uint32_t y,
                                     uint8_t out[MAP_TILE_BYTES])
{
    uint8_t entry[INDEX_ENTRY_LEN];
    map_tile_result_t found = find_entry(pack, z, x, y, entry);
    if (found != MAP_TILE_OK) return found;

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

/* ------------------------------------------------------------------ */
/* Baked overlays                                                       */
/* ------------------------------------------------------------------ */

int map_pack_overlay_count(const map_pack_t *pack) { return pack ? pack->overlay_count : 0; }
bool map_pack_overlays_damaged(const map_pack_t *pack) { return pack && pack->ovly_damaged; }

const char *map_pack_overlay_name(const map_pack_t *pack, int overlay)
{
    if (!pack || overlay < 0 || overlay >= pack->overlay_count) return NULL;
    return pack->overlays[overlay].name;
}

int map_pack_overlay_items(const map_pack_t *pack, int overlay)
{
    if (!pack || overlay < 0 || overlay >= pack->overlay_count) return 0;
    return pack->overlays[overlay].items;
}

/* Item `item`'s bytes: kind, icon, n, its vertices, label, props. NULL if
 * there is none such. parse_overlays() checked every length in it. */
static const uint8_t *item_bytes(const map_pack_t *pack, int overlay, int item)
{
    if (!pack || overlay < 0 || overlay >= pack->overlay_count || item < 0 ||
        item >= pack->overlays[overlay].items) {
        return NULL;
    }
    return pack->ovly + pack->item_at[pack->overlays[overlay].first + item];
}

/* Its label's length byte, after the vertices. */
static const uint8_t *item_label(const uint8_t *it) { return it + 4 + 8 * (size_t)rd16(it + 2); }

bool map_pack_overlay_item(const map_pack_t *pack, int overlay, int item, map_pack_item_t *out)
{
    const uint8_t *it = item_bytes(pack, overlay, item);
    if (!it) return false;
    out->kind = it[0];
    out->icon = it[1];
    out->vertices = rd16(it + 2);
    const uint8_t *label = item_label(it);
    copy_text(out->label, sizeof out->label, label + 1, label[0]);
    out->props = label[1 + label[0]];
    return true;
}

bool map_pack_overlay_vertex(const map_pack_t *pack, int overlay, int item, int v, double *lat,
                             double *lon)
{
    const uint8_t *it = item_bytes(pack, overlay, item);
    if (!it || v < 0 || v >= rd16(it + 2)) return false;
    *lat = (int32_t)rd32(it + 4 + 8 * (size_t)v) / 1e7;
    *lon = (int32_t)rd32(it + 8 + 8 * (size_t)v) / 1e7;
    return true;
}

bool map_pack_overlay_prop(const map_pack_t *pack, int overlay, int item, int p, char *key,
                           size_t key_cap, char *value, size_t value_cap)
{
    const uint8_t *it = item_bytes(pack, overlay, item);
    if (!it || key_cap == 0 || value_cap == 0) return false;
    const uint8_t *at = item_label(it);
    at += 1 + at[0];
    int props = *at++;
    if (p < 0 || p >= props) return false;
    for (int k = 0; k < p; k++) {
        at += 1 + at[0];
        at += 1 + at[0];
    }
    copy_text(key, key_cap, at + 1, at[0]);
    at += 1 + at[0];
    copy_text(value, value_cap, at + 1, at[0]);
    return true;
}
