/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: what the web page can do to files.
 *
 * Plain POSIX calls on paths xfer_path_parse() has checked, so the same code
 * runs on the device, in the simulator and in the host tests. Each returns 0
 * or an errno value saying what went wrong.
 *
 * SPIFFS has no folders: a file's name is its whole path, "/config/a.json".
 * Listing /spiffs gives "config/a.json", which this shows as a folder
 * "config", and a folder exists only while a file is in it, so making one
 * does nothing and removing one is refused while it has files, which it
 * always has. A root is "flat" like this only on a device; the simulator's
 * stand-in for SPIFFS is a host folder.
 *
 * A file arrives in a temporary file at the top of its root and replaces the
 * old one only once it is whole, so an upload cut short leaves the old one
 * as it was.
 */
#pragma once

#include "filexfer/xfer_path.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The temporary file an upload is written to, at the top of its root. */
#define XFER_UPLOAD_TMP  ".xfer-upload.tmp"

/* Where the root is on this build: "/sdcard" and "/spiffs" on a device. */
const char *xfer_fs_mount(xfer_root_t root);

/* Puts a root elsewhere, and says whether it is flat: for the simulator and
 * the tests. */
void xfer_fs_set_mount(xfer_root_t root, const char *path, bool flat);

bool xfer_fs_flat(xfer_root_t root);

/* The path on this build's file system. False if it does not fit. */
bool xfer_fs_real(const xfer_path_t *p, char *out, size_t cap);

typedef struct {
    const char *name;
    bool        dir;
    int64_t     size;    /* 0 for a folder */
    int64_t     mtime;   /* Unix seconds, 0 if unknown */
} xfer_entry_t;

/* Called for each entry in a folder; false stops the listing. */
typedef bool (*xfer_entry_fn)(const xfer_entry_t *entry, void *ctx);

/* Calls `fn` for what is in the folder `dir`, in no order, leaving out an
 * upload's temporary file. ENOENT if there is no such folder, ENOTDIR if it
 * is a file. A NULL `fn` only finds out which. */
int xfer_fs_list(const xfer_path_t *dir, xfer_entry_fn fn, void *ctx);

/* The folders a flat root's listing has already shown. */
#define XFER_FLAT_DIRS 32
typedef struct {
    char names[XFER_FLAT_DIRS][XFER_SPIFFS_NAME_MAX + 1];
    int  count;
} xfer_seen_t;

/* For a name a flat root's readdir gave, "a.json" or "sub/b.json": the entry
 * it stands for in the folder listed, "a.json" or the folder "sub", written
 * to `part`. False if it adds nothing: a folder already shown, or an empty
 * name. */
bool xfer_flat_entry(const char *name, xfer_seen_t *seen, char *part, size_t cap, bool *dir);

/* Opens a file to be read, with its size. EISDIR for a folder. */
int xfer_fs_open_read(const xfer_path_t *p, FILE **out, int64_t *size);

typedef struct {
    FILE *file;
    char  tmp[XFER_PATH_MAX + 64];
    char  dest[XFER_PATH_MAX + 64];
} xfer_upload_t;

/* Starts writing `p`. EEXIST if it is there and `overwrite` is false, EISDIR
 * if it is a folder, ENOENT if its folder is not there, EINVAL for a root. */
int xfer_fs_upload_begin(xfer_upload_t *up, const xfer_path_t *p, bool overwrite);
int xfer_fs_upload_write(xfer_upload_t *up, const void *data, size_t len);
/* Puts the file in place of the old one. The upload is over either way. */
int xfer_fs_upload_commit(xfer_upload_t *up);
/* Throws away what was written; the old file stays. */
void xfer_fs_upload_abort(xfer_upload_t *up);

/* Removes a file, or a folder that is empty. ENOTEMPTY for one that is not. */
int xfer_fs_delete(const xfer_path_t *p);

/* Makes a folder. EEXIST if something has the name. Does nothing on a flat
 * root. */
int xfer_fs_mkdir(const xfer_path_t *p);

/* Renames a file or folder within its folder, writing the new path to `to`.
 * EINVAL for a bad name, EEXIST if the name is taken, ENOTSUP for a folder
 * on a flat root. */
int xfer_fs_rename(const xfer_path_t *from, const char *name, xfer_path_t *to);
