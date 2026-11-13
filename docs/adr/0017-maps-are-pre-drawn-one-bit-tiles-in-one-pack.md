# Maps are pre-drawn one-bit tiles, in one Pack per user

The T-Deck Max shows an offline map of only the places its user chose, on a 240 x 320 e-paper panel that shows black or white, refreshes in about half a second and ghosts. Its S3 has a few hundred kilobytes of internal RAM and 8 MB of PSRAM.

**The map is drawn on a computer, not on the device.** The Map Pack tool (`tools/maps/`) reads an OpenStreetMap extract and draws 256 x 256 tiles at one bit a pixel, north up, with no anti-aliasing, at four fixed levels, z14 to z17: roads by class with hand-set widths, hatched water and parks, buildings at the two deepest levels, a dozen icons, and the labels, Arabic shaped and joined. The device only inflates tiles and copies their pixels: it holds no map font, no shaper for map labels, and no geometry.

**A Pack is one file for every Region**, with a versioned header, a section directory, the Regions' outlines, the OpenStreetMap credit, a sorted tile index and one raw-DEFLATE payload per tile with its own CRC (`docs/maps/map-pack-format.md`). Each Region chooses its own deepest level, so street detail goes only where it is wanted. The device finds a tile by binary search on the card, so its memory does not grow with the Pack.

**It is unsigned and read as untrusted.** Every offset and length is checked against the file, a tile is inflated into a fixed buffer and checked against its CRC, and a newer major version is refused with a message. A bad tile is drawn as the "no map here" hatching.

## Considered Options

**Vector tiles drawn on the device** would give arbitrary zoom, rotation and smaller files, at the cost of a renderer, map fonts and Arabic shaping on the S3, a frame that takes far longer than a blit, and cartography that has to hold up at one bit per pixel without a computer's time to tune it.

**A standard container (MBTiles, PMTiles)** would let other tools read the Pack, but MBTiles is SQLite, which the device does not have, and PMTiles' directories are compressed and nested, more to parse defensively for nothing this map needs.

**Many tile files in folders**, as most tile caches do, would cost a FAT cluster each, take hours to copy, and leave a copy cut short as a half map with no way to tell.

## Consequences

A Pack's look is fixed when it is built: a new style, or new data, means building it again. There are four levels and no rotation. Arabic labels are only as good as the build machine's HarfBuzz, and the tiles carry DejaVu Sans' glyphs. Colour screens need a colour variant of the payload, which the header reserves. Search needs a name index, for which the format reserves a section.
