/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — reading an EPUB as plain text, chapter by chapter.
 *
 * Its parsers are C ports of CrossPoint Reader's, Copyright (c) 2025 Dave
 * Allie; see epub.c and THIRD_PARTY_LICENSES.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct epub epub_t;

typedef struct {
    const char *title;    /* UTF-8, as the book's table of contents gives it */
    int         chapter;  /* the chapter it opens, an index into the spine */
    const char *anchor;   /* the id it points at in that chapter, or NULL for
                           * its start: one file often holds many chapters */
    int         depth;    /* 1 for a top-level entry */
} epub_toc_entry_t;

/* Opens the book at `path` and reads its index: title, language, the chapters
 * in reading order, and its table of contents. NULL if it is not an EPUB. */
epub_t *epub_open(const char *path);

void epub_close(epub_t *book);

/* "" when the book does not say. */
const char *epub_title(const epub_t *book);
const char *epub_language(const epub_t *book);

/* The chapters, in reading order: the book's spine. */
int epub_chapter_count(const epub_t *book);

/* The table of contents: empty for a book that has none, or none whose
 * entries name one of its chapters. */
int epub_toc_count(const epub_t *book);
const epub_toc_entry_t *epub_toc_get(const epub_t *book, int index);

/* The text of chapter `chapter`, which the caller frees with free(). NULL if
 * the chapter cannot be read. See epub_html_text() for the form it takes, and
 * for the anchors. */
char *epub_chapter_text(epub_t *book, int chapter, const char *const *anchors,
                        size_t anchor_count, size_t *anchor_at, size_t *len_out);

/* The readable text of an XHTML document, which the caller frees with free():
 * paragraphs separated by a blank line, list items marked with a bullet,
 * entities expanded, runs of white space collapsed to one, and nothing from
 * <head>, <script> or <style>. HTML that is not well-formed XML is still read,
 * more roughly. NULL only when out of memory.
 *
 * `anchors` are `anchor_count` element ids, as table-of-contents entries give
 * them, and `anchor_at[i]` receives where in the text the element with id
 * `anchors[i]` begins, or 0 if none has it. All of one chapter's are found in
 * the one pass. */
char *epub_html_text(const char *html, size_t len, const char *const *anchors,
                     size_t anchor_count, size_t *anchor_at, size_t *len_out);
