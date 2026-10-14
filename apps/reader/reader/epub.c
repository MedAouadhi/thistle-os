/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — reading an EPUB as plain text, chapter by chapter.
 *
 * The parsers are C ports of CrossPoint Reader's ContainerParser,
 * ContentOpfParser, TocNcxParser, TocNavParser and the text rules of its
 * ChapterHtmlSlimParser, with FsHelpers::normalisePath; Copyright (c) 2025
 * Dave Allie, MIT. See THIRD_PARTY_LICENSES.md.
 *
 * CrossPoint lays a chapter out into pages itself. Here a chapter becomes
 * plain text, which the Reader pages through with LVGL, so only what decides
 * the words and where paragraphs break is kept: no CSS, images or
 * hyphenation. Unlike CrossPoint, hrefs are percent-decoded and resolved
 * against the file that holds them, and a chapter Expat rejects is still
 * read, by a rougher tag stripper.
 */
#include "reader/epub.h"
#include "reader/epub_port.h"
#include "reader/epub_zip.h"
#include "reader/html_entities.h"

#include "third_party/expat/expat.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "epub";

/* ------------------------------------------------------------------ */
/* Growable text                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   failed;   /* an allocation failed; the contents are incomplete */
} buf_t;

static void buf_append(buf_t *b, const char *s, size_t n)
{
    if (b->failed || n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = epub_realloc(b->data, cap);
        if (!p) {
            b->failed = true;
            return;
        }
        b->data = p;
        b->cap  = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_reset(buf_t *b)
{
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_free(buf_t *b)
{
    epub_free(b->data);
    memset(b, 0, sizeof(*b));
}

/* The contents as a NUL-terminated string the caller owns, or NULL if an
 * allocation failed along the way. */
static char *buf_take(buf_t *b, size_t *len_out)
{
    if (b->failed) {
        buf_free(b);
        return NULL;
    }
    if (!b->data) {
        b->data = epub_malloc(1);
        if (!b->data) return NULL;
        b->data[0] = '\0';
    }
    char *s = b->data;
    if (len_out) *len_out = b->len;
    memset(b, 0, sizeof(*b));
    return s;
}

static char *dup_n(const char *s, size_t n)
{
    char *d = epub_malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *dup_str(const char *s)
{
    return s ? dup_n(s, strlen(s)) : NULL;
}

static bool is_xml_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* How many characters the UTF-8 string `s` holds. */
static size_t utf8_length(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    }
    return n;
}

/* `s` with its runs of white space made single spaces and its ends trimmed,
 * as a title or a table-of-contents entry should read. */
static char *collapse_space(const char *s, size_t n)
{
    char *out = epub_malloc(n + 1);
    if (!out) return NULL;
    size_t len = 0;
    bool space = false;
    for (size_t i = 0; i < n; i++) {
        if (is_xml_space(s[i])) {
            space = true;
            continue;
        }
        if (space && len > 0) out[len++] = ' ';
        space = false;
        out[len++] = s[i];
    }
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* XML                                                                  */
/* ------------------------------------------------------------------ */

static const XML_Memory_Handling_Suite XML_MEMORY = {
    epub_malloc, epub_realloc, epub_free,
};

/* The element name without its namespace prefix: "opf:item" is "item". */
static const char *local_name(const char *name)
{
    const char *colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static const char *attr(const XML_Char **atts, const char *name)
{
    for (int i = 0; atts && atts[i]; i += 2) {
        if (strcmp(atts[i], name) == 0) return atts[i + 1];
    }
    return NULL;
}

/* Whether the space-separated list `list` holds `word`, as EPUB's
 * properties="nav" and epub:type="toc" are written. */
static bool has_word(const char *list, const char *word)
{
    size_t wlen = strlen(word);
    for (const char *p = list; p && *p;) {
        while (*p == ' ') p++;
        size_t n = strcspn(p, " ");
        if (n == wlen && strncmp(p, word, n) == 0) return true;
        p += n;
    }
    return false;
}

static XML_Parser new_parser(void *ctx)
{
    XML_Parser p = XML_ParserCreate_MM(NULL, &XML_MEMORY, NULL);
    if (!p) return NULL;
    XML_SetHashSalt(p, epub_hash_salt());
    XML_SetUserData(p, ctx);
    return p;
}

static bool run_parser(XML_Parser p, const char *xml, size_t len, const char *what)
{
    if (len > INT_MAX) return false;
    if (XML_Parse(p, xml, (int)len, XML_TRUE) == XML_STATUS_OK) return true;
    ESP_LOGW(TAG, "%s: %s at line %lu", what, XML_ErrorString(XML_GetErrorCode(p)),
             (unsigned long)XML_GetCurrentLineNumber(p));
    return false;
}

/* ------------------------------------------------------------------ */
/* Paths inside the archive                                             */
/* ------------------------------------------------------------------ */

/* The directory holding `path`, with its trailing '/', or "". */
static char *dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return dup_n(path, slash ? (size_t)(slash - path) + 1 : 0);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `path` with "." and ".." resolved and empty segments dropped. */
static char *normalise(const char *path)
{
    size_t n = strlen(path);
    char *out = epub_malloc(n + 1);
    size_t *starts = epub_malloc(sizeof(size_t) * (n + 1));
    if (!out || !starts) {
        epub_free(out);
        epub_free(starts);
        return NULL;
    }

    size_t len = 0;
    size_t depth = 0;
    const char *p = path;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg);
        if (*p == '/') p++;

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) len = starts[--depth];
            continue;
        }
        starts[depth++] = len;
        if (len > 0) out[len++] = '/';
        memcpy(out + len, seg, seg_len);
        len += seg_len;
    }
    out[len] = '\0';
    epub_free(starts);
    return out;
}

/* The archive path an href names: relative to `base`, a directory as dir_of()
 * gives it, without its fragment, and percent-decoded, since an href is a URL
 * and an archive name is not ("Chapter%201.xhtml" is "Chapter 1.xhtml"). NULL
 * for an href that names no file, such as "#note-3". */
static char *resolve_href(const char *base, const char *href)
{
    size_t href_len = strcspn(href, "#?");
    if (href_len == 0) return NULL;

    buf_t raw = {0};
    if (href[0] == '/') {
        href++;
        href_len--;
    } else {
        buf_append(&raw, base, strlen(base));
    }
    for (size_t i = 0; i < href_len; i++) {
        char c = href[i];
        if (c == '%' && i + 2 < href_len) {
            int hi = hex_value(href[i + 1]);
            int lo = hex_value(href[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)(hi << 4 | lo);
                i += 2;
            }
        }
        buf_append(&raw, &c, 1);
    }

    char *joined = buf_take(&raw, NULL);
    if (!joined) return NULL;
    char *path = normalise(joined);
    epub_free(joined);
    return path;
}

/* ------------------------------------------------------------------ */
/* The book                                                             */
/* ------------------------------------------------------------------ */

struct epub {
    epub_zip_t       *zip;
    char             *title;
    char             *author;
    char             *language;
    char            **chapters;    /* archive paths, in reading order */
    uint64_t         *chapter_at;  /* the documents' sizes before each, then all */
    int               chapter_count;
    epub_toc_entry_t *toc;
    int               toc_count;
    int               toc_cap;
};

static int find_chapter(const epub_t *book, const char *path)
{
    for (int i = 0; i < book->chapter_count; i++) {
        if (strcmp(book->chapters[i], path) == 0) return i;
    }
    return -1;
}

/* Adds an entry for `href`, relative to `base`, if it names a chapter. */
static void add_toc_entry(epub_t *book, const char *base, const buf_t *label,
                          const char *href, int depth)
{
    if (!label->data || !href) return;
    char *path = resolve_href(base, href);
    int chapter = path ? find_chapter(book, path) : -1;
    epub_free(path);
    if (chapter < 0) return;

    char *title = collapse_space(label->data, label->len);
    const char *hash = strchr(href, '#');
    char *anchor = hash && hash[1] ? dup_str(hash + 1) : NULL;
    if (!title || !title[0] || (hash && hash[1] && !anchor)) {
        epub_free(title);
        epub_free(anchor);
        return;
    }
    if (book->toc_count == book->toc_cap) {
        int cap = book->toc_cap ? book->toc_cap * 2 : 16;
        epub_toc_entry_t *toc = epub_realloc(book->toc, sizeof(*toc) * (size_t)cap);
        if (!toc) {
            epub_free(title);
            epub_free(anchor);
            return;
        }
        book->toc     = toc;
        book->toc_cap = cap;
    }
    book->toc[book->toc_count++] = (epub_toc_entry_t){
        .title = title, .chapter = chapter, .anchor = anchor,
        .depth = depth > 0 ? depth : 1,
    };
}

/* --- META-INF/container.xml: where the package document is ---------- */

static void XMLCALL container_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    char **opf_path = ud;
    if (*opf_path || strcmp(local_name(name), "rootfile") != 0) return;
    const char *type = attr(atts, "media-type");
    const char *path = attr(atts, "full-path");
    if (path && (!type || strcmp(type, "application/oebps-package+xml") == 0)) {
        *opf_path = dup_str(path);
    }
}

static char *read_opf_path(epub_zip_t *zip)
{
    size_t len = 0;
    char *xml = epub_zip_read(zip, "META-INF/container.xml", &len);
    if (!xml) return NULL;

    char *opf_path = NULL;
    XML_Parser p = new_parser(&opf_path);
    if (p) {
        XML_SetStartElementHandler(p, container_start);
        run_parser(p, xml, len, "container.xml");
        XML_ParserFree(p);
    }
    epub_free(xml);
    return opf_path;
}

/* --- The package document: metadata, manifest, spine --------------- */

typedef struct {
    char *id;
    char *path;
    char *media_type;
    char *properties;
} item_t;

typedef struct {
    epub_t     *book;
    const char *base;
    bool        in_metadata;
    char      **capture;      /* the book's title, author or language while in one */
    buf_t       text;
    item_t     *items;
    int         item_count;
    int         item_cap;
    char      **spine;        /* idrefs */
    int         spine_count;
    int         spine_cap;
    char       *ncx_id;       /* the spine's toc="" */
} opf_ctx_t;

static void push_item(opf_ctx_t *c, const XML_Char **atts)
{
    const char *id = attr(atts, "id");
    const char *href = attr(atts, "href");
    if (!id || !href) return;
    if (c->item_count == c->item_cap) {
        int cap = c->item_cap ? c->item_cap * 2 : 32;
        item_t *items = epub_realloc(c->items, sizeof(*items) * (size_t)cap);
        if (!items) return;
        c->items    = items;
        c->item_cap = cap;
    }
    item_t *it = &c->items[c->item_count++];
    it->id         = dup_str(id);
    it->path       = resolve_href(c->base, href);
    it->media_type = dup_str(attr(atts, "media-type"));
    it->properties = dup_str(attr(atts, "properties"));
}

static void push_spine(opf_ctx_t *c, const char *idref)
{
    if (!idref) return;
    if (c->spine_count == c->spine_cap) {
        int cap = c->spine_cap ? c->spine_cap * 2 : 32;
        char **spine = epub_realloc(c->spine, sizeof(*spine) * (size_t)cap);
        if (!spine) return;
        c->spine     = spine;
        c->spine_cap = cap;
    }
    c->spine[c->spine_count++] = dup_str(idref);
}

static void XMLCALL opf_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    opf_ctx_t *c = ud;
    const char *tag = local_name(name);

    if (strcmp(tag, "metadata") == 0) {
        c->in_metadata = true;
    } else if (c->in_metadata) {
        /* The first of each: a second dc:title is a subtitle. */
        if (strcmp(tag, "title") == 0 && !c->book->title) {
            c->capture = &c->book->title;
            buf_reset(&c->text);
        } else if (strcmp(tag, "creator") == 0 && !c->book->author) {
            c->capture = &c->book->author;
            buf_reset(&c->text);
        } else if (strcmp(tag, "language") == 0 && !c->book->language) {
            c->capture = &c->book->language;
            buf_reset(&c->text);
        }
    } else if (strcmp(tag, "item") == 0) {
        push_item(c, atts);
    } else if (strcmp(tag, "spine") == 0) {
        c->ncx_id = dup_str(attr(atts, "toc"));
    } else if (strcmp(tag, "itemref") == 0) {
        push_spine(c, attr(atts, "idref"));
    }
}

static void XMLCALL opf_end(void *ud, const XML_Char *name)
{
    opf_ctx_t *c = ud;
    const char *tag = local_name(name);

    if (strcmp(tag, "metadata") == 0) {
        c->in_metadata = false;
    } else if (c->capture && (strcmp(tag, "title") == 0 || strcmp(tag, "creator") == 0 ||
                              strcmp(tag, "language") == 0)) {
        *c->capture = collapse_space(c->text.data ? c->text.data : "", c->text.len);
        c->capture = NULL;
    }
}

static void XMLCALL opf_text(void *ud, const XML_Char *s, int len)
{
    opf_ctx_t *c = ud;
    if (c->capture) buf_append(&c->text, s, (size_t)len);
}

static const item_t *find_item(const opf_ctx_t *c, const char *id)
{
    for (int i = 0; id && i < c->item_count; i++) {
        if (c->items[i].id && strcmp(c->items[i].id, id) == 0) return &c->items[i];
    }
    return NULL;
}

/* --- Table of contents, EPUB 2: the NCX ----------------------------- */

typedef struct {
    epub_t     *book;
    const char *base;
    int         depth;      /* navPoint nesting */
    bool        in_label;
    bool        in_text;
    buf_t       label;
    char       *src;
} ncx_ctx_t;

static void XMLCALL ncx_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    ncx_ctx_t *c = ud;
    const char *tag = local_name(name);

    if (strcmp(tag, "navPoint") == 0) {
        c->depth++;
        buf_reset(&c->label);
        epub_free(c->src);
        c->src = NULL;
    } else if (strcmp(tag, "navLabel") == 0) {
        c->in_label = true;
    } else if (strcmp(tag, "text") == 0 && c->in_label) {
        c->in_text = true;
    } else if (strcmp(tag, "content") == 0 && c->depth > 0) {
        epub_free(c->src);
        c->src = dup_str(attr(atts, "src"));
    }
}

static void XMLCALL ncx_end(void *ud, const XML_Char *name)
{
    ncx_ctx_t *c = ud;
    const char *tag = local_name(name);

    if (strcmp(tag, "text") == 0) {
        c->in_text = false;
    } else if (strcmp(tag, "navLabel") == 0) {
        c->in_label = false;
    } else if (strcmp(tag, "content") == 0 && c->depth > 0) {
        /* The NCX puts a navPoint's label before its content. */
        add_toc_entry(c->book, c->base, &c->label, c->src, c->depth);
        buf_reset(&c->label);
        epub_free(c->src);
        c->src = NULL;
    } else if (strcmp(tag, "navPoint") == 0 && c->depth > 0) {
        c->depth--;
    }
}

static void XMLCALL ncx_text(void *ud, const XML_Char *s, int len)
{
    ncx_ctx_t *c = ud;
    if (c->in_text) buf_append(&c->label, s, (size_t)len);
}

/* --- Table of contents, EPUB 3: the navigation document ------------- */

typedef struct {
    epub_t     *book;
    const char *base;
    int         depth;      /* element nesting */
    int         toc_depth;  /* depth of the <nav epub:type="toc">, 0 outside it */
    bool        done;       /* the toc nav has been read */
    int         ol_depth;
    bool        in_link;
    buf_t       label;
    char       *href;
} nav_ctx_t;

static void XMLCALL nav_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    nav_ctx_t *c = ud;
    const char *tag = local_name(name);
    c->depth++;

    if (!c->toc_depth) {
        if (!c->done && strcmp(tag, "nav") == 0 &&
            (has_word(attr(atts, "epub:type"), "toc") || has_word(attr(atts, "type"), "toc"))) {
            c->toc_depth = c->depth;
        }
        return;
    }
    if (strcmp(tag, "ol") == 0) {
        c->ol_depth++;
    } else if (strcmp(tag, "a") == 0) {
        c->in_link = true;
        buf_reset(&c->label);
        epub_free(c->href);
        c->href = dup_str(attr(atts, "href"));
    }
}

static void XMLCALL nav_end(void *ud, const XML_Char *name)
{
    nav_ctx_t *c = ud;
    const char *tag = local_name(name);

    if (c->toc_depth) {
        if (c->depth == c->toc_depth) {
            c->toc_depth = 0;
            c->done = true;
        } else if (strcmp(tag, "a") == 0 && c->in_link) {
            add_toc_entry(c->book, c->base, &c->label, c->href, c->ol_depth);
            c->in_link = false;
        } else if (strcmp(tag, "ol") == 0 && c->ol_depth > 0) {
            c->ol_depth--;
        }
    }
    c->depth--;
}

static void XMLCALL nav_text(void *ud, const XML_Char *s, int len)
{
    nav_ctx_t *c = ud;
    if (c->in_link) buf_append(&c->label, s, (size_t)len);
}

static void read_toc(epub_t *book, const item_t *item, bool is_nav)
{
    if (!item || !item->path) return;
    size_t len = 0;
    char *xml = epub_zip_read(book->zip, item->path, &len);
    char *base = dir_of(item->path);
    if (!xml || !base) {
        epub_free(xml);
        epub_free(base);
        return;
    }

    if (is_nav) {
        nav_ctx_t c = {.book = book, .base = base};
        XML_Parser p = new_parser(&c);
        if (p) {
            XML_SetElementHandler(p, nav_start, nav_end);
            XML_SetCharacterDataHandler(p, nav_text);
            run_parser(p, xml, len, item->path);
            XML_ParserFree(p);
        }
        buf_free(&c.label);
        epub_free(c.href);
    } else {
        ncx_ctx_t c = {.book = book, .base = base};
        XML_Parser p = new_parser(&c);
        if (p) {
            XML_SetElementHandler(p, ncx_start, ncx_end);
            XML_SetCharacterDataHandler(p, ncx_text);
            run_parser(p, xml, len, item->path);
            XML_ParserFree(p);
        }
        buf_free(&c.label);
        epub_free(c.src);
    }
    epub_free(xml);
    epub_free(base);
}

static void free_opf_ctx(opf_ctx_t *c)
{
    for (int i = 0; i < c->item_count; i++) {
        epub_free(c->items[i].id);
        epub_free(c->items[i].path);
        epub_free(c->items[i].media_type);
        epub_free(c->items[i].properties);
    }
    for (int i = 0; i < c->spine_count; i++) epub_free(c->spine[i]);
    epub_free(c->items);
    epub_free(c->spine);
    epub_free(c->ncx_id);
    buf_free(&c->text);
}

static bool read_package(epub_t *book, const char *opf_path)
{
    size_t len = 0;
    char *xml = epub_zip_read(book->zip, opf_path, &len);
    char *base = dir_of(opf_path);
    if (!xml || !base) {
        ESP_LOGW(TAG, "no package document at %s", opf_path);
        epub_free(xml);
        epub_free(base);
        return false;
    }

    opf_ctx_t c = {.book = book, .base = base};
    XML_Parser p = new_parser(&c);
    bool parsed = false;
    if (p) {
        XML_SetElementHandler(p, opf_start, opf_end);
        XML_SetCharacterDataHandler(p, opf_text);
        parsed = run_parser(p, xml, len, opf_path);
        XML_ParserFree(p);
    }
    epub_free(xml);

    /* The spine, kept to the documents a reader can show: a spine may also
     * list an image or an SVG page. */
    if (parsed && c.spine_count > 0) {
        book->chapters = epub_malloc(sizeof(char *) * (size_t)c.spine_count);
    }
    for (int i = 0; book->chapters && i < c.spine_count; i++) {
        const item_t *it = find_item(&c, c.spine[i]);
        if (it && it->path && it->media_type && strstr(it->media_type, "html")) {
            book->chapters[book->chapter_count++] = dup_str(it->path);
        }
    }

    /* Where each chapter begins in the book, by its document's size. */
    if (book->chapter_count > 0) {
        book->chapter_at = epub_malloc(sizeof(uint64_t) * (size_t)(book->chapter_count + 1));
    }
    if (book->chapter_at) {
        book->chapter_at[0] = 0;
        for (int i = 0; i < book->chapter_count; i++) {
            size_t size = book->chapters[i] ? epub_zip_size(book->zip, book->chapters[i]) : 0;
            book->chapter_at[i + 1] = book->chapter_at[i] + size;
        }
    }

    /* The EPUB 3 navigation document if there is one, else the NCX. */
    if (book->chapter_count > 0) {
        const item_t *nav = NULL, *ncx = find_item(&c, c.ncx_id);
        for (int i = 0; i < c.item_count; i++) {
            const item_t *it = &c.items[i];
            if (!nav && has_word(it->properties, "nav")) nav = it;
            if (!ncx && it->media_type && strcmp(it->media_type, "application/x-dtbncx+xml") == 0) {
                ncx = it;
            }
        }
        read_toc(book, nav, true);
        if (book->toc_count == 0) read_toc(book, ncx, false);
    }

    free_opf_ctx(&c);
    epub_free(base);
    return book->chapter_count > 0;
}

epub_t *epub_open(const char *path)
{
    epub_t *book = epub_malloc(sizeof(*book));
    if (!book) return NULL;
    memset(book, 0, sizeof(*book));

    book->zip = epub_zip_open(path);
    char *opf_path = book->zip ? read_opf_path(book->zip) : NULL;
    if (!opf_path || !read_package(book, opf_path)) {
        ESP_LOGW(TAG, "%s is not a readable EPUB", path);
        epub_free(opf_path);
        epub_close(book);
        return NULL;
    }
    epub_free(opf_path);
    ESP_LOGI(TAG, "%s: \"%s\" by \"%s\", %d chapters, %d contents entries, language '%s'",
             path, epub_title(book), epub_author(book), book->chapter_count, book->toc_count,
             epub_language(book));
    return book;
}

void epub_close(epub_t *book)
{
    if (!book) return;
    epub_zip_close(book->zip);
    epub_free(book->title);
    epub_free(book->author);
    epub_free(book->language);
    for (int i = 0; i < book->chapter_count; i++) epub_free(book->chapters[i]);
    epub_free(book->chapters);
    epub_free(book->chapter_at);
    for (int i = 0; i < book->toc_count; i++) {
        epub_free((char *)book->toc[i].title);
        epub_free((char *)book->toc[i].anchor);
    }
    epub_free(book->toc);
    epub_free(book);
}

const char *epub_title(const epub_t *book)
{
    return book && book->title ? book->title : "";
}

const char *epub_author(const epub_t *book)
{
    return book && book->author ? book->author : "";
}

const char *epub_language(const epub_t *book)
{
    return book && book->language ? book->language : "";
}

int epub_chapter_count(const epub_t *book)
{
    return book ? book->chapter_count : 0;
}

int epub_permille(const epub_t *book, int chapter, size_t offset, size_t len)
{
    if (!book || book->chapter_count <= 0 || chapter < 0) return 0;
    if (chapter >= book->chapter_count) return 1000;
    if (offset > len) offset = len;

    /* A book whose sizes cannot be had counts its chapters alike. */
    uint64_t total = book->chapter_at ? book->chapter_at[book->chapter_count] : 0;
    uint64_t start = total ? book->chapter_at[chapter] : (uint64_t)chapter;
    uint64_t size  = total ? book->chapter_at[chapter + 1] - start : 1;
    if (!total) total = (uint64_t)book->chapter_count;

    uint64_t at = start + (len ? size * offset / len : 0);
    return (int)(at * 1000 / total);
}

int epub_toc_count(const epub_t *book)
{
    return book ? book->toc_count : 0;
}

const epub_toc_entry_t *epub_toc_get(const epub_t *book, int index)
{
    if (!book || index < 0 || index >= book->toc_count) return NULL;
    return &book->toc[index];
}

char *epub_chapter_text(epub_t *book, int chapter, const char *const *anchors,
                        size_t anchor_count, size_t *anchor_at, size_t *len_out)
{
    if (!book || chapter < 0 || chapter >= book->chapter_count) return NULL;
    size_t len = 0;
    char *html = epub_zip_read(book->zip, book->chapters[chapter], &len);
    if (!html) return NULL;
    char *text = epub_html_text(html, len, anchors, anchor_count, anchor_at, len_out);
    epub_free(html);
    return text;
}

/* ------------------------------------------------------------------ */
/* A chapter's text                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    buf_t       out;
    int         depth;
    int         skip_depth;   /* depth of the element being skipped, 0 if none */
    bool        space;        /* white space since the last character */
    int         breaks;       /* 1: a line break is due, 2: a paragraph break */
    bool        bullet;       /* a list item has begun and has no text yet */
    const char *const *anchors;   /* the ids being looked for */
    size_t      anchor_count;
    size_t     *anchor_at;        /* where each one's text begins */
    uint8_t    *anchor_state;     /* per anchor: ANCHOR_* */
    size_t      anchors_pending;  /* how many are ANCHOR_PENDING */
} text_ctx_t;

enum { ANCHOR_UNSEEN, ANCHOR_PENDING, ANCHOR_PLACED };

/* An element begins: each anchor naming it is placed where the next visible
 * character goes. An <a name=""> is an anchor in older books. */
static void note_anchor(text_ctx_t *c, const char *id)
{
    if (!id || !c->anchor_state) return;
    for (size_t i = 0; i < c->anchor_count; i++) {
        if (c->anchor_state[i] == ANCHOR_UNSEEN && c->anchors[i] &&
            strcmp(id, c->anchors[i]) == 0) {
            c->anchor_state[i] = ANCHOR_PENDING;
            c->anchors_pending++;
        }
    }
}

/* Places the anchors still waiting for text at `at`. */
static void place_anchors(text_ctx_t *c, size_t at)
{
    for (size_t i = 0; c->anchors_pending > 0 && i < c->anchor_count; i++) {
        if (c->anchor_state[i] == ANCHOR_PENDING) {
            c->anchor_state[i] = ANCHOR_PLACED;
            c->anchor_at[i] = at;
            c->anchors_pending--;
        }
    }
}

/* Elements whose content is not part of the text. */
static bool is_skipped(const char *tag)
{
    static const char *const SKIPPED[] = {"head", "script", "style", "svg", "math"};
    for (size_t i = 0; i < sizeof(SKIPPED) / sizeof(SKIPPED[0]); i++) {
        if (strcasecmp(tag, SKIPPED[i]) == 0) return true;
    }
    return false;
}

/* Elements that begin and end a paragraph. CrossPoint's list is p, li, div,
 * br and blockquote, with the headings; its CSS handles the rest. */
static bool is_block(const char *tag)
{
    static const char *const BLOCKS[] = {
        "p", "div", "li", "ul", "ol", "dl", "dt", "dd", "blockquote",
        "h1", "h2", "h3", "h4", "h5", "h6", "hr", "pre", "section",
        "article", "aside", "header", "footer", "nav", "figure",
        "figcaption", "table", "tr", "caption", "address", "center",
        "main", "body",
    };
    for (size_t i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++) {
        if (strcasecmp(tag, BLOCKS[i]) == 0) return true;
    }
    return false;
}

static void request_break(text_ctx_t *c, int kind)
{
    if (kind > c->breaks) c->breaks = kind;
}

/* Appends one visible character's bytes, preceded by whatever break or space
 * is due. Nothing is due at the very start, so the text never begins or ends
 * with white space. */
static void put_visible(text_ctx_t *c, const char *s, size_t n)
{
    if (c->out.len > 0) {
        if (c->breaks == 2) {
            buf_append(&c->out, "\n\n", 2);
        } else if (c->breaks == 1) {
            buf_append(&c->out, "\n", 1);
        } else if (c->space) {
            buf_append(&c->out, " ", 1);
        }
    }
    if (c->anchors_pending) place_anchors(c, c->out.len);
    if (c->bullet) {
        buf_append(&c->out, "\xE2\x80\xA2 ", 4);
        c->bullet = false;
    }
    c->breaks = 0;
    c->space  = false;
    buf_append(&c->out, s, n);
}

/* Text as the document has it: white space collapses, a no-break space counts
 * as white space (a paragraph of one is a spacer, not text), and invisible
 * marks with no glyph are dropped. */
static void put_text(text_ctx_t *c, const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        if (is_xml_space(s[i])) {
            c->space = true;
            i++;
        } else if (u[i] == 0xC2 && i + 1 < n && u[i + 1] == 0xA0) {   /* no-break space */
            c->space = true;
            i += 2;
        } else if (u[i] == 0xC2 && i + 1 < n && u[i + 1] == 0xAD) {   /* soft hyphen */
            i += 2;
        } else if (u[i] == 0xE2 && i + 2 < n && u[i + 1] == 0x80 &&
                   u[i + 2] >= 0x8B && u[i + 2] <= 0x8F) {            /* U+200B..U+200F */
            i += 3;
        } else if (u[i] == 0xEF && i + 2 < n && u[i + 1] == 0xBB && u[i + 2] == 0xBF) {
            i += 3;                                                   /* byte-order mark */
        } else {
            size_t len = 1;
            if (u[i] >= 0xF0) len = 4;
            else if (u[i] >= 0xE0) len = 3;
            else if (u[i] >= 0xC0) len = 2;
            if (i + len > n) len = n - i;
            put_visible(c, s + i, len);
            i += len;
        }
    }
}

static void open_element(text_ctx_t *c, const char *tag)
{
    if (strcasecmp(tag, "br") == 0) {
        request_break(c, 1);
    } else if (is_block(tag)) {
        request_break(c, 2);
        if (strcasecmp(tag, "li") == 0) c->bullet = true;
    }
}

static void close_element(text_ctx_t *c, const char *tag)
{
    if (is_block(tag)) {
        request_break(c, 2);
    } else if (strcasecmp(tag, "td") == 0 || strcasecmp(tag, "th") == 0) {
        c->space = true;
    }
}

static void XMLCALL text_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    text_ctx_t *c = ud;
    const char *tag = local_name(name);
    note_anchor(c, attr(atts, "id"));
    if (strcasecmp(tag, "a") == 0) note_anchor(c, attr(atts, "name"));

    c->depth++;
    if (c->skip_depth) return;

    if (is_skipped(tag)) {
        c->skip_depth = c->depth;
        return;
    }
    /* An image of one letter is a drop cap: without it "NOT all that" reads
     * "OT all that". A longer description is not part of the prose. */
    if (strcasecmp(tag, "img") == 0) {
        const char *alt = attr(atts, "alt");
        if (alt && alt[0] && utf8_length(alt) == 1) put_text(c, alt, strlen(alt));
        return;
    }
    open_element(c, tag);
}

static void XMLCALL text_end(void *ud, const XML_Char *name)
{
    text_ctx_t *c = ud;
    if (c->skip_depth) {
        if (c->skip_depth == c->depth) c->skip_depth = 0;
    } else {
        close_element(c, local_name(name));
    }
    c->depth--;
}

static void XMLCALL text_chars(void *ud, const XML_Char *s, int len)
{
    text_ctx_t *c = ud;
    if (!c->skip_depth) put_text(c, s, (size_t)len);
}

/* Entity references XML does not define, such as &nbsp;, which Expat passes
 * here, as CrossPoint relies on. */
static void XMLCALL text_default(void *ud, const XML_Char *s, int len)
{
    if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
        const char *value = html_entity_lookup(s, (size_t)len);
        text_chars(ud, value ? value : s, value ? (int)strlen(value) : len);
    }
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | cp >> 18);
        out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* The entity at `s` ("&amp;", "&#8217;", "&#x2019;"), put as text. Returns
 * the bytes it took, or 0 if `s` does not begin with one. */
static size_t put_entity(text_ctx_t *c, const char *s, size_t n)
{
    const char *semi = memchr(s, ';', n < 12 ? n : 12);
    if (!semi) return 0;
    size_t len = (size_t)(semi - s) + 1;

    if (len > 3 && s[1] == '#') {
        bool hex = s[2] == 'x' || s[2] == 'X';
        uint32_t cp = 0;
        for (size_t i = hex ? 3 : 2; i < len - 1; i++) {
            int d = hex ? hex_value(s[i]) : (isdigit((unsigned char)s[i]) ? s[i] - '0' : -1);
            if (d < 0 || cp > 0x10FFFF) return 0;
            cp = cp * (hex ? 16 : 10) + (uint32_t)d;
        }
        char utf8[4];
        size_t ulen = utf8_encode(cp, utf8);
        if (ulen == 0) return 0;
        put_text(c, utf8, ulen);
        return len;
    }
    if (len == 6 && strncmp(s, "&apos;", 6) == 0) {
        put_text(c, "'", 1);
        return len;
    }
    const char *value = html_entity_lookup(s, len);
    if (!value) return 0;
    put_text(c, value, strlen(value));
    return len;
}

static const char *find_ci(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        if (strncasecmp(s + i, needle, m) == 0) return s + i;
    }
    return NULL;
}

/* Whether the tag text `tag` (from '<' to '>') has attribute `name` with
 * exactly the value `value`, in either kind of quotes. */
static bool tag_has_value(const char *tag, size_t n, const char *name, const char *value)
{
    size_t name_len = strlen(name), value_len = strlen(value);
    for (size_t i = 1; i + name_len + 2 + value_len < n; i++) {
        if (!isspace((unsigned char)tag[i - 1])) continue;
        if (strncasecmp(tag + i, name, name_len) != 0 || tag[i + name_len] != '=') continue;
        char quote = tag[i + name_len + 1];
        const char *v = tag + i + name_len + 2;
        if ((quote == '"' || quote == '\'') && strncmp(v, value, value_len) == 0 &&
            v[value_len] == quote) {
            return true;
        }
    }
    return false;
}

/* The text of HTML Expat would not take, such as a <br> never closed: tags
 * are dropped and read for where paragraphs break, as Expat's would be. */
static void strip_markup(text_ctx_t *c, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&') {
            size_t used = put_entity(c, s + i, n - i);
            if (used) {
                i += used;
                continue;
            }
        }
        if (s[i] != '<') {
            /* The whole run to the next tag or entity, so that put_text()
             * sees each character's bytes together. */
            size_t run = (s[i] == '&') ? 1 : 0;
            while (i + run < n && s[i + run] != '<' && s[i + run] != '&') run++;
            put_text(c, s + i, run);
            i += run;
            continue;
        }

        if (n - i >= 4 && strncmp(s + i, "<!--", 4) == 0) {
            const char *end = find_ci(s + i + 4, n - i - 4, "-->");
            i = end ? (size_t)(end - s) + 3 : n;
            continue;
        }

        size_t j = i + 1;
        bool closing = j < n && s[j] == '/';
        if (closing) j++;
        size_t name_start = j;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-')) j++;
        char tag[16];
        size_t tag_len = j - name_start < sizeof(tag) - 1 ? j - name_start : sizeof(tag) - 1;
        memcpy(tag, s + name_start, tag_len);
        tag[tag_len] = '\0';

        const char *gt = memchr(s + j, '>', n - j);
        size_t after = gt ? (size_t)(gt - s) + 1 : n;
        bool self_closing = gt && gt > s + j && gt[-1] == '/';

        if (tag_len > 0) {
            const char *name = local_name(tag);
            for (size_t k = 0; !closing && c->anchor_state && k < c->anchor_count; k++) {
                const char *id = c->anchors[k];
                if (id && (tag_has_value(s + i, after - i, "id", id) ||
                           (strcasecmp(name, "a") == 0 && tag_has_value(s + i, after - i, "name", id)))) {
                    note_anchor(c, id);
                }
            }
            if (!closing && !self_closing && is_skipped(name)) {
                char close[20];
                snprintf(close, sizeof(close), "</%s", name);
                const char *end = find_ci(s + after, n - after, close);
                const char *end_gt = end ? memchr(end, '>', (size_t)(s + n - end)) : NULL;
                i = end_gt ? (size_t)(end_gt - s) + 1 : n;
                continue;
            }
            if (closing) {
                close_element(c, name);
            } else {
                open_element(c, name);
            }
        }
        i = after;
    }
}

char *epub_html_text(const char *html, size_t len, const char *const *anchors,
                     size_t anchor_count, size_t *anchor_at, size_t *len_out)
{
    uint8_t *state = NULL;
    if (anchors && anchor_at && anchor_count > 0) {
        state = epub_malloc(anchor_count);
        if (!state) return NULL;
    }
    const text_ctx_t fresh = {
        .anchors = anchors, .anchor_count = anchor_count,
        .anchor_at = anchor_at, .anchor_state = state,
    };
    if (state) memset(state, ANCHOR_UNSEEN, anchor_count);

    text_ctx_t c = fresh;
    XML_Parser p = new_parser(&c);
    bool parsed = false;
    if (p) {
        XML_SetElementHandler(p, text_start, text_end);
        XML_SetCharacterDataHandler(p, text_chars);
        XML_SetDefaultHandlerExpand(p, text_default);
        parsed = run_parser(p, html, len, "chapter");
        XML_ParserFree(p);
    }
    if (!parsed) {
        buf_free(&c.out);
        c = fresh;
        if (state) memset(state, ANCHOR_UNSEEN, anchor_count);
        strip_markup(&c, html, len);
    }
    if (state) {
        /* One with no text after it is at the end; one never seen, at 0. */
        place_anchors(&c, c.out.len);
        for (size_t i = 0; i < anchor_count; i++) {
            if (state[i] == ANCHOR_UNSEEN) anchor_at[i] = 0;
        }
        epub_free(state);
    }
    return buf_take(&c.out, len_out);
}
