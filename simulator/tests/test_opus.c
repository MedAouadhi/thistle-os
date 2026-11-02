/*
 * SPDX-License-Identifier: BSD-3-Clause
 * The device's Opus (components/opus): a known clip, encoded and decoded at
 * Tailnet chat's settings, 16 kHz mono in 20 ms frames, comes back as long
 * and as loud as it went in.
 */
#include "test_runner.h"
#include "thistle_opus.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define CLIP_FRAMES 100 /* 2 s */
#define CLIP_SAMPLES (CLIP_FRAMES * THISTLE_OPUS_FRAME)

/* The known clip: a voiced, speech-like sound, the same on every run. A
 * glottal pitch gliding from 120 to 180 Hz, its harmonics shaped by two
 * formants near 700 and 1200 Hz, in syllables four times a second with a
 * pause between them. Its RMS is about 1270 (-28 dBFS), its peak 7530. */
static void make_clip(int16_t *pcm, int n)
{
    double phase = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / THISTLE_OPUS_RATE;
        double f0 = 120.0 + 60.0 * (0.5 + 0.5 * sin(2 * M_PI * 0.5 * t));
        phase += 2 * M_PI * f0 / THISTLE_OPUS_RATE;
        double v = 0;
        for (int h = 1; h * f0 < 3800; h++) {
            double f = h * f0;
            double formants = 1.0 / (1.0 + pow((f - 700) / 150, 2)) +
                              0.6 / (1.0 + pow((f - 1200) / 200, 2)) + 0.05;
            v += formants * sin(h * phase) / h;
        }
        double syllable = sin(2 * M_PI * 4.0 * t);
        double env = syllable > 0 ? syllable : 0;
        pcm[i] = (int16_t)lrint(9000.0 * env * v);
    }
}

static double rms(const int16_t *pcm, int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (double)pcm[i] * pcm[i];
    }
    return n ? sqrt(sum / n) : 0;
}

static double db(double ratio) { return 20.0 * log10(ratio); }

/* Encode the clip a frame at a time, decode each packet, and return the
 * decoded samples; *bytes is what the packets came to. */
static int round_trip(const int16_t *in, int16_t *out, int frames, size_t *bytes)
{
    OpusEncoder *enc = thistle_opus_encoder_new(THISTLE_OPUS_BITRATE);
    OpusDecoder *dec = thistle_opus_decoder_new();
    if (!enc || !dec) {
        return -1;
    }
    int total = 0;
    *bytes = 0;
    for (int f = 0; f < frames; f++) {
        uint8_t packet[THISTLE_OPUS_MAX_PACKET];
        int len = opus_encode(enc, in + f * THISTLE_OPUS_FRAME, THISTLE_OPUS_FRAME, packet,
                              sizeof(packet));
        if (len <= 0) {
            total = -2;
            break;
        }
        *bytes += (size_t)len;
        int got = opus_decode(dec, packet, len, out + total, THISTLE_OPUS_FRAME, 0);
        if (got < 0) {
            total = -3;
            break;
        }
        total += got;
    }
    thistle_opus_encoder_free(enc);
    thistle_opus_decoder_free(dec);
    return total;
}

TEST(opus_a_clip_comes_back_as_long_as_it_went_in)
{
    static int16_t in[CLIP_SAMPLES], out[CLIP_SAMPLES];
    make_clip(in, CLIP_SAMPLES);
    size_t bytes = 0;
    ASSERT_EQ(round_trip(in, out, CLIP_FRAMES, &bytes), CLIP_SAMPLES);
}

TEST(opus_a_clip_comes_back_as_loud_as_it_went_in)
{
    static int16_t in[CLIP_SAMPLES], out[CLIP_SAMPLES];
    make_clip(in, CLIP_SAMPLES);
    size_t bytes = 0;
    ASSERT_EQ(round_trip(in, out, CLIP_FRAMES, &bytes), CLIP_SAMPLES);
    double level_in = rms(in, CLIP_SAMPLES);
    ASSERT_TRUE(level_in > 1200 && level_in < 1350);
    /* The codec delays its output a few ms; over 2 s that is lost in the
     * sum. Within 1.5 dB is as loud. */
    ASSERT_NEAR(db(rms(out, CLIP_SAMPLES) / level_in), 0.0, 1.5);
}

TEST(opus_a_clip_costs_about_the_bitrate_asked_for)
{
    static int16_t in[CLIP_SAMPLES], out[CLIP_SAMPLES];
    make_clip(in, CLIP_SAMPLES);
    size_t bytes = 0;
    ASSERT_EQ(round_trip(in, out, CLIP_FRAMES, &bytes), CLIP_SAMPLES);
    /* 2 s at 24 kbit/s is 6000 bytes; VBR and the pauses move it. */
    ASSERT_TRUE(bytes > 2000 && bytes < 9000);
}

TEST(opus_silence_stays_silent)
{
    static int16_t in[CLIP_SAMPLES], out[CLIP_SAMPLES];
    memset(in, 0, sizeof(in));
    size_t bytes = 0;
    ASSERT_EQ(round_trip(in, out, CLIP_FRAMES, &bytes), CLIP_SAMPLES);
    ASSERT_TRUE(rms(out, CLIP_SAMPLES) < 5.0);
}

TEST(opus_a_lost_frame_is_concealed_with_a_whole_frame)
{
    OpusDecoder *dec = thistle_opus_decoder_new();
    ASSERT_TRUE(dec != NULL);
    int16_t pcm[THISTLE_OPUS_FRAME];
    int got = opus_decode(dec, NULL, 0, pcm, THISTLE_OPUS_FRAME, 0);
    thistle_opus_decoder_free(dec);
    ASSERT_EQ(got, THISTLE_OPUS_FRAME);
}

TEST(opus_the_pseudostack_is_measured)
{
    static int16_t in[CLIP_SAMPLES], out[CLIP_SAMPLES];
    make_clip(in, CLIP_SAMPLES);
    size_t bytes = 0;
    ASSERT_EQ(round_trip(in, out, CLIP_FRAMES, &bytes), CLIP_SAMPLES);
    size_t used = thistle_opus_scratch_used();
    ASSERT_TRUE(used > 0);
    ASSERT_TRUE(used <= thistle_opus_scratch_size());
}

TEST(opus_is_one_takers_at_a_time)
{
    const char *holder = NULL;
    ASSERT_TRUE(thistle_opus_take("a call", NULL));
    ASSERT_TRUE(!thistle_opus_take("a voice message", &holder));
    ASSERT_STR_EQ(holder, "a call");
    thistle_opus_give();
    ASSERT_TRUE(thistle_opus_take("a voice message", &holder));
    thistle_opus_give();
}
