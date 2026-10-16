/*
 * Unit tests for dictation's rules: the WAV and the form it is sent in, the
 * answer read back, the configuration, and how a transcript joins the text
 * already in the field.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_runner.h"
#include "ui/dictation_core.h"

#include <stdint.h>
#include <string.h>

#define JSON(s) (s), strlen(s)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ── The WAV ──────────────────────────────────────────────────────────── */

TEST(dictation_wav_header_says_16k_mono_16_bit_pcm) {
    uint8_t h[DICTATION_WAV_HEADER_BYTES];
    dictation_wav_header(h, 16000, 32000);
    ASSERT_TRUE(memcmp(h, "RIFF", 4) == 0);
    ASSERT_EQ(le32(h + 4), 36 + 32000);
    ASSERT_TRUE(memcmp(h + 8, "WAVEfmt ", 8) == 0);
    ASSERT_EQ(le32(h + 16), 16);
    ASSERT_EQ(le16(h + 20), 1);        /* PCM */
    ASSERT_EQ(le16(h + 22), 1);        /* mono */
    ASSERT_EQ(le32(h + 24), 16000);
    ASSERT_EQ(le32(h + 28), 32000);    /* bytes a second */
    ASSERT_EQ(le16(h + 32), 2);        /* bytes a frame */
    ASSERT_EQ(le16(h + 34), 16);
    ASSERT_TRUE(memcmp(h + 36, "data", 4) == 0);
    ASSERT_EQ(le32(h + 40), 32000);
}

TEST(dictation_a_minute_fits_the_buffer_it_is_given) {
    ASSERT_EQ(DICTATION_MAX_PCM_BYTES, 60 * 32000);
}

/* ── The form ─────────────────────────────────────────────────────────── */

TEST(dictation_form_carries_the_model_the_language_and_the_file) {
    dictation_form_t f;
    ASSERT_TRUE(dictation_form_build(&f, "thistleBOUNDARY", "cohere-transcribe-arabic-07-2026",
                                     "ar"));
    ASSERT_EQ(f.head_len, strlen(f.head));
    ASSERT_STR_EQ(f.head,
                  "--thistleBOUNDARY\r\n"
                  "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                  "cohere-transcribe-arabic-07-2026\r\n"
                  "--thistleBOUNDARY\r\n"
                  "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                  "ar\r\n"
                  "--thistleBOUNDARY\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n");
    ASSERT_STR_EQ(f.tail, "\r\n--thistleBOUNDARY--\r\n");
    ASSERT_EQ(f.tail_len, strlen(f.tail));
    ASSERT_STR_EQ(f.content_type, "multipart/form-data; boundary=thistleBOUNDARY");
}

TEST(dictation_form_refuses_values_that_would_break_it) {
    dictation_form_t f;
    ASSERT_TRUE(!dictation_form_build(&f, "b", "model\r\n--b", "ar"));
    ASSERT_TRUE(!dictation_form_build(&f, "b", "mo\"del", "ar"));
    ASSERT_TRUE(!dictation_form_build(&f, "b", "model", ""));
    ASSERT_TRUE(!dictation_form_build(&f, "", "model", "ar"));
    ASSERT_TRUE(!dictation_form_build(&f, "this-boundary-is-longer-than-forty-characters",
                                      "model", "ar"));
}

/* ── The answer ───────────────────────────────────────────────────────── */

TEST(dictation_reads_the_text_of_an_answer) {
    char out[64];
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\": \"Remind me to buy bread.\"}"), "text",
                                      out, sizeof(out)));
    ASSERT_STR_EQ(out, "Remind me to buy bread.");
}

TEST(dictation_reads_arabic_sent_as_it_is_or_escaped) {
    char raw[64], escaped[64];
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\":\"صباح الخير\"}"), "text", raw,
                                      sizeof(raw)));
    ASSERT_TRUE(dictation_json_string(
        JSON("{\"text\":\"\\u0635\\u0628\\u0627\\u062d \\u0627\\u0644\\u062e\\u064a\\u0631\"}"),
        "text", escaped, sizeof(escaped)));
    ASSERT_STR_EQ(raw, "صباح الخير");
    ASSERT_STR_EQ(escaped, "صباح الخير");
}

TEST(dictation_reads_a_surrogate_pair_as_one_character) {
    char out[16];
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\":\"\\ud83d\\ude00!\"}"), "text", out,
                                      sizeof(out)));
    ASSERT_STR_EQ(out, "\xF0\x9F\x98\x80!");
    /* Alone, half a pair is no character, and becomes the replacement one. */
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\":\"a\\ud83db\"}"), "text", out,
                                      sizeof(out)));
    ASSERT_STR_EQ(out, "a\xEF\xBF\xBD" "b");
}

TEST(dictation_reads_escapes) {
    char out[32];
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\":\"a\\\"b\\\\c\\/d\\ne\"}"), "text", out,
                                      sizeof(out)));
    ASSERT_STR_EQ(out, "a\"b\\c/d\ne");
}

TEST(dictation_finds_the_key_past_other_members_of_any_kind) {
    char out[32];
    const char *json = "{\"id\":\"x\",\"meta\":{\"text\":\"inner\",\"n\":[1,2,{\"a\":null}]},"
                       "\"ok\":true,\"n\":-1.5e3,\"text\":\"outer\"}";
    ASSERT_TRUE(dictation_json_string(JSON(json), "text", out, sizeof(out)));
    ASSERT_STR_EQ(out, "outer");
}

TEST(dictation_cuts_a_long_text_between_characters) {
    char out[6];
    /* Three two-byte letters: five bytes hold two of them and the NUL. */
    ASSERT_TRUE(dictation_json_string(JSON("{\"text\":\"صبا\"}"), "text", out, sizeof(out)));
    ASSERT_STR_EQ(out, "صب");
}

TEST(dictation_finds_nothing_in_what_is_not_an_answer) {
    char out[16] = "stale";
    ASSERT_TRUE(!dictation_json_string(JSON("{\"message\":\"no\"}"), "text", out, sizeof(out)));
    ASSERT_STR_EQ(out, "");
    ASSERT_TRUE(!dictation_json_string(JSON("{\"text\":42}"), "text", out, sizeof(out)));
    ASSERT_TRUE(!dictation_json_string(JSON("[\"text\"]"), "text", out, sizeof(out)));
    ASSERT_TRUE(!dictation_json_string(JSON("<html>Bad gateway</html>"), "text", out,
                                       sizeof(out)));
    ASSERT_TRUE(!dictation_json_string(JSON("{\"text\":\"unterminated"), "text", out,
                                       sizeof(out)));
    ASSERT_TRUE(!dictation_json_string(NULL, 0, "text", out, sizeof(out)));
}

TEST(dictation_reads_the_message_of_a_refusal) {
    char out[64];
    ASSERT_TRUE(dictation_json_string(JSON("{\"id\":\"9\",\"message\":\"invalid api token\"}"),
                                      "message", out, sizeof(out)));
    ASSERT_STR_EQ(out, "invalid api token");
}

TEST(dictation_says_what_a_refusal_means) {
    char out[64];
    dictation_status_words(401, "invalid api token", out, sizeof(out));
    ASSERT_STR_EQ(out, "Cohere refused the API key");
    dictation_status_words(429, "", out, sizeof(out));
    ASSERT_STR_EQ(out, "Cohere says too many requests; wait a little");
    dictation_status_words(503, "", out, sizeof(out));
    ASSERT_STR_EQ(out, "Cohere is having trouble; try again");
    dictation_status_words(400, "file too short", out, sizeof(out));
    ASSERT_STR_EQ(out, "Cohere: file too short");
    dictation_status_words(418, "", out, sizeof(out));
    ASSERT_STR_EQ(out, "Cohere answered 418");
    dictation_status_words(-1, "", out, sizeof(out));
    ASSERT_STR_EQ(out, "Could not reach Cohere");
}

/* ── The configuration ────────────────────────────────────────────────── */

TEST(dictation_config_defaults_are_the_hermes_agents) {
    dictation_config_t c;
    dictation_config_init(&c, "built-in");
    ASSERT_STR_EQ(c.url, "https://api.cohere.com/v2/audio/transcriptions");
    ASSERT_STR_EQ(c.model, "cohere-transcribe-arabic-07-2026");
    ASSERT_STR_EQ(c.language, "ar");
    ASSERT_STR_EQ(c.api_key, "built-in");
    dictation_config_init(&c, NULL);
    ASSERT_STR_EQ(c.api_key, "");
}

TEST(dictation_config_takes_what_the_card_gives) {
    dictation_config_t c;
    dictation_config_init(&c, "built-in");
    ASSERT_TRUE(dictation_config_apply(
        &c, JSON("{\"api_key\":\"card\",\"model\":\"m2\",\"url\":\"http://127.0.0.1:9/t\"}")));
    ASSERT_STR_EQ(c.api_key, "card");
    ASSERT_STR_EQ(c.model, "m2");
    ASSERT_STR_EQ(c.url, "http://127.0.0.1:9/t");
    /* The language is the user's, chosen in the quick panel, not the card's. */
    ASSERT_TRUE(dictation_config_apply(&c, JSON("{\"language\":\"en\"}")));
    ASSERT_STR_EQ(c.language, "ar");
}

TEST(dictation_config_keeps_defaults_for_empty_or_oversized_values) {
    dictation_config_t c;
    dictation_config_init(&c, "built-in");
    char json[400];
    char long_key[200];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"api_key\":\"%s\",\"model\":\"\",\"url\":3}", long_key);
    ASSERT_TRUE(dictation_config_apply(&c, JSON(json)));
    ASSERT_STR_EQ(c.api_key, "built-in");
    ASSERT_STR_EQ(c.model, "cohere-transcribe-arabic-07-2026");
    ASSERT_STR_EQ(c.url, "https://api.cohere.com/v2/audio/transcriptions");
}

TEST(dictation_config_ignores_a_file_that_is_not_json) {
    dictation_config_t c;
    dictation_config_init(&c, "built-in");
    ASSERT_TRUE(!dictation_config_apply(&c, JSON("api_key=card")));
    ASSERT_STR_EQ(c.api_key, "built-in");
}

/* ── Into the field ───────────────────────────────────────────────────── */

TEST(dictation_joins_a_word_before_the_cursor_with_a_space) {
    char out[64];
    ASSERT_EQ(dictation_join(out, sizeof(out), "buy bread", 'o', false), 10);
    ASSERT_STR_EQ(out, " buy bread");
}

TEST(dictation_adds_no_space_at_the_start_or_after_one) {
    char out[64];
    dictation_join(out, sizeof(out), "buy bread", 0, false);
    ASSERT_STR_EQ(out, "buy bread");
    dictation_join(out, sizeof(out), "buy bread", ' ', false);
    ASSERT_STR_EQ(out, "buy bread");
    dictation_join(out, sizeof(out), "buy bread", '\n', false);
    ASSERT_STR_EQ(out, "buy bread");
    dictation_join(out, sizeof(out), "buy bread", '(', false);
    ASSERT_STR_EQ(out, "buy bread");
}

TEST(dictation_adds_no_space_before_punctuation) {
    char out[64];
    dictation_join(out, sizeof(out), ", and milk.", 'd', false);
    ASSERT_STR_EQ(out, ", and milk.");
    dictation_join(out, sizeof(out), "?", 'y', false);
    ASSERT_STR_EQ(out, "?");
}

TEST(dictation_takes_an_arabic_letter_before_the_cursor_for_a_word) {
    char out[64];
    /* The last byte of "ر" (U+0631, D8 B1). */
    dictation_join(out, sizeof(out), "حليب", 0xB1, false);
    ASSERT_STR_EQ(out, " حليب");
}

TEST(dictation_trims_the_transcript) {
    char out[64];
    dictation_join(out, sizeof(out), "  hello world. \n", 0, false);
    ASSERT_STR_EQ(out, "hello world.");
    ASSERT_EQ(dictation_join(out, sizeof(out), " \r\n\t ", 'x', false), 0);
    ASSERT_STR_EQ(out, "");
    ASSERT_EQ(dictation_join(out, sizeof(out), NULL, 'x', false), 0);
}

TEST(dictation_keeps_line_breaks_in_a_text_area_and_not_in_a_line) {
    char out[64];
    dictation_join(out, sizeof(out), "one.\r\ntwo.\nthree.", 0, false);
    ASSERT_STR_EQ(out, "one.\r\ntwo.\nthree.");
    dictation_join(out, sizeof(out), "one.\r\ntwo.\nthree.", 0, true);
    ASSERT_STR_EQ(out, "one. two. three.");
}

TEST(dictation_cuts_a_long_join_between_characters) {
    char out[6];
    /* A space and two two-byte letters fill five bytes; the third is cut. */
    ASSERT_EQ(dictation_join(out, sizeof(out), "صبا", 'x', false), 5);
    ASSERT_STR_EQ(out, " صب");
}

TEST(dictation_finds_the_byte_before_a_cursor_counted_in_characters) {
    ASSERT_EQ(dictation_byte_before("", 0), 0);
    ASSERT_EQ(dictation_byte_before("abc", 0), 0);
    ASSERT_EQ(dictation_byte_before("abc", 2), 'b');
    ASSERT_EQ(dictation_byte_before("abc", 9), 'c');
    /* "صب x": the cursor after the two letters is at character 2, byte 4. */
    ASSERT_EQ(dictation_byte_before("صب x", 2), 0xA8);
    ASSERT_EQ(dictation_byte_before("صب x", 3), ' ');
    ASSERT_EQ(dictation_byte_before(NULL, 3), 0);
}
