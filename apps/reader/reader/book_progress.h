/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — where the Reader left each book.
 *
 * One small file a book, named for the book's path from the card's root, in a
 * directory of ThistleOS's own. Another reader that shares the card, such as
 * CrossPoint with its /.crosspoint, keeps its own files and never sees these.
 *
 * The place is a byte of the chapter's text rather than a page, since how many
 * pages a chapter takes depends on the screen and the font: a book left on a
 * T-Deck Pro opens at the same paragraph on a T-Deck.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How far through the book a place saved before the Reader kept it was. */
#define BOOK_PROGRESS_UNKNOWN 0xffff

typedef struct {
    uint32_t book_size;  /* the book's size in bytes; another size is another book */
    uint32_t chapter;    /* an index into an EPUB's spine; 0 for a text file */
    uint32_t offset;     /* the byte of the chapter's text the page began at */
    uint32_t text_len;   /* how long the chapter's text was */
    uint16_t permille;   /* how far through the book the page ended, 0 to 1000,
                          * or BOOK_PROGRESS_UNKNOWN; for the library to show */
} book_progress_t;

/* `book` is the book's path from the card's root, "/books/Moby Dick.epub",
 * which is the same on every device the card goes into. Case does not matter,
 * as on FAT. */

/* False when the book has none, or its file is damaged or another book's. */
bool book_progress_load(const char *dir, const char *book, book_progress_t *out);

/* Creates `dir` if it must. A file already there is written over in place,
 * which leaves FAT's tables alone; a write cut short is caught on loading. */
bool book_progress_save(const char *dir, const char *book, const book_progress_t *progress);

/* ------------------------------------------------------------------ */
/* The books opened last                                                */
/* ------------------------------------------------------------------ */

/* Newest first, in <dir>/recent.bin beside the progress files, with the title
 * and author each book gives, so the library can show a book by them without
 * opening it. */
#define BOOK_RECENT_MAX 12

typedef struct {
    char book[384];     /* its path from the card's root, as above */
    char title[128];    /* "" when it gives none, or is not an EPUB */
    char author[96];
} book_recent_t;

/* How many books were read into `list`: 0 when none were kept, or the file is
 * damaged. */
int book_recent_load(const char *dir, book_recent_t list[BOOK_RECENT_MAX]);

/* Puts `book` first, moving it up if it was kept already and dropping the
 * oldest if the list is full, and saves the list. A title or author too long
 * for it is cut short. False when `book` is too long, or the list cannot be
 * written. */
bool book_recent_put(const char *dir, const char *book, const char *title, const char *author);
