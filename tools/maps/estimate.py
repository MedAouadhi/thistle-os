# SPDX-License-Identifier: BSD-3-Clause
"""How big a Map Pack of some Regions will be, before building it.

Two estimates, both counting the tiles exactly as a build would:

- estimate_pack(), quick and needing no map data, which the browser mode
  runs on every change. It prices each tile at a city's average for its
  level. A Pack of Monaco (900 m around the casino, to z17; 103 tiles)
  came out 13 % under it; anywhere sparser comes out further under, the
  fixture's few made-up streets at a tenth of it. So it is an upper bound
  for anything but the densest city, and the budget warning it drives
  comes early, never late.
- measure_pack(), which draws a sample of each level's tiles from the
  extract, up to SAMPLE_BLOCKS blocks of 8 x 8 a level spread across the
  Regions, and prices the rest at the sample's average. Within 5 % of
  the built Pack on the fixture, where the sample is every tile
  (test_thistle_maps.py); on a big Pack, as good as the sample is
  typical of the rest. Where town and sea or fields mix, a small sample
  misses: 2 km around Monaco, six blocks a level came out 21 % under the
  built Pack and three 4 % over. Twelve is every block there, 27 s.
"""

from __future__ import annotations

from dataclasses import dataclass

import pack_writer
from regions import MAX_ZOOM, MIN_ZOOM, Region, pack_tiles

# Bytes of raw DEFLATE per 256 x 256 one-bit tile (8192 bytes inflated), by
# level. Lower levels hold more ink per tile, so compress worse.
AVERAGE_TILE_BYTES = {14: 4200, 15: 3600, 16: 3000, 17: 2400}

DEFAULT_BUDGET_BYTES = 1 << 30  # 1 GB, the browser mode's warning

# Blocks a level drawn for measure_pack(): each block is drawn whole, as a
# build draws it, so its tiles' labels and edges are as they will be.
SAMPLE_BLOCKS = 12


@dataclass(frozen=True)
class Estimate:
    tiles: int
    tiles_per_zoom: dict[int, int]
    bytes: int
    measured: bool = False

    def over_budget(self, budget: int = DEFAULT_BUDGET_BYTES) -> bool:
        return self.bytes > budget

    def as_json(self, budget: int = DEFAULT_BUDGET_BYTES) -> dict:
        return {"tiles": self.tiles,
                "tiles_per_zoom": {str(z): n for z, n in self.tiles_per_zoom.items()},
                "bytes": self.bytes,
                "measured": self.measured,
                "budget": budget,
                "over_budget": self.over_budget(budget)}


def _per_zoom(tiles) -> dict[int, int]:
    per_zoom = {z: 0 for z in range(MIN_ZOOM, MAX_ZOOM + 1)}
    for z, _x, _y in tiles:
        per_zoom[z] += 1
    return per_zoom


def estimate_pack(regions: list[Region]) -> Estimate:
    tiles = pack_tiles(regions)
    per_zoom = _per_zoom(tiles)
    payload = sum(AVERAGE_TILE_BYTES[z] * n for z, n in per_zoom.items())
    fixed = pack_writer.fixed_size(regions, pack_writer.CREDIT, len(tiles))
    return Estimate(len(tiles), per_zoom, fixed + payload)


def sample_tiles(tiles: list[tuple[int, int, int]], blocks: int = SAMPLE_BLOCKS
                 ) -> list[tuple[int, int, int]]:
    """The tiles of up to `blocks` blocks a level, evenly spread over the
    level's blocks in order."""
    import render

    by_block: dict[tuple[int, int, int], list[tuple[int, int, int]]] = {}
    for z, x, y in tiles:
        by_block.setdefault((z, x // render.META, y // render.META), []).append((z, x, y))
    out = []
    for z in range(MIN_ZOOM, MAX_ZOOM + 1):
        keys = sorted(k for k in by_block if k[0] == z)
        if len(keys) > blocks:
            keys = [keys[(2 * i + 1) * len(keys) // (2 * blocks)] for i in range(blocks)]
        for key in keys:
            out.extend(by_block[key])
    return out


def measure_pack(regions: list[Region], data, fonts, blocks: int = SAMPLE_BLOCKS) -> Estimate:
    """The estimate from tiles drawn from the extract's data (MapData)."""
    import render

    tiles = pack_tiles(regions)
    per_zoom = _per_zoom(tiles)
    drawn = render.render_tiles(data, sample_tiles(tiles, blocks), fonts)
    sizes: dict[int, list[int]] = {}
    for (z, _x, _y), bitmap in drawn.items():
        sizes.setdefault(z, []).append(len(pack_writer.deflate(bitmap)))
    payload = 0
    for z, n in per_zoom.items():
        if n:
            payload += round(sum(sizes[z]) / len(sizes[z]) * n)
    fixed = pack_writer.fixed_size(regions, pack_writer.CREDIT, len(tiles))
    return Estimate(len(tiles), per_zoom, fixed + payload, measured=True)
