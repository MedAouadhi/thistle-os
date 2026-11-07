/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the MeshCore app's addresses and words (meshchat_core.h)
 */
#include "meshchat/meshchat_core.h"

#include <stdio.h>
#include <string.h>

void meshchat_address(const uint8_t *pub_key, char out[MESHCHAT_ADDR_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < MESHCHAT_KEY_PREFIX; i++) {
        out[2 * i] = hex[pub_key[i] >> 4];
        out[2 * i + 1] = hex[pub_key[i] & 0x0F];
    }
    out[MESHCHAT_ADDR_LEN - 1] = '\0';
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool meshchat_parse_address(const char *addr, uint8_t prefix[MESHCHAT_KEY_PREFIX])
{
    if (!addr || strlen(addr) != MESHCHAT_KEY_PREFIX * 2) return false;
    for (int i = 0; i < MESHCHAT_KEY_PREFIX; i++) {
        int hi = nibble(addr[2 * i]), lo = nibble(addr[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        prefix[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

const char *meshchat_route(uint8_t path_len, char *out, size_t n)
{
    if (path_len == 0xFF) snprintf(out, n, "route unknown");
    else if (path_len == 0) snprintf(out, n, "in range");
    else snprintf(out, n, "%u hop%s", (unsigned)path_len, path_len == 1 ? "" : "s");
    return out;
}

uint32_t meshchat_text_limit(bool channel, size_t self_name_len, uint32_t max)
{
    if (!channel) return max;
    size_t prefix = self_name_len + 2;
    return prefix < max ? (uint32_t)(max - prefix) : 0;
}
