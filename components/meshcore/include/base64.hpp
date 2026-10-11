// SPDX-License-Identifier: BSD-3-Clause
// base64.hpp — the decoder BaseChatMesh::addChannel() reads a channel key with.
//
// MeshCore's own builds take this from the Arduino "base64" library. A stub
// that decoded nothing made every channel key 0 bytes long, so addChannel()
// refused even the Public channel every MeshCore node shares.
#pragma once

#include <stdint.h>
#include <stddef.h>

static inline int base64_sextet(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decode `src_len` characters of standard or URL-safe base64 into `dst`,
 * stopping at the first '=' or other character outside the alphabet.
 * Returns the number of bytes written: 3 for every 4 characters, fewer for a
 * short final group. */
static inline int decode_base64(const unsigned char* src, size_t src_len, unsigned char* dst) {
    uint32_t acc = 0;
    int bits = 0;
    int out = 0;
    for (size_t i = 0; i < src_len; i++) {
        int v = base64_sextet(src[i]);
        if (v < 0) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}
