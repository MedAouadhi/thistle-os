# The Map Pack format, version 1.1

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

What the tool builds from, and what the browser mode (`thistle_maps.py serve`) saves: a
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

| Offset | Size | Field | Version 1.1 |
|---|---|---|---|
| 0 | 8 | magic | `THSTMAP` and a zero byte |
| 8 | 2 | major version | 1 |
| 10 | 2 | minor version | 0, or 1 with an OVLY section |
| 12 | 2 | header length | 32 |
| 14 | 1 | variant | 0: 256 x 256 tiles, one bit a pixel, 1 is ink (black) |
| 15 | 1 | reserved | 0 |
| 16 | 1 | lowest zoom | 14 |
| 17 | 1 | deepest zoom | 17 |
| 18 | 2 | section count | 4, or 5 with OVLY; at most 32 |
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

Each of ATTR, REGN, TIDX and TILE is present exactly once. OVLY, added in
1.1, is present at most once, and only in a Pack the tool built with
`--transit`; a Pack without it stays 1.0. A section lies wholly inside the
file, after the directory. A reader ignores a tag it does not know.
Reserved for a later minor version:

- `NAME`, a **name index** for place and street search.

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

### OVLY: overlays baked in (1.1)

Vector items the Maps app draws over the tiles as switchable **Overlays**
(`docs/maps/overlay-api.md`), unlike the tiles, which hold nothing that
switches. The tool writes one when asked for a static transit network
(below).

```
u16 count                   overlays, at most 8
count times:
    u8  name length         1 to 31
    ... name                UTF-8: "Transit lines"
    u16 items               at most 256
    items times:
        u8  kind            0 point, 1 line, 2 polygon, 3 text
        u8  icon            a point's: 0 dot, 1 ring, 2 square, 3 triangle, 4 cross;
                            0 for any other kind
        u16 n               vertices: 1 for a point or a text, at least 2 for a
                            line, 3 for a polygon (a ring, not closed)
        n times:  i32 latitude x 1e7, i32 longitude x 1e7
                            within -90 to 90 and -180 to 180
        u8  label length    at most 63
        ... label           UTF-8: a point's or a text's label; a line's or a
                            polygon's is shown only on its info card
        u8  properties      at most 10
        properties times:
            u8  key length      1 to 31
            ... key             UTF-8
            u8  value length    at most 159
            ... value           UTF-8
```

The section ends exactly after the last overlay, and an overlay's items hold
at most 16384 vertices between them: the map engine's limits
(`map_overlay.h`). The section is at most 2 MB. Its order is the order the
items are drawn and decluttered in: the first item of an overlay is placed
first, so the tool puts the important ones first.

A reader checks OVLY whole when it opens the Pack: its CRC, then every
count, length, kind, icon and coordinate above. One that does not hold is
left out, and the Pack opens with no overlays (the Maps app logs "the
Pack's overlays are damaged: left out"): the tiles are as good as ever.

### A static transit network

`thistle_maps.py build REGIONS.geojson --transit SOURCE -o X.tmap` bakes
the transit network inside the Regions into OVLY (`tools/maps/transit.py`).
SOURCE is `osm`, the extract's own route relations (`type=route` with
`route` one of train, subway, light_rail, monorail, tram, ferry,
trolleybus, bus: their ways, and their nodes whose role is `stop*` or
`platform*`), or a GTFS feed, a `.zip` or a folder (`routes.txt`,
`trips.txt`, `stops.txt`, and `shapes.txt` and `stop_times.txt` if it has
them; a route with no shape is drawn stop to stop along its first trip). It
writes two overlays:

| Overlay | Items |
|---|---|
| `Transit lines` | a line for each stretch the same routes run along, each way and every variant drawn once; its label the routes' names, `5, 23`; the property `mode` (`bus`, `tram, bus`) |
| `Transit stops` | a point for a stop with its platforms and stop positions, those of one name within 300 m being one (a GTFS stop is its parent station), at their middle; labelled with its name; icon a square for train, subway or light rail, a ring for tram, a triangle for ferry, a dot for bus; the properties `lines` and `mode` |

Lines are kept where they cross the Pack's z14 tiles, stops inside them;
lines are simplified to a pixel at z17. The most important come first, and
past 256 items or 16384 vertices the rest is left out, which the build
says (`transit: 90 lines, 256 stops (412 left out, 256 at most)`): lines
by mode (rail, subway, light rail, monorail, tram, ferry, trolleybus, bus),
then the longest; stops by mode, then stations, then the most routes. The
same source gives the same bytes.

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
  to fill it exactly and the result to match the entry's CRC;
- checks OVLY, if there is one, as its section says, and leaves it out if it
  does not hold.

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
browser mode's live estimate: a Pack of Monaco came out 13 % under it, and
anywhere sparser further under, so it is an upper bound for a budget.
`measure_pack()` (`estimate --extract`, the browser mode's Measure) draws
up to twelve blocks of 8 x 8 tiles a level from the extract and prices the
rest at their average: within 5 % of the built fixture. Each level holds
about four times the tiles of the one above.
