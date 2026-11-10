# The Map Pack format, version 1.0

A **Map Pack** is one file holding a map of the **Regions** a user drew,
pre-drawn as one-bit tiles, for the Maps app (`apps/maps/`) and any app
that embeds the map engine (`components/map_engine/`). The Map Pack tool,
`tools/maps/thistle_maps.py`, writes it; the user copies it to
`/sdcard/maps/`. ADR-0017 has the reasons for pre-drawn tiles in one file.

Readers: `components/map_engine/src/map_pack.c` on the device, and the
independent reader in `tools/maps/test_thistle_maps.py`, written from this
document alone, which the tool's tests and the simulator's
(`simulator/tests/test_maps.py`) read the golden fixture with.

## The Regions file

What the tool builds from, and what the browser mode (ticket 05) saves: a
GeoJSON FeatureCollection, longitude before latitude as GeoJSON has it.

```json
{
  "type": "FeatureCollection",
  "thistle_regions": 1,
  "features": [
    {"type": "Feature",
     "properties": {"name": "Medina", "radius_m": 350, "deepest_zoom": 17},
     "geometry": {"type": "Point", "coordinates": [10.18, 36.80]}},
    {"type": "Feature",
     "properties": {"name": "Lac Nord", "deepest_zoom": 15},
     "geometry": {"type": "Polygon", "coordinates": [[[10.196, 36.813], ...]]}}
  ]
}
```

- A **circle** is a `Point` with `properties.radius_m`, in metres, above 0.
- A **freehand polygon** is a `Polygon`; only its outer ring is used.
- `deepest_zoom` is 14, 15, 16 or 17; it is 16 when absent. Any other value,
  and a file with no Region, is refused.
- `name` is optional (`Region 1`, `Region 2`, ... otherwise).
- `thistle_regions` is the file's version, 1; readers ignore other members.

A Region needs every tile it touches from z14 to its own `deepest_zoom`. A
tile is one Region's if any part of it is inside: within `radius_m` of the
circle's centre (great-circle distance to the nearest point of the tile), or
overlapping the polygon. The Pack holds the union of all Regions' tiles.

## Layout

Every number is little-endian. Offsets are from the start of the file.

```
header             32 bytes
section directory  16 bytes a section
sections           where the directory says
```

### Header

| Offset | Size | Field | Version 1.0 |
|---|---|---|---|
| 0 | 8 | magic | `THSTMAP` and a zero byte |
| 8 | 2 | major version | 1 |
| 10 | 2 | minor version | 0 |
| 12 | 2 | header length | 32 |
| 14 | 1 | variant | 0: 256 x 256 tiles, one bit a pixel, 1 is ink (black) |
| 15 | 1 | reserved | 0 |
| 16 | 1 | lowest zoom | 14 |
| 17 | 1 | deepest zoom | 17 |
| 18 | 2 | section count | 4 in 1.0; at most 32 |
| 20 | 4 | tile count | entries in TIDX |
| 24 | 4 | reserved | 0 |
| 28 | 4 | header CRC | CRC-32 of bytes 0 to 27, then of the whole directory |

**Versions.** A reader refuses a major version above its own and says the
Pack was made by a newer tool (the Maps app: "made by a newer Map Pack tool
(format 2)"). A minor version only adds sections, which an older reader of
the same major reads past; it never changes what a 1.0 section means.

**Variants.** Variant 0 is the one-bit Pack for e-paper. The byte is
reserved for a **colour variant** for LCD boards (out of scope for now); a
reader that cannot show a variant refuses the Pack ("made for a colour
screen"). A colour variant would change only the tiles' payload, not this
layout.

CRC-32 is IEEE 802.3's, zlib's `crc32()`.

### Section directory

`section count` entries of 16 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | tag, four ASCII letters |
| 4 | 4 | offset |
| 8 | 4 | length |
| 12 | 4 | CRC-32 of the section's bytes; 0 for TILE |

Each of ATTR, REGN, TIDX and TILE is present exactly once in 1.0. A section
lies wholly inside the file, after the directory. A reader ignores a tag it
does not know. Reserved for later minor versions:

- `NAME`, a **name index** for place and street search;
- `OVLY`, vector overlay items baked into the Pack, such as a static transit
  network (ticket 06).

### ATTR: the credit

The data's attribution, UTF-8, no terminator, at most 1024 bytes. Every Pack
the tool writes holds OpenStreetMap's:

> © OpenStreetMap contributors. Map data under the Open Database Licence
> (ODbL) 1.0: https://www.openstreetmap.org/copyright

An app showing the map shows the credit; the Maps app's About line shows its
first sentence.

### REGN: the Regions

```
u16 count
count times:
    u8  kind            0 circle, 1 polygon
    u8  deepest zoom    14 to 17
    u16 name length     bytes
    ... name            UTF-8, at most 255 bytes
    circle:   i32 latitude x 1e7, i32 longitude x 1e7, u32 radius in metres
    polygon:  u16 n (at least 3), then n times i32 latitude x 1e7, i32 longitude x 1e7
              (the ring, not closed: the last point is not the first again)
```

The section ends exactly after the last Region. The order is the Regions
file's; the Maps app opens on the first Region's centre (a polygon's centre
is the middle of its bounding box).

### TIDX: the tile index

`tile count` entries of 24 bytes, sorted by zoom, then x, then y, so a
reader finds a tile by binary search without loading the index:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | x |
| 4 | 4 | y |
| 8 | 1 | zoom, 14 to 17 |
| 9 | 1 | flags, 0 |
| 10 | 2 | reserved, 0 |
| 12 | 4 | payload offset, inside TILE |
| 16 | 4 | payload length |
| 20 | 4 | CRC-32 of the inflated tile |

x and y are Web Mercator (OpenStreetMap's) tile numbers at that zoom.

### TILE: the tiles

Each tile's payload, one after another: **raw DEFLATE** (no zlib or gzip
header) of the tile's 8192 bytes, rows top to bottom, 32 bytes a row, the
leftmost pixel the high bit, 1 for ink. The section's own CRC is 0: a reader
checks each tile against its index entry's CRC instead, after inflating it.
A payload longer than 16 KB is damage.

## Reading it safely

The Pack is not signed: it is data, copied by hand, and read as untrusted.
The device:

- checks the magic, the version, the header length, the header CRC, the
  variant, and that each needed section is present once and lies inside the
  file; TIDX must be exactly 24 bytes a tile;
- checks the CRC of ATTR and REGN, and REGN's structure byte by byte; a
  Region's latitude must be within Web Mercator's range;
- reads one index entry at a time, refusing a payload outside TILE or longer
  than 16 KB;
- inflates a payload into a fixed 8192-byte buffer, and requires the stream
  to fill it exactly and the result to match the entry's CRC.

A Pack that fails at opening is refused with its reason, and the app says
so. A tile that fails is drawn as **No map here**, as a missing tile is.

## No map here

Where a Pack has no tile (outside every Region) or a damaged one, the view
draws hatching, aligned to the world's pixels at that zoom so it runs on
across tiles: world pixel (x, y) is ink when `(x + y) mod 8 = 0` or
`(x - y) mod 8 = 0`.

## Projection

Web Mercator with 256-pixel tiles: at zoom z the world is 256 x 2^z pixels
a side, and

```
x = (lon + 180) / 360 x 256 x 2^z
y = (1/2 - ln((1 + sin lat) / (1 - sin lat)) / (4 pi)) x 256 x 2^z
```

A view of w x h pixels around (lat, lon) has its top left at world pixel
(floor(x + 1/2) - floor(w/2), floor(y + 1/2) - floor(h/2)).

## Cartography

What the tool draws, by level (`tools/maps/render.py`):

| | z14 | z15 | z16 | z17 |
|---|---|---|---|---|
| water: horizontal hatching, outlined | yes | yes | yes | yes |
| parks, woods, grass: diagonal hatching, outlined | yes | yes | yes | yes |
| buildings: a 50 % screen, outlined | | | yes | yes |
| motorways to tertiary roads | lines | the wider cased | cased | cased |
| residential streets | hairlines | hairlines | cased | cased |
| service roads | | hairlines | lines | cased |
| paths | | | dashed | dashed |
| place names (city to neighbourhood) | towns, suburbs | and quarters | and squares | yes |
| road names, along the road | | major roads | all named | all named |
| icons (`tools/maps/icons.py`) | | | yes | yes, with names |

Buildings are drawn at the two deepest of the four levels, z16 and z17,
whatever a Region's own depth. The icons are a mosque, another place of
worship, a hospital, a pharmacy, fuel, a café, a restaurant, a school, a
bank or ATM, parking, toilets, police, a post office, a shop and a bus stop.

Labels are drawn in DejaVu Sans, shaped by HarfBuzz through Pillow's raqm,
so Arabic is joined and right to left; they are drawn anti-aliased and cut
at a third of full ink, which keeps the thin joins of Arabic letters that
FreeType's own one-bit mode drops, on a white plate. Labels never overlap
within a block of 8 x 8 tiles; a label near a block's edge may be cut where
the neighbouring block placed another.

Patterns are aligned to world pixels, so hatching meets across tiles.

## Size

`estimate.py`'s `estimate_pack(regions)` counts the tiles exactly and prices
each at an average for its level (z14 4.2 KB to z17 2.4 KB, a city's), the
browser mode's live estimate. Each level holds about four times the tiles of
the one above.
