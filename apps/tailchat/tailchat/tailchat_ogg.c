// SPDX-License-Identifier: BSD-3-Clause
/* Ogg/Opus for voice messages: see tailchat_ogg.h. */
#include "tailchat_ogg.h"

#include <string.h>

#define HEADER 27
#define FLAG_CONTINUED 0x01
#define FLAG_BOS 0x02
#define FLAG_EOS 0x04
/* Samples at 48 kHz in a 20 ms frame. */
#define FRAME_48K 960

static uint32_t crc_step(uint32_t crc, uint8_t byte)
{
    crc ^= (uint32_t)byte << 24;
    for (int i = 0; i < 8; i++) crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    return crc;
}

static uint32_t crc_of(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) crc = crc_step(crc, data[i]);
    return crc;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

uint32_t tailchat_ogg_ms(const tailchat_ogg_info_t *info)
{
    if (info->last_granule <= info->pre_skip) return 0;
    return (uint32_t)((info->last_granule - info->pre_skip) / 48);
}

/* One page of whole packets, each under 255 * 255 bytes. */
static size_t page(uint8_t *out, size_t cap, uint8_t flags, uint64_t granule, uint32_t serial, uint32_t seq,
                   const uint8_t *body, const uint16_t *lens, size_t count)
{
    size_t segments = 0, size = 0;
    for (size_t i = 0; i < count; i++) {
        segments += lens[i] / 255 + 1;
        size += lens[i];
    }
    if (segments > 255 || HEADER + segments + size > cap) return 0;
    memcpy(out, "OggS", 4);
    out[4] = 0;
    out[5] = flags;
    put_le(out + 6, granule, 8);
    put_le(out + 14, serial, 4);
    put_le(out + 18, seq, 4);
    memset(out + 22, 0, 4);
    out[26] = (uint8_t)segments;
    uint8_t *lacing = out + HEADER;
    for (size_t i = 0; i < count; i++) {
        uint16_t n = lens[i];
        while (n >= 255) {
            *lacing++ = 255;
            n -= 255;
        }
        *lacing++ = (uint8_t)n;
    }
    memcpy(lacing, body, size);
    size_t total = HEADER + segments + size;
    put_le(out + 22, crc_of(out, total), 4);
    return total;
}

size_t tailchat_ogg_write(uint8_t *out, size_t cap, const uint8_t *packets, const uint16_t *lens,
                          size_t count, uint16_t pre_skip, uint32_t serial)
{
    uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1};
    put_le(head + 10, pre_skip, 2);
    put_le(head + 12, 16000, 4);
    /* Output gain 0, mapping family 0: already zero. */
    static const uint8_t vendor[] = "ThistleOS";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le(tags + 8, sizeof(vendor) - 1, 4);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_le(tags + 12 + sizeof(vendor) - 1, 0, 4);

    size_t at = 0, n;
    uint32_t seq = 0;
    uint16_t len = sizeof(head);
    if (!(n = page(out, cap, FLAG_BOS, 0, serial, seq++, head, &len, 1))) return 0;
    at += n;
    len = sizeof(tags);
    if (!(n = page(out + at, cap - at, 0, 0, serial, seq++, tags, &len, 1))) return 0;
    at += n;

    size_t done = 0, offset = 0;
    if (count == 0) {
        /* No audio: an empty last page still ends the stream. */
        if (!(n = page(out + at, cap - at, FLAG_EOS, pre_skip, serial, seq, NULL, NULL, 0))) return 0;
        return at + n;
    }
    while (done < count) {
        size_t take = count - done;
        if (take > TAILCHAT_OGG_PAGE_PACKETS) take = TAILCHAT_OGG_PAGE_PACKETS;
        size_t bytes = 0;
        for (size_t i = 0; i < take; i++) bytes += lens[done + i];
        uint64_t granule = pre_skip + (uint64_t)(done + take) * FRAME_48K;
        uint8_t flags = done + take == count ? FLAG_EOS : 0;
        if (!(n = page(out + at, cap - at, flags, granule, serial, seq++, packets + offset, lens + done, take)))
            return 0;
        at += n;
        offset += bytes;
        done += take;
    }
    return at;
}

bool tailchat_ogg_read(const uint8_t *file, size_t len, uint8_t *packets, size_t cap, uint16_t *lens,
                       size_t max, tailchat_ogg_info_t *info)
{
    memset(info, 0, sizeof(*info));
    size_t pos = 0, used = 0, header_packets = 0;
    size_t partial = 0;      /* bytes of the packet under way */
    uint8_t head[19];
    bool eos = false;
    /* The first two packets go to `head` and are dropped; the rest to
     * `packets`. A packet under way lives at packets + used. */
    while (pos < len) {
        if (len - pos < HEADER || memcmp(file + pos, "OggS", 4) != 0 || file[pos + 4] != 0) return false;
        uint8_t nseg = file[pos + 26];
        if (len - pos < HEADER + (size_t)nseg) return false;
        const uint8_t *lacing = file + pos + HEADER;
        size_t size = 0;
        for (int i = 0; i < nseg; i++) size += lacing[i];
        size_t total = HEADER + nseg + size;
        if (len - pos < total) return false;
        uint32_t want = (uint32_t)get_le(file + pos + 22, 4);
        uint32_t crc = crc_of(file + pos, 22);
        for (int i = 0; i < 4; i++) crc = crc_step(crc, 0);
        for (size_t i = 26; i < total; i++) crc = crc_step(crc, file[pos + i]);
        if (crc != want) return false;
        int64_t granule = (int64_t)get_le(file + pos + 6, 8);
        if (granule >= 0) info->last_granule = (uint64_t)granule;
        if (file[pos + 5] & FLAG_EOS) eos = true;

        const uint8_t *body = file + pos + HEADER + nseg;
        for (int i = 0; i < nseg; i++) {
            uint8_t n = lacing[i];
            if (used + partial + n > cap) return false;
            memcpy(packets + used + partial, body, n);
            body += n;
            partial += n;
            if (n == 255) continue;
            /* A packet ends here. */
            if (header_packets == 0) {
                if (partial < 19 || memcmp(packets + used, "OpusHead", 8) != 0) return false;
                memcpy(head, packets + used, sizeof(head));
                header_packets++;
            } else if (header_packets == 1) {
                if (partial < 8 || memcmp(packets + used, "OpusTags", 8) != 0) return false;
                header_packets++;
            } else {
                if (info->count >= max || partial > UINT16_MAX) return false;
                lens[info->count++] = (uint16_t)partial;
                used += partial;
            }
            partial = 0;
        }
        pos += total;
    }
    if (header_packets < 2 || partial || !eos) return false;
    info->channels = head[9];
    info->pre_skip = (uint16_t)get_le(head + 10, 2);
    info->input_rate = (uint32_t)get_le(head + 12, 4);
    return true;
}
