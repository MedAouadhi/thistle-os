/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Hermes's Markdown as blocks and styled spans. See
 * hermes_markdown.h.
 */
#include "hermes/hermes_markdown.h"

#include <string.h>

#include "third_party/md4c/md4c.h"

#define MARKDOWN_LIST_LEVELS 4
#define MARKDOWN_HREF_BYTES 96

/* Ordered to match the public HERMES_MARKDOWN_STYLE_* bit positions. */
typedef enum {
    MARKDOWN_STYLE_KIND_STRONG,
    MARKDOWN_STYLE_KIND_EMPHASIS,
    MARKDOWN_STYLE_KIND_CODE,
    MARKDOWN_STYLE_KIND_LINK,
    MARKDOWN_STYLE_KIND_STRIKETHROUGH,
    MARKDOWN_STYLE_KINDS,
} markdown_style_kind_t;

typedef struct {
    hermes_markdown_document_t *document;
    uint8_t style_depth[MARKDOWN_STYLE_KINDS];
    uint8_t style;
    uint16_t list_ordinal[MARKDOWN_LIST_LEVELS];
    bool list_ordered[MARKDOWN_LIST_LEVELS];
    uint8_t list_depth;
    uint8_t untracked_lists;
    uint8_t quote_depth;
    bool span_open;
    uint16_t link_text_offset;
    char link_href[MARKDOWN_HREF_BYTES];
    /* The active span accumulator. A paragraph, heading, list item, code or
       quote block opens one of these, and so does each table cell; the two
       paths share the span and style machinery because a cell is a span
       group in its own right. Exactly one of ``block`` and ``cell`` is
       non-NULL while text is being accumulated. */
    hermes_markdown_block_t *block;
    hermes_markdown_cell_t *cell;
    /* The table currently being entered, set on MD_BLOCK_TABLE and finalised
       on its leave. The header flag is toggled by MD_BLOCK_THEAD/TBODY. */
    hermes_markdown_block_t *table;
    uint8_t table_first_row;
    bool table_in_header;
    /* The row currently being filled. Cells are only created while a row is
       open, so a row that exceeded the row bound cannot leave orphan cells
       misattributed to the last row that did fit. */
    hermes_markdown_row_t *row;
} markdown_context_t;

static uint8_t style_bit(markdown_style_kind_t kind)
{
    return (uint8_t)(1u << (unsigned)kind);
}

static void push_style(
    markdown_context_t *context,
    markdown_style_kind_t kind
)
{
    if (context->style_depth[kind] < UINT8_MAX) {
        context->style_depth[kind]++;
    }
    context->style |= style_bit(kind);
}

static void pop_style(
    markdown_context_t *context,
    markdown_style_kind_t kind
)
{
    if (context->style_depth[kind] == 0) {
        return;
    }
    context->style_depth[kind]--;
    if (context->style_depth[kind] == 0) {
        context->style &= (uint8_t)~style_bit(kind);
    }
}

static size_t utf8_bounded_length(const char *text, size_t length)
{
    while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80) {
        length--;
    }
    return length;
}

/* Forward declarations: blocks and cells are each other's cleanup target,
    so open_block closes a cell before opening a block and vice versa. */
static void close_block(markdown_context_t *context);
static void close_cell(markdown_context_t *context);

/* Reserve one byte so every span can be NUL-terminated in the arena. */
static size_t remaining_text_bytes(const markdown_context_t *context)
{
    const hermes_markdown_document_t *document = context->document;
    size_t used = document->text_length;
    return used + 1 < HERMES_MARKDOWN_TEXT_BYTES
        ? HERMES_MARKDOWN_TEXT_BYTES - 1 - used
        : 0;
}

static void close_span(markdown_context_t *context)
{
    hermes_markdown_document_t *document = context->document;
    if (!context->span_open) {
        return;
    }
    if (document->text_length < HERMES_MARKDOWN_TEXT_BYTES) {
        document->text[document->text_length++] = '\0';
    }
    context->span_open = false;
}

static void close_block(markdown_context_t *context)
{
    if (context->block == NULL) {
        return;
    }
    close_span(context);
    /* An empty block (one md4c opened but no text arrived in) is taken back
       out of the document so the renderer never sees it. A table is kept
       even though it has no own spans -- its content lives in its cells. */
    if (
        context->block->span_count == 0 &&
        context->block->kind != HERMES_MARKDOWN_BLOCK_TABLE
    ) {
        context->document->block_count--;
    }
    context->block = NULL;
}

static void open_block(
    markdown_context_t *context,
    hermes_markdown_block_kind_t kind,
    uint8_t heading_level,
    uint16_t ordinal
)
{
    close_cell(context);
    close_block(context);
    hermes_markdown_document_t *document = context->document;
    if (document->block_count == HERMES_MARKDOWN_BLOCKS) {
        document->truncated = true;
        return;
    }
    hermes_markdown_block_t *block =
        &document->blocks[document->block_count++];
    memset(block, 0, sizeof(*block));
    block->kind = (uint8_t)kind;
    block->heading_level = heading_level;
    block->ordinal = ordinal;
    block->list_depth = context->list_depth;
    block->ordered = context->list_depth > 0 &&
        context->list_ordered[context->list_depth - 1];
    block->first_span = document->span_count;
    context->block = block;
}

static void close_cell(markdown_context_t *context)
{
    if (context->cell == NULL) {
        return;
    }
    close_span(context);
    /* A cell is kept even when it is empty, because its position in the row
       is structural: a blank cell still occupies its column in the grid. */
    context->cell = NULL;
}

static void open_cell(
    markdown_context_t *context,
    hermes_markdown_align_t align,
    bool header
)
{
    close_cell(context);
    if (context->row == NULL) {
        /* The row itself was over capacity; do not leak a cell with no row
           to belong to. */
        context->document->truncated = true;
        return;
    }
    hermes_markdown_document_t *document = context->document;
    if (document->cell_count == HERMES_MARKDOWN_TABLE_CELLS) {
        document->truncated = true;
        return;
    }
    hermes_markdown_cell_t *cell =
        &document->cells[document->cell_count++];
    memset(cell, 0, sizeof(*cell));
    cell->first_span = document->span_count;
    cell->align = (uint8_t)align;
    cell->header = header;
    context->cell = cell;
}

static void open_row(markdown_context_t *context, bool header)
{
    hermes_markdown_document_t *document = context->document;
    if (document->row_count == HERMES_MARKDOWN_TABLE_ROWS) {
        document->truncated = true;
        context->row = NULL;
        return;
    }
    hermes_markdown_row_t *row = &document->rows[document->row_count++];
    memset(row, 0, sizeof(*row));
    row->first_cell = document->cell_count;
    row->header = header;
    context->row = row;
}

static void close_row(markdown_context_t *context)
{
    hermes_markdown_document_t *document = context->document;
    if (context->row == NULL) {
        return;
    }
    hermes_markdown_row_t *row = context->row;
    context->row = NULL;
    /* A row whose every cell was over capacity has no cells at all -- take
       it back out rather than leave an empty row in the table. */
    if (row->cell_count == 0 && document->cell_count == row->first_cell) {
        document->row_count--;
        return;
    }
    row->cell_count = (uint8_t)(document->cell_count - row->first_cell);
}

static void append_text(
    markdown_context_t *context,
    const char *text,
    size_t length
)
{
    hermes_markdown_document_t *document = context->document;
    if (length == 0) {
        return;
    }
    /* The active span group: a block's, unless a table cell is open, in which
       case the cell's. The two paths are the same machinery :- a span group
       is a contiguous run of equally-styled spans, and a cell is a span
       group in its own right. */
    hermes_markdown_block_t *block = context->cell != NULL
        ? NULL
        : context->block;
    hermes_markdown_cell_t *cell = context->cell;
    if (block == NULL && cell == NULL) {
        return;
    }
    if (
        context->span_open &&
        document->spans[document->span_count - 1].style != context->style
    ) {
        close_span(context);
    }

    size_t available = remaining_text_bytes(context);
    size_t writable = length <= available
        ? length
        : utf8_bounded_length(text, available);
    if (writable < length) {
        document->truncated = true;
    }
    if (writable == 0) {
        return;
    }
    if (!context->span_open) {
        if (document->span_count == HERMES_MARKDOWN_SPANS) {
            document->truncated = true;
            return;
        }
        hermes_markdown_span_t *span =
            &document->spans[document->span_count++];
        span->offset = document->text_length;
        span->length = 0;
        span->style = context->style;
        if (block != NULL) {
            block->span_count++;
        }
        else {
            cell->span_count++;
        }
        context->span_open = true;
    }
    memcpy(document->text + document->text_length, text, writable);
    document->text_length = (uint16_t)(document->text_length + writable);
    hermes_markdown_span_t *span =
        &document->spans[document->span_count - 1];
    span->length = (uint16_t)(span->length + writable);
    if (block != NULL) {
        block->length = (uint16_t)(block->length + writable);
    }
}

static void copy_attribute(
    const MD_ATTRIBUTE *attribute,
    char *destination,
    size_t capacity
)
{
    destination[0] = '\0';
    if (attribute == NULL || attribute->text == NULL) {
        return;
    }
    size_t length = attribute->size < capacity
        ? attribute->size
        : utf8_bounded_length(attribute->text, capacity - 1);
    memcpy(destination, attribute->text, length);
    destination[length] = '\0';
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    markdown_context_t *context = userdata;
    switch (type) {
        case MD_BLOCK_H:
            open_block(
                context,
                HERMES_MARKDOWN_BLOCK_HEADING,
                (uint8_t)((const MD_BLOCK_H_DETAIL *)detail)->level,
                0
            );
            break;
        case MD_BLOCK_CODE:
            open_block(context, HERMES_MARKDOWN_BLOCK_CODE, 0, 0);
            push_style(context, MARKDOWN_STYLE_KIND_CODE);
            break;
        case MD_BLOCK_P:
            open_block(
                context,
                context->quote_depth > 0
                    ? HERMES_MARKDOWN_BLOCK_QUOTE
                    : HERMES_MARKDOWN_BLOCK_PARAGRAPH,
                0,
                0
            );
            break;
        case MD_BLOCK_TABLE: {
            /* The table is its own block, but its displayable text lives in
               its cells rather than in the block's span group, so the block
               is not set as the active accumulator. Its row range is filled
               in when the table closes. */
            close_block(context);
            hermes_markdown_document_t *document = context->document;
            if (document->block_count == HERMES_MARKDOWN_BLOCKS) {
                document->truncated = true;
                break;
            }
            hermes_markdown_block_t *table =
                &document->blocks[document->block_count++];
            memset(table, 0, sizeof(*table));
            table->kind = (uint8_t)HERMES_MARKDOWN_BLOCK_TABLE;
            context->table = table;
            context->table_first_row = document->row_count;
            context->table_in_header = false;
            break;
        }
        case MD_BLOCK_HR: {
            /* Appended directly rather than opened as the active
               accumulator, for the same reason a table is: `close_block`
               takes an empty block back out of the document, and a rule is
               legitimately empty. Its `list_depth` stays 0 so a rule spans
               the panel rather than being indented by whatever list it
               happens to follow. */
            close_block(context);
            hermes_markdown_document_t *document = context->document;
            if (document->block_count == HERMES_MARKDOWN_BLOCKS) {
                document->truncated = true;
                break;
            }
            hermes_markdown_block_t *rule =
                &document->blocks[document->block_count++];
            memset(rule, 0, sizeof(*rule));
            rule->kind = (uint8_t)HERMES_MARKDOWN_BLOCK_RULE;
            break;
        }
        case MD_BLOCK_THEAD:
            context->table_in_header = true;
            break;
        case MD_BLOCK_TBODY:
            context->table_in_header = false;
            break;
        case MD_BLOCK_TR:
            open_row(context, context->table_in_header);
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            open_cell(
                context,
                (hermes_markdown_align_t)((const MD_BLOCK_TD_DETAIL *)
                    detail)->align,
                type == MD_BLOCK_TH
            );
            break;
        case MD_BLOCK_QUOTE:
            context->quote_depth++;
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            /* Lists deeper than the tracked levels are counted so the
               matching leave_block cannot underflow the real depth. */
            if (context->list_depth < MARKDOWN_LIST_LEVELS) {
                context->list_ordered[context->list_depth] =
                    type == MD_BLOCK_OL;
                context->list_ordinal[context->list_depth] =
                    type == MD_BLOCK_OL
                        ? (uint16_t)(
                            (const MD_BLOCK_OL_DETAIL *)detail
                          )->start
                        : 0;
                context->list_depth++;
            } else {
                context->untracked_lists++;
            }
            break;
        case MD_BLOCK_LI: {
            uint8_t level = context->list_depth > 0
                ? (uint8_t)(context->list_depth - 1)
                : 0;
            uint16_t ordinal = context->list_ordered[level]
                ? context->list_ordinal[level]++
                : 0;
            open_block(
                context,
                HERMES_MARKDOWN_BLOCK_LIST_ITEM,
                0,
                ordinal
            );
            break;
        }
        default:
            break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    (void)detail;
    markdown_context_t *context = userdata;
    switch (type) {
        case MD_BLOCK_CODE:
            pop_style(context, MARKDOWN_STYLE_KIND_CODE);
            close_block(context);
            break;
        case MD_BLOCK_H:
        case MD_BLOCK_P:
        case MD_BLOCK_LI:
            close_block(context);
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            close_cell(context);
            break;
        case MD_BLOCK_TR:
            close_cell(context);
            close_row(context);
            break;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            break;
        case MD_BLOCK_TABLE:
            close_cell(context);
            if (context->table != NULL) {
                context->table->first_row = context->table_first_row;
                context->table->row_count = (uint8_t)(
                    context->document->row_count -
                    context->table_first_row
                );
                context->table = NULL;
            }
            break;
        case MD_BLOCK_QUOTE:
            if (context->quote_depth > 0) {
                context->quote_depth--;
            }
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (context->untracked_lists > 0) {
                context->untracked_lists--;
            } else if (context->list_depth > 0) {
                context->list_depth--;
            }
            break;
        default:
            break;
    }
    return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    markdown_context_t *context = userdata;
    switch (type) {
        case MD_SPAN_STRONG:
            push_style(context, MARKDOWN_STYLE_KIND_STRONG);
            break;
        case MD_SPAN_EM:
            push_style(context, MARKDOWN_STYLE_KIND_EMPHASIS);
            break;
        case MD_SPAN_CODE:
            push_style(context, MARKDOWN_STYLE_KIND_CODE);
            break;
        case MD_SPAN_DEL:
            push_style(context, MARKDOWN_STYLE_KIND_STRIKETHROUGH);
            break;
        case MD_SPAN_A:
            copy_attribute(
                &((const MD_SPAN_A_DETAIL *)detail)->href,
                context->link_href,
                sizeof(context->link_href)
            );
            close_span(context);
            context->link_text_offset = context->document->text_length;
            push_style(context, MARKDOWN_STYLE_KIND_LINK);
            break;
        default:
            break;
    }
    return 0;
}

static bool link_href_is_redundant(const markdown_context_t *context)
{
    const hermes_markdown_document_t *document = context->document;
    size_t label_length =
        (size_t)(document->text_length - context->link_text_offset);
    return strlen(context->link_href) == label_length &&
        memcmp(
            document->text + context->link_text_offset,
            context->link_href,
            label_length
        ) == 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    (void)detail;
    markdown_context_t *context = userdata;
    switch (type) {
        case MD_SPAN_STRONG:
            pop_style(context, MARKDOWN_STYLE_KIND_STRONG);
            break;
        case MD_SPAN_EM:
            pop_style(context, MARKDOWN_STYLE_KIND_EMPHASIS);
            break;
        case MD_SPAN_CODE:
            pop_style(context, MARKDOWN_STYLE_KIND_CODE);
            break;
        case MD_SPAN_DEL:
            pop_style(context, MARKDOWN_STYLE_KIND_STRIKETHROUGH);
            break;
        case MD_SPAN_A:
            pop_style(context, MARKDOWN_STYLE_KIND_LINK);
            if (
                context->link_href[0] != '\0' &&
                !link_href_is_redundant(context)
            ) {
                append_text(context, " (", 2);
                append_text(
                    context,
                    context->link_href,
                    strlen(context->link_href)
                );
                append_text(context, ")", 1);
            }
            context->link_href[0] = '\0';
            break;
        default:
            break;
    }
    return 0;
}

static int on_text(
    MD_TEXTTYPE type,
    const MD_CHAR *text,
    MD_SIZE size,
    void *userdata
)
{
    markdown_context_t *context = userdata;
    switch (type) {
        case MD_TEXT_NORMAL:
        case MD_TEXT_ENTITY:
        case MD_TEXT_CODE:
        case MD_TEXT_LATEXMATH:
            append_text(context, text, size);
            break;
        case MD_TEXT_BR:
            append_text(context, "\n", 1);
            break;
        case MD_TEXT_SOFTBR:
            append_text(context, " ", 1);
            break;
        case MD_TEXT_NULLCHAR:
            append_text(context, "\xEF\xBF\xBD", 3);
            break;
        default:
            break;
    }
    return 0;
}

void hermes_markdown_initialize(hermes_markdown_document_t *document)
{
    if (document != NULL) {
        memset(document, 0, sizeof(*document));
    }
}

bool hermes_markdown_parse(
    const char *text,
    size_t length,
    hermes_markdown_document_t *document
)
{
    if (text == NULL || document == NULL) {
        return false;
    }
    hermes_markdown_initialize(document);
    static const MD_PARSER parser = {
        .abi_version = 0,
        .flags = MD_FLAG_NOHTML | MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES,
        .enter_block = enter_block,
        .leave_block = leave_block,
        .enter_span = enter_span,
        .leave_span = leave_span,
        .text = on_text,
    };
    markdown_context_t context;
    memset(&context, 0, sizeof(context));
    context.document = document;
    if (md_parse(text, (MD_SIZE)length, &parser, &context) != 0) {
        hermes_markdown_initialize(document);
        return false;
    }
    close_cell(&context);
    close_block(&context);
    return true;
}

const char *hermes_markdown_span_text(
    const hermes_markdown_document_t *document,
    const hermes_markdown_span_t *span
)
{
    if (
        document == NULL ||
        span == NULL ||
        span->offset >= HERMES_MARKDOWN_TEXT_BYTES
    ) {
        return "";
    }
    return document->text + span->offset;
}
