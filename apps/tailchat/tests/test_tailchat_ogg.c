/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Host tests for the Chat app's Ogg/Opus, against the protocol's vector
 * docs/tailnet-chat/vectors/voice.opus (its path is argv[1]).
 *
 *     apps/tailchat/tests/run_tests.sh
 */
#include "tailchat/tailchat_ogg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #cond); \
            s_failed++;                                                    \
        }                                                                  \
    } while (0)

static uint8_t s_file[16384];
static size_t s_file_len;
static uint8_t s_packets[16384];
static uint16_t s_lens[200];

/* The vector's first and last Opus packets, from voice.json's "frames". */
static const char FIRST_HEX[] =
    "4881a6f2f3b171bd00000201ac1359183e64ff6b62bab6eebe0f8081b24879edc371c0333c3ee0bab782aa06cb9c945d"
    "92be29344f0a4efd749739a6e0b4e1d86bcf555752d7e8fa046ae60133832bb0c6b15131057947ab";

static size_t unhex(const char *hex, uint8_t *out)
{
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        sscanf(hex + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
    return n;
}

static void the_vector_reads_as_its_frames(void)
{
    tailchat_ogg_info_t info;
    EXPECT(tailchat_ogg_read(s_file, s_file_len, s_packets, sizeof(s_packets), s_lens, 200, &info));
    EXPECT(info.count == 76);
    EXPECT(info.channels == 1);
    EXPECT(info.input_rate == 16000);
    EXPECT(info.pre_skip == 312);
    EXPECT(tailchat_ogg_ms(&info) == 1500);
    uint8_t first[200];
    size_t n = unhex(FIRST_HEX, first);
    EXPECT(s_lens[0] == n);
    EXPECT(memcmp(s_packets, first, n) == 0);
    EXPECT(s_lens[75] == 117);
}

static void a_damaged_file_is_refused(void)
{
    tailchat_ogg_info_t info;
    uint8_t copy[sizeof(s_file)];
    memcpy(copy, s_file, s_file_len);
    copy[s_file_len / 2] ^= 0x40;
    EXPECT(!tailchat_ogg_read(copy, s_file_len, s_packets, sizeof(s_packets), s_lens, 200, &info));
    /* Cut short: half a file is not a file. */
    EXPECT(!tailchat_ogg_read(s_file, s_file_len - 100, s_packets, sizeof(s_packets), s_lens, 200, &info));
    EXPECT(!tailchat_ogg_read((const uint8_t *)"OggS", 4, s_packets, sizeof(s_packets), s_lens, 200, &info));
    /* Too many packets for the room given. */
    EXPECT(!tailchat_ogg_read(s_file, s_file_len, s_packets, sizeof(s_packets), s_lens, 10, &info));
}

static void what_is_written_reads_back_as_the_same_frames(void)
{
    tailchat_ogg_info_t info;
    EXPECT(tailchat_ogg_read(s_file, s_file_len, s_packets, sizeof(s_packets), s_lens, 200, &info));
    size_t total = 0;
    for (size_t i = 0; i < info.count; i++) total += s_lens[i];

    static uint8_t written[16384];
    size_t len = tailchat_ogg_write(written, sizeof(written), s_packets, s_lens, info.count, 312, 0x1234);
    EXPECT(len > total);
    EXPECT(memcmp(written, "OggS", 4) == 0);

    static uint8_t again[16384];
    static uint16_t again_lens[200];
    tailchat_ogg_info_t back;
    EXPECT(tailchat_ogg_read(written, len, again, sizeof(again), again_lens, 200, &back));
    EXPECT(back.count == info.count);
    EXPECT(back.pre_skip == 312);
    EXPECT(back.input_rate == 16000);
    EXPECT(back.channels == 1);
    /* 76 frames of 20 ms. */
    EXPECT(tailchat_ogg_ms(&back) == 1520);
    EXPECT(memcmp(again, s_packets, total) == 0);
    EXPECT(memcmp(again_lens, s_lens, info.count * sizeof(uint16_t)) == 0);

    /* No room: nothing written. */
    EXPECT(tailchat_ogg_write(written, 200, s_packets, s_lens, info.count, 312, 1) == 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s voice.opus\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    s_file_len = fread(s_file, 1, sizeof(s_file), f);
    fclose(f);

    the_vector_reads_as_its_frames();
    a_damaged_file_is_refused();
    what_is_written_reads_back_as_the_same_frames();
    if (s_failed) {
        fprintf(stderr, "%d failed\n", s_failed);
        return 1;
    }
    printf("tailchat_ogg: all tests passed\n");
    return 0;
}
