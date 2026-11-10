// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The map engine's reader over the Map Pack tool's golden fixture
// (tools/maps/testdata/fixture.tmap) and copies of it damaged by hand: cut
// short at every length, a bit flipped in every byte, fields made wrong
// with their CRCs made right again so the checks behind the CRCs are
// reached, and random damage. A Pack is the user's file from a card: what
// it shows is either its tiles, the hatching, or a refusal, and the reader
// never reads or writes outside its buffers (this runs under ASan and
// UBSan, apps/../components/map_engine/tests/run_tests.sh).
//
// The views composed from it are checked pixel by pixel against the tiles
// read one at a time and the hatching docs/maps/map-pack-format.md
// specifies.
//
// The baked overlays (OVLY, format 1.1) are read from the transit fixture
// (fixture-transit.tmap, the same Pack with the fixture's transit network
// baked in) and from copies of its OVLY damaged every way: a damaged OVLY
// leaves the Pack with no overlays and its tiles as they were.
//
//     test_map_pack FIXTURE.tmap WORK_DIR [TRANSIT.tmap]

#include "map_engine/map_pack.h"
#include "map_engine/map_projection.h"
#include "map_compose.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;
static char s_path[512];

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            s_failures++;                                                     \
        }                                                                     \
    } while (0)

/* Says which case failed, once, without flooding the output. */
#define EXPECT_AT(cond, what, i)                                              \
    do {                                                                      \
        if (!(cond)) {                                                        \
            if (s_failures < 40)                                              \
                fprintf(stderr, "%s:%d: %s, %s %ld\n", __FILE__, __LINE__,    \
                        #cond, what, (long)(i));                              \
            s_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* Bytes, read here without the reader's code                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *d;
    size_t   len;
} buf_t;

static buf_t s_golden;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* CRC-32, a bit at a time: not the reader's. */
static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static buf_t copy(const buf_t *b)
{
    buf_t c = {malloc(b->len ? b->len : 1), b->len};
    if (b->len) memcpy(c.d, b->d, b->len);
    return c;
}

typedef struct {
    char     tag[5];
    uint32_t off, len;
} sect_t;

/* The directory of a Pack whose directory lies inside it; -1 if not. */
static int sections(const buf_t *b, sect_t *out, int max)
{
    if (b->len < 32) return -1;
    int n = b->d[18] | (b->d[19] << 8);
    if (n > max || 32 + (size_t)n * 16 > b->len) return -1;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = b->d + 32 + 16 * i;
        memcpy(out[i].tag, e, 4);
        out[i].tag[4] = '\0';
        out[i].off = rd32(e + 4);
        out[i].len = rd32(e + 8);
    }
    return n;
}

static const sect_t *find(const sect_t *s, int n, const char *tag)
{
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].tag, tag) == 0) return &s[i];
    return NULL;
}

/* Makes the header's and each section's CRC right again, for what lies
 * inside the file, as a careful tool would have written them. */
static void rehash_some(buf_t *b, bool sections_too)
{
    sect_t s[64];
    int n = sections(b, s, 64);
    if (n < 0) return;
    for (int i = 0; sections_too && i < n; i++) {
        uint32_t crc = 0;
        if (strcmp(s[i].tag, "TILE") != 0) {
            if ((uint64_t)s[i].off + s[i].len > b->len) continue;
            crc = crc32_ref(b->d + s[i].off, s[i].len);
        }
        wr32(b->d + 32 + 16 * i + 12, crc);
    }
    uint8_t *tmp = malloc(28 + 16 * (size_t)n);
    memcpy(tmp, b->d, 28);
    memcpy(tmp + 28, b->d + 32, 16 * (size_t)n);
    wr32(b->d + 28, crc32_ref(tmp, 28 + 16 * (size_t)n));
    free(tmp);
}

static void rehash(buf_t *b)
{
    rehash_some(b, true);
}

typedef struct {
    const char    *tag;
    const uint8_t *data;
    size_t         len;
} rep_t;

/* The golden Pack with sections replaced or added: laid out again in order,
 * TIDX's payload offsets moved with TILE, every CRC made right. */
static buf_t rebuild(const rep_t *reps, int nreps)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    const uint8_t *body[64];
    size_t len[64];
    char tags[64][5];
    for (int i = 0; i < n; i++) {
        memcpy(tags[i], s[i].tag, 5);
        body[i] = s_golden.d + s[i].off;
        len[i] = s[i].len;
    }
    for (int r = 0; r < nreps; r++) {
        int at = -1;
        for (int i = 0; i < n; i++)
            if (strcmp(tags[i], reps[r].tag) == 0) at = i;
        if (at < 0) {
            at = n++;
            memcpy(tags[at], reps[r].tag, 5);
        }
        body[at] = reps[r].data;
        len[at] = reps[r].len;
    }
    size_t total = 32 + 16 * (size_t)n;
    for (int i = 0; i < n; i++) total += len[i];
    buf_t b = {calloc(1, total), total};
    memcpy(b.d, s_golden.d, 32);
    wr16(b.d + 18, (unsigned)n);
    size_t at = 32 + 16 * (size_t)n;
    uint32_t old_tile = find(s, sections(&s_golden, s, 64), "TILE")->off, new_tile = 0;
    size_t tidx_at = 0, tidx_len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *e = b.d + 32 + 16 * i;
        memcpy(e, tags[i], 4);
        wr32(e + 4, (uint32_t)at);
        wr32(e + 8, (uint32_t)len[i]);
        memcpy(b.d + at, body[i], len[i]);
        if (strcmp(tags[i], "TILE") == 0) new_tile = (uint32_t)at;
        if (strcmp(tags[i], "TIDX") == 0) {
            tidx_at = at;
            tidx_len = len[i];
        }
        at += len[i];
    }
    for (size_t e = tidx_at; e + 24 <= tidx_at + tidx_len; e += 24)
        wr32(b.d + e + 12, rd32(b.d + e + 12) - old_tile + new_tile);
    rehash(&b);
    return b;
}

typedef struct {
    uint32_t x, y;
    int      z;
    uint32_t off, len;
} entry_t;

static int s_entry_count;
static entry_t s_entries[64];

static void read_golden_index(void)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    const sect_t *t = find(s, n, "TIDX");
    for (uint32_t e = 0; e < t->len / 24 && s_entry_count < 64; e++) {
        const uint8_t *p = s_golden.d + t->off + 24 * e;
        s_entries[s_entry_count++] =
            (entry_t){rd32(p), rd32(p + 4), p[8], rd32(p + 12), rd32(p + 16)};
    }
}

/* ------------------------------------------------------------------ */
/* Opening                                                              */
/* ------------------------------------------------------------------ */

static map_pack_err_t open_bytes(const uint8_t *d, size_t len, map_pack_t **pack, unsigned *newer)
{
    FILE *f = fopen(s_path, "wb");
    if (!f) {
        perror(s_path);
        exit(2);
    }
    if (len) fwrite(d, 1, len, f);
    fclose(f);
    return map_pack_open(s_path, pack, newer);
}

static map_pack_err_t open_buf(const buf_t *b, map_pack_t **pack)
{
    return open_bytes(b->d, b->len, pack, NULL);
}

/* Opens b, expecting err; closes it if it opened. */
static void expect_open(const buf_t *b, map_pack_err_t err, const char *what)
{
    map_pack_t *pack = NULL;
    map_pack_err_t got = open_buf(b, &pack);
    if (got != err) {
        fprintf(stderr, "%s: opened as %d (%s), not %d\n", what, (int)got,
                map_pack_err_text(got), (int)err);
        s_failures++;
    }
    EXPECT((pack != NULL) == (got == MAP_PACK_OK));
    map_pack_close(pack);
}

/* ------------------------------------------------------------------ */
/* Raw DEFLATE, by hand: stored blocks                                  */
/* ------------------------------------------------------------------ */

static size_t stored(const uint8_t *src, size_t n, uint8_t *out)
{
    size_t at = 0;
    do {
        size_t chunk = n > 65535 ? 65535 : n;
        out[at++] = chunk == n ? 1 : 0;  /* BFINAL, BTYPE 00 */
        wr16(out + at, (unsigned)chunk);
        wr16(out + at + 2, (unsigned)(~chunk & 0xFFFF));
        at += 4;
        memcpy(out + at, src, chunk);
        at += chunk;
        src += chunk;
        n -= chunk;
    } while (n > 0);
    return at;
}

/* The golden Pack with tile k's payload replaced by payload, and its
 * index entry's CRC by crc. */
static buf_t with_tile(int k, const uint8_t *payload, size_t len, uint32_t crc)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    const sect_t *ts = find(s, n, "TILE"), *xs = find(s, n, "TIDX");
    uint8_t *tile = malloc(ts->len + len);
    memcpy(tile, s_golden.d + ts->off, ts->len);
    memcpy(tile + ts->len, payload, len);
    uint8_t *idx = malloc(xs->len);
    memcpy(idx, s_golden.d + xs->off, xs->len);
    wr32(idx + 24 * k + 12, ts->off + ts->len);
    wr32(idx + 24 * k + 16, (uint32_t)len);
    wr32(idx + 24 * k + 20, crc);
    rep_t reps[] = {{"TIDX", idx, xs->len}, {"TILE", tile, ts->len + len}};
    buf_t b = rebuild(reps, 2);
    free(tile);
    free(idx);
    return b;
}

/* The golden Pack with tile k's index entry's field at `field` set. */
static buf_t with_entry_field(int k, int field, uint32_t value)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    const sect_t *xs = find(s, n, "TIDX");
    uint8_t *idx = malloc(xs->len);
    memcpy(idx, s_golden.d + xs->off, xs->len);
    wr32(idx + 24 * k + field, value);
    rep_t reps[] = {{"TIDX", idx, xs->len}};
    buf_t b = rebuild(reps, 1);
    free(idx);
    return b;
}

static map_tile_result_t read_entry(map_pack_t *pack, int k, uint8_t *out)
{
    return map_pack_read_tile(pack, s_entries[k].z, s_entries[k].x, s_entries[k].y, out);
}

/* ------------------------------------------------------------------ */
/* Cases                                                                */
/* ------------------------------------------------------------------ */

static void crc_is_ieee(void)
{
    const uint8_t v[] = "123456789";
    EXPECT(map_crc32(0, v, 9) == 0xCBF43926u);
    EXPECT(map_crc32(map_crc32(0, v, 4), v + 4, 5) == 0xCBF43926u);
    EXPECT(crc32_ref(v, 9) == 0xCBF43926u);
}

static void golden_opens_and_every_tile_reads(void)
{
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&s_golden, &pack) == MAP_PACK_OK);
    if (!pack) return;
    EXPECT(map_pack_version_major(pack) == 1);
    EXPECT(map_pack_version_minor(pack) == 0);
    EXPECT(map_pack_tile_count(pack) == 22);
    EXPECT(strncmp(map_pack_credit(pack), "\xC2\xA9 OpenStreetMap contributors.", 30) == 0);
    EXPECT(map_pack_region_count(pack) == 2);
    const map_region_t *medina = map_pack_region(pack, 0), *lac = map_pack_region(pack, 1);
    EXPECT(strcmp(medina->name, "Medina") == 0);
    EXPECT(medina->circle && medina->deepest_zoom == 17);
    EXPECT(fabs(medina->centre_lat - 36.8) < 1e-7 && fabs(medina->centre_lon - 10.18) < 1e-7);
    EXPECT(strcmp(lac->name, "Lac Nord") == 0);
    EXPECT(!lac->circle && lac->deepest_zoom == 15);
    EXPECT(fabs(lac->centre_lat - (36.8126644 + 36.8176949) / 2) < 1e-7);
    EXPECT(fabs(lac->centre_lon - (10.1964093 + 10.2035907) / 2) < 1e-7);
    EXPECT(map_pack_region(pack, 2) == NULL && map_pack_region(pack, -1) == NULL);

    EXPECT(s_entry_count == 22);
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    for (int k = 0; k < s_entry_count; k++) EXPECT_AT(read_entry(pack, k, tile) == MAP_TILE_OK, "tile", k);
    /* Outside every Region, and outside the four levels. */
    EXPECT(map_pack_read_tile(pack, 16, 0, 0, tile) == MAP_TILE_NONE);
    EXPECT(map_pack_read_tile(pack, 14, 8655, 6389, tile) == MAP_TILE_NONE);
    EXPECT(map_pack_read_tile(pack, 13, 4327, 3194, tile) == MAP_TILE_NONE);
    EXPECT(map_pack_read_tile(pack, 18, 138482, 102214, tile) == MAP_TILE_NONE);
    EXPECT(map_pack_read_tile(pack, 17, 0xFFFFFFFFu, 0xFFFFFFFFu, tile) == MAP_TILE_NONE);
    EXPECT(map_pack_read_tile(NULL, 16, 34620, 25553, tile) == MAP_TILE_NONE);
    free(tile);
    map_pack_close(pack);
}

static void cut_short_at_every_length_is_refused(void)
{
    for (size_t len = 0; len < s_golden.len; len++) {
        map_pack_t *pack = NULL;
        map_pack_err_t err = open_bytes(s_golden.d, len, &pack, NULL);
        EXPECT_AT(err == MAP_PACK_ERR_NOT_PACK || err == MAP_PACK_ERR_DAMAGED, "length", len);
        EXPECT_AT(pack == NULL, "length", len);
        map_pack_close(pack);
    }
    map_pack_t *pack = NULL;
    EXPECT(map_pack_open("/nonexistent/x.tmap", &pack, NULL) == MAP_PACK_ERR_IO && !pack);
}

/* Where a byte of the golden Pack is. */
typedef enum { IN_MAGIC, IN_MAJOR, IN_HEADER, IN_DIRECTORY, IN_CHECKED, IN_INDEX, IN_TILE } place_t;

static place_t place_of(size_t i, int *tile)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    if (i < 8) return IN_MAGIC;
    if (i < 10) return IN_MAJOR;
    if (i < 32) return IN_HEADER;
    if (i < 32 + 16 * (size_t)n) return IN_DIRECTORY;
    const sect_t *x = find(s, n, "TIDX");
    if (i >= x->off && i < x->off + x->len) return IN_INDEX;
    for (int k = 0; k < s_entry_count; k++) {
        if (i >= s_entries[k].off && i < s_entries[k].off + s_entries[k].len) {
            *tile = k;
            return IN_TILE;
        }
    }
    return IN_CHECKED;  /* ATTR or REGN, whose CRCs are checked at opening */
}

static void a_bit_flipped_in_every_byte(void)
{
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    for (size_t i = 0; i < s_golden.len; i++) {
        buf_t b = copy(&s_golden);
        b.d[i] ^= (uint8_t)(1u << (i % 8));
        int k = -1;
        place_t where = place_of(i, &k);
        map_pack_t *pack = NULL;
        unsigned newer = 0;
        map_pack_err_t err = open_bytes(b.d, b.len, &pack, &newer);
        switch (where) {
        case IN_MAGIC:
            EXPECT_AT(err == MAP_PACK_ERR_NOT_PACK, "byte", i);
            break;
        case IN_MAJOR: {
            unsigned major = b.d[8] | (b.d[9] << 8);
            EXPECT_AT(major == 0 ? err == MAP_PACK_ERR_NOT_PACK
                                 : err == MAP_PACK_ERR_NEWER && newer == major, "byte", i);
            break;
        }
        case IN_HEADER:
        case IN_DIRECTORY:
        case IN_CHECKED:
            EXPECT_AT(err == MAP_PACK_ERR_DAMAGED, "byte", i);
            break;
        case IN_INDEX:
            /* The index is read an entry at a time: whatever it now says,
             * the tiles read as tiles, none, or damaged. */
            EXPECT_AT(err == MAP_PACK_OK, "byte", i);
            for (int e = 0; pack && e < s_entry_count; e++) {
                map_tile_result_t r = read_entry(pack, e, tile);
                EXPECT_AT(r == MAP_TILE_OK || r == MAP_TILE_NONE || r == MAP_TILE_BAD, "byte", i);
            }
            break;
        case IN_TILE:
            EXPECT_AT(err == MAP_PACK_OK, "byte", i);
            for (int e = 0; pack && e < s_entry_count; e++) {
                map_tile_result_t r = read_entry(pack, e, tile);
                /* The damaged one may still inflate to the same pixels (a
                 * flipped padding bit); any other is untouched. */
                if (e == k) EXPECT_AT(r == MAP_TILE_BAD || r == MAP_TILE_OK, "byte", i);
                else EXPECT_AT(r == MAP_TILE_OK, "byte", i);
            }
            break;
        }
        map_pack_close(pack);
        free(b.d);
    }
    free(tile);
}

static void a_newer_format_is_refused_and_a_newer_minor_read(void)
{
    buf_t b = copy(&s_golden);
    wr16(b.d + 8, 2);
    map_pack_t *pack = NULL;
    unsigned newer = 0;
    EXPECT(open_bytes(b.d, b.len, &pack, &newer) == MAP_PACK_ERR_NEWER && newer == 2 && !pack);
    /* Refused for its version whatever else it holds. */
    memset(b.d + 12, 0xEE, b.len - 12);
    EXPECT(open_bytes(b.d, b.len, &pack, &newer) == MAP_PACK_ERR_NEWER && !pack);
    wr16(b.d + 8, 0xFFFF);
    EXPECT(open_bytes(b.d, b.len, &pack, &newer) == MAP_PACK_ERR_NEWER && newer == 0xFFFF);
    free(b.d);

    b = copy(&s_golden);
    wr16(b.d + 8, 0);
    rehash(&b);
    expect_open(&b, MAP_PACK_ERR_NOT_PACK, "major 0");
    free(b.d);

    /* 1.7, with a section 1.0 does not know: read past. */
    const uint8_t names[] = "a name index of a later version";
    rep_t add[] = {{"NAME", names, sizeof names}};
    b = rebuild(add, 1);
    wr16(b.d + 10, 7);
    rehash(&b);
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
    if (pack) {
        EXPECT(map_pack_version_minor(pack) == 7);
        uint8_t *tile = malloc(MAP_TILE_BYTES);
        for (int k = 0; k < s_entry_count; k++) EXPECT_AT(read_entry(pack, k, tile) == MAP_TILE_OK, "tile", k);
        free(tile);
        map_pack_close(pack);
    }
    free(b.d);
}

/* A header field set to value, the CRC made right. */
static void header_field(int at, int size, uint32_t value, map_pack_err_t err, const char *what)
{
    buf_t b = copy(&s_golden);
    if (size == 1) b.d[at] = (uint8_t)value;
    else if (size == 2) wr16(b.d + at, value);
    else wr32(b.d + at, value);
    rehash(&b);
    expect_open(&b, err, what);
    free(b.d);
}

static void header_fields_that_do_not_hold(void)
{
    header_field(14, 1, 1, MAP_PACK_ERR_VARIANT, "colour variant");
    header_field(12, 2, 33, MAP_PACK_ERR_DAMAGED, "header length 33");
    header_field(12, 2, 0, MAP_PACK_ERR_DAMAGED, "header length 0");
    header_field(16, 1, 13, MAP_PACK_ERR_DAMAGED, "lowest zoom 13");
    header_field(17, 1, 18, MAP_PACK_ERR_DAMAGED, "deepest zoom 18");
    buf_t b = copy(&s_golden);
    b.d[16] = 16;
    b.d[17] = 15;
    rehash(&b);
    expect_open(&b, MAP_PACK_ERR_DAMAGED, "lowest above deepest");
    free(b.d);
    header_field(18, 2, 0, MAP_PACK_ERR_DAMAGED, "no sections");
    header_field(18, 2, 33, MAP_PACK_ERR_DAMAGED, "33 sections");
    header_field(18, 2, 0xFFFF, MAP_PACK_ERR_DAMAGED, "65535 sections");
    header_field(18, 2, 3, MAP_PACK_ERR_DAMAGED, "a section left out");
    header_field(20, 4, 21, MAP_PACK_ERR_DAMAGED, "a tile too few");
    header_field(20, 4, 23, MAP_PACK_ERR_DAMAGED, "a tile too many");
    /* 24 times it is TIDX's length, 528, in 32 bits. */
    header_field(20, 4, 22 + (1u << 29), MAP_PACK_ERR_DAMAGED, "a tile count that wraps");
    header_field(20, 4, 0xFFFFFFFFu, MAP_PACK_ERR_DAMAGED, "every tile");
}

/* Directory entry i's field at `field` set. */
static void directory_field(const char *tag, int field, uint32_t value, map_pack_err_t err,
                            const char *what)
{
    buf_t b = copy(&s_golden);
    sect_t s[64];
    int n = sections(&b, s, 64);
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].tag, tag) == 0) {
            if (field == 0) memcpy(b.d + 32 + 16 * i, &value, 4);
            else wr32(b.d + 32 + 16 * i + field, value);
        }
    /* The header's CRC only: the sections' are what a case may set. */
    rehash_some(&b, false);
    expect_open(&b, err, what);
    free(b.d);
}

static void sections_that_do_not_hold(void)
{
    uint32_t regn;
    memcpy(&regn, "REGN", 4);
    directory_field("ATTR", 0, regn, MAP_PACK_ERR_DAMAGED, "REGN twice, no ATTR");
    uint32_t junk;
    memcpy(&junk, "JUNK", 4);
    directory_field("TILE", 0, junk, MAP_PACK_ERR_DAMAGED, "no TILE");
    directory_field("TIDX", 0, junk, MAP_PACK_ERR_DAMAGED, "no TIDX");
    directory_field("REGN", 4, (uint32_t)s_golden.len, MAP_PACK_ERR_DAMAGED, "REGN past the end");
    directory_field("REGN", 4, 0, MAP_PACK_ERR_DAMAGED, "REGN over the header");
    directory_field("REGN", 4, 40, MAP_PACK_ERR_DAMAGED, "REGN over the directory");
    directory_field("TILE", 4, 0xFFFFFF00u, MAP_PACK_ERR_DAMAGED, "TILE's end wraps");
    directory_field("TILE", 8, 0xFFFFFFFFu, MAP_PACK_ERR_DAMAGED, "TILE's length wraps");
    directory_field("TIDX", 8, 22 * 24 + 1, MAP_PACK_ERR_DAMAGED, "TIDX not 24 bytes a tile");
    directory_field("ATTR", 12, 0x12345678u, MAP_PACK_ERR_DAMAGED, "ATTR's CRC");
    directory_field("REGN", 12, 0x12345678u, MAP_PACK_ERR_DAMAGED, "REGN's CRC");
    /* TIDX's own CRC is not read at opening (its entries are read one at a
     * time), and TILE has none. */
    directory_field("TIDX", 12, 0x12345678u, MAP_PACK_OK, "TIDX's CRC");
    directory_field("TILE", 12, 0x12345678u, MAP_PACK_OK, "TILE's CRC");
}

static void the_credit(void)
{
    uint8_t long_credit[1025];
    memset(long_credit, 'a', sizeof long_credit);
    rep_t too_long[] = {{"ATTR", long_credit, sizeof long_credit}};
    buf_t b = rebuild(too_long, 1);
    expect_open(&b, MAP_PACK_ERR_DAMAGED, "a credit over 1024 bytes");
    free(b.d);

    rep_t longest[] = {{"ATTR", long_credit, 1024}};
    b = rebuild(longest, 1);
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK && strlen(map_pack_credit(pack)) == 1024);
    map_pack_close(pack);
    free(b.d);

    rep_t empty[] = {{"ATTR", (const uint8_t *)"", 0}};
    b = rebuild(empty, 1);
    pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK && strcmp(map_pack_credit(pack), "") == 0);
    map_pack_close(pack);
    free(b.d);

    const uint8_t odd[] = {'a', '\n', 'b', 0, 'c', 0x7f, 'd'};
    rep_t controls[] = {{"ATTR", odd, sizeof odd}};
    b = rebuild(controls, 1);
    pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK && strcmp(map_pack_credit(pack), "a b c d") == 0);
    map_pack_close(pack);
    free(b.d);
}

/* REGN builders */
static size_t region_circle(uint8_t *p, int kind, int deepest, const char *name, size_t name_len,
                            int32_t lat, int32_t lon)
{
    p[0] = (uint8_t)kind;
    p[1] = (uint8_t)deepest;
    wr16(p + 2, (unsigned)name_len);
    memcpy(p + 4, name, name_len);
    size_t at = 4 + name_len;
    wr32(p + at, (uint32_t)lat);
    wr32(p + at + 4, (uint32_t)lon);
    wr32(p + at + 8, 350);
    return at + 12;
}

static void expect_regions(const uint8_t *regn, size_t len, map_pack_err_t err, const char *what)
{
    rep_t r[] = {{"REGN", regn, len}};
    buf_t b = rebuild(r, 1);
    expect_open(&b, err, what);
    free(b.d);
}

static void regions_that_do_not_hold(void)
{
    uint8_t r[2048];
    size_t n;
    const int32_t LAT = 368000000, LON = 101800000;

    wr16(r, 0);
    expect_regions(r, 2, MAP_PACK_ERR_DAMAGED, "no Region");
    expect_regions(r, 1, MAP_PACK_ERR_DAMAGED, "a count cut short");

    wr16(r, 1);
    n = 2 + region_circle(r + 2, 0, 17, "M", 1, LAT, LON);
    expect_regions(r, n, MAP_PACK_OK, "one circle");
    expect_regions(r, n - 1, MAP_PACK_ERR_DAMAGED, "a circle cut short");
    for (size_t cut = 0; cut < n; cut++) expect_regions(r, cut, MAP_PACK_ERR_DAMAGED, "cut short");
    r[n] = 0;
    expect_regions(r, n + 1, MAP_PACK_ERR_DAMAGED, "a byte after the last Region");

    wr16(r, 0xFFFF);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "65535 Regions, one there");

    wr16(r, 1);
    region_circle(r + 2, 2, 17, "M", 1, LAT, LON);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "kind 2");
    region_circle(r + 2, 0, 13, "M", 1, LAT, LON);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "deepest z13");
    region_circle(r + 2, 0, 18, "M", 1, LAT, LON);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "deepest z18");
    region_circle(r + 2, 0, 17, "M", 1, 890000000, LON);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "latitude 89");
    region_circle(r + 2, 0, 17, "M", 1, LAT, 1810000000);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "longitude 181");
    region_circle(r + 2, 0, 17, "M", 1, LAT, LON);
    wr16(r + 4, 0xFFFF);
    expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "a name past the end");

    /* Polygons: at least three points, all there. */
    for (unsigned points = 0; points < 5; points++) {
        wr16(r, 1);
        r[2] = 1;
        r[3] = 16;
        wr16(r + 4, 0);
        wr16(r + 6, points);
        n = 8;
        for (unsigned k = 0; k < points; k++, n += 8) {
            wr32(r + n, (uint32_t)(LAT + (int32_t)k * 1000));
            wr32(r + n + 4, (uint32_t)(LON + (int32_t)(k % 2) * 1000));
        }
        expect_regions(r, n, points >= 3 ? MAP_PACK_OK : MAP_PACK_ERR_DAMAGED, "a polygon");
        if (points >= 3) {
            expect_regions(r, n - 1, MAP_PACK_ERR_DAMAGED, "a polygon cut short");
            wr16(r + 6, 0xFFFF);
            expect_regions(r, n, MAP_PACK_ERR_DAMAGED, "65535 points, some there");
        }
    }

    /* More Regions than are kept: read past, the first 64 kept. */
    wr16(r, 70);
    n = 2;
    for (int k = 0; k < 70; k++) n += region_circle(r + n, 0, 16, "R", 1, LAT, LON);
    rep_t many[] = {{"REGN", r, n}};
    buf_t b = rebuild(many, 1);
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK && map_pack_region_count(pack) == MAP_PACK_MAX_REGIONS);
    map_pack_close(pack);
    free(b.d);

    /* A long name is cut at a character's end; control characters are
     * spaces. */
    char name[255];
    for (int k = 0; k < 254; k += 2) {
        name[k] = (char)0xC3;
        name[k + 1] = (char)0xA9;  /* é */
    }
    name[0] = '\t';
    name[1] = 'x';
    wr16(r, 1);
    n = 2 + region_circle(r + 2, 0, 17, name, 254, LAT, LON);
    rep_t long_name[] = {{"REGN", r, n}};
    b = rebuild(long_name, 1);
    pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
    if (pack) {
        const char *got = map_pack_region(pack, 0)->name;
        EXPECT(strlen(got) == 62 && got[0] == ' ' && got[1] == 'x');
        EXPECT(((unsigned char)got[60] == 0xC3) && ((unsigned char)got[61] == 0xA9));
        map_pack_close(pack);
    }
    free(b.d);
}

static void index_entries_that_do_not_hold(void)
{
    sect_t s[64];
    int n = sections(&s_golden, s, 64);
    const sect_t *ts = find(s, n, "TILE");
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    struct {
        int         field;
        uint32_t    value;
        const char *what;
    } cases[] = {
        {16, 0, "a payload of no bytes"},
        {16, 16 * 1024 + 1, "a payload over 16 KB"},
        {16, 0xFFFFFFFFu, "a payload of 4 GB"},
        {12, 0, "a payload in the header"},
        {12, ts->off - 1, "a payload starting before TILE"},
        {12, ts->off + ts->len - 1, "a payload running past TILE"},
        {12, 0xFFFFFFF0u, "a payload whose end wraps"},
        {20, 0x12345678u, "a CRC that does not hold"},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        buf_t b = with_entry_field(3, cases[c].field, cases[c].value);
        map_pack_t *pack = NULL;
        EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
        for (int k = 0; pack && k < s_entry_count; k++) {
            map_tile_result_t r = read_entry(pack, k, tile);
            if (r != (k == 3 ? MAP_TILE_BAD : MAP_TILE_OK)) {
                fprintf(stderr, "%s: tile %d read as %d\n", cases[c].what, k, (int)r);
                s_failures++;
            }
        }
        map_pack_close(pack);
        free(b.d);
    }

    /* Entries out of order: the binary search misses some, and finds
     * nothing it should not. */
    buf_t b = with_entry_field(0, 0, 0xFFFFFFFFu);
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
    for (int k = 0; pack && k < s_entry_count; k++) {
        map_tile_result_t r = read_entry(pack, k, tile);
        EXPECT_AT(r == MAP_TILE_OK || r == MAP_TILE_NONE, "tile", k);
    }
    map_pack_close(pack);
    free(b.d);
    free(tile);
}

static void payloads_that_do_not_hold(void)
{
    uint8_t *pixels = calloc(1, MAP_TILE_BYTES + 1000);
    uint8_t *payload = malloc(MAP_TILE_BYTES + 2000);
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    for (int i = 0; i < MAP_TILE_BYTES + 1000; i++) pixels[i] = (uint8_t)(i * 7);
    uint32_t right = map_crc32(0, pixels, MAP_TILE_BYTES);
    struct {
        size_t      pixels;
        bool        ok;
        const char *what;
    } cases[] = {
        {MAP_TILE_BYTES, true, "a stored tile"},
        {100, false, "too few pixels"},
        {MAP_TILE_BYTES - 1, false, "a byte too few"},
        {MAP_TILE_BYTES + 1, false, "a byte too many"},
        {MAP_TILE_BYTES + 1000, false, "far too many"},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t len = stored(pixels, cases[c].pixels, payload);
        buf_t b = with_tile(5, payload, len, right);
        map_pack_t *pack = NULL;
        EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
        if (pack) {
            map_tile_result_t r = read_entry(pack, 5, tile);
            if (r != (cases[c].ok ? MAP_TILE_OK : MAP_TILE_BAD)) {
                fprintf(stderr, "%s: read as %d\n", cases[c].what, (int)r);
                s_failures++;
            }
            if (cases[c].ok) EXPECT(memcmp(tile, pixels, MAP_TILE_BYTES) == 0);
            map_pack_close(pack);
        }
        free(b.d);
    }

    /* Cut short, the stream's every length. */
    size_t len = stored(pixels, MAP_TILE_BYTES, payload);
    for (size_t cut = 1; cut < len; cut += 97) {
        buf_t b = with_tile(5, payload, cut, right);
        map_pack_t *pack = NULL;
        EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
        if (pack) EXPECT_AT(read_entry(pack, 5, tile) == MAP_TILE_BAD, "cut at", cut);
        map_pack_close(pack);
        free(b.d);
    }

    /* Not DEFLATE: a reserved block type, and a back reference before the
     * start (a fixed-Huffman block: length 3, distance 1, as the first
     * symbol). */
    const uint8_t reserved[] = {0x07, 0, 0, 0};
    const uint8_t before_start[] = {0x03, 0x02, 0x00, 0x00};
    const uint8_t *junk[] = {reserved, before_start};
    for (int j = 0; j < 2; j++) {
        buf_t b = with_tile(5, junk[j], 4, right);
        map_pack_t *pack = NULL;
        EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
        if (pack) EXPECT_AT(read_entry(pack, 5, tile) == MAP_TILE_BAD, "junk", j);
        map_pack_close(pack);
        free(b.d);
    }
    free(pixels);
    free(payload);
    free(tile);
}

/* ------------------------------------------------------------------ */
/* Composing views                                                      */
/* ------------------------------------------------------------------ */

/* Every pixel of a composed view: the tile's, read on its own, or the
 * hatching. Returns the places checked. */
static int check_view(map_pack_t *pack, int zoom, int64_t left, int64_t top, int w, int h,
                      const uint16_t *px, const map_compose_stats_t *st, const char *what)
{
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    int64_t world = (int64_t)256 << zoom;
    int wrong = 0, places = 0, tiles = 0;
    for (int64_t ty = (int64_t)floor(top / 256.0); ty * 256 < top + h; ty++) {
        for (int64_t tx = (int64_t)floor(left / 256.0); tx * 256 < left + w; tx++) {
            places++;
            map_tile_result_t r = MAP_TILE_NONE;
            if (pack && tx >= 0 && ty >= 0 && tx * 256 < world && ty * 256 < world)
                r = map_pack_read_tile(pack, zoom, (uint32_t)tx, (uint32_t)ty, tile);
            if (r == MAP_TILE_OK) tiles++;
            for (int64_t gy = ty * 256; gy < ty * 256 + 256; gy++) {
                if (gy < top || gy >= top + h) continue;
                for (int64_t gx = tx * 256; gx < tx * 256 + 256; gx++) {
                    if (gx < left || gx >= left + w) continue;
                    int px_x = (int)(gx - tx * 256), px_y = (int)(gy - ty * 256);
                    bool ink = r == MAP_TILE_OK
                                   ? (tile[px_y * 32 + px_x / 8] & (0x80 >> (px_x % 8))) != 0
                                   : map_no_map_here(gx, gy);
                    if (px[(gy - top) * w + (gx - left)] != (ink ? MAP_INK : MAP_PAPER)) wrong++;
                }
            }
        }
    }
    if (wrong) {
        fprintf(stderr, "%s: %d pixels wrong\n", what, wrong);
        s_failures++;
    }
    EXPECT(st->tiles + st->missing + st->damaged == places);
    EXPECT(st->tiles == tiles);
    EXPECT(st->read + st->cached == places);
    free(tile);
    return places;
}

static void views_are_their_tiles_and_hatching(void)
{
    enum { W = 240, H = 296 };
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&s_golden, &pack) == MAP_PACK_OK);
    if (!pack) return;
    map_tile_cache_t *cache = map_tile_cache_create(12);
    EXPECT(cache != NULL);
    uint16_t *px = malloc(sizeof(uint16_t) * W * H);
    map_compose_stats_t st;

    for (int zoom = MAP_MIN_ZOOM; zoom <= MAP_MAX_ZOOM; zoom++) {
        double gx, gy;
        map_world_pixel(36.8, 10.18, zoom, &gx, &gy);
        int64_t left = (int64_t)floor(gx + 0.5) - W / 2, top = (int64_t)floor(gy + 0.5) - H / 2;
        const int steps[] = {-40, -3, -2, -1, 0, 1, 2, 3, 40};
        for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
            int step = steps[i];
            int64_t l = left + step * (W / 2), t = top + step * (H / 2);
            map_compose(cache, pack, zoom, l, t, W, H, px, &st);
            check_view(pack, zoom, l, t, W, H, px, &st, "a view around Medina");
            if (step == 0) EXPECT(st.tiles > 0);
            if (step == -40 || step == 40) EXPECT(st.tiles == 0 && st.damaged == 0);
            /* Again: every place from the cache, nothing read. */
            map_compose(cache, pack, zoom, l, t, W, H, px, &st);
            EXPECT(st.read == 0);
            check_view(pack, zoom, l, t, W, H, px, &st, "the same view again");
        }
    }

    /* The world's edges, and past them: hatching. */
    int64_t edges[][2] = {{-100, -100}, {-1000000, 5}, {((int64_t)256 << 14) - 50, 0},
                          {((int64_t)256 << 17) + 7, ((int64_t)256 << 17) - 1}};
    for (size_t e = 0; e < sizeof edges / sizeof edges[0]; e++) {
        map_compose(cache, pack, e < 3 ? 14 : 17, edges[e][0], edges[e][1], W, H, px, &st);
        check_view(pack, e < 3 ? 14 : 17, edges[e][0], edges[e][1], W, H, px, &st, "an edge");
        EXPECT(st.tiles == 0);
    }

    /* A cache of one tile still composes right. */
    map_tile_cache_t *small = map_tile_cache_create(1);
    double gx, gy;
    map_world_pixel(36.8, 10.18, 17, &gx, &gy);
    map_compose(small, pack, 17, (int64_t)gx - 333, (int64_t)gy - 77, W, H, px, &st);
    check_view(pack, 17, (int64_t)gx - 333, (int64_t)gy - 77, W, H, px, &st, "a cache of one");
    map_tile_cache_destroy(small);

    /* No Pack: all hatching. */
    map_compose(cache, NULL, 16, 1000, 1000, W, H, px, &st);
    check_view(NULL, 16, 1000, 1000, W, H, px, &st, "no Pack");
    EXPECT(st.missing == 6);

    free(px);
    map_tile_cache_destroy(cache);
    map_pack_close(pack);
}

static void a_damaged_tile_is_hatched(void)
{
    enum { W = 240, H = 296 };
    int k = -1;
    for (int e = 0; e < s_entry_count; e++)
        if (s_entries[e].z == 16 && s_entries[e].x == 34620 && s_entries[e].y == 25553) k = e;
    EXPECT(k >= 0);
    if (k < 0) return;
    buf_t b = copy(&s_golden);
    b.d[s_entries[k].off + s_entries[k].len / 2] ^= 0x10;
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
    if (pack) {
        map_tile_cache_t *cache = map_tile_cache_create(12);
        uint16_t *px = malloc(sizeof(uint16_t) * W * H);
        double gx, gy;
        map_world_pixel(36.8, 10.18, 16, &gx, &gy);
        int64_t left = (int64_t)floor(gx + 0.5) - W / 2, top = (int64_t)floor(gy + 0.5) - H / 2;
        map_compose_stats_t st;
        map_compose(cache, pack, 16, left, top, W, H, px, &st);
        check_view(pack, 16, left, top, W, H, px, &st, "a damaged tile");
        EXPECT(st.damaged == 1 && st.tiles == 3);
        free(px);
        map_tile_cache_destroy(cache);
        map_pack_close(pack);
    }
    free(b.d);
}

/* ------------------------------------------------------------------ */
/* Random damage                                                        */
/* ------------------------------------------------------------------ */

static uint32_t s_rand = 0x2545F491u;

static uint32_t next_rand(void)
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return s_rand;
}

static void random_damage_never_escapes(void)
{
    enum { W = 240, H = 296, ROUNDS = 2000 };
    uint16_t *px = malloc(sizeof(uint16_t) * W * H);
    uint8_t *tile = malloc(MAP_TILE_BYTES);
    int opened = 0;
    for (int round = 0; round < ROUNDS; round++) {
        buf_t b = copy(&s_golden);
        int bytes = 1 + (int)(next_rand() % 8);
        for (int i = 0; i < bytes; i++) {
            /* Most damage where the checks are, some anywhere. */
            size_t at = next_rand() % (round % 3 == 0 ? b.len : 600);
            b.d[at] = (uint8_t)next_rand();
        }
        if (round % 4 == 3) b.len -= next_rand() % 64;
        /* Half the time, CRCs made right, so the damage reaches the
         * checks behind them. */
        if (round % 2 == 0) rehash(&b);
        map_pack_t *pack = NULL;
        map_pack_err_t err = open_buf(&b, &pack);
        EXPECT_AT((err == MAP_PACK_OK) == (pack != NULL), "round", round);
        if (pack) {
            opened++;
            for (int k = 0; k < s_entry_count; k++) (void)read_entry(pack, k, tile);
            for (int r = 0; r < map_pack_region_count(pack); r++) {
                const map_region_t *reg = map_pack_region(pack, r);
                EXPECT_AT(strlen(reg->name) < MAP_REGION_NAME_MAX, "round", round);
                EXPECT_AT(reg->deepest_zoom >= MAP_MIN_ZOOM && reg->deepest_zoom <= MAP_MAX_ZOOM,
                          "round", round);
            }
            map_tile_cache_t *cache = map_tile_cache_create(4);
            const map_region_t *first = map_pack_region(pack, 0);
            double gx, gy;
            map_world_pixel(first->centre_lat, first->centre_lon, first->deepest_zoom, &gx, &gy);
            map_compose_stats_t st;
            map_compose(cache, pack, first->deepest_zoom, (int64_t)gx - W / 2, (int64_t)gy - H / 2,
                        W, H, px, &st);
            map_tile_cache_destroy(cache);
            map_pack_close(pack);
        }
        free(b.d);
    }
    printf("random damage: %d of %d damaged Packs opened, none escaped\n", opened, ROUNDS);
    free(px);
    free(tile);
}

/* ------------------------------------------------------------------ */
/* Baked overlays                                                       */
/* ------------------------------------------------------------------ */

static buf_t s_transit;

static buf_t read_file(const char *path)
{
    buf_t b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    b.len = (size_t)ftell(f);
    b.d = malloc(b.len ? b.len : 1);
    fseek(f, 0, SEEK_SET);
    if (fread(b.d, 1, b.len, f) != b.len) exit(2);
    fclose(f);
    return b;
}

static const sect_t *transit_ovly(void)
{
    static sect_t s[64];
    int n = sections(&s_transit, s, 64);
    return find(s, n, "OVLY");
}

static int find_item(map_pack_t *pack, int o, const char *label)
{
    map_pack_item_t it;
    for (int i = 0; map_pack_overlay_item(pack, o, i, &it); i++)
        if (strcmp(it.label, label) == 0) return i;
    return -1;
}

static void the_transit_pack_has_its_overlays(void)
{
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&s_transit, &pack) == MAP_PACK_OK);
    if (!pack) return;
    EXPECT(map_pack_version_major(pack) == 1 && map_pack_version_minor(pack) == 1);
    EXPECT(!map_pack_overlays_damaged(pack));
    EXPECT(map_pack_overlay_count(pack) == 2);
    EXPECT(strcmp(map_pack_overlay_name(pack, 0), "Transit lines") == 0);
    EXPECT(strcmp(map_pack_overlay_name(pack, 1), "Transit stops") == 0);
    EXPECT(map_pack_overlay_name(pack, 2) == NULL && map_pack_overlay_name(pack, -1) == NULL);
    EXPECT(map_pack_overlay_items(pack, 0) == 2 && map_pack_overlay_items(pack, 1) == 3);

    map_pack_item_t it;
    EXPECT(map_pack_overlay_item(pack, 0, 0, &it));
    EXPECT(it.kind == 1 && it.vertices == 2 && strcmp(it.label, "T1") == 0 && it.props == 1);
    double lat = 0, lon = 0;
    EXPECT(map_pack_overlay_vertex(pack, 0, 0, 1, &lat, &lon));
    /* make_fixture.py: the tram runs 140 m north of the Medina's centre. */
    EXPECT(fabs(lat - (36.8 + 140 / 111320.0)) < 1e-5 && fabs(lon - 10.18) < 0.01);
    EXPECT(!map_pack_overlay_vertex(pack, 0, 0, 2, &lat, &lon));
    EXPECT(!map_pack_overlay_item(pack, 0, 2, &it));

    int bab = find_item(pack, 1, "Bab Bhar");
    EXPECT(bab >= 0);
    EXPECT(map_pack_overlay_item(pack, 1, bab, &it));
    EXPECT(it.kind == 0 && it.icon == 1 && it.vertices == 1 && it.props == 2);
    char key[MAP_PACK_KEY_MAX], value[MAP_PACK_VALUE_MAX];
    EXPECT(map_pack_overlay_prop(pack, 1, bab, 0, key, sizeof key, value, sizeof value));
    EXPECT(strcmp(key, "lines") == 0 && strcmp(value, "T1") == 0);
    EXPECT(map_pack_overlay_prop(pack, 1, bab, 1, key, sizeof key, value, sizeof value));
    EXPECT(strcmp(key, "mode") == 0 && strcmp(value, "tram") == 0);
    EXPECT(!map_pack_overlay_prop(pack, 1, bab, 2, key, sizeof key, value, sizeof value));
    /* Cut to fit, at a character's end. */
    char small[3];
    EXPECT(map_pack_overlay_prop(pack, 1, bab, 1, key, 3, small, sizeof small));
    EXPECT(strcmp(key, "mo") == 0 && strcmp(small, "tr") == 0);
    map_pack_close(pack);

    /* The plain fixture has none. */
    EXPECT(open_buf(&s_golden, &pack) == MAP_PACK_OK);
    if (pack) {
        EXPECT(map_pack_overlay_count(pack) == 0 && !map_pack_overlays_damaged(pack));
        EXPECT(!map_pack_overlay_item(pack, 0, 0, &it));
        map_pack_close(pack);
    }
}

/* Reads every item of every overlay, as an app would, so ASan sees any
 * read outside the section; how many items there were. */
static int read_every_item(map_pack_t *pack)
{
    int total = 0;
    for (int o = 0; o < map_pack_overlay_count(pack); o++) {
        EXPECT(map_pack_overlay_name(pack, o) != NULL);
        map_pack_item_t it;
        for (int i = 0; map_pack_overlay_item(pack, o, i, &it); i++) {
            total++;
            EXPECT(it.kind >= 0 && it.kind <= 3 && it.icon >= 0 && it.icon <= 4);
            EXPECT(strlen(it.label) < MAP_PACK_LABEL_MAX);
            double lat, lon;
            for (int v = 0; map_pack_overlay_vertex(pack, o, i, v, &lat, &lon); v++)
                EXPECT(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180);
            char k[MAP_PACK_KEY_MAX], val[MAP_PACK_VALUE_MAX];
            for (int p = 0; map_pack_overlay_prop(pack, o, i, p, k, sizeof k, val, sizeof val); p++)
                EXPECT(k[0] != '\0');
        }
    }
    return total;
}

/* The plain golden Pack with ovly as its OVLY, every CRC right, opened:
 * it always opens, its tiles read; whether its overlays held. */
static bool open_with_ovly(const uint8_t *ovly, size_t len, bool *damaged, int *items)
{
    rep_t add[] = {{"OVLY", ovly, len}};
    buf_t b = rebuild(add, 1);
    wr16(b.d + 10, 1);
    rehash(&b);
    map_pack_t *pack = NULL;
    bool ok = open_buf(&b, &pack) == MAP_PACK_OK;
    if (pack) {
        *damaged = map_pack_overlays_damaged(pack);
        EXPECT(!*damaged || map_pack_overlay_count(pack) == 0);
        *items = read_every_item(pack);
        uint8_t *tile = malloc(MAP_TILE_BYTES);
        EXPECT(read_entry(pack, 0, tile) == MAP_TILE_OK);
        free(tile);
        map_pack_close(pack);
    }
    free(b.d);
    return ok;
}

static void damaged_overlays_leave_the_map(void)
{
    const sect_t *o = transit_ovly();
    EXPECT(o != NULL);
    if (!o) return;
    const uint8_t *ovly = s_transit.d + o->off;
    bool damaged = true;
    int items = 0;
    EXPECT(open_with_ovly(ovly, o->len, &damaged, &items) && !damaged && items == 5);

    /* Cut short at every length, its CRC made right: never read past. */
    for (uint32_t len = 0; len < o->len; len++) {
        EXPECT_AT(open_with_ovly(ovly, len, &damaged, &items), "OVLY length", len);
        EXPECT_AT(damaged, "OVLY length", len);
    }
    /* Trailing bytes. */
    uint8_t *more = malloc(o->len + 3);
    memcpy(more, ovly, o->len);
    memset(more + o->len, 0, 3);
    EXPECT(open_with_ovly(more, o->len + 3, &damaged, &items) && damaged);
    free(more);

    /* Every bit flipped, the CRC made right: whatever it now says, it is
     * read inside the section or left out. */
    uint8_t *flip = malloc(o->len);
    for (uint32_t i = 0; i < o->len * 8; i++) {
        memcpy(flip, ovly, o->len);
        flip[i / 8] ^= (uint8_t)(1u << (i % 8));
        EXPECT_AT(open_with_ovly(flip, o->len, &damaged, &items), "OVLY bit", i);
    }
    free(flip);

    /* Its CRC wrong: left out, the map opens. */
    buf_t b = copy(&s_transit);
    b.d[o->off + o->len / 2] ^= 0x10;
    rehash_some(&b, false);
    map_pack_t *pack = NULL;
    EXPECT(open_buf(&b, &pack) == MAP_PACK_OK);
    if (pack) {
        EXPECT(map_pack_overlays_damaged(pack) && map_pack_overlay_count(pack) == 0);
        map_pack_close(pack);
    }
    free(b.d);

    /* Two OVLY sections: the Pack does not hold. */
    sect_t s[64];
    int n = sections(&s_transit, s, 64);
    for (int i = 0; i < n; i++) {
        if (strcmp(s[i].tag, "TILE") == 0) {
            b = copy(&s_transit);
            memcpy(b.d + 32 + 16 * i, "OVLY", 4);
            rehash(&b);
            expect_open(&b, MAP_PACK_ERR_DAMAGED, "two OVLY");
            free(b.d);
        }
    }
}

/* Hand-made sections: the limits the format sets. */
static void overlays_past_the_limits_are_left_out(void)
{
    static uint8_t d[600000];
    bool damaged;
    int items;
    size_t at;

    /* 9 overlays, each empty. */
    at = 0;
    d[at++] = 9;
    d[at++] = 0;
    for (int i = 0; i < 9; i++) {
        d[at++] = 1;
        d[at++] = 'a';
        d[at++] = 0;
        d[at++] = 0;
    }
    EXPECT(open_with_ovly(d, at, &damaged, &items) && damaged);
    d[0] = 8;
    EXPECT(open_with_ovly(d, at - 4, &damaged, &items) && !damaged && items == 0);

    /* 257 points in one overlay. */
    at = 0;
    d[at++] = 1;
    d[at++] = 0;
    d[at++] = 1;
    d[at++] = 'p';
    wr16(d + at, 257);
    at += 2;
    for (int i = 0; i < 257; i++) {
        d[at++] = 0; /* point */
        d[at++] = 0;
        wr16(d + at, 1);
        at += 2;
        wr32(d + at, 368000000);
        wr32(d + at + 4, 101800000);
        at += 8;
        d[at++] = 0; /* label */
        d[at++] = 0; /* props */
    }
    EXPECT(open_with_ovly(d, at, &damaged, &items) && damaged);
    wr16(d + 4, 256);
    EXPECT(open_with_ovly(d, at - 14, &damaged, &items) && !damaged && items == 256);

    /* A line of 16385 vertices; an icon past the last; a latitude past the pole. */
    at = 0;
    d[at++] = 1;
    d[at++] = 0;
    d[at++] = 1;
    d[at++] = 'l';
    wr16(d + at, 1);
    at += 2;
    d[at++] = 1;
    d[at++] = 0;
    size_t count_at = at;
    at += 2;
    for (int i = 0; i < 16385; i++) {
        wr32(d + at, 368000000 + (uint32_t)i);
        wr32(d + at + 4, 101800000);
        at += 8;
    }
    d[at++] = 0;
    d[at++] = 0;
    wr16(d + count_at, 16385 & 0xFFFF);
    EXPECT(open_with_ovly(d, at, &damaged, &items) && damaged);

    uint8_t point[] = {1, 0, 1, 'p', 1, 0, 0, 5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT(open_with_ovly(point, sizeof point, &damaged, &items) && damaged); /* icon 5 */
    point[7] = 4;
    wr32(point + 10, (uint32_t)(int32_t)910000000); /* 91 degrees */
    EXPECT(open_with_ovly(point, sizeof point, &damaged, &items) && damaged);
    wr32(point + 10, 368000000);
    EXPECT(open_with_ovly(point, sizeof point, &damaged, &items) && !damaged && items == 1);
    point[6] = 1; /* a line of one point */
    EXPECT(open_with_ovly(point, sizeof point, &damaged, &items) && damaged);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s FIXTURE.tmap WORK_DIR [TRANSIT.tmap]\n", argv[0]);
        return 2;
    }
    snprintf(s_path, sizeof s_path, "%s/pack.tmap", argv[2]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    s_golden.len = (size_t)ftell(f);
    s_golden.d = malloc(s_golden.len);
    fseek(f, 0, SEEK_SET);
    if (fread(s_golden.d, 1, s_golden.len, f) != s_golden.len) return 2;
    fclose(f);
    read_golden_index();

    crc_is_ieee();
    golden_opens_and_every_tile_reads();
    cut_short_at_every_length_is_refused();
    a_bit_flipped_in_every_byte();
    a_newer_format_is_refused_and_a_newer_minor_read();
    header_fields_that_do_not_hold();
    sections_that_do_not_hold();
    the_credit();
    regions_that_do_not_hold();
    index_entries_that_do_not_hold();
    payloads_that_do_not_hold();
    views_are_their_tiles_and_hatching();
    a_damaged_tile_is_hatched();
    random_damage_never_escapes();
    if (argc == 4) {
        s_transit = read_file(argv[3]);
        the_transit_pack_has_its_overlays();
        damaged_overlays_leave_the_map();
        overlays_past_the_limits_are_left_out();
        free(s_transit.d);
    }

    free(s_golden.d);
    if (s_failures) {
        fprintf(stderr, "%d map pack checks failed\n", s_failures);
        return 1;
    }
    printf("map pack: all checks passed\n");
    return 0;
}
