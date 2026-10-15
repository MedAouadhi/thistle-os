/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: the paths the web page names.
 *
 * The page names a file by where it is on the device: "/sdcard/notes/a.txt",
 * "/spiffs/config/system.json". Everything it sends goes through
 * xfer_path_parse() before the file system sees it, which is what keeps a
 * request inside the two roots. Nothing here touches a file.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A path as the page names it, NUL included. FAT allows longer; the URI it
 * travels in does not (CONFIG_HTTPD_MAX_URI_LEN). */
#define XFER_PATH_MAX 256

/* SPIFFS keeps a file's whole name, "/config/system.json", in
 * CONFIG_SPIFFS_OBJ_NAME_LEN bytes, NUL included. */
#define XFER_SPIFFS_NAME_MAX 31

typedef enum {
    XFER_ROOT_NONE = -1,   /* "/", above both */
    XFER_ROOT_SDCARD = 0,
    XFER_ROOT_SPIFFS,
    XFER_ROOT_COUNT,
} xfer_root_t;

typedef struct {
    xfer_root_t root;
    char        path[XFER_PATH_MAX];  /* normalised: "/sdcard/notes", "/" */
    size_t      rest_at;              /* where the part past the root starts */
} xfer_path_t;

/* The path past its root: "/notes" for "/sdcard/notes", "" for "/sdcard". */
static inline const char *xfer_path_rest(const xfer_path_t *p)
{
    return p->path + p->rest_at;
}

/* The root's name in a path: "sdcard", "spiffs". */
const char *xfer_root_name(xfer_root_t root);

/* Decodes a query string value: "%2F" and "+" as a query string writes '/'
 * and ' '. False if a '%' is not followed by two hex digits, it decodes to a
 * NUL, or it does not fit in `cap` bytes. */
bool xfer_url_decode(const char *src, char *out, size_t cap);

/* Checks a path the page sent, already decoded, and normalises it: repeated
 * '/' made one, a trailing '/' dropped. False for anything that is not
 * absolute, names a root other than /sdcard or /spiffs, has a "." or ".."
 * part, a control character, a '\\', or a character FAT forbids
 * (: * ? " < > |), a part ending in '.' or ' ', or is too long: XFER_PATH_MAX
 * in all, XFER_SPIFFS_NAME_MAX past /spiffs. "/" alone parses, as
 * XFER_ROOT_NONE. */
bool xfer_path_parse(const char *in, xfer_path_t *out);

/* Checks a name for a file or folder, as a rename gives it: one part of a
 * path, by the rules above. */
bool xfer_name_ok(const char *name);

/* The last part of a path: "a.txt" for "/sdcard/notes/a.txt". */
const char *xfer_basename(const char *path);

/* The Content-Type a file is served with to be looked at in the browser:
 * text as plain text, whatever it is (an .html file is shown, not run),
 * common pictures as themselves except SVG, which can carry script, and
 * anything else as bytes. */
const char *xfer_content_type(const char *name);

/* A Content-Disposition header that saves the file under its own name:
 * attachment; filename="a.txt"; filename*=UTF-8''a.txt. The plain filename
 * has anything but printable ASCII, '"' and '\\' as '_'. False if it does not
 * fit. */
bool xfer_content_disposition(const char *name, char *out, size_t cap);

/* Writes `s` into `out` as the inside of a JSON string: '"', '\\' and
 * control characters escaped. False if it does not fit. */
bool xfer_json_escape(const char *s, char *out, size_t cap);
