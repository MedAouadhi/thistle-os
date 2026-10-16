#include "hal/board.h"
#include "hal/sdcard_path.h"
/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — Reader app UI
 *
 * Three screens:
 *   Library   — the books read last, to go back to, then the .epub, .txt and
 *               .md files in /sdcard/books and at the top of the card
 *   Reading   — a page of text; an EPUB is read a chapter at a time
 *   Contents  — an EPUB's table of contents, or its chapters by number
 *
 * Pages are cut where LVGL's own line breaking would cut them, with the same
 * font and width as the label that shows them, so a page neither runs off
 * the bottom nor stops short. Arabic is shaped here, before that measuring:
 * see arabic_shape.c for why LVGL does not do it.
 *
 * A book opens where it was left, and the library names the books read last
 * by their own titles and says how far each has been read: see
 * book_progress.h.
 */
#include "reader/reader_app.h"
#include "reader/book_progress.h"
#include "reader/book_title.h"
#include "reader/epub.h"
#include "reader/epub_port.h"

#include "ui/arabic_shape.h"
#include "ui/manager.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "thistle/app_manager.h"

#include "lvgl.h"
#include "src/misc/lv_text_private.h" /* lv_text_get_next_line(), as the label uses */
#include "esp_log.h"
#include "esp_timer.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "reader_ui";

LV_FONT_DECLARE(reader_font_16);
LV_FONT_DECLARE(reader_font_16_mono);

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */

static int s_app_w = 240;
static int s_app_h = 296;
#define TITLE_BAR_H      30
#define FOOTER_H         24
#define PROGRESS_H        3   /* the line across the footer's top, as far as read */
#define CELL_PAD_W        8   /* a list row's left and right padding */
#define TEXT_MARGIN_H     8   /* top margin inside the text area */
#define TEXT_MARGIN_W     8   /* left/right margin */
/* DejaVu's line is 24 px at 16 px (23 in the mono font), tall enough for
 * Arabic's marks above and below; two less still clears them and fits
 * another line on a page. */
#define LINE_SPACE       (-2)

#define BOOKS_PATH      THISTLE_SDCARD "/books"
#define MAX_BOOK_FILES   64
/* The books read last that the library shows above the rest: as many as fit
 * on the T-Deck's screen, which is not as tall as the Pro's. */
#define RECENT_SHOWN      3
/* A row a book, and a heading above each of the two sections. */
#define MAX_LIBRARY_ROWS (MAX_BOOK_FILES + RECENT_SHOWN + 2)
/* A plain text file is read whole; past this it would crowd out everything
 * else in PSRAM. */
#define MAX_TEXT_FILE   (4 * 1024 * 1024)

/* ThistleOS's own, apart from any other reader's on the same card. */
#define PROGRESS_PATH   THISTLE_SDCARD "/data/reader/progress"
/* The place is saved once the pages stop turning for this long: one write for
 * a run of pages, made while the reader reads rather than as a page turns. */
#define SAVE_DELAY_MS   2000

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static struct {
    lv_obj_t *root;

    /* Library */
    lv_obj_t *library_screen;
    lv_obj_t *lib_list;          /* an lv_table, one row a book or a heading */
    struct book_entry *books;    /* the books on the card, by name */
    int       book_count;
    int16_t   row_book[MAX_LIBRARY_ROWS];  /* each row's book, or -1 for a heading */
    int       row_count;
    bool      filling_list;      /* a list's rows are being set, not chosen */

    /* Reading */
    lv_obj_t *reader_screen;
    lv_obj_t *text_label;
    lv_obj_t *footer_label;      /* where in the chapter, or the file */
    lv_obj_t *footer_far;        /* how far through the book, as a percentage */
    lv_obj_t *progress_line;     /* the same, as a line across the footer's top */
    const lv_font_t *font;       /* the font books are set in */
    int32_t   text_w;            /* the label's width, for measuring lines */
    int       lines_per_page;

    /* Contents */
    lv_obj_t *toc_screen;
    lv_obj_t *toc_list;          /* an lv_table, one row an entry */

    /* The open book: an EPUB, or a whole text file in `text` */
    epub_t   *epub;
    int       chapter;           /* the EPUB chapter `text` holds */
    int      *toc_page;          /* per contents entry: the page it begins on
                                  * in this chapter, or -1 if in another */
    char     *text;              /* shaped, NUL-terminated */
    size_t    text_len;
    size_t   *pages;             /* where each page begins, then text_len */
    int       page_count;
    int       page;

    /* Where the open book was left */
    char      book_key[384];     /* its path from the card's root; "" for none */
    uint32_t  book_size;
    book_progress_t saved;       /* its place on the card, if has_saved */
    bool      has_saved;
    lv_timer_t *save_timer;      /* paused unless a save is due */
} s_reader;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void switch_to_library(void);
static void switch_to_reading(void);
static void switch_to_contents(void);
static void open_book(const char *path);
static void show_page(int page);
static void save_progress(void);
static const char *book_key(const char *path);
static bool is_heading_row(uint32_t row);

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void format_size(off_t size, char *buf, size_t buf_len)
{
    if (size < 1024)
        snprintf(buf, buf_len, "%ld B", (long)size);
    else if (size < 1024 * 1024)
        snprintf(buf, buf_len, "%.1f KB", size / 1024.0);
    else
        snprintf(buf, buf_len, "%.1f MB", size / (1024.0 * 1024.0));
}

/* Any case: a FAT short name, as written by older tools, is upper case. */
static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static bool is_readable_ext(const char *name)
{
    return has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".epub");
}

/* The languages written right to left, by their BCP 47 primary subtag. */
static bool is_rtl_language(const char *lang)
{
    static const char *const RTL[] = {"ar", "fa", "he", "ur", "ps", "sd", "ug", "yi", "dv", "ku"};
    for (size_t i = 0; i < sizeof(RTL) / sizeof(RTL[0]); i++) {
        size_t n = strlen(RTL[i]);
        if (strncasecmp(lang, RTL[i], n) == 0 && (lang[n] == '\0' || lang[n] == '-' || lang[n] == '_')) {
            return true;
        }
    }
    return false;
}

/* Puts `obj` in the keypad's group and gives it the focus, so the keyboard
 * reaches the screen being shown. */
static void focus(lv_obj_t *obj)
{
    lv_group_t *grp = lv_group_get_default();
    if (!grp || !obj) return;
    if (lv_obj_get_group(obj) != grp) lv_group_add_obj(grp, obj);
    lv_group_focus_obj(obj);
}

/* Whether the event came from the touch panel rather than a key: Enter on a
 * focused object clicks it too, at no point on the screen. */
static bool from_touch(void)
{
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
}

/* ------------------------------------------------------------------ */
/* The text being read                                                  */
/* ------------------------------------------------------------------ */

static void free_text(void)
{
    free(s_reader.text);
    free(s_reader.pages);
    s_reader.text       = NULL;
    s_reader.pages      = NULL;
    s_reader.text_len   = 0;
    s_reader.page_count = 0;
    s_reader.page       = 0;
}

static void close_book(void)
{
    save_progress();
    free_text();
    epub_close(s_reader.epub);
    free(s_reader.toc_page);
    s_reader.epub        = NULL;
    s_reader.toc_page    = NULL;
    s_reader.chapter     = 0;
    s_reader.book_key[0] = '\0';
    s_reader.has_saved   = false;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* Cuts the text into pages: lines broken by lv_text_get_next_line() with the
 * label's font and width, as the label breaks them, lines_per_page to a page.
 * A page does not begin with the blank line between two paragraphs.
 *
 * `breaks`, sorted, are where a new page must begin: a book's chapters start
 * on a page of their own, as on paper, even when one file holds many. */
static bool paginate(const size_t *breaks, size_t break_count)
{
    const char *text = s_reader.text;
    size_t len = s_reader.text_len;
    size_t cap = 64;
    size_t *pages = epub_malloc(cap * sizeof(size_t));
    if (!pages) return false;

    int count = 0;
    int line = 0;
    size_t pos = 0;
    size_t b = 0;
    while (pos < len) {
        while (b < break_count && breaks[b] <= pos) {
            if (breaks[b] == pos && line > 0) line = 0;
            b++;
        }
        if (line == 0) {
            while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) pos++;
            if (pos >= len) break;
            if ((size_t)count + 2 > cap) {
                size_t *grown = epub_realloc(pages, cap * 2 * sizeof(size_t));
                if (!grown) {
                    free(pages);
                    return false;
                }
                pages = grown;
                cap *= 2;
            }
            pages[count++] = pos;
        }
        uint32_t n = lv_text_get_next_line(text + pos, s_reader.font, 0,
                                           s_reader.text_w, NULL, LV_TEXT_FLAG_NONE);
        /* A line runs to a break that falls inside it only for a chapter
         * starting mid-line, which the text never has; it is cut there. */
        if (b < break_count && breaks[b] > pos && breaks[b] < pos + n) n = (uint32_t)(breaks[b] - pos);
        pos += n ? n : 1;
        if (++line == s_reader.lines_per_page) line = 0;
    }
    if (count == 0) pages[count++] = 0;
    pages[count] = len;

    free(s_reader.pages);
    s_reader.pages      = pages;
    s_reader.page_count = count;
    return true;
}

/* The page holding byte `offset` of the text. */
static int page_of(size_t offset)
{
    int page = 0;
    while (page + 1 < s_reader.page_count && s_reader.pages[page + 1] <= offset) page++;
    return page;
}

/* ------------------------------------------------------------------ */
/* Where the book was left                                              */
/* ------------------------------------------------------------------ */

/* Where a place saved in the chapter now open is in its text. A chapter
 * whose text is no longer as long, as after a change to how books are read,
 * is opened as far through. */
static size_t place_in_text(const book_progress_t *left)
{
    if (left->text_len == 0 || left->text_len == s_reader.text_len) return left->offset;
    return (size_t)((uint64_t)left->offset * s_reader.text_len / left->text_len);
}

/* How far through the book the end of the page shown is, in thousandths:
 * the last page of a book is 1000. */
static uint16_t book_permille(void)
{
    size_t end = s_reader.pages[s_reader.page + 1];
    if (s_reader.epub) {
        return (uint16_t)epub_permille(s_reader.epub, s_reader.chapter, end, s_reader.text_len);
    }
    return s_reader.text_len ? (uint16_t)((uint64_t)end * 1000 / s_reader.text_len) : 1000;
}

/* Saves the open book's place: the first byte of the page shown. */
static void save_progress(void)
{
    if (s_reader.save_timer) lv_timer_pause(s_reader.save_timer);
    if (!s_reader.book_key[0] || !s_reader.text || s_reader.page_count <= 0) return;

    uint32_t chapter = s_reader.epub ? (uint32_t)s_reader.chapter : 0;
    book_progress_t now = {
        .book_size = s_reader.book_size,
        .chapter   = chapter,
        .offset    = (uint32_t)s_reader.pages[s_reader.page],
        .text_len  = (uint32_t)s_reader.text_len,
        .permille  = book_permille(),
    };
    /* Not the page's first byte while the page still holds the place saved.
     * On a screen whose pages begin elsewhere, it comes before that place,
     * and saving it would move the place back each time the card changed
     * devices. How far through the book that is, as this screen pages it,
     * is saved with the place as it was: a place saved before the Reader
     * kept it gets it. */
    if (s_reader.has_saved && s_reader.saved.chapter == chapter &&
        page_of(place_in_text(&s_reader.saved)) == s_reader.page) {
        if (s_reader.saved.permille == now.permille) return;
        now.offset   = s_reader.saved.offset;
        now.text_len = s_reader.saved.text_len;
    }

    if (book_progress_save(PROGRESS_PATH, s_reader.book_key, &now)) {
        s_reader.saved     = now;
        s_reader.has_saved = true;
    }
}

static void save_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    save_progress();
}

/* Takes `text` (len bytes, from epub_malloc) as the text being read, shaping
 * any Arabic in it, and pages it. `offsets` are positions in it, moved to the
 * same places in the shaped text; the first `break_count` of them are where
 * pages must break. */
static bool take_text(char *text, size_t len, size_t *offsets, size_t offset_count,
                      size_t break_count)
{
    if (arabic_present(text, len)) {
        size_t shaped_len = 0;
        char *shaped = arabic_shape(text, len, offsets, offset_count, &shaped_len);
        free(text);
        if (!shaped) return false;
        text = shaped;
        len  = shaped_len;
    }

    free_text();
    s_reader.text     = text;
    s_reader.text_len = len;

    size_t *breaks = NULL;
    if (break_count > 0) {
        breaks = epub_malloc(break_count * sizeof(size_t));
        if (!breaks) break_count = 0;
        else memcpy(breaks, offsets, break_count * sizeof(size_t));
        if (breaks) qsort(breaks, break_count, sizeof(size_t), cmp_size);
    }
    bool ok = paginate(breaks, break_count);
    free(breaks);
    if (!ok) {
        free_text();
        return false;
    }
    return true;
}

static bool load_text_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > MAX_TEXT_FILE) {
        ESP_LOGW(TAG, "%s: %ld bytes, not read", path, size);
        fclose(f);
        return false;
    }

    char *text = epub_malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        ESP_LOGE(TAG, "OOM: text of %ld bytes", size);
        return false;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = '\0';
    return take_text(text, got, NULL, 0, 0);
}

/* Shows `chapter`, or, if it has no text (a cover, a page of pictures), the
 * nearest one after it with step 1 or before it with step -1: at contents
 * entry `toc_entry` when it is not -1, else at its first page, or its last
 * when `at_end`. False, with nothing changed, when no chapter that way has
 * text.
 *
 * One file often holds many of the book's chapters, so every contents entry
 * in the one loaded is placed on its page, for the contents screen to show
 * where the reader is. */
static bool open_chapter(int chapter, int step, int toc_entry, bool at_end)
{
    int64_t t0 = esp_timer_get_time();
    int toc_count = epub_toc_count(s_reader.epub);
    const char **anchors = NULL;
    size_t *at = NULL;
    int *which = NULL;
    if (toc_count > 0) {
        anchors = epub_malloc(sizeof(*anchors) * (size_t)toc_count);
        at      = epub_malloc(sizeof(*at) * (size_t)toc_count);
        which   = epub_malloc(sizeof(*which) * (size_t)toc_count);
        if (!s_reader.toc_page) s_reader.toc_page = epub_malloc(sizeof(int) * (size_t)toc_count);
        if (!anchors || !at || !which || !s_reader.toc_page) toc_count = 0;
    }

    bool opened = false;
    for (int c = chapter; c >= 0 && c < epub_chapter_count(s_reader.epub); c += step) {
        /* Top-level entries first: those are the chapters, which begin a
         * page; one below is often a subtitle a line under its heading. */
        size_t n = 0, top = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < toc_count; i++) {
                const epub_toc_entry_t *e = epub_toc_get(s_reader.epub, i);
                if (e->chapter == c && e->anchor && (e->depth == 1) == (pass == 0)) {
                    anchors[n] = e->anchor;
                    which[n++] = i;
                }
            }
            if (pass == 0) top = n;
        }

        size_t len = 0;
        char *text = epub_chapter_text(s_reader.epub, c, anchors, n, at, &len);
        if (!text || len == 0) {
            free(text);
            toc_entry = -1;
            continue;
        }
        if (!take_text(text, len, at, n, top)) {
            toast_warn("Not enough memory for this chapter");
            break;
        }
        s_reader.chapter = c;

        for (int i = 0; i < toc_count; i++) {
            s_reader.toc_page[i] = epub_toc_get(s_reader.epub, i)->chapter == c ? 0 : -1;
        }
        for (size_t k = 0; k < n; k++) s_reader.toc_page[which[k]] = page_of(at[k]);

        ESP_LOGI(TAG, "Chapter %d/%d: %u bytes, %d pages, in %lld ms", c + 1,
                 epub_chapter_count(s_reader.epub), (unsigned)s_reader.text_len,
                 s_reader.page_count, (esp_timer_get_time() - t0) / 1000);
        int page = toc_entry >= 0 && toc_entry < toc_count && s_reader.toc_page[toc_entry] >= 0
                       ? s_reader.toc_page[toc_entry]
                       : at_end ? s_reader.page_count - 1 : 0;
        show_page(page);
        opened = true;
        break;
    }

    free(anchors);
    free(at);
    free(which);
    return opened;
}

/* ------------------------------------------------------------------ */
/* Page display                                                         */
/* ------------------------------------------------------------------ */

static void show_page(int page)
{
    if (!s_reader.text || s_reader.page_count <= 0) return;
    if (page < 0) page = 0;
    if (page >= s_reader.page_count) page = s_reader.page_count - 1;
    s_reader.page = page;

    size_t start = s_reader.pages[page];
    size_t end   = s_reader.pages[page + 1];
    while (end > start && (s_reader.text[end - 1] == '\n' || s_reader.text[end - 1] == '\r')) end--;

    char *page_text = malloc(end - start + 1);
    if (!page_text) {
        lv_label_set_text(s_reader.text_label, "(out of memory)");
        return;
    }
    memcpy(page_text, s_reader.text + start, end - start);
    page_text[end - start] = '\0';
    lv_label_set_text(s_reader.text_label, page_text);
    free(page_text);

    char footer[64];
    if (s_reader.epub) {
        snprintf(footer, sizeof(footer), "Ch %d/%d   Page %d/%d",
                 s_reader.chapter + 1, epub_chapter_count(s_reader.epub),
                 page + 1, s_reader.page_count);
    } else {
        snprintf(footer, sizeof(footer), "Page %d/%d", page + 1, s_reader.page_count);
    }
    lv_label_set_text(s_reader.footer_label, footer);

    uint16_t far = book_permille();
    lv_label_set_text_fmt(s_reader.footer_far, "%u%%", far / 10u);
    lv_obj_set_width(s_reader.progress_line, (int32_t)((int64_t)s_app_w * far / 1000));

    if (s_reader.save_timer) {
        lv_timer_reset(s_reader.save_timer);
        lv_timer_resume(s_reader.save_timer);
    }
}

static void next_page(void)
{
    if (s_reader.page + 1 < s_reader.page_count) {
        show_page(s_reader.page + 1);
    } else if (s_reader.epub && !open_chapter(s_reader.chapter + 1, 1, -1, false)) {
        toast_info("End of book");
    }
}

static void prev_page(void)
{
    if (s_reader.page > 0) {
        show_page(s_reader.page - 1);
    } else if (s_reader.epub) {
        open_chapter(s_reader.chapter - 1, -1, -1, true);
    }
}

/* ------------------------------------------------------------------ */
/* Reading screen                                                       */
/* ------------------------------------------------------------------ */

static void reader_key_cb(lv_event_t *e)
{
    switch (lv_event_get_key(e)) {
        case ' ':
        case 'n':
        case 'j':
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            next_page();
            break;

        case LV_KEY_BACKSPACE:
        case 'p':
        case 'k':
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            prev_page();
            break;

        case 'c':
        case 't':
            if (s_reader.epub) switch_to_contents();
            break;

        case 'q':
        case 'Q':
            switch_to_library();
            break;

        default:
            break;
    }
}

/* A tap on the left third of the page goes back; anywhere else, and Enter,
 * goes on. */
static void reader_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (from_touch()) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t area;
        lv_obj_get_coords(s_reader.reader_screen, &area);
        if (p.x < area.x1 + lv_area_get_width(&area) / 3) {
            prev_page();
            return;
        }
    }
    next_page();
}

static void footer_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_reader.epub) switch_to_contents();
}

static void create_reader_screen(void)
{
    const theme_colors_t *clr = theme_get_colors();

    s_reader.reader_screen = lv_obj_create(s_reader.root);
    lv_obj_set_size(s_reader.reader_screen, s_app_w, s_app_h);
    lv_obj_set_pos(s_reader.reader_screen, 0, 0);
    lv_obj_set_style_bg_color(s_reader.reader_screen, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_reader.reader_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_reader.reader_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_reader.reader_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_reader.reader_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_reader.reader_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN);

    /* Text area — fills the space above the footer; taps pass to the screen */
    lv_obj_t *text_area = lv_obj_create(s_reader.reader_screen);
    lv_obj_set_pos(text_area, 0, 0);
    lv_obj_set_size(text_area, s_app_w, s_app_h - FOOTER_H);
    lv_obj_set_style_bg_color(text_area, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(text_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(text_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(text_area, TEXT_MARGIN_W, LV_PART_MAIN);
    lv_obj_set_style_pad_right(text_area, TEXT_MARGIN_W, LV_PART_MAIN);
    lv_obj_set_style_pad_top(text_area, TEXT_MARGIN_H, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(text_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(text_area, 0, LV_PART_MAIN);
    lv_obj_clear_flag(text_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(text_area, LV_OBJ_FLAG_CLICKABLE);

    /* Text label — wrapped at the area's width, as paginate() measures it.
     * LONG_CLIP would not do: in LVGL 9 it turns wrapping off, and every
     * paragraph ran off the edge as one line. The area clips the bottom. */
    s_reader.text_label = lv_label_create(text_area);
    lv_label_set_long_mode(s_reader.text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(s_reader.text_label, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_font(s_reader.text_label, s_reader.font, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_reader.text_label, LINE_SPACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_reader.text_label, clr->text, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_reader.text_label, LV_TEXT_ALIGN_AUTO, LV_PART_MAIN);
    lv_label_set_text(s_reader.text_label, "");

    /* What a page holds: the label's own width, and as many lines as fit. */
    lv_obj_update_layout(text_area);
    s_reader.text_w = lv_obj_get_content_width(text_area);
    int32_t text_h = lv_obj_get_content_height(text_area);
    int32_t line_h = lv_font_get_line_height(s_reader.font) + LINE_SPACE;
    /* n lines stand n * line_h - LINE_SPACE tall. */
    s_reader.lines_per_page = (int)((text_h + LINE_SPACE) / line_h);
    if (s_reader.lines_per_page < 1) s_reader.lines_per_page = 1;

    /* Footer bar (24 px); a tap opens the contents */
    lv_obj_t *footer_bar = lv_obj_create(s_reader.reader_screen);
    lv_obj_set_pos(footer_bar, 0, s_app_h - FOOTER_H);
    lv_obj_set_size(footer_bar, s_app_w, FOOTER_H);
    lv_obj_set_style_bg_color(footer_bar, clr->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(footer_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(footer_bar, clr->text_secondary, LV_PART_MAIN);
    lv_obj_set_style_border_width(footer_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(footer_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(footer_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(footer_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(footer_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(footer_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(footer_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(footer_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(footer_bar, footer_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_reader.footer_label = lv_label_create(footer_bar);
    lv_label_set_long_mode(s_reader.footer_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_reader.footer_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_reader.footer_label, clr->text_secondary, LV_PART_MAIN);
    lv_obj_align(s_reader.footer_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_reader.footer_label, "Page --/--");

    s_reader.footer_far = lv_label_create(footer_bar);
    lv_obj_set_style_text_font(s_reader.footer_far, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_reader.footer_far, clr->text, LV_PART_MAIN);
    lv_obj_align(s_reader.footer_far, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_reader.footer_far, "");

    /* Over the footer's top border, from the left edge: its padding and
     * border are outside its content, where a child is placed from. */
    s_reader.progress_line = lv_obj_create(footer_bar);
    lv_obj_remove_style_all(s_reader.progress_line);
    lv_obj_set_style_bg_color(s_reader.progress_line, clr->text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_reader.progress_line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(s_reader.progress_line, 0, PROGRESS_H);
    lv_obj_align(s_reader.progress_line, LV_ALIGN_TOP_LEFT, -8, -1);
    lv_obj_clear_flag(s_reader.progress_line, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(s_reader.reader_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_reader.reader_screen, reader_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_reader.reader_screen, reader_clicked_cb, LV_EVENT_CLICKED, NULL);
}

static void open_book(const char *path)
{
    close_book();

    /* Its place is kept by its path from the card's root, which any device
     * the card goes into sees the same, and its size: a book that has changed
     * since starts at the beginning. */
    snprintf(s_reader.book_key, sizeof(s_reader.book_key), "%s", book_key(path));
    struct stat st;
    s_reader.book_size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    book_progress_t left = {0};
    bool resume = book_progress_load(PROGRESS_PATH, s_reader.book_key, &left) &&
                  left.book_size == s_reader.book_size;

    bool ok;
    if (has_ext(path, ".epub")) {
        s_reader.epub = epub_open(path);
        resume = resume && s_reader.epub && left.chapter < (uint32_t)epub_chapter_count(s_reader.epub);
        ok = s_reader.epub && ((resume && open_chapter((int)left.chapter, 1, -1, false)) ||
                               open_chapter(0, 1, -1, false));
        resume = resume && ok && (uint32_t)s_reader.chapter == left.chapter;
    } else {
        ok = load_text_file(path);
        if (ok) show_page(0);
    }
    if (!ok) {
        close_book();
        toast_warn("Cannot open book");
        return;
    }
    if (resume) {
        show_page(page_of(place_in_text(&left)));
        s_reader.saved     = left;
        s_reader.has_saved = true;
        ESP_LOGI(TAG, "%s: back at chapter %d, page %d of %d", s_reader.book_key,
                 s_reader.chapter + 1, s_reader.page + 1, s_reader.page_count);
    }
    book_recent_put(PROGRESS_PATH, s_reader.book_key, epub_title(s_reader.epub),
                    epub_author(s_reader.epub));

    /* A right-to-left book reads from the right even on a page that begins
     * with a number or a Latin word. */
    lv_base_dir_t dir = s_reader.epub && is_rtl_language(epub_language(s_reader.epub))
                            ? LV_BASE_DIR_RTL : LV_BASE_DIR_AUTO;
    lv_obj_set_style_base_dir(s_reader.text_label, dir, LV_PART_MAIN);
    switch_to_reading();
}

static void switch_to_reading(void)
{
    lv_obj_add_flag(s_reader.library_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_reader.toc_screen) lv_obj_add_flag(s_reader.toc_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN);
    focus(s_reader.reader_screen);
}

/* ------------------------------------------------------------------ */
/* Lists: the library and the contents                                  */
/* ------------------------------------------------------------------ */

/* A list is one lv_table, a row of text costing a string in LVGL's 64 KB
 * pool. A row of widgets with their own styles cost some 400 bytes: Moby
 * Dick's 145 contents entries ran the pool out, and LVGL then stops in an
 * assert for good. Up and down move the selection; a tap or Enter opens. */

static void open_library_row(uint32_t row);
static void open_contents_row(uint32_t row);

static void list_changed_cb(lv_event_t *e)
{
    if (s_reader.filling_list) return;

    /* The selection also moves with the arrow keys, which must not open it. */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD &&
        lv_indev_get_key(indev) != LV_KEY_ENTER) {
        return;
    }

    lv_obj_t *table = lv_event_get_target(e);
    uint32_t row = 0, col = 0;
    lv_table_get_selected_cell(table, &row, &col);
    if (row == LV_TABLE_CELL_NONE) return;
    if (table == s_reader.lib_list) {
        open_library_row(row);
    } else if (table == s_reader.toc_list) {
        open_contents_row(row);
    }
}

/* j and k move like the arrows, which the table has moved already; the
 * library's headings are passed over. q goes back. */
static void list_key_cb(lv_event_t *e)
{
    lv_obj_t *table = lv_event_get_target(e);
    uint32_t key = lv_event_get_key(e);
    int step = key == 'j' || key == LV_KEY_DOWN ? 1 : key == 'k' || key == LV_KEY_UP ? -1 : 0;
    if (step) {
        uint32_t row = 0, col = 0;
        lv_table_get_selected_cell(table, &row, &col);
        uint32_t rows = lv_table_get_row_count(table);
        if (row == LV_TABLE_CELL_NONE) row = 0;
        else if (key == 'j' && row + 1 < rows) row++;
        else if (key == 'k' && row > 0) row--;
        if (table == s_reader.lib_list) {
            uint32_t from = row;
            while (is_heading_row(row) && (int)row + step >= 0 && (int)row + step < (int)rows) {
                row = (uint32_t)((int)row + step);
            }
            /* None that way: the heading at the top stays above the first. */
            if (is_heading_row(row)) {
                row = from;
                while (is_heading_row(row) && row + 1 < rows) row++;
            }
        }
        lv_table_set_selected_cell(table, (uint16_t)row, 0);
        /* The heading at the top is shown when the first book is. */
        if (table == s_reader.lib_list && row == 1 && is_heading_row(0)) {
            lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
        }
    } else if (key == 'q' || key == 'Q') {
        if (table == s_reader.toc_list) {
            switch_to_reading();
        } else {
            app_manager_launch("com.thistle.launcher");
        }
    }
}

/* Selects `row` without opening it. */
static void select_row(lv_obj_t *table, uint32_t row)
{
    if (lv_table_get_row_count(table) == 0) return;
    s_reader.filling_list = true;
    lv_table_set_selected_cell(table, (uint16_t)row, 0);
    s_reader.filling_list = false;
}

/* Sets a cell, shaping any Arabic in its text first. */
static void set_cell(lv_obj_t *table, uint32_t row, uint32_t col, const char *text)
{
    size_t len = strlen(text);
    char *shaped = arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, NULL) : NULL;
    lv_table_set_cell_value(table, row, col, shaped ? shaped : text);
    free(shaped);
}

static lv_obj_t *create_list_screen(const char *title, lv_event_cb_t title_clicked,
                                    lv_obj_t **list_out)
{
    const theme_colors_t *clr = theme_get_colors();

    lv_obj_t *screen = lv_obj_create(s_reader.root);
    lv_obj_set_size(screen, s_app_w, s_app_h);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_bar = lv_obj_create(screen);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_size(title_bar, s_app_w, TITLE_BAR_H);
    lv_obj_set_style_bg_color(title_bar, clr->surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(title_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(title_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(title_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(title_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(title_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(title_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(title_bar, clr->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(title_bar, 1, LV_PART_MAIN);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    if (title_clicked) {
        lv_obj_add_flag(title_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title_bar, title_clicked, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, title);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_lbl, clr->text, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *table = lv_table_create(screen);
    lv_obj_set_pos(table, 0, TITLE_BAR_H);
    lv_obj_set_size(table, s_app_w, s_app_h - TITLE_BAR_H);
    lv_obj_set_style_bg_color(table, clr->bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(table, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(table, s_reader.font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, clr->text, LV_PART_ITEMS);
    lv_obj_set_style_text_line_space(table, LINE_SPACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table, clr->surface, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
    theme_rule_table_rows(table);
    lv_obj_set_style_pad_left(table, CELL_PAD_W, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, CELL_PAD_W, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 4, LV_PART_ITEMS);
    /* The selected row, inverted, as the other lists draw a pressed one. */
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(table, clr->bg, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(table, clr->bg, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(table, clr->primary, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(table, 2, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(table, list_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, list_key_cb, LV_EVENT_KEY, NULL);
    lv_table_set_column_count(table, 1);
    lv_table_set_column_width(table, 0, s_app_w);

    *list_out = table;
    return screen;
}

/* ------------------------------------------------------------------ */
/* Library                                                              */
/* ------------------------------------------------------------------ */

typedef struct book_entry {
    char     path[384];
    char     name[256];   /* shown: its title, or its file's name tidied */
    char     author[96];  /* "" when not known */
    off_t    size;
    uint16_t permille;    /* how far it has been read; BOOK_PROGRESS_UNKNOWN */
    int      recent;      /* its place among the books read last, or -1 */
} book_entry_t;

static int book_cmp(const void *a, const void *b)
{
    return strcasecmp(((const book_entry_t *)a)->name, ((const book_entry_t *)b)->name);
}

/* A book's path from the card's root, which is what its place and the list
 * of books read last are kept by. */
static const char *book_key(const char *path)
{
    size_t root_len = strlen(THISTLE_SDCARD);
    return strncmp(path, THISTLE_SDCARD, root_len) == 0 && path[root_len] == '/'
               ? path + root_len : path;
}

/* Adds the books in `dir` to `entries`. */
static void scan_books(const char *dir_path, book_entry_t *entries, int *count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && *count < MAX_BOOK_FILES) {
        /* Dot files include macOS's "._" companions, which are not books. */
        if (de->d_name[0] == '.' || !is_readable_ext(de->d_name)) continue;

        book_entry_t *b = &entries[*count];
        if (snprintf(b->path, sizeof(b->path), "%s/%s", dir_path, de->d_name) >= (int)sizeof(b->path)) {
            continue;
        }
        struct stat st;
        if (stat(b->path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        b->size = st.st_size;
        (*count)++;
    }
    closedir(dir);
}

/* Names each book, by the title it gave when last opened if it is among the
 * books read last, else by its file's name, and finds how far those have
 * been read. */
static void name_books(void)
{
    book_recent_t *recent = epub_malloc(sizeof(book_recent_t) * BOOK_RECENT_MAX);
    int recent_count = recent ? book_recent_load(PROGRESS_PATH, recent) : 0;

    for (int i = 0; i < s_reader.book_count; i++) {
        book_entry_t *b = &s_reader.books[i];
        const char *key = book_key(b->path);
        const char *slash = strrchr(b->path, '/');
        book_title_from_file(slash ? slash + 1 : b->path, b->name, sizeof(b->name));
        b->author[0] = '\0';
        b->permille  = BOOK_PROGRESS_UNKNOWN;
        b->recent    = -1;

        for (int r = 0; r < recent_count; r++) {
            if (strcasecmp(recent[r].book, key) != 0) continue;
            b->recent = r;
            if (recent[r].title[0]) snprintf(b->name, sizeof(b->name), "%s", recent[r].title);
            snprintf(b->author, sizeof(b->author), "%s", recent[r].author);
            book_progress_t left;
            if (book_progress_load(PROGRESS_PATH, key, &left) && left.book_size == (uint32_t)b->size) {
                b->permille = left.permille;
            }
            break;
        }
    }
    free(recent);
    qsort(s_reader.books, (size_t)s_reader.book_count, sizeof(book_entry_t), book_cmp);
}

/* Appends `text` to `cell`, shaping any Arabic in it, in at most `max_lines`
 * lines of a list row, the last of them `reserve` px short: cut, if it does
 * not fit, with an ellipsis. Lines are measured as the row will break them. */
static void append_fitted(char *cell, size_t cap, const char *text, int max_lines, int32_t reserve)
{
    static const char ELLIPSIS[] = "\xE2\x80\xA6";
    size_t len = strlen(text);
    char *shaped = arabic_present(text, len) ? arabic_shape(text, len, NULL, 0, &len) : NULL;
    if (shaped) text = shaped;

    const lv_font_t *font = s_reader.font;
    int32_t width = s_app_w - 2 * CELL_PAD_W;
    size_t pos = 0, line_start = 0;
    for (int line = 0; pos < len && line < max_lines; line++) {
        line_start = pos;
        int32_t room = line == max_lines - 1 ? width - reserve : width;
        uint32_t n = lv_text_get_next_line(text + pos, font, 0, room, NULL, LV_TEXT_FLAG_NONE);
        pos += n ? n : 1;
    }

    size_t end = pos;
    bool cut = pos < len;
    if (cut) {
        int32_t room = width - reserve - lv_text_get_width(ELLIPSIS, sizeof(ELLIPSIS) - 1, font, 0);
        while (end > line_start &&
               lv_text_get_width(text + line_start, (uint32_t)(end - line_start), font, 0) > room) {
            do end--; while (end > line_start && ((unsigned char)text[end] & 0xc0) == 0x80);
        }
    }
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\n')) end--;

    size_t used = strlen(cell);
    if (used + end + sizeof(ELLIPSIS) < cap) {
        memcpy(cell + used, text, end);
        strcpy(cell + used + end, cut ? ELLIPSIS : "");
    }
    free(shaped);
}

/* A book's row: its title, in two lines at most, and under it its author and
 * how far it has been read, or, for a book not opened, its size. */
static void book_cell(const book_entry_t *b, char *cell, size_t cap)
{
    cell[0] = '\0';
    append_fitted(cell, cap, b->name, 2, 0);
    strcat(cell, "\n");

    char far[24] = "";
    if (b->permille >= 1000 && b->permille != BOOK_PROGRESS_UNKNOWN) {
        snprintf(far, sizeof(far), "Finished");
    } else if (b->permille != BOOK_PROGRESS_UNKNOWN) {
        snprintf(far, sizeof(far), "%u%%", b->permille / 10u);
    }
    if (b->author[0] && far[0]) {
        /* The author cut short before how far, which matters more. */
        char after[32];
        snprintf(after, sizeof(after), " \xC2\xB7 %s", far);
        append_fitted(cell, cap, b->author, 1,
                      lv_text_get_width(after, (uint32_t)strlen(after), s_reader.font, 0));
        strncat(cell, after, cap - strlen(cell) - 1);
    } else if (b->author[0] || far[0]) {
        append_fitted(cell, cap, b->author[0] ? b->author : far, 1, 0);
    } else {
        char size[16];
        format_size(b->size, size, sizeof(size));
        strncat(cell, size, cap - strlen(cell) - 1);
    }
}

static void add_heading_row(const char *text)
{
    lv_table_set_cell_value(s_reader.lib_list, (uint32_t)s_reader.row_count, 0, text);
    s_reader.row_book[s_reader.row_count++] = -1;
}

static void add_book_row(int book)
{
    char cell[2 * sizeof(s_reader.books[0].name)];
    book_cell(&s_reader.books[book], cell, sizeof(cell));
    lv_table_set_cell_value(s_reader.lib_list, (uint32_t)s_reader.row_count, 0, cell);
    s_reader.row_book[s_reader.row_count++] = (int16_t)book;
}

static bool is_heading_row(uint32_t row)
{
    return row < (uint32_t)s_reader.row_count && s_reader.row_book[row] < 0;
}

/* Fills the library: the books read last, newest first, then all of them by
 * name. The newest is selected, to go back to it with a tap or Enter. */
static void fill_library(void)
{
    name_books();

    int recent[RECENT_SHOWN];
    int recent_count = 0;
    for (int r = 0; r < BOOK_RECENT_MAX && recent_count < RECENT_SHOWN; r++) {
        for (int i = 0; i < s_reader.book_count; i++) {
            if (s_reader.books[i].recent == r) recent[recent_count++] = i;
        }
    }

    lv_obj_t *table = s_reader.lib_list;
    s_reader.filling_list = true;
    s_reader.row_count = 0;
    lv_table_set_row_count(table, (uint32_t)(s_reader.book_count + (recent_count ? recent_count + 2 : 0)));
    if (s_reader.book_count == 0) {
        lv_table_set_row_count(table, 1);
        add_heading_row("No books found. Add .epub or .txt files to the SD card.");
    }
    if (recent_count > 0) {
        add_heading_row("Recent");
        for (int i = 0; i < recent_count; i++) add_book_row(recent[i]);
        add_heading_row("All books");
    }
    for (int i = 0; i < s_reader.book_count; i++) add_book_row(i);
    s_reader.filling_list = false;

    for (int row = 0; row < s_reader.row_count; row++) {
        if (!is_heading_row((uint32_t)row)) {
            select_row(table, (uint32_t)row);
            break;
        }
    }
    lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
}

/* Headings are set smaller and plainer than the books, and never drawn
 * selected. */
static void library_draw_cb(lv_event_t *e)
{
    static const lv_font_t *const HEADING_FONT = &lv_font_montserrat_14;
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS || !is_heading_row(base->id1)) return;

    const theme_colors_t *clr = theme_get_colors();
    if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_task_get_fill_dsc(task)->color = clr->bg;
    } else if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
        label->color = clr->text_secondary;
        label->font  = HEADING_FONT;
        /* The table centres a line of the books' font, which is taller. */
        label->ofs_y = (lv_font_get_line_height(s_reader.font) -
                        lv_font_get_line_height(HEADING_FONT)) / 2;
    }
}

static void open_library_row(uint32_t row)
{
    if (row < (uint32_t)s_reader.row_count && s_reader.row_book[row] >= 0) {
        open_book(s_reader.books[s_reader.row_book[row]].path);
    }
}

static void populate_library(void)
{
    free(s_reader.books);
    s_reader.book_count = 0;
    s_reader.books = epub_malloc(MAX_BOOK_FILES * sizeof(book_entry_t));
    if (!s_reader.books) return;
    scan_books(BOOKS_PATH, s_reader.books, &s_reader.book_count);
    scan_books(THISTLE_SDCARD, s_reader.books, &s_reader.book_count);
    fill_library();
    focus(s_reader.lib_list);
}

static void switch_to_library(void)
{
    lv_obj_add_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_reader.toc_screen) lv_obj_add_flag(s_reader.toc_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_reader.library_screen, LV_OBJ_FLAG_HIDDEN);

    /* Free the book to reclaim PSRAM, its place saved first, for the library
     * to put it at the top with how far it has been read. */
    close_book();
    lv_label_set_text(s_reader.text_label, "");
    if (s_reader.books) fill_library();
    focus(s_reader.lib_list);
}

/* ------------------------------------------------------------------ */
/* Contents                                                             */
/* ------------------------------------------------------------------ */

static void open_contents_row(uint32_t row)
{
    if (epub_toc_count(s_reader.epub) > 0) {
        const epub_toc_entry_t *entry = epub_toc_get(s_reader.epub, (int)row);
        if (entry) open_chapter(entry->chapter, 1, (int)row, false);
    } else {
        open_chapter((int)row, 1, -1, false);
    }
    switch_to_reading();
}

static void contents_title_clicked_cb(lv_event_t *e)
{
    (void)e;
    switch_to_reading();
}

/* The table of contents, or, for a book without one, its chapters by
 * number. The last entry at or before the page being read is selected. */
static void switch_to_contents(void)
{
    if (!s_reader.toc_screen) {
        s_reader.toc_screen = create_list_screen("< Contents", contents_title_clicked_cb,
                                                 &s_reader.toc_list);
    }
    lv_obj_t *table = s_reader.toc_list;

    int toc_count = epub_toc_count(s_reader.epub);
    int rows = toc_count > 0 ? toc_count : epub_chapter_count(s_reader.epub);
    int current = 0;
    s_reader.filling_list = true;
    lv_table_set_row_count(table, (uint32_t)(rows > 0 ? rows : 1));
    for (int i = 0; i < rows; i++) {
        char title[200];
        int chapter;
        if (toc_count > 0) {
            const epub_toc_entry_t *entry = epub_toc_get(s_reader.epub, i);
            int indent = entry->depth > 1 ? (entry->depth - 1) * 2 : 0;
            if (indent > 8) indent = 8;
            snprintf(title, sizeof(title), "%*s%s", indent, "", entry->title);
            chapter = entry->chapter;
        } else {
            snprintf(title, sizeof(title), "Chapter %d", i + 1);
            chapter = i;
        }
        set_cell(table, (uint32_t)i, 0, title);

        bool before = toc_count > 0
                          ? chapter < s_reader.chapter ||
                                (chapter == s_reader.chapter && s_reader.toc_page &&
                                 s_reader.toc_page[i] >= 0 && s_reader.toc_page[i] <= s_reader.page)
                          : chapter <= s_reader.chapter;
        if (before) current = i;
    }
    s_reader.filling_list = false;

    lv_obj_add_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_reader.toc_screen, LV_OBJ_FLAG_HIDDEN);
    focus(table);
    select_row(table, (uint32_t)current);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

static void library_title_clicked_cb(lv_event_t *e)
{
    (void)e;
    app_manager_launch("com.thistle.launcher");
}

/* The Back key, as q: from the contents to the page, and from the page to the
 * library. */
static bool reader_back(void)
{
    if (s_reader.toc_screen && !lv_obj_has_flag(s_reader.toc_screen, LV_OBJ_FLAG_HIDDEN)) {
        switch_to_reading();
        return true;
    }
    if (!lv_obj_has_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN)) {
        switch_to_library();
        return true;
    }
    return false;
}

/* E-paper shows a pixel black or white, black only if it is at least half
 * ink. Antialiased, the dots of Arabic letters are spread over a few lighter
 * pixels and some of them drop out; the mono font draws each pixel inked or
 * not to begin with, so the dots stay whole. A colour screen keeps the
 * smoother antialiased font, which drawn in one bit would look jagged. */
static const lv_font_t *book_font(void)
{
    const hal_registry_t *reg = hal_get_registry();
    if (reg && reg->display && reg->display->type == HAL_DISPLAY_TYPE_EPAPER) {
        return &reader_font_16_mono;
    }
    return &reader_font_16;
}

esp_err_t reader_ui_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating reader UI");

    if (parent == NULL) {
        parent = lv_scr_act();
    }

    lv_obj_update_layout(parent);
    s_app_w = lv_obj_get_width(parent);
    s_app_h = lv_obj_get_height(parent);
    if (s_app_w == 0) s_app_w = 240;
    if (s_app_h == 0) s_app_h = 296;

    memset(&s_reader, 0, sizeof(s_reader));
    s_reader.font = book_font();
    s_reader.save_timer = lv_timer_create(save_timer_cb, SAVE_DELAY_MS, NULL);
    if (s_reader.save_timer) lv_timer_pause(s_reader.save_timer);

    s_reader.root = lv_obj_create(parent);
    lv_obj_set_size(s_reader.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_reader.root, 0, 0);
    lv_obj_set_style_bg_opa(s_reader.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_reader.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_reader.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_reader.root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_reader.root, LV_OBJ_FLAG_SCROLLABLE);

    s_reader.library_screen = create_list_screen("< Library", library_title_clicked_cb,
                                                 &s_reader.lib_list);
    lv_obj_add_flag(s_reader.lib_list, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_reader.lib_list, library_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    create_reader_screen();
    populate_library();
    ui_manager_set_back_handler(s_reader.root, reader_back);
    return ESP_OK;
}

void reader_ui_show(void)
{
    if (!s_reader.root) return;
    lv_obj_clear_flag(s_reader.root, LV_OBJ_FLAG_HIDDEN);
    if (!lv_obj_has_flag(s_reader.reader_screen, LV_OBJ_FLAG_HIDDEN)) {
        focus(s_reader.reader_screen);
    }
}

void reader_ui_hide(void)
{
    save_progress();
    if (s_reader.root) {
        lv_obj_add_flag(s_reader.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void reader_ui_destroy(void)
{
    close_book();
    if (s_reader.save_timer) {
        lv_timer_delete(s_reader.save_timer);
        s_reader.save_timer = NULL;
    }
    free(s_reader.books);
    s_reader.books = NULL;
    if (s_reader.root) {
        lv_obj_delete(s_reader.root);
        s_reader.root = NULL;
    }
}
