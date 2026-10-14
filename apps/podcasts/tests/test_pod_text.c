// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// Descriptions out of HTML, durations, dates, sizes and query strings.

#include "podcasts/pod_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_failures;

static void expect_str(const char *what, const char *actual, const char *expected, int line)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d: %s\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, line, what,
            expected, actual);
    s_failures++;
}

static void check_html(const char *html, size_t cap, const char *expected, int line)
{
    char out[512];
    pod_text_from_html(html, out, cap);
    expect_str(html, out, expected, line);
}

#define HTML(html, expected) check_html((html), 512, (expected), __LINE__)

static void test_html(void)
{
    HTML("Plain words", "Plain words");
    HTML("  spaces\n\tand   lines  ", "spaces and lines");
    HTML("<p>One</p><p>Two</p>", "One\n\nTwo");
    HTML("<p>The Illegal Curve Hockey Show </p>\n<p><br></p>\n<p>Part of the network.</p>",
         "The Illegal Curve Hockey Show\n\nPart of the network.");
    HTML("Line<br>next<br/>last", "Line\nnext\nlast");
    HTML("<ul><li>a</li><li>b</li></ul>after", "a\nb\n\nafter");
    HTML("Tom &amp; Jerry &lt;3 &quot;q&quot; &#39;s&#x27;", "Tom & Jerry <3 \"q\" 's'");
    HTML("it&rsquo;s &ldquo;great&rdquo;&hellip; &mdash;", "it\xE2\x80\x99s \xE2\x80\x9Cgreat\xE2\x80\x9D\xE2\x80\xA6 \xE2\x80\x94");
    HTML("a&nbsp;b &#160;c", "a b c");
    HTML("AT&T & co &bogus; &", "AT&T & co &bogus; &");
    HTML("<a href=\"https://x.y/?a=1&b=2\">link</a> text", "link text");
    HTML("<script>var a = '<p>';</script>Shown<style>p{}</style>", "Shown");
    HTML("<b>bold</b><i>it</i>", "boldit");
    HTML("unclosed <tag", "unclosed");
    HTML("I <3 cats & 1 < 2", "I <3 cats & 1 < 2");
    HTML("<!-- note -->Q&amp;A", "Q&A");
    HTML("", "");

    /* Cut to fit, between characters, with nothing after what did not fit. */
    check_html("abc d\xC3\xA9" "f", 7, "abc d", __LINE__);
    check_html("abc d\xC3\xA9" "f", 8, "abc d\xC3\xA9", __LINE__);
    check_html("<p>one</p><p>two</p>", 6, "one", __LINE__);
    char tiny[1];
    pod_text_from_html("anything", tiny, sizeof tiny);
    expect_str("cap 1", tiny, "", __LINE__);
}

static void test_copy(void)
{
    char out[5];
    pod_text_copy(out, sizeof out, "ab\xC3\xA9\xC3\xA9");
    expect_str("copy", out, "ab\xC3\xA9", __LINE__);
    pod_text_copy(out, sizeof out, NULL);
    expect_str("copy NULL", out, "", __LINE__);
}

static void test_clock_and_duration(void)
{
    char out[32];
    pod_text_clock(0, out, sizeof out);
    expect_str("clock 0", out, "0:00", __LINE__);
    pod_text_clock(245, out, sizeof out);
    expect_str("clock", out, "4:05", __LINE__);
    pod_text_clock(3723, out, sizeof out);
    expect_str("clock h", out, "1:02:03", __LINE__);
    pod_text_clock(-5, out, sizeof out);
    expect_str("clock negative", out, "0:00", __LINE__);

    pod_text_duration(0, out, sizeof out);
    expect_str("duration 0", out, "", __LINE__);
    pod_text_duration(40, out, sizeof out);
    expect_str("duration s", out, "40 s", __LINE__);
    pod_text_duration(1851, out, sizeof out);
    expect_str("duration min", out, "31 min", __LINE__);
    pod_text_duration(3900, out, sizeof out);
    expect_str("duration h min", out, "1 h 5 min", __LINE__);
    pod_text_duration(7190, out, sizeof out);
    expect_str("duration h", out, "2 h", __LINE__);
}

static void test_date(void)
{
    char out[32];
    /* 2026-10-14 15:00 UTC */
    const int64_t now = 1791990000;
    pod_text_date(0, now, out, sizeof out);
    expect_str("date 0", out, "", __LINE__);
    pod_text_date(now - 3600, now, out, sizeof out);
    expect_str("today", out, "Today", __LINE__);
    pod_text_date(now - 86400, now, out, sizeof out);
    expect_str("yesterday", out, "Yesterday", __LINE__);
    pod_text_date(now - 3 * 86400, now, out, sizeof out);
    expect_str("days ago", out, "3 days ago", __LINE__);
    pod_text_date(1791887402 - 30 * 86400, now, out, sizeof out);
    expect_str("this year", out, "13 Sep", __LINE__);
    pod_text_date(1613713388, now, out, sizeof out);
    expect_str("other year", out, "19 Feb 2021", __LINE__);
}

static void test_size(void)
{
    char out[32];
    pod_text_size(512000, out, sizeof out);
    expect_str("KB", out, "512 KB", __LINE__);
    pod_text_size(29624511, out, sizeof out);
    expect_str("MB", out, "30 MB", __LINE__);
    pod_text_size(1234567890, out, sizeof out);
    expect_str("GB", out, "1.2 GB", __LINE__);
}

static void test_url_encode(void)
{
    char out[64];
    if (!pod_text_url_encode("no agenda", out, sizeof out)) s_failures++;
    expect_str("space", out, "no%20agenda", __LINE__);
    pod_text_url_encode("caf\xC3\xA9 & r\xC3\xA9sum\xC3\xA9?a=b/c", out, sizeof out);
    expect_str("utf8", out, "caf%C3%A9%20%26%20r%C3%A9sum%C3%A9%3Fa%3Db%2Fc", __LINE__);
    pod_text_url_encode("Az09-_.~", out, sizeof out);
    expect_str("unreserved", out, "Az09-_.~", __LINE__);
    char small[6];
    if (pod_text_url_encode("a b c", small, sizeof small)) s_failures++;
    expect_str("too long", small, "", __LINE__);
}

int main(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    test_html();
    test_copy();
    test_clock_and_duration();
    test_date();
    test_size();
    test_url_encode();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_pod_text: all checks passed\n");
    return 0;
}
