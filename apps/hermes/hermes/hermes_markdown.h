/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's Markdown as blocks and styled spans.
 *
 * hermes_esp32's hermes_markdown component, as it is there, over md4c 0.5.2
 * (third_party/md4c/): the bridge splits a long reply into parts that are
 * each valid Markdown for this parser (its ADR 0051), and parsing them the
 * same way is what keeps the two devices drawing the same thing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERMES_MARKDOWN_TEXT_BYTES 1024
#define HERMES_MARKDOWN_BLOCKS 16
/* GFM tables turn each cell into its own span group, so a modest table
   exhausts the old span bound of 48 before any surrounding prose is counted.
   The grown figure is one shared render-time buffer rather than a per-message
   cost, so it does not multiply across the Device Transcript. See ADR 0050
   and `docs/evidence/` for the measured cost. */
#define HERMES_MARKDOWN_SPANS 128
#define HERMES_MARKDOWN_TABLE_ROWS 16
#define HERMES_MARKDOWN_TABLE_CELLS 96

typedef enum {
    HERMES_MARKDOWN_BLOCK_PARAGRAPH,
    HERMES_MARKDOWN_BLOCK_HEADING,
    HERMES_MARKDOWN_BLOCK_LIST_ITEM,
    HERMES_MARKDOWN_BLOCK_CODE,
    HERMES_MARKDOWN_BLOCK_QUOTE,
    HERMES_MARKDOWN_BLOCK_TABLE,
    /* A thematic break. It carries no text at all, so it is the one kind
       whose block has neither spans nor rows -- the renderer draws it from
       the kind alone. */
    HERMES_MARKDOWN_BLOCK_RULE,
} hermes_markdown_block_kind_t;

/* The integer values match md4c's ``MD_ALIGN`` so a cell's alignment is
   stored as the parser reports it, without a translation layer. */
typedef enum {
    HERMES_MARKDOWN_ALIGN_DEFAULT = 0,
    HERMES_MARKDOWN_ALIGN_LEFT,
    HERMES_MARKDOWN_ALIGN_CENTER,
    HERMES_MARKDOWN_ALIGN_RIGHT,
} hermes_markdown_align_t;

typedef enum {
    HERMES_MARKDOWN_STYLE_PLAIN = 0,
    HERMES_MARKDOWN_STYLE_STRONG = 1u << 0,
    HERMES_MARKDOWN_STYLE_EMPHASIS = 1u << 1,
    HERMES_MARKDOWN_STYLE_CODE = 1u << 2,
    HERMES_MARKDOWN_STYLE_LINK = 1u << 3,
    HERMES_MARKDOWN_STYLE_STRIKETHROUGH = 1u << 4,
} hermes_markdown_style_t;

typedef struct {
    uint16_t offset;
    uint16_t length;
    uint8_t style;
} hermes_markdown_span_t;

typedef struct {
    uint16_t length;
    uint16_t ordinal;
    uint8_t first_span;
    uint8_t span_count;
    uint8_t kind;
    uint8_t heading_level;
    uint8_t list_depth;
    bool ordered;
    /* Only a ``HERMES_MARKDOWN_BLOCK_TABLE`` block uses these: the range of
       rows in ``document->rows``. A table's own displayable text lives in
       its cells, so its ``first_span`` and ``span_count`` are unused. */
    uint8_t first_row;
    uint8_t row_count;
} hermes_markdown_block_t;

/* One row of a table: a contiguous range of cells in ``document->cells``,
   and whether the row belongs to the header section. */
typedef struct {
    uint8_t first_cell;
    uint8_t cell_count;
    bool header;
} hermes_markdown_row_t;

/* One cell of a table. The cell is a span group in its own right -- it uses
   the same ``spans`` array and the same styling machinery as a paragraph --
   so inline emphasis, per-block typeface selection, base direction and the
   Face Chain all apply to a cell for free. ``align`` is the column's declared
   alignment and ``header`` distinguishes the header row from a body row. */
typedef struct {
    uint8_t first_span;
    uint8_t span_count;
    uint8_t align;
    bool header;
} hermes_markdown_cell_t;

typedef struct {
    char text[HERMES_MARKDOWN_TEXT_BYTES];
    hermes_markdown_block_t blocks[HERMES_MARKDOWN_BLOCKS];
    hermes_markdown_span_t spans[HERMES_MARKDOWN_SPANS];
    hermes_markdown_row_t rows[HERMES_MARKDOWN_TABLE_ROWS];
    hermes_markdown_cell_t cells[HERMES_MARKDOWN_TABLE_CELLS];
    uint16_t text_length;
    uint8_t block_count;
    uint8_t span_count;
    uint8_t row_count;
    uint8_t cell_count;
    bool truncated;
} hermes_markdown_document_t;

void hermes_markdown_initialize(hermes_markdown_document_t *document);

/**
 * Parse bounded Markdown into displayable blocks and styled spans.
 *
 * Content beyond the document bounds is dropped and reported through
 * ``truncated`` rather than failing, so long streaming replies keep
 * rendering. Raw HTML never reaches the document.
 */
bool hermes_markdown_parse(
    const char *text,
    size_t length,
    hermes_markdown_document_t *document
);

/**
 * Every span is an independently displayable NUL-terminated string; a
 * block's displayable text is the concatenation of its spans.
 */
const char *hermes_markdown_span_text(
    const hermes_markdown_document_t *document,
    const hermes_markdown_span_t *span
);
