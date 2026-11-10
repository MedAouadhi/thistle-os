# SPDX-License-Identifier: BSD-3-Clause
"""Writes a Map Pack, as docs/maps/map-pack-format.md lays it out.

    header (32 bytes) | section directory (16 bytes each) | sections ...

The sections, in this order: ATTR (the credit), REGN (the Regions'
outlines), TIDX (the tile index), TILE (every tile's raw DEFLATE, one after
another). Every number is little-endian. Nothing in it depends on the time
or the machine, so the same Regions and extract give the same bytes.
"""

from __future__ import annotations

import struct
import zlib

from regions import MAX_ZOOM, MIN_ZOOM, Region

MAGIC = b"THSTMAP\x00"
VERSION_MAJOR = 1
VERSION_MINOR = 0
HEADER_LEN = 32
VARIANT_MONO = 0          # 256 x 256, one bit a pixel, 1 = ink (black)
TILE_BYTES = 256 * 256 // 8
INDEX_ENTRY = 24

CREDIT = ("© OpenStreetMap contributors. Map data under the Open Database "
          "Licence (ODbL) 1.0: https://www.openstreetmap.org/copyright")


def _e7(value: float) -> int:
    return int(round(value * 1e7))


def regions_section(regions: list[Region]) -> bytes:
    out = bytearray(struct.pack("<H", len(regions)))
    for region in regions:
        name = region.name.encode("utf-8")[:255]
        out += struct.pack("<BBH", 0 if region.is_circle else 1, region.deepest_zoom, len(name))
        out += name
        if region.is_circle:
            lat, lon = region.centre
            out += struct.pack("<iiI", _e7(lat), _e7(lon), int(round(region.radius_m)))
        else:
            out += struct.pack("<H", len(region.points))
            for lat, lon in region.points:
                out += struct.pack("<ii", _e7(lat), _e7(lon))
    return bytes(out)


def _layout(section_lengths: list[int]) -> list[int]:
    offset = HEADER_LEN + 16 * len(section_lengths)
    offsets = []
    for length in section_lengths:
        offsets.append(offset)
        offset += length
    return offsets


def fixed_size(regions: list[Region], credit: str, tile_count: int) -> int:
    """A Pack's bytes other than its tiles' payloads."""
    return (HEADER_LEN + 16 * 4 + len(credit.encode("utf-8"))
            + len(regions_section(regions)) + INDEX_ENTRY * tile_count)


def deflate(bitmap: bytes) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY)
    return compressor.compress(bitmap) + compressor.flush()


def write_pack(path, regions: list[Region], tiles: dict[tuple[int, int, int], bytes],
               credit: str = CREDIT) -> int:
    """Writes the Pack; tiles maps (z, x, y) to its 8192-byte bitmap.
    Returns the file's size."""
    keys = sorted(tiles)
    attr = credit.encode("utf-8")
    regn = regions_section(regions)
    payloads = []
    for key in keys:
        bitmap = tiles[key]
        if len(bitmap) != TILE_BYTES:
            raise ValueError(f"tile {key} is {len(bitmap)} bytes")
        payloads.append(deflate(bitmap))

    index_len = INDEX_ENTRY * len(keys)
    tile_len = sum(len(p) for p in payloads)
    offsets = _layout([len(attr), len(regn), index_len, tile_len])
    tile_start = offsets[3]

    index = bytearray()
    at = tile_start
    for key, payload in zip(keys, payloads):
        z, x, y = key
        index += struct.pack("<IIBBHIII", x, y, z, 0, 0, at, len(payload),
                             zlib.crc32(tiles[key]))
        at += len(payload)
    tile_data = b"".join(payloads)

    sections = [(b"ATTR", attr), (b"REGN", regn), (b"TIDX", bytes(index)), (b"TILE", tile_data)]
    # TILE's own CRC is 0: each tile carries one, and a reader never reads
    # the section whole.
    directory = b"".join(struct.pack("<4sIII", tag, off, len(body),
                                     0 if tag == b"TILE" else zlib.crc32(body))
                         for (tag, body), off in zip(sections, offsets))
    head = MAGIC + struct.pack("<HHHBBBBHII", VERSION_MAJOR, VERSION_MINOR, HEADER_LEN,
                               VARIANT_MONO, 0, MIN_ZOOM, MAX_ZOOM, len(sections),
                               len(keys), 0)
    assert len(head) == 28
    header = head + struct.pack("<I", zlib.crc32(head + directory))

    data = header + directory + b"".join(body for _tag, body in sections)
    with open(path, "wb") as fh:
        fh.write(data)
    return len(data)
