/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the paths the web page names.
 */
#include "filexfer/xfer_path.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const ROOT_NAMES[XFER_ROOT_COUNT] = {"sdcard", "spiffs"};

const char *xfer_root_name(xfer_root_t root)
{
    return root >= 0 && root < XFER_ROOT_COUNT ? ROOT_NAMES[root] : "";
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool xfer_url_decode(const char *src, char *out, size_t cap)
{
    if (!src || !out || cap == 0) return false;
    size_t n = 0;
    for (const char *p = src; *p; p++) {
        char c = *p;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = hex_value(p[1]);
            int lo = hi < 0 ? -1 : hex_value(p[2]);
            if (lo < 0) return false;
            c = (char)(hi << 4 | lo);
            if (c == '\0') return false;
            p += 2;
        }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

static bool part_ok(const char *part, size_t len)
{
    if (len == 0) return false;
    if (len == 1 && part[0] == '.') return false;
    if (len == 2 && part[0] == '.' && part[1] == '.') return false;
    /* FAT drops a trailing dot or space, so "a." would name "a". */
    if (part[len - 1] == '.' || part[len - 1] == ' ') return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)part[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (strchr("\\:*?\"<>|/", c)) return false;
    }
    return true;
}

bool xfer_name_ok(const char *name)
{
    return name && part_ok(name, strlen(name)) && strlen(name) < XFER_PATH_MAX;
}

bool xfer_path_parse(const char *in, xfer_path_t *out)
{
    if (!in || !out || in[0] != '/') return false;

    char norm[XFER_PATH_MAX];
    size_t n = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!part_ok(p, len)) return false;
        if (n + 1 + len + 1 > sizeof norm) return false;
        norm[n++] = '/';
        memcpy(norm + n, p, len);
        n += len;
        p += len;
    }
    norm[n] = '\0';

    if (n == 0) {
        out->root = XFER_ROOT_NONE;
        strcpy(out->path, "/");
        out->rest_at = 1;
        return true;
    }

    xfer_root_t root = XFER_ROOT_NONE;
    size_t root_len = 0;
    for (int r = 0; r < XFER_ROOT_COUNT; r++) {
        size_t len = strlen(ROOT_NAMES[r]);
        if (strncmp(norm + 1, ROOT_NAMES[r], len) == 0 &&
            (norm[1 + len] == '/' || norm[1 + len] == '\0')) {
            root = (xfer_root_t)r;
            root_len = 1 + len;
            break;
        }
    }
    if (root == XFER_ROOT_NONE) return false;
    if (root == XFER_ROOT_SPIFFS && n - root_len > XFER_SPIFFS_NAME_MAX) return false;

    out->root = root;
    memcpy(out->path, norm, n + 1);
    out->rest_at = root_len;
    return true;
}

const char *xfer_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    for (; *exts; exts++) {
        if (strcasecmp(dot + 1, *exts) == 0) return true;
    }
    return false;
}

const char *xfer_content_type(const char *name)
{
    static const char *const text[] = {
        "txt", "md", "json", "csv", "log", "ini", "cfg", "conf", "toml", "yaml",
        "yml", "xml", "html", "htm", "css", "js", "c", "h", "py", "rs", "sh",
        "vcf", "jsonl", "srt", NULL,
    };
    static const struct { const char *ext; const char *type; } pictures[] = {
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"bmp", "image/bmp"}, {"webp", "image/webp"},
    };

    if (has_ext(name, text)) return "text/plain; charset=utf-8";
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        for (size_t i = 0; i < sizeof pictures / sizeof pictures[0]; i++) {
            if (strcasecmp(dot + 1, pictures[i].ext) == 0) return pictures[i].type;
        }
    }
    return "application/octet-stream";
}

bool xfer_content_disposition(const char *name, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    int head = snprintf(out, cap, "attachment; filename=\"");
    if (head < 0 || (size_t)head >= cap) return false;
    size_t n = (size_t)head;

    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (n + 1 >= cap) return false;
        out[n++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '_';
    }
    const char *mid = "\"; filename*=UTF-8''";
    size_t mid_len = strlen(mid);
    if (n + mid_len >= cap) return false;
    memcpy(out + n, mid, mid_len);
    n += mid_len;

    /* RFC 5987's attr-char, which is all that goes unencoded. */
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     strchr("!#$&+-.^_`|~", c);
        if (plain && c) {
            if (n + 1 >= cap) return false;
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap) return false;
            out[n++] = '%';
            out[n++] = HEX[c >> 4];
            out[n++] = HEX[c & 15];
        }
    }
    out[n] = '\0';
    return true;
}

bool xfer_json_escape(const char *s, char *out, size_t cap)
{
    static const char HEX[] = "0123456789abcdef";
    size_t n = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char esc = 0;
        switch (c) {
        case '"': esc = '"'; break;
        case '\\': esc = '\\'; break;
        case '\n': esc = 'n'; break;
        case '\r': esc = 'r'; break;
        case '\t': esc = 't'; break;
        default: break;
        }
        if (esc) {
            if (n + 2 >= cap) return false;
            out[n++] = '\\';
            out[n++] = esc;
        } else if (c < 0x20) {
            if (n + 6 >= cap) return false;
            memcpy(out + n, "\\u00", 4);
            out[n + 4] = HEX[c >> 4];
            out[n + 5] = HEX[c & 15];
            n += 6;
        } else {
            if (n + 1 >= cap) return false;
            out[n++] = (char)c;
        }
    }
    if (n >= cap) return false;
    out[n] = '\0';
    return true;
}
