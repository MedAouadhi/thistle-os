# SPDX-License-Identifier: BSD-3-Clause
"""How big a Map Pack of some Regions will be, before building it.

The browser mode (ticket 05) calls estimate_pack() as the user draws, so it
is quick: it counts tiles exactly, as a build would, and prices each at an
average compressed size for its level. The averages are a city's; the
fixture's tiles are smaller, a countryside's smaller still. Ticket 05 sets
the tolerance against built Packs.
"""

from __future__ import annotations

from dataclasses import dataclass

import pack_writer
from regions import MAX_ZOOM, MIN_ZOOM, Region, pack_tiles

# Bytes of raw DEFLATE per 256 x 256 one-bit tile (8192 bytes inflated), by
# level. Lower levels hold more ink per tile, so compress worse.
AVERAGE_TILE_BYTES = {14: 4200, 15: 3600, 16: 3000, 17: 2400}

DEFAULT_BUDGET_BYTES = 1 << 30  # 1 GB, the browser mode's warning


@dataclass(frozen=True)
class Estimate:
    tiles: int
    tiles_per_zoom: dict[int, int]
    bytes: int

    def over_budget(self, budget: int = DEFAULT_BUDGET_BYTES) -> bool:
        return self.bytes > budget


def estimate_pack(regions: list[Region]) -> Estimate:
    tiles = pack_tiles(regions)
    per_zoom = {z: 0 for z in range(MIN_ZOOM, MAX_ZOOM + 1)}
    for z, _x, _y in tiles:
        per_zoom[z] += 1
    payload = sum(AVERAGE_TILE_BYTES[z] * n for z, n in per_zoom.items())
    fixed = pack_writer.fixed_size(regions, pack_writer.CREDIT, len(tiles))
    return Estimate(len(tiles), per_zoom, fixed + payload)
