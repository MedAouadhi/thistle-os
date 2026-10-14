// SPDX-License-Identifier: BSD-3-Clause
// Host tests for reading SMS from the A7682E's storage. Answers are written
// as the modem sends them with echo off. The 7-bit vectors were packed with
// a short script that puts each septet at bit 7*i, least significant bit
// first, after the user data header and its fill bits; the packer gives the
// textbook E8329BFD4697D9EC37 for "hellohello".

#include "a7682e_sms.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NUMBER_UCS2 "002B0034003900310037003600310032003300340035003600370038"
#define SCTS        "25/10/13,12:34:56+08"

static char g_resp[4096];

// An AT+CMGR answer for a received message, with AT+CSDH=1.
static const char *cmgr(const char *oa, int fo, int dcs, const char *length, const char *data)
{
    int n = snprintf(g_resp, sizeof(g_resp),
                     "\r\n+CMGR: \"REC UNREAD\",\"%s\",\"\",\"" SCTS "\",145,%d,0,%d,"
                     "\"+491770610000\",145,%s\r\n%s\r\n\r\nOK\r\n",
                     oa, fo, dcs, length, data);
    assert(n > 0 && (size_t)n < sizeof(g_resp));
    return g_resp;
}

// Parses into a struct full of junk, so that every field is seen to be set.
static esp_err_t read_sms(const char *resp, bool ucs2, hal_sms_t *sms)
{
    memset(sms, 0xA5, sizeof(*sms));
    return a7682e_parse_cmgr(resp, ucs2, sms);
}

static bool is_whole(const hal_sms_t *sms)
{
    return sms->parts == 0 && sms->part == 0 && sms->concat_ref == 0;
}

static bool valid_utf8(const char *s)
{
    const unsigned char *b = (const unsigned char *)s;
    while (*b) {
        int n = *b < 0x80 ? 1 : (*b & 0xE0) == 0xC0 ? 2 : (*b & 0xF0) == 0xE0 ? 3
              : (*b & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0) {
            return false;
        }
        for (int i = 1; i < n; i++) {
            if ((b[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        b += n;
    }
    return true;
}

static void test_storage_used(void)
{
    assert(a7682e_parse_storage_used(
               "\r\n+CPMS: \"ME\",3,50,\"ME\",3,50,\"ME\",3,50\r\n\r\nOK\r\n") == 3);
    assert(a7682e_parse_storage_used("\r\n+CPMS: ME,12,50,ME,12,50,ME,12,50\r\n") == 12);
    // As the manual prints it, and the write command's form without names.
    assert(a7682e_parse_storage_used("+CPMS: \"ME\", 0, 180,\"ME\", 0, 180,\"ME\", 0, 180") == 0);
    assert(a7682e_parse_storage_used("\r\n+CPMS: 7,50,7,50,7,50\r\n\r\nOK\r\n") == 7);

    assert(a7682e_parse_storage_used("\r\nOK\r\n") == -1);
    assert(a7682e_parse_storage_used("\r\n+CMS ERROR: 302\r\n") == -1);
    assert(a7682e_parse_storage_used("+CPMS: (\"ME\",\"SM\"),(\"ME\",\"SM\")") == -1);
    assert(a7682e_parse_storage_used("+CPMS: \"ME\",") == -1);
    assert(a7682e_parse_storage_used(NULL) == -1);
}

static void test_index_list(void)
{
    int idx[16];
    assert(a7682e_parse_index_list("\r\n+CMGD: (1,2,5-7),(0-4)\r\n\r\nOK\r\n", idx, 16) == 5);
    assert(idx[0] == 1 && idx[1] == 2 && idx[2] == 5 && idx[3] == 6 && idx[4] == 7);

    assert(a7682e_parse_index_list("\r\n+CMGD: (),(0-4)\r\n\r\nOK\r\n", idx, 16) == 0);
    assert(a7682e_parse_index_list("+CMGD: ()", idx, 16) == 0);

    assert(a7682e_parse_index_list("+CMGD: (3),(0-4)", idx, 16) == 1);
    assert(idx[0] == 3);

    // Cut at max, inside a range too.
    idx[3] = -1;
    assert(a7682e_parse_index_list("+CMGD: (1,2,5-7),(0-4)", idx, 3) == 3);
    assert(idx[0] == 1 && idx[1] == 2 && idx[2] == 5 && idx[3] == -1);
    assert(a7682e_parse_index_list("+CMGD: (1-100),(0-4)", idx, 4) == 4);
    assert(idx[3] == 4);
    assert(a7682e_parse_index_list("+CMGD: (1,2),(0-4)", idx, 0) == 0);

    assert(a7682e_parse_index_list("\r\nOK\r\n", idx, 16) == -1);
    assert(a7682e_parse_index_list("+CMGD: 1", idx, 16) == -1);
    assert(a7682e_parse_index_list(NULL, idx, 16) == -1);
}

static void test_ucs2_charset(void)
{
    hal_sms_t sms;
    // "Grüße aus Köln: 5 € fürs Bier", a 7-bit message the modem shows as UCS2.
    assert(read_sms(cmgr(NUMBER_UCS2, 4, 0, "29",
                         "0047007200FC00DF006500200061007500730020004B00F6006C006E003A0020"
                         "0035002020AC0020006600FC0072007300200042006900650072"),
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Grüße aus Köln: 5 € fürs Bier") == 0);
    assert(strcmp(sms.sender, "+4917612345678") == 0);
    assert(strcmp(sms.sent_at, SCTS) == 0);
    assert(is_whole(&sms));

    // "Hallo\nWelt".
    assert(read_sms(cmgr(NUMBER_UCS2, 4, 0, "10",
                         "00480061006C006C006F000A00570065006C0074"),
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hallo\nWelt") == 0);

    // Some firmware converts the time stamp to UCS2 as well.
    assert(read_sms("\r\n+CMGR: \"REC READ\",\"" NUMBER_UCS2 "\",\"\","
                    "\"00320035002F00310030002F00310033002C00310032003A00330034003A"
                    "00350036002B00300038\",145,4,0,0,\"+491770610000\",145,2\r\n"
                    "00480069\r\n\r\nOK\r\n",
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.sent_at, SCTS) == 0);
    assert(strcmp(sms.text, "Hi") == 0);
}

static void test_ira_charset(void)
{
    hal_sms_t sms;
    assert(read_sms(cmgr("+4917612345678", 4, 0, "11", "Hello world"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hello world") == 0);
    assert(strcmp(sms.sender, "+4917612345678") == 0);
    assert(strcmp(sms.sent_at, SCTS) == 0);
    assert(is_whole(&sms));

    // Line ends inside the text, one of them a line that reads OK.
    assert(read_sms(cmgr("+4917612345678", 4, 0, "12", "line1\r\nline2"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "line1\nline2") == 0);
    assert(read_sms(cmgr("+4917612345678", 4, 0, "12", "fine\r\nOK\r\nbye"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "fine\nOK\nbye") == 0);

    // A phonebook name with a comma in it; numeric <stat>.
    assert(read_sms("\r\n+CMGR: 1,\"+4917612345678\",\"Doe, John\",\"" SCTS "\",145,4,0,0,"
                    "\"+491770610000\",145,5\r\nHello\r\n\r\nOK\r\n",
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hello") == 0);
    assert(strcmp(sms.sent_at, SCTS) == 0);

    // An empty message, and a blank line before the text.
    assert(read_sms(cmgr("+4917612345678", 4, 0, "0", ""), false, &sms) == ESP_OK);
    assert(sms.text[0] == '\0');
    assert(read_sms(cmgr("+4917612345678", 4, 0, "5", "\r\nHello"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hello") == 0);

    // Control characters go; a byte that is not UTF-8 is taken as Latin-1.
    assert(read_sms(cmgr("+4917612345678", 4, 0, "6", "a\tb\x01" "c\xFC"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcü") == 0);
}

static void test_ucs2_coding(void)
{
    hal_sms_t sms;
    // "Hi 😀!": U+1F600 as the surrogate pair D83D DE00.
    assert(read_sms(cmgr("+4917612345678", 4, 8, "12", "004800690020D83DDE000021"),
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hi \xF0\x9F\x98\x80!") == 0);

    // The same in UCS2 mode: the octets in hex either way.
    assert(read_sms(cmgr(NUMBER_UCS2, 4, 8, "12", "004800690020D83DDE000021"),
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hi \xF0\x9F\x98\x80!") == 0);
    assert(strcmp(sms.sender, "+4917612345678") == 0);

    // Half a pair becomes U+FFFD.
    assert(read_sms(cmgr("+4917612345678", 4, 8, "8", "0041D83D0042DE00"),
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "A\xEF\xBF\xBD" "B\xEF\xBF\xBD") == 0);
}

static void test_data_coding_groups(void)
{
    // 20AC is "€" in UCS2, " ¬" in 8-bit, and four characters of 7-bit text.
    static const struct {
        int dcs;
        const char *text;
    } cases[] = {
        { 0x08, "€" },     // general, UCS2
        { 0x18, "€" },     // general, UCS2, class 0
        { 0x48, "€" },     // automatic deletion, UCS2
        { 0xE0, "€" },     // message waiting, store, UCS2
        { 0x04, " ¬" },    // general, 8-bit
        { 0xF4, " ¬" },    // data coding / message class, 8-bit
        { 0x00, "20AC" },
        { 0xF0, "20AC" },
        { 0xC0, "20AC" },  // message waiting, discard
        { 0xD0, "20AC" },  // message waiting, store
        { 0x0C, "20AC" },  // reserved alphabet
        { 0x80, "20AC" },  // reserved group
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hal_sms_t sms;
        assert(read_sms(cmgr("+4917612345678", 4, cases[i].dcs, "4", "20AC"), false, &sms)
               == ESP_OK);
        assert(strcmp(sms.text, cases[i].text) == 0);
    }
}

static void test_concatenated_gsm7(void)
{
    hal_sms_t sms;
    // UDH 05 00 03 2A 02 01 (part 1 of 2, reference 0x2A): six octets, so
    // the text starts at septet 7 after one fill bit. "Hallo {Welt} ä €5"
    // is 20 septets with the escapes, 27 with the header.
    const char *part1 = "0500032A0201906136FB0DDAA0AE65367D9302ED419B720D";
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "27", part1), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hallo {Welt} ä €5") == 0);
    assert(sms.concat_ref == 0x2A && sms.parts == 2 && sms.part == 1);

    // The header's octets in hex whatever the character set.
    assert(read_sms(cmgr(NUMBER_UCS2, 0x44, 0, "27", part1), true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hallo {Welt} ä €5") == 0);
    assert(sms.concat_ref == 0x2A && sms.parts == 2 && sms.part == 1);

    // UDH 06 08 04 12 34 03 02 (part 2 of 3, 16-bit reference 0x1234): seven
    // octets, eight septets, no fill bits. "[Teil 2] ok".
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "21",
                         "060804123403021B1EB59C6683641B1FE8BD06"),
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "[Teil 2] ok") == 0);
    assert(sms.concat_ref == 0x1234 && sms.parts == 3 && sms.part == 2);

    // "abcdefgh" after the 6-octet header: 15 septets, 14 octets, the last
    // seven bits fill that would read as '@'.
    const char *padded = "0500032A0201C2E231B96C3EA301";
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "15", padded), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcdefgh") == 0);
    // <length> without the header's septets, without <length>, and wrong.
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "8", padded), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcdefgh") == 0);
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "", padded), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcdefgh") == 0);
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "99", padded), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcdefgh") == 0);
    assert(sms.concat_ref == 0x2A && sms.parts == 2 && sms.part == 1);

    // Numbering that cannot be used reads as a whole message: part 3 of 2,
    // part 0, one part.
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "15", "0500032A0203C2E231B96C3EA301"),
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "abcdefgh") == 0);
    assert(is_whole(&sms));
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "15", "0500032A0200C2E231B96C3EA301"),
                    false, &sms) == ESP_OK);
    assert(is_whole(&sms));
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "15", "0500032A0101C2E231B96C3EA301"),
                    false, &sms) == ESP_OK);
    assert(is_whole(&sms));

    // A header longer than the data leaves no text, and reads nothing past it.
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "", "0A00032A02"), false, &sms) == ESP_OK);
    assert(sms.text[0] == '\0');
    assert(is_whole(&sms));

    // Not hex after all: shown as it came.
    assert(read_sms(cmgr("+4917612345678", 0x44, 0, "5", "Hello"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hello") == 0);
    assert(is_whole(&sms));
}

static void test_concatenated_ucs2(void)
{
    hal_sms_t sms;
    // UDH 05 00 03 7F 03 03 (part 3 of 3, reference 0x7F), then "Ende ✓".
    assert(read_sms(cmgr(NUMBER_UCS2, 0x44, 8, "18", "0500037F03030045006E0064006500202713"),
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Ende ✓") == 0);
    assert(sms.concat_ref == 0x7F && sms.parts == 3 && sms.part == 3);
}

static void test_8bit(void)
{
    hal_sms_t sms;
    // "Hi", LF, é in Latin-1, two control bytes that are dropped, "!".
    assert(read_sms(cmgr("+4917612345678", 4, 4, "7", "48690AE9018021"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hi\né!") == 0);
    assert(is_whole(&sms));

    // With a header: UDH 05 00 03 01 02 02, then "ok".
    assert(read_sms(cmgr("+4917612345678", 0x44, 4, "8", "0500030102026F6B"),
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "ok") == 0);
    assert(sms.concat_ref == 1 && sms.parts == 2 && sms.part == 2);

    // Odd length: not hex.
    assert(read_sms(cmgr("+4917612345678", 4, 4, "2", "486"), false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "486") == 0);
}

static void test_alphanumeric_sender(void)
{
    hal_sms_t sms;
    assert(read_sms(cmgr("00440048004C", 4, 0, "4", "0048006900210021"), true, &sms) == ESP_OK);
    assert(strcmp(sms.sender, "DHL") == 0);
    assert(strcmp(sms.text, "Hi!!") == 0);

    assert(read_sms(cmgr("DHL", 4, 0, "4", "Hi!!"), false, &sms) == ESP_OK);
    assert(strcmp(sms.sender, "DHL") == 0);
    assert(strcmp(sms.text, "Hi!!") == 0);
}

static void test_not_received(void)
{
    hal_sms_t sms;
    // A status report.
    assert(read_sms("\r\n+CMGR: \"REC UNREAD\",6,233,\"+4917612345678\",145,\"" SCTS "\","
                    "\"25/10/13,12:35:01+08\",0\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_NOT_SUPPORTED);
    assert(sms.sender[0] == '\0' && sms.text[0] == '\0' && sms.sent_at[0] == '\0');
    assert(is_whole(&sms));

    // Stored to send, as the manual's example and as sent.
    assert(read_sms("\r\n+CMGR: \"STO UNSENT\",\"+10011\",,145,17,0,0,167,\"+8613800100500\","
                    "145,11\r\nHello World\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_NOT_SUPPORTED);
    assert(read_sms("\r\n+CMGR: \"STO SENT\",\"+10011\",,145,17,0,0,167,\"+8613800100500\","
                    "145,11\r\nHello World\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_NOT_SUPPORTED);
    assert(read_sms("\r\n+CMGR: 3,\"+10011\",,145,17,0,0,167,\"+86138\",145,2\r\nHi\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_NOT_SUPPORTED);

    // A broadcast, and a first octet that is not an SMS-DELIVER.
    assert(read_sms("\r\n+CMGR: \"REC UNREAD\",1234,50,0,1,1\r\nHello\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_NOT_SUPPORTED);
    assert(read_sms(cmgr("+4917612345678", 6, 0, "2", "Hi"), false, &sms)
           == ESP_ERR_NOT_SUPPORTED);
}

static void test_unreadable(void)
{
    hal_sms_t sms;
    assert(read_sms("\r\nOK\r\n", false, &sms) == ESP_ERR_INVALID_RESPONSE);
    assert(read_sms("\r\n+CMS ERROR: 321\r\n", false, &sms) == ESP_ERR_INVALID_RESPONSE);
    assert(sms.sender[0] == '\0' && sms.text[0] == '\0');
    assert(read_sms("\r\n+CMGR: \"REC FOO\",\"+49\",,\"" SCTS "\"\r\nHi\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_INVALID_RESPONSE);
    assert(read_sms("\r\n+CMGR: \"REC READ\",\"+49\r\nHi\r\n\r\nOK\r\n", false, &sms)
           == ESP_ERR_INVALID_RESPONSE);
    assert(read_sms("\r\n+CMGR: \"REC READ\"\r\nHi\r\n\r\nOK\r\n", false, &sms)
           == ESP_ERR_INVALID_RESPONSE);
    assert(read_sms("\r\n+CMGR: \"REC READ\",\"+49\",,\"" SCTS "\",145,x,0,0,\"+49\",145,2\r\n"
                    "Hi\r\n\r\nOK\r\n",
                    false, &sms) == ESP_ERR_INVALID_RESPONSE);
    assert(read_sms(NULL, false, &sms) == ESP_ERR_INVALID_ARG);
    assert(a7682e_parse_cmgr("+CMGR: \"REC READ\",\"+49\"", false, NULL) == ESP_ERR_INVALID_ARG);
}

static void test_truncation(void)
{
    // "a" then 170 euro signs and a "b", in UCS2: 512 bytes of UTF-8.
    char data[4 * 172 + 1];
    strcpy(data, "0061");
    for (int i = 0; i < 170; i++) {
        strcat(data, "20AC");
    }
    strcat(data, "0062");

    hal_sms_t sms;
    assert(read_sms(cmgr(NUMBER_UCS2, 4, 0, "172", data), true, &sms) == ESP_OK);
    // 1 + 3 * 159 bytes fit in 480; the 160th euro sign does not, and the
    // "b" that would fit after it is not taken either.
    assert(strlen(sms.text) == 478);
    assert(valid_utf8(sms.text));
    assert(memcmp(sms.text + 475, "€", 3) == 0);

    assert(read_sms(cmgr("+4917612345678", 4, 8, "344", data), false, &sms) == ESP_OK);
    assert(strlen(sms.text) == 478);
    assert(valid_utf8(sms.text));

    // A sender of 12 euro signs keeps 10 of them in its 32 bytes.
    assert(read_sms(cmgr("20AC20AC20AC20AC20AC20AC20AC20AC20AC20AC20AC20AC", 4, 0, "4",
                         "00480069"),
                    true, &sms) == ESP_OK);
    assert(strlen(sms.sender) == 30);
    assert(valid_utf8(sms.sender));

    // Plain ASCII past the buffer is cut at 480.
    char ascii[600];
    memset(ascii, 'x', sizeof(ascii) - 1);
    ascii[sizeof(ascii) - 1] = '\0';
    assert(read_sms(cmgr("+4917612345678", 4, 0, "160", ascii), false, &sms) == ESP_OK);
    assert(strlen(sms.text) == 480);
}

static void test_csdh_off(void)
{
    hal_sms_t sms;
    // The header stops at <scts>: a 7-bit SMS-DELIVER, shown as UCS2.
    assert(read_sms("\r\n+CMGR: \"REC UNREAD\",\"" NUMBER_UCS2 "\",\"\",\"" SCTS "\"\r\n"
                    "0048006500790021\r\n\r\nOK\r\n",
                    true, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hey!") == 0);
    assert(strcmp(sms.sender, "+4917612345678") == 0);
    assert(strcmp(sms.sent_at, SCTS) == 0);
    assert(is_whole(&sms));

    assert(read_sms("\r\n+CMGR: \"REC READ\",\"+4917612345678\",,\"" SCTS "\"\r\n"
                    "Hey!\r\n\r\nOK\r\n",
                    false, &sms) == ESP_OK);
    assert(strcmp(sms.text, "Hey!") == 0);
}

int main(void)
{
    test_storage_used();
    test_index_list();
    test_ucs2_charset();
    test_ira_charset();
    test_ucs2_coding();
    test_data_coding_groups();
    test_concatenated_gsm7();
    test_concatenated_ucs2();
    test_8bit();
    test_alphanumeric_sender();
    test_not_received();
    test_unreadable();
    test_truncation();
    test_csdh_off();
    puts("a7682e sms tests passed");
    return 0;
}
