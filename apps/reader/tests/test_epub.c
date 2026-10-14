// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) ThistleOS contributors
//
// The Reader's EPUB parsing, against the books make_fixtures.py writes:
//     test_epub FIXTURE_DIR

#include "reader/epub.h"
#include "reader/html_entities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_dir;
static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            s_failures++;                                                      \
        }                                                                      \
    } while (0)

static void print_escaped(const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\n') {
            fputs("\\n", stderr);
        } else {
            fputc(*s, stderr);
        }
    }
}

static void check_str(const char *actual, const char *expected, const char *what, int line)
{
    if (actual && strcmp(actual, expected) == 0) return;
    fprintf(stderr, "%s:%d: %s\n  expected: \"", __FILE__, line, what);
    print_escaped(expected);
    fprintf(stderr, "\"\n  actual:   \"");
    print_escaped(actual ? actual : "(null)");
    fprintf(stderr, "\"\n");
    s_failures++;
}

#define CHECK_STR(actual, expected) check_str((actual), (expected), #actual, __LINE__)

static epub_t *open_fixture(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    return epub_open(path);
}

/* Chapter `n`'s text, checked against `expected`. */
static void check_chapter(epub_t *book, int n, const char *expected, int line)
{
    size_t len = 0;
    char *text = epub_chapter_text(book, n, NULL, 0, NULL, &len);
    check_str(text, expected, "chapter text", line);
    if (text) CHECK(len == strlen(text));
    free(text);
}

#define CHECK_CHAPTER(book, n, expected) check_chapter((book), (n), (expected), __LINE__)

static const char CH1_TEXT[] =
    "Chapter One\n\n"
    "It was a dark and stormy night\xE2\x80\x94or so they said & wrote.\n\n"
    "\xE2\x80\x9CQuotes\xE2\x80\x9D and it\xE2\x80\x99s fine.\n\n"
    "Line one\nline two\n\n"
    "\xE2\x80\xA2 First\n\n"
    "\xE2\x80\xA2 Second\n\n"
    "a b\n\n"
    "softhyphen";

static void test_epub2_index_and_contents(void)
{
    epub_t *book = open_fixture("epub2.epub");
    CHECK(book != NULL);
    if (!book) return;

    /* Its first title, white space collapsed; the second is a subtitle. */
    CHECK_STR(epub_title(book), "A Test Book");
    CHECK_STR(epub_language(book), "en");
    /* The first of its creators, likewise. */
    CHECK_STR(epub_author(book), "Ann Author");

    /* The spine without the image and the idref the manifest lacks. */
    CHECK(epub_chapter_count(book) == 3);

    /* The NCX, nested entry and all, without the entry for a missing file. */
    CHECK(epub_toc_count(book) == 4);
    static const struct { const char *title; int chapter; const char *anchor; int depth; } TOC[] = {
        {"Cover", 0, NULL, 1}, {"Chapter One", 1, "start", 1},
        {"A Section", 1, "s2", 2}, {"Chapter Two", 2, NULL, 1},
    };
    for (int i = 0; i < 4 && i < epub_toc_count(book); i++) {
        const epub_toc_entry_t *e = epub_toc_get(book, i);
        CHECK_STR(e->title, TOC[i].title);
        CHECK(e->chapter == TOC[i].chapter);
        if (TOC[i].anchor) {
            CHECK_STR(e->anchor, TOC[i].anchor);
        } else {
            CHECK(e->anchor == NULL);
        }
        CHECK(e->depth == TOC[i].depth);
    }
    CHECK(epub_toc_get(book, 4) == NULL);
    CHECK(epub_toc_get(book, -1) == NULL);
    epub_close(book);
}

static void test_epub2_chapter_text(void)
{
    epub_t *book = open_fixture("epub2.epub");
    CHECK(book != NULL);
    if (!book) return;

    /* A cover page is an image and nothing else. */
    CHECK_CHAPTER(book, 0, "");
    CHECK_CHAPTER(book, 1, CH1_TEXT);
    /* Named in the manifest as "Chapter%202.xhtml". */
    CHECK_CHAPTER(book, 2, "The second chapter.");
    CHECK(epub_chapter_text(book, 3, NULL, 0, NULL, NULL) == NULL);
    CHECK(epub_chapter_text(book, -1, NULL, 0, NULL, NULL) == NULL);
    epub_close(book);
}

static void test_stored_archive_with_comment(void)
{
    epub_t *book = open_fixture("stored.epub");
    CHECK(book != NULL);
    if (!book) return;
    CHECK_STR(epub_title(book), "A Test Book");
    CHECK_CHAPTER(book, 1, CH1_TEXT);
    epub_close(book);
}

static void test_epub3_arabic_with_nav(void)
{
    epub_t *book = open_fixture("epub3_ar.epub");
    CHECK(book != NULL);
    if (!book) return;

    CHECK_STR(epub_title(book), "\xD9\x83\xD8\xAA\xD8\xA7\xD8\xA8 \xD8\xA7\xD9\x84\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1");
    CHECK_STR(epub_language(book), "ar");
    CHECK_STR(epub_author(book), "");
    CHECK(epub_chapter_count(book) == 2);

    /* The toc nav, not the landmarks; text in a <span> inside the link counts. */
    CHECK(epub_toc_count(book) == 2);
    if (epub_toc_count(book) == 2) {
        CHECK_STR(epub_toc_get(book, 0)->title,
                  "\xD8\xA7\xD9\x84\xD9\x81\xD8\xB5\xD9\x84 \xD8\xA7\xD9\x84\xD8\xA3\xD9\x88\xD9\x84");
        CHECK(epub_toc_get(book, 0)->chapter == 0);
        CHECK(epub_toc_get(book, 1)->chapter == 1);
    }

    /* &nbsp; with no DTD to excuse it: Expat refuses the document, and the
     * fallback reads it. */
    CHECK_CHAPTER(book, 0,
                  "\xD8\xA7\xD9\x84\xD9\x81\xD8\xB5\xD9\x84 \xD8\xA7\xD9\x84\xD8\xA3\xD9\x88\xD9\x84\n\n"
                  "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85.\n\n"
                  "\xD8\xB3\xD8\xB7\xD8\xB1 \xD8\xAB\xD8\xA7\xD9\x86.");
    /* Listed in the manifest as text/../text/ch2.xhtml. */
    CHECK_CHAPTER(book, 1, "\xD9\x86\xD9\x87\xD8\xA7\xD9\x8A\xD8\xA9");
    epub_close(book);
}

/* How far through the book a place is, each chapter weighed by its
 * document's size: the cover is a few lines of markup, chapter one most of
 * the book. */
static void test_position_in_book(void)
{
    epub_t *book = open_fixture("epub2.epub");
    CHECK(book != NULL);
    if (!book) return;

    CHECK(epub_permille(book, 0, 0, 100) == 0);
    CHECK(epub_permille(book, 2, 100, 100) == 1000);

    /* A chapter ends where the next begins, whatever its text's length. */
    int ch1 = epub_permille(book, 1, 0, 70);
    int ch2 = epub_permille(book, 2, 0, 30);
    CHECK(epub_permille(book, 0, 50, 50) == ch1);
    CHECK(epub_permille(book, 1, 70, 70) == ch2);
    CHECK(ch1 > 0 && ch1 < ch2 && ch2 < 1000);
    CHECK(ch1 < ch2 - ch1);

    int quarter = epub_permille(book, 1, 25, 100);
    int three_quarters = epub_permille(book, 1, 75, 100);
    CHECK(ch1 < quarter && quarter < three_quarters && three_quarters < ch2);

    /* Places that are not in the book are held to it. */
    CHECK(epub_permille(book, 1, 200, 100) == ch2);
    CHECK(epub_permille(book, 1, 5, 0) == ch1);
    CHECK(epub_permille(book, -1, 0, 1) == 0);
    CHECK(epub_permille(book, 3, 0, 1) == 1000);
    CHECK(epub_permille(NULL, 1, 0, 1) == 0);
    epub_close(book);
}

static void test_html_that_is_not_xml(void)
{
    epub_t *book = open_fixture("broken_html.epub");
    CHECK(book != NULL);
    if (!book) return;
    CHECK(epub_toc_count(book) == 0);
    CHECK_CHAPTER(book, 1,
                  "Unclosed paragraph\nnext line & more 'quoted' \xC3\xA9\n\n"
                  "Second &bogus; paragraph\n\n"
                  "Last");
    epub_close(book);
}

static void test_broken_archives(void)
{
    CHECK(open_fixture("not_zip.epub") == NULL);
    CHECK(open_fixture("truncated.epub") == NULL);
    CHECK(open_fixture("no_such_file.epub") == NULL);

    /* The index is intact, so the book opens; the scrambled chapter does not
     * read, and the others still do. */
    epub_t *book = open_fixture("corrupt_deflate.epub");
    CHECK(book != NULL);
    if (!book) return;
    CHECK(epub_chapter_text(book, 1, NULL, 0, NULL, NULL) == NULL);
    CHECK_CHAPTER(book, 2, "The second chapter.");
    epub_close(book);
}

static void test_html_text_directly(void)
{
    static const char HTML[] = "<p>a<b>b</b>c</p><p>  </p><h2>T</h2>";
    size_t len = 0;
    char *text = epub_html_text(HTML, sizeof(HTML) - 1, NULL, 0, NULL, &len);
    CHECK_STR(text, "abc\n\nT");
    free(text);

    text = epub_html_text("", 0, NULL, 0, NULL, &len);
    CHECK_STR(text, "");
    CHECK(len == 0);
    free(text);

    /* A drop cap, as Project Gutenberg's Pride and Prejudice sets one; an
     * image described at length is not part of the text. */
    static const char IMAGES[] =
        "<div><p><span>\n<img alt=\"N\" src=\"n.png\"/></span>OT all that</p>"
        "<p>A<img alt=\"a picture of a bear\" src=\"b.png\"/>B<img src=\"c.png\"/>C</p></div>";
    text = epub_html_text(IMAGES, sizeof(IMAGES) - 1, NULL, 0, NULL, NULL);
    CHECK_STR(text, "NOT all that\n\nABC");
    free(text);
}

static size_t anchor_in_chapter(epub_t *book, int chapter, const char *anchor)
{
    size_t at = 12345;
    char *text = epub_chapter_text(book, chapter, &anchor, 1, &at, NULL);
    CHECK(text != NULL);
    free(text);
    return at;
}

static size_t anchor_in_html(const char *html, const char *anchor, const char *expect_text)
{
    size_t at = 12345;
    char *text = epub_html_text(html, strlen(html), &anchor, 1, &at, NULL);
    check_str(text, expect_text, "html text", __LINE__);
    free(text);
    return at;
}

static void test_anchors(void)
{
    epub_t *book = open_fixture("epub2.epub");
    CHECK(book != NULL);
    if (book) {
        /* "A Section" points into chapter one, at its last paragraph. */
        CHECK(anchor_in_chapter(book, 1, "s2") == strlen(CH1_TEXT) - strlen("softhyphen"));
        CHECK(anchor_in_chapter(book, 1, "start") == 0);
        CHECK(anchor_in_chapter(book, 1, "no-such-id") == 0);

        /* All of a chapter's at once, as the Reader asks when it loads one. */
        const char *anchors[] = {"s2", "no-such-id", "start", "s2"};
        size_t at[4] = {9, 9, 9, 9};
        char *text = epub_chapter_text(book, 1, anchors, 4, at, NULL);
        CHECK(text != NULL);
        CHECK(at[0] == strlen(CH1_TEXT) - strlen("softhyphen"));
        CHECK(at[1] == 0);
        CHECK(at[2] == 0);
        CHECK(at[3] == at[0]);
        free(text);
        epub_close(book);
    }

    /* Before a list item's bullet, and an <a name=""> as older books mark one. */
    CHECK(anchor_in_html("<div><p>One</p><ul><li id=\"b\">Two</li></ul></div>", "b",
                         "One\n\n\xE2\x80\xA2 Two") == 5);
    CHECK(anchor_in_html("<div><p>One</p><p><a name=\"n\"/>Two</p></div>", "n",
                         "One\n\nTwo") == 5);
    /* At the end when nothing follows it. */
    CHECK(anchor_in_html("<div><p>One</p><p id=\"e\"/></div>", "e", "One") == 3);
    /* And in HTML only the fallback reads. */
    CHECK(anchor_in_html("<p>One<br><div class=x id='end'>Last</div>", "end",
                         "One\n\nLast") == 5);
}

static void test_entity_table_is_sorted(void)
{
    for (size_t i = 1; i < html_entity_count(); i++) {
        if (strcmp(html_entity_key(i - 1), html_entity_key(i)) >= 0) {
            fprintf(stderr, "entity table out of order at %s, %s\n",
                    html_entity_key(i - 1), html_entity_key(i));
            s_failures++;
        }
    }
    CHECK_STR(html_entity_lookup("&mdash;", 7), "\xE2\x80\x94");
    CHECK(html_entity_lookup("&mdash", 6) == NULL);
    CHECK(html_entity_lookup("&nosuch;", 8) == NULL);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
        return 2;
    }
    s_dir = argv[1];

    test_epub2_index_and_contents();
    test_epub2_chapter_text();
    test_stored_archive_with_comment();
    test_epub3_arabic_with_nav();
    test_position_in_book();
    test_html_that_is_not_xml();
    test_broken_archives();
    test_html_text_directly();
    test_anchors();
    test_entity_table_is_sorted();

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_epub: all checks passed\n");
    return 0;
}
