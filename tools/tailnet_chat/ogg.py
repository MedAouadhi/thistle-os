"""Reading an Ogg/Opus file (RFC 3533, RFC 7845) into its Opus packets.

Enough for Tailnet chat's voice messages: one logical stream, its OpusHead,
its OpusTags, and the audio packets after them. Every page's CRC is checked.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


def _crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        table.append(r & 0xFFFFFFFF)
    return table


_CRC = _crc_table()


def crc(data: bytes) -> int:
    r = 0
    for b in data:
        r = ((r << 8) & 0xFFFFFFFF) ^ _CRC[((r >> 24) & 0xFF) ^ b]
    return r


@dataclass
class Page:
    offset: int
    flags: int
    granule: int
    serial: int
    sequence: int
    packets: list = field(default_factory=list)  # (bytes, complete)


def pages(data: bytes):
    pos = 0
    while pos < len(data):
        if data[pos:pos + 4] != b"OggS" or len(data) - pos < 27:
            raise ValueError(f"no Ogg page at byte {pos}")
        version, flags, granule, serial, seq, checksum, nseg = struct.unpack_from(
            "<BBqIIIB", data, pos + 4)
        if version != 0:
            raise ValueError("Ogg version is not 0")
        lacing = data[pos + 27:pos + 27 + nseg]
        body_at = pos + 27 + nseg
        size = sum(lacing)
        end = body_at + size
        if len(lacing) != nseg or end > len(data):
            raise ValueError(f"truncated Ogg page at byte {pos}")
        header = bytearray(data[pos:body_at])
        header[22:26] = b"\0\0\0\0"
        if crc(bytes(header) + data[body_at:end]) != checksum:
            raise ValueError(f"bad CRC on the Ogg page at byte {pos}")
        page = Page(pos, flags, granule, serial, seq)
        at, run = body_at, 0
        for n in lacing:
            run += n
            if n < 255:
                page.packets.append((data[at:at + run], True))
                at += run
                run = 0
        if run:
            page.packets.append((data[at:at + run], False))
        yield page
        pos = end


@dataclass
class OpusFile:
    channels: int
    input_rate: int
    pre_skip: int
    last_granule: int
    frames: list  # the audio packets, bytes each

    @property
    def duration_ms(self) -> int:
        return (self.last_granule - self.pre_skip) * 1000 // 48000


def packets(data: bytes) -> tuple[list, int]:
    out, partial, last_granule = [], b"", 0
    for page in pages(data):
        if page.granule >= 0:
            last_granule = page.granule
        for body, complete in page.packets:
            partial += body
            if complete:
                out.append(partial)
                partial = b""
    if partial:
        raise ValueError("the last Ogg packet is not complete")
    return out, last_granule


def read_opus(data: bytes) -> OpusFile:
    pkts, last_granule = packets(data)
    if len(pkts) < 2 or not pkts[0].startswith(b"OpusHead") or not pkts[1].startswith(b"OpusTags"):
        raise ValueError("not an Ogg/Opus file")
    head = pkts[0]
    channels = head[9]
    pre_skip, input_rate = struct.unpack_from("<HI", head, 10)
    return OpusFile(channels, input_rate, pre_skip, last_granule, pkts[2:])
