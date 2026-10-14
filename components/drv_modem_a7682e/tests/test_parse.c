// SPDX-License-Identifier: BSD-3-Clause
// Host tests for the A7682E answer parsers. The responses are written as the
// modem sends them with echo off: a CRLF before and after each line.

#include "a7682e_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_final_result(void)
{
    assert(a7682e_final_result("\r\n+CPIN: READY\r\n\r\nOK\r\n") == 1);
    assert(a7682e_final_result("\r\nERROR\r\n") == -1);
    assert(a7682e_final_result("\r\n+CME ERROR: SIM not inserted\r\n") == -1);
    assert(a7682e_final_result("\r\n+CMS ERROR: 321\r\n") == -1);

    // Still arriving: the error's text, or the OK after an answer line.
    assert(a7682e_final_result("\r\n+CME ERROR: SIM") == 0);
    assert(a7682e_final_result("\r\n+CPIN: READY\r\n") == 0);
    assert(a7682e_final_result("\r\n+CPIN: READY\r\n\r\nO") == 0);

    // An answer line that merely contains the words is not final.
    assert(a7682e_final_result("\r\n+CMGR: \"REC READ\",\"+46700\"\r\nOK then, ERROR later\r\n")
           == 0);
    assert(a7682e_final_result("") == 0);
    assert(a7682e_final_result(NULL) == 0);
}

static void test_sim(void)
{
    assert(a7682e_parse_sim("\r\n+CPIN: READY\r\n\r\nOK\r\n") == HAL_SIM_READY);
    assert(a7682e_parse_sim("\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n") == HAL_SIM_PIN_REQUIRED);
    assert(a7682e_parse_sim("\r\n+CPIN: SIM PUK\r\n\r\nOK\r\n") == HAL_SIM_PUK_REQUIRED);
    assert(a7682e_parse_sim("\r\n+CPIN: SIM PIN2\r\n\r\nOK\r\n") == HAL_SIM_READY);
    assert(a7682e_parse_sim("\r\n+CPIN: PH-NET PIN\r\n\r\nOK\r\n") == HAL_SIM_ERROR);
    assert(a7682e_parse_sim("\r\n+CPIN: SIM REMOVED\r\n") == HAL_SIM_ABSENT);
    assert(a7682e_parse_sim("\r\n+CPIN: NOT INSERTED\r\n") == HAL_SIM_ABSENT);
    assert(a7682e_parse_sim("\r\n+CPIN: NOT READY\r\n") == HAL_SIM_UNKNOWN);

    // AT+CMEE=2 and AT+CMEE=1 forms of the same errors.
    assert(a7682e_parse_sim("\r\n+CME ERROR: SIM not inserted\r\n") == HAL_SIM_ABSENT);
    assert(a7682e_parse_sim("\r\n+CME ERROR: 10\r\n") == HAL_SIM_ABSENT);
    assert(a7682e_parse_sim("\r\n+CME ERROR: SIM busy\r\n") == HAL_SIM_UNKNOWN);
    assert(a7682e_parse_sim("\r\n+CME ERROR: 14\r\n") == HAL_SIM_UNKNOWN);
    assert(a7682e_parse_sim("\r\n+CME ERROR: 11\r\n") == HAL_SIM_PIN_REQUIRED);
    assert(a7682e_parse_sim("\r\n+CME ERROR: SIM failure\r\n") == HAL_SIM_ERROR);

    // A bare ERROR (AT+CMEE=0) or nothing at all says nothing about the SIM.
    assert(a7682e_parse_sim("\r\nERROR\r\n") == HAL_SIM_UNKNOWN);
    assert(a7682e_parse_sim("") == HAL_SIM_UNKNOWN);
    assert(a7682e_parse_sim(NULL) == HAL_SIM_UNKNOWN);
}

static void test_registration(void)
{
    assert(a7682e_parse_reg("+CEREG: 0,1") == HAL_CELL_REG_HOME);
    assert(a7682e_parse_reg("+CEREG: 0,5") == HAL_CELL_REG_ROAMING);
    assert(a7682e_parse_reg("+CEREG: 0,2") == HAL_CELL_REG_SEARCHING);
    assert(a7682e_parse_reg("+CEREG: 0,3") == HAL_CELL_REG_DENIED);
    assert(a7682e_parse_reg("+CEREG: 0,0") == HAL_CELL_REG_NOT_REGISTERED);
    assert(a7682e_parse_reg("+CEREG: 0,4") == HAL_CELL_REG_UNKNOWN);
    assert(a7682e_parse_reg("+CGREG: 0,1") == HAL_CELL_REG_HOME);
    assert(a7682e_parse_reg("+CREG: 0,5") == HAL_CELL_REG_ROAMING);

    // n=2 adds location after the status.
    assert(a7682e_parse_reg("+CEREG: 2,1,\"00C3\",\"0001ABCD\",7") == HAL_CELL_REG_HOME);
    // A URC has no <n>: a bare status, or a status followed by location.
    assert(a7682e_parse_reg("+CEREG: 1") == HAL_CELL_REG_HOME);
    assert(a7682e_parse_reg("+CEREG: 5,\"00C3\",\"0001ABCD\",7") == HAL_CELL_REG_ROAMING);

    // Registered for SMS only, or for emergencies, carries no data.
    assert(a7682e_parse_reg("+CEREG: 0,6") == HAL_CELL_REG_NOT_REGISTERED);
    assert(a7682e_parse_reg("+CEREG: 0,11") == HAL_CELL_REG_NOT_REGISTERED);

    assert(a7682e_parse_reg("\r\nERROR\r\n") == HAL_CELL_REG_UNKNOWN);
    assert(a7682e_parse_reg("+CEREG: ") == HAL_CELL_REG_UNKNOWN);
    assert(a7682e_parse_reg(NULL) == HAL_CELL_REG_UNKNOWN);
}

static void test_best_registration(void)
{
    // LTE searching while 2G is registered: data can go over 2G.
    assert(a7682e_reg_best(HAL_CELL_REG_SEARCHING, HAL_CELL_REG_HOME) == HAL_CELL_REG_HOME);
    assert(a7682e_reg_best(HAL_CELL_REG_ROAMING, HAL_CELL_REG_SEARCHING) == HAL_CELL_REG_ROAMING);
    assert(a7682e_reg_best(HAL_CELL_REG_DENIED, HAL_CELL_REG_SEARCHING) == HAL_CELL_REG_SEARCHING);
    assert(a7682e_reg_best(HAL_CELL_REG_UNKNOWN, HAL_CELL_REG_NOT_REGISTERED)
           == HAL_CELL_REG_NOT_REGISTERED);
    assert(a7682e_reg_is_registered(HAL_CELL_REG_HOME));
    assert(a7682e_reg_is_registered(HAL_CELL_REG_ROAMING));
    assert(!a7682e_reg_is_registered(HAL_CELL_REG_SEARCHING));
}

static void test_operator(void)
{
    char name[24];
    assert(a7682e_parse_operator("+COPS: 0,0,\"Telia S\",7", name, sizeof(name)));
    assert(strcmp(name, "Telia S") == 0);

    // Unregistered: mode only.
    assert(!a7682e_parse_operator("+COPS: 0", name, sizeof(name)));
    assert(name[0] == '\0');

    // A name longer than the buffer is cut, not overrun.
    char small[6];
    assert(a7682e_parse_operator("+COPS: 0,0,\"Vodafone NL\",7", small, sizeof(small)));
    assert(strcmp(small, "Vodaf") == 0);

    assert(!a7682e_parse_operator("+COPS: 0,0,\"\",7", name, sizeof(name)));
    assert(!a7682e_parse_operator(NULL, name, sizeof(name)));
}

static void test_context_apn(void)
{
    char apn[32];
    // As a T-Deck Max's module reports it after attaching, IMS bearer and all.
    const char *attached =
        "\r\n+CGDCONT: 1,\"IP\",\"internet.mnc003.mcc262.gprs\",\"10.153.124.211\",0,0,,,,\r\n"
        "+CGDCONT: 8,\"IPV4V6\",\"IMS\",\"254.128.0.0.0.0.0.0.53.35.179.17.36.213.234.94\",0,0,0,2,1,1\r\n"
        "\r\nOK\r\n";
    assert(a7682e_parse_context_apn(attached, 1, apn, sizeof(apn)));
    assert(strcmp(apn, "internet.mnc003.mcc262.gprs") == 0);
    assert(a7682e_parse_context_apn(attached, 8, apn, sizeof(apn)));
    assert(strcmp(apn, "IMS") == 0);

    // Defined with no APN, not defined, cut to fit, malformed.
    assert(!a7682e_parse_context_apn("+CGDCONT: 1,\"IP\",\"\",\"0.0.0.0\",0,0\r\nOK", 1, apn, sizeof(apn)));
    assert(apn[0] == '\0');
    assert(!a7682e_parse_context_apn(attached, 2, apn, sizeof(apn)));
    assert(!a7682e_parse_context_apn("+CGDCONT: 11,\"IP\",\"x\"", 1, apn, sizeof(apn)));
    char small[9];
    assert(a7682e_parse_context_apn(attached, 1, small, sizeof(small)));
    assert(strcmp(small, "internet") == 0);
    assert(!a7682e_parse_context_apn("+CGDCONT: 1,", 1, apn, sizeof(apn)));
    assert(!a7682e_parse_context_apn("+CGDCONT: 1,\"IP\"", 1, apn, sizeof(apn)));
    assert(!a7682e_parse_context_apn("+CGDCONT: 1,\"IP\",\"open", 1, apn, sizeof(apn)));
    assert(!a7682e_parse_context_apn(NULL, 1, apn, sizeof(apn)));
}

static void test_clock(void)
{
    int64_t utc = 0;
    int offset = -1;
    // 14:05:09 at UTC+2 (+08 quarters) is 12:05:09 UTC, 13 October 2026.
    assert(a7682e_parse_cclk("\r\n+CCLK: \"26/10/13,14:05:09+08\"\r\n\r\nOK\r\n", &utc, &offset));
    assert(utc == 1791893109 && offset == 120);
    // As esp_modem_at() hands it over, without the line ends.
    assert(a7682e_parse_cclk("+CCLK: \"26/10/13,14:05:09+08\"", &utc, &offset));
    assert(utc == 1791893109);

    // West of Greenwich, half-hour zones, and a local date that is not UTC's.
    assert(a7682e_parse_cclk("+CCLK: \"26/01/15,07:00:00-20\"", &utc, &offset));
    assert(utc == 1768478400 && offset == -300);
    assert(a7682e_parse_cclk("+CCLK: \"26/10/13,17:35:09+22\"", &utc, &offset));
    assert(utc == 1791893109 && offset == 330);
    assert(a7682e_parse_cclk("+CCLK: \"26/01/01,01:00:00+08\"", &utc, &offset));
    assert(utc == 1767222000);
    assert(a7682e_parse_cclk("+CCLK: \"26/10/13,12:05:09+00\"", &utc, &offset));
    assert(utc == 1791893109 && offset == 0);

    // A clock nothing has set reads as the epoch it counts from, for the
    // kernel to reject.
    assert(a7682e_parse_cclk("+CCLK: \"70/01/01,00:00:12+00\"", &utc, &offset));
    assert(utc == 12);
    assert(a7682e_parse_cclk("+CCLK: \"80/01/06,00:00:00+00\"", &utc, &offset));
    assert(utc == 315964800);

    // Local time without its offset says nothing about UTC.
    assert(!a7682e_parse_cclk("+CCLK: \"26/10/13,14:05:09\"", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: \"26/13/13,14:05:09+08\"", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: \"26/02/30,14:05:09+08\"", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: \"26/10/13,24:05:09+08\"", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: \"26/10/13,14:05:09+97\"", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: \"26/10/13,14:05:09+08", &utc, &offset));
    assert(!a7682e_parse_cclk("+CCLK: 26/10/13,14:05:09+08", &utc, &offset));
    assert(!a7682e_parse_cclk("\r\nOK\r\n", &utc, &offset));
    assert(!a7682e_parse_cclk(NULL, &utc, &offset));
}

int main(void)
{
    test_final_result();
    test_sim();
    test_registration();
    test_best_registration();
    test_operator();
    test_context_apn();
    test_clock();
    puts("a7682e parse tests passed");
    return 0;
}
