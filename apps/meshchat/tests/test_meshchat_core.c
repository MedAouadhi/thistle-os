/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — host tests for meshchat_core.c
 */
#include "meshchat/meshchat_core.h"

#include <stdio.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            s_failed++;                                                   \
        }                                                                 \
    } while (0)

static void test_an_address_is_the_start_of_the_key_in_hex(void)
{
    uint8_t key[32] = {0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0xff};
    char addr[MESHCHAT_ADDR_LEN];
    meshchat_address(key, addr);
    CHECK(strcmp(addr, "1a2b3c4d5e6f") == 0);
    uint8_t prefix[MESHCHAT_KEY_PREFIX];
    CHECK(meshchat_parse_address(addr, prefix));
    CHECK(memcmp(prefix, key, sizeof prefix) == 0);
    CHECK(meshchat_parse_address("1A2B3C4D5E6F", prefix) && prefix[0] == 0x1a);
    CHECK(!meshchat_parse_address("", prefix));
    CHECK(!meshchat_parse_address("1a2b3c4d5e6", prefix));
    CHECK(!meshchat_parse_address("1a2b3c4d5e6g", prefix));
    CHECK(!meshchat_parse_address(NULL, prefix));
}

static void test_how_far_a_contact_is(void)
{
    char out[24];
    CHECK(strcmp(meshchat_route(0, out, sizeof out), "in range") == 0);
    CHECK(strcmp(meshchat_route(1, out, sizeof out), "1 hop") == 0);
    CHECK(strcmp(meshchat_route(3, out, sizeof out), "3 hops") == 0);
    CHECK(strcmp(meshchat_route(0xFF, out, sizeof out), "route unknown") == 0);
}

static void test_the_public_channel_carries_our_name_in_front(void)
{
    CHECK(meshchat_text_limit(false, 10, 160) == 160);
    CHECK(meshchat_text_limit(true, 10, 160) == 148);
    CHECK(meshchat_text_limit(true, 200, 160) == 0);
}

int main(void)
{
    test_an_address_is_the_start_of_the_key_in_hex();
    test_how_far_a_contact_is();
    test_the_public_channel_carries_our_name_in_front();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("meshchat_core: all passed\n");
    return 0;
}
