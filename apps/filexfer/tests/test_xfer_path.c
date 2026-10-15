// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The paths the web page sends: decoded, checked, kept inside the two roots.

#include "filexfer/xfer_path.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static void expect_str(const char *actual, const char *expected, int line)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d:\n  expected: \"%s\"\n  actual:   \"%s\"\n", __FILE__, line, expected,
            actual);
    s_failures++;
}

#define EXPECT_STR(a, e) expect_str((a), (e), __LINE__)

static void accepts(const char *in, const char *path, const char *rest, xfer_root_t root, int line)
{
    xfer_path_t p;
    if (!xfer_path_parse(in, &p)) {
        fprintf(stderr, "%s:%d: refused \"%s\"\n", __FILE__, line, in);
        s_failures++;
        return;
    }
    expect_str(p.path, path, line);
    expect_str(xfer_path_rest(&p), rest, line);
    if (p.root != root) {
        fprintf(stderr, "%s:%d: \"%s\" in root %d, not %d\n", __FILE__, line, in, p.root, root);
        s_failures++;
    }
}

static void refuses(const char *in, int line)
{
    xfer_path_t p;
    if (xfer_path_parse(in, &p)) {
        fprintf(stderr, "%s:%d: accepted \"%s\" as \"%s\"\n", __FILE__, line, in, p.path);
        s_failures++;
    }
}

#define ACCEPTS(in, path, rest, root) accepts((in), (path), (rest), (root), __LINE__)
#define REFUSES(in) refuses((in), __LINE__)

static void test_parse(void)
{
    ACCEPTS("/", "/", "", XFER_ROOT_NONE);
    ACCEPTS("//", "/", "", XFER_ROOT_NONE);
    ACCEPTS("/sdcard", "/sdcard", "", XFER_ROOT_SDCARD);
    ACCEPTS("/sdcard/", "/sdcard", "", XFER_ROOT_SDCARD);
    ACCEPTS("/sdcard/notes/todo.txt", "/sdcard/notes/todo.txt", "/notes/todo.txt", XFER_ROOT_SDCARD);
    ACCEPTS("//sdcard//notes///todo.txt/", "/sdcard/notes/todo.txt", "/notes/todo.txt",
            XFER_ROOT_SDCARD);
    ACCEPTS("/spiffs/config/system.json", "/spiffs/config/system.json", "/config/system.json",
            XFER_ROOT_SPIFFS);
    ACCEPTS("/sdcard/My Books/Ünïcödé.epub", "/sdcard/My Books/Ünïcödé.epub",
            "/My Books/Ünïcödé.epub", XFER_ROOT_SDCARD);
    ACCEPTS("/sdcard/.hidden", "/sdcard/.hidden", "/.hidden", XFER_ROOT_SDCARD);
    ACCEPTS("/sdcard/a..b", "/sdcard/a..b", "/a..b", XFER_ROOT_SDCARD);

    /* Out of the roots, or not a path at all */
    REFUSES("");
    REFUSES("sdcard/a.txt");
    REFUSES("/etc/passwd");
    REFUSES("/sdcardx/a.txt");
    REFUSES("/SDCARD/a.txt");
    REFUSES("/data/a.txt");

    /* Climbing out */
    REFUSES("/sdcard/..");
    REFUSES("/sdcard/../spiffs/x");
    REFUSES("/sdcard/a/../../etc");
    REFUSES("/sdcard/./a");
    REFUSES("/..");

    /* Characters FAT will not take, or would take as something else */
    REFUSES("/sdcard/a\\b");
    REFUSES("/sdcard/a:b");
    REFUSES("/sdcard/a*");
    REFUSES("/sdcard/what?");
    REFUSES("/sdcard/\"q\"");
    REFUSES("/sdcard/a<b>");
    REFUSES("/sdcard/a|b");
    REFUSES("/sdcard/line\nbreak");
    REFUSES("/sdcard/tab\there");
    REFUSES("/sdcard/del\x7f");
    REFUSES("/sdcard/trailing.");
    REFUSES("/sdcard/trailing ");

    /* Too long: past SPIFFS's name, or past any path */
    /* SPIFFS's name has the leading '/': 31 bytes in all. */
    ACCEPTS("/spiffs/012345678901234567890123456789", "/spiffs/012345678901234567890123456789",
            "/012345678901234567890123456789", XFER_ROOT_SPIFFS);
    REFUSES("/spiffs/0123456789012345678901234567890");
    char longest[XFER_PATH_MAX + 8];
    strcpy(longest, "/sdcard/");
    while (strlen(longest) < XFER_PATH_MAX - 1) strcat(longest, "a");
    xfer_path_t p;
    CHECK(xfer_path_parse(longest, &p));
    strcat(longest, "a");
    CHECK(!xfer_path_parse(longest, &p));
}

static void test_decode(void)
{
    char out[64];
    CHECK(xfer_url_decode("%2Fsdcard%2Fa%20b.txt", out, sizeof out));
    EXPECT_STR(out, "/sdcard/a b.txt");
    CHECK(xfer_url_decode("/sdcard/a+b", out, sizeof out));
    EXPECT_STR(out, "/sdcard/a b");
    CHECK(xfer_url_decode("%C3%BC%c3%bc", out, sizeof out));
    EXPECT_STR(out, "üü");
    CHECK(xfer_url_decode("", out, sizeof out));
    EXPECT_STR(out, "");
    CHECK(!xfer_url_decode("%", out, sizeof out));
    CHECK(!xfer_url_decode("%4", out, sizeof out));
    CHECK(!xfer_url_decode("%zz", out, sizeof out));
    CHECK(!xfer_url_decode("a%00b", out, sizeof out));
    CHECK(!xfer_url_decode("abcdef", out, 6));
    CHECK(xfer_url_decode("abcde", out, 6));

    /* An encoded climb is still a climb once decoded. */
    xfer_path_t p;
    CHECK(xfer_url_decode("%2Fsdcard%2F%2E%2E%2Fspiffs", out, sizeof out));
    CHECK(!xfer_path_parse(out, &p));
}

static void test_names(void)
{
    CHECK(xfer_name_ok("todo.txt"));
    CHECK(xfer_name_ok("A folder"));
    CHECK(!xfer_name_ok(""));
    CHECK(!xfer_name_ok("."));
    CHECK(!xfer_name_ok(".."));
    CHECK(!xfer_name_ok("a/b"));
    CHECK(!xfer_name_ok("a\\b"));
    CHECK(!xfer_name_ok("name."));
    EXPECT_STR(xfer_basename("/sdcard/notes/a.txt"), "a.txt");
    EXPECT_STR(xfer_basename("/sdcard"), "sdcard");
    EXPECT_STR(xfer_root_name(XFER_ROOT_SPIFFS), "spiffs");
}

static void test_content_type(void)
{
    EXPECT_STR(xfer_content_type("a.txt"), "text/plain; charset=utf-8");
    EXPECT_STR(xfer_content_type("board.JSON"), "text/plain; charset=utf-8");
    /* Shown, never run */
    EXPECT_STR(xfer_content_type("page.html"), "text/plain; charset=utf-8");
    EXPECT_STR(xfer_content_type("app.js"), "text/plain; charset=utf-8");
    EXPECT_STR(xfer_content_type("photo.JPG"), "image/jpeg");
    EXPECT_STR(xfer_content_type("logo.svg"), "application/octet-stream");
    EXPECT_STR(xfer_content_type("app.elf"), "application/octet-stream");
    EXPECT_STR(xfer_content_type("README"), "application/octet-stream");
    EXPECT_STR(xfer_content_type(".txt"), "application/octet-stream");
}

static void test_disposition(void)
{
    char out[160];
    CHECK(xfer_content_disposition("a.txt", out, sizeof out));
    EXPECT_STR(out, "attachment; filename=\"a.txt\"; filename*=UTF-8''a.txt");
    CHECK(xfer_content_disposition("my \"quoted\" file.txt", out, sizeof out));
    EXPECT_STR(out, "attachment; filename=\"my _quoted_ file.txt\"; "
                    "filename*=UTF-8''my%20%22quoted%22%20file.txt");
    CHECK(xfer_content_disposition("Ü.txt", out, sizeof out));
    EXPECT_STR(out, "attachment; filename=\"__.txt\"; filename*=UTF-8''%C3%9C.txt");
    CHECK(!xfer_content_disposition("a-rather-long-name.txt", out, 40));
}

static void test_json(void)
{
    char out[64];
    CHECK(xfer_json_escape("plain", out, sizeof out));
    EXPECT_STR(out, "plain");
    CHECK(xfer_json_escape("a\"b\\c\nd\te\x01", out, sizeof out));
    EXPECT_STR(out, "a\\\"b\\\\c\\nd\\te\\u0001");
    CHECK(xfer_json_escape("ü", out, sizeof out));
    EXPECT_STR(out, "ü");
    CHECK(!xfer_json_escape("abcd", out, 4));
    CHECK(xfer_json_escape("abc", out, 4));
    CHECK(!xfer_json_escape("\x01", out, 6));
}

int main(void)
{
    test_parse();
    test_decode();
    test_names();
    test_content_type();
    test_disposition();
    test_json();
    if (s_failures) {
        fprintf(stderr, "test_xfer_path: %d failed\n", s_failures);
        return 1;
    }
    printf("test_xfer_path: all passed\n");
    return 0;
}
