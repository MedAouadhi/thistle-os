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

typedef struct {
    uint32_t book_size;  /* the book's size in bytes; another size is another book */
    uint32_t chapter;    /* an index into an EPUB's spine; 0 for a text file */
    uint32_t offset;     /* the byte of the chapter's text the page began at */
    uint32_t text_len;   /* how long the chapter's text was */
} book_progress_t;

/* `book` is the book's path from the card's root, "/books/Moby Dick.epub",
 * which is the same on every device the card goes into. Case does not matter,
 * as on FAT. */

/* False when the book has none, or its file is damaged or another book's. */
bool book_progress_load(const char *dir, const char *book, book_progress_t *out);

/* Creates `dir` if it must. A file already there is written over in place,
 * which leaves FAT's tables alone; a write cut short is caught on loading. */
bool book_progress_save(const char *dir, const char *book, const book_progress_t *progress);
