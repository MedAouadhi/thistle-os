/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Map Pack reader
 *
 * A Map Pack is one file of pre-drawn one-bit tiles, made by the Map Pack
 * tool (tools/maps/) from the Regions a user drew; its format is
 * docs/maps/map-pack-format.md. It is unsigned data from the card, so it is
 * read as untrusted: every offset and length is checked against the file,
 * a tile is inflated into a fixed buffer and checked against its CRC, and a
 * format newer than this reader is refused with a message.
 *
 * Opening reads the header, the section directory, the credit and the
 * Regions; a tile is found by a binary search of the index on the card, so
 * memory does not grow with the Pack.
 *
 * Not thread-safe: one task uses a Pack at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_TILE_PX          256
#define MAP_TILE_BYTES       (MAP_TILE_PX * MAP_TILE_PX / 8)  /* one bit a pixel */
#define MAP_MIN_ZOOM         14
#define MAP_MAX_ZOOM         17
#define MAP_PACK_MAJOR       1     /* the newest format this reader reads */
#define MAP_REGION_NAME_MAX  64
#define MAP_PACK_MAX_REGIONS 64    /* more are read past, not kept */

typedef struct map_pack map_pack_t;

typedef enum {
    MAP_PACK_OK = 0,
    MAP_PACK_ERR_IO,        /* the file cannot be opened or read */
    MAP_PACK_ERR_NOT_PACK,  /* no Map Pack header */
    MAP_PACK_ERR_NEWER,     /* a newer format than MAP_PACK_MAJOR */
    MAP_PACK_ERR_VARIANT,   /* a variant this screen does not show (colour) */
    MAP_PACK_ERR_DAMAGED,   /* a CRC, an offset or a length that does not hold */
    MAP_PACK_ERR_NO_MEMORY,
} map_pack_err_t;

typedef struct {
    char   name[MAP_REGION_NAME_MAX];  /* UTF-8, cut at a character's end */
    bool   circle;                     /* else a freehand polygon */
    int    deepest_zoom;
    double centre_lat;                 /* a circle's centre, a polygon's box's */
    double centre_lon;
} map_region_t;

typedef enum {
    MAP_TILE_OK = 0,
    MAP_TILE_NONE,  /* the Pack has no tile there: outside every Region */
    MAP_TILE_BAD,   /* damaged: an entry, a payload or a CRC that does not hold */
} map_tile_result_t;

/* Opens the Pack at path. On failure *out is NULL and, for
 * MAP_PACK_ERR_NEWER, *newer_major (if not NULL) is the Pack's format. */
map_pack_err_t map_pack_open(const char *path, map_pack_t **out, unsigned *newer_major);
void map_pack_close(map_pack_t *pack);

/* A sentence for the user saying why a Pack did not open. */
const char *map_pack_err_text(map_pack_err_t err);

unsigned map_pack_version_major(const map_pack_t *pack);
unsigned map_pack_version_minor(const map_pack_t *pack);
uint32_t map_pack_tile_count(const map_pack_t *pack);
int map_pack_region_count(const map_pack_t *pack);
const map_region_t *map_pack_region(const map_pack_t *pack, int i);

/* The data's credit, as the Pack holds it: OpenStreetMap's. Never NULL. */
const char *map_pack_credit(const map_pack_t *pack);

/* Reads tile (z, x, y) into out, MAP_TILE_BYTES: rows top down, the left
 * pixel the high bit, 1 for ink. On anything but MAP_TILE_OK, out holds
 * nothing useful. */
map_tile_result_t map_pack_read_tile(map_pack_t *pack, int z, uint32_t x, uint32_t y,
                                     uint8_t out[MAP_TILE_BYTES]);

/* CRC-32 (IEEE 802.3), as the Pack's checks use, continued as zlib's
 * crc32() is: start with 0, pass each result on with the next bytes. */
uint32_t map_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Inflates raw DEFLATE into exactly out_len bytes. False if the stream is
 * damaged or does not fill out exactly. */
bool map_inflate(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
