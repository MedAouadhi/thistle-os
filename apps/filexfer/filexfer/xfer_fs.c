/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — File Transfer: what the web page can do to files.
 */
#include "filexfer/xfer_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    char path[128];
    bool flat;
} s_mounts[XFER_ROOT_COUNT] = {
    [XFER_ROOT_SDCARD] = {"/sdcard", false},
    [XFER_ROOT_SPIFFS] = {"/spiffs", true},
};

const char *xfer_fs_mount(xfer_root_t root)
{
    return root >= 0 && root < XFER_ROOT_COUNT ? s_mounts[root].path : "";
}

void xfer_fs_set_mount(xfer_root_t root, const char *path, bool flat)
{
    if (root < 0 || root >= XFER_ROOT_COUNT || !path) return;
    snprintf(s_mounts[root].path, sizeof s_mounts[root].path, "%s", path);
    s_mounts[root].flat = flat;
}

bool xfer_fs_flat(xfer_root_t root)
{
    return root >= 0 && root < XFER_ROOT_COUNT && s_mounts[root].flat;
}

bool xfer_fs_real(const xfer_path_t *p, char *out, size_t cap)
{
    if (p->root == XFER_ROOT_NONE) return false;
    int n = snprintf(out, cap, "%s%s", xfer_fs_mount(p->root), xfer_path_rest(p));
    return n >= 0 && (size_t)n < cap;
}

/* What the call that just failed said, or `fallback` if it said nothing. */
static int failed(int fallback)
{
    return errno ? errno : fallback;
}

static bool tmp_path(xfer_root_t root, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/" XFER_UPLOAD_TMP, xfer_fs_mount(root));
    return n >= 0 && (size_t)n < cap;
}

bool xfer_flat_entry(const char *name, xfer_seen_t *seen, char *part, size_t cap, bool *dir)
{
    while (*name == '/') name++;
    if (!*name || cap == 0) return false;

    const char *slash = strchr(name, '/');
    size_t len = slash ? (size_t)(slash - name) : strlen(name);
    if (len + 1 > cap) return false;
    memcpy(part, name, len);
    part[len] = '\0';
    *dir = slash != NULL;
    if (!*dir) return true;

    for (int i = 0; i < seen->count; i++) {
        if (strcmp(seen->names[i], part) == 0) return false;
    }
    /* Past XFER_FLAT_DIRS a folder may be shown twice, which is all that
     * running out of room costs. */
    if (seen->count < XFER_FLAT_DIRS && len < sizeof seen->names[0]) {
        memcpy(seen->names[seen->count++], part, len + 1);
    }
    return true;
}

static bool is_mount(const xfer_path_t *p)
{
    return p->root != XFER_ROOT_NONE && xfer_path_rest(p)[0] == '\0';
}

int xfer_fs_list(const xfer_path_t *dir, xfer_entry_fn fn, void *ctx)
{
    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(dir, real, sizeof real)) return ENAMETOOLONG;

    /* stat() fails on a FAT volume's root, so only below it. */
    struct stat st;
    if (!is_mount(dir) && !xfer_fs_flat(dir->root) && stat(real, &st) == 0 && !S_ISDIR(st.st_mode)) {
        return ENOTDIR;
    }
    errno = 0;
    DIR *d = opendir(real);
    if (!d) return failed(ENOENT);
    if (!fn) {
        closedir(d);
        return 0;
    }

    bool flat = xfer_fs_flat(dir->root);
    /* Off the stack: the server's is small. */
    xfer_seen_t *seen = flat ? calloc(1, sizeof *seen) : NULL;
    if (flat && !seen) {
        closedir(d);
        return ENOMEM;
    }
    char full[XFER_PATH_MAX + 128 + 256];
    char part[256];
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (is_mount(dir) && strcmp(name, XFER_UPLOAD_TMP) == 0) continue;

        xfer_entry_t e = {.name = name};
        if (flat) {
            if (!xfer_flat_entry(name, seen, part, sizeof part, &e.dir)) continue;
            e.name = part;
        }
        if (!e.dir) {
            snprintf(full, sizeof full, "%s/%s", real, e.name);
            if (stat(full, &st) == 0) {
                e.dir = S_ISDIR(st.st_mode);
                e.size = e.dir ? 0 : (int64_t)st.st_size;
                e.mtime = (int64_t)st.st_mtime;
            }
        }
        if (!fn(&e, ctx)) break;
    }
    closedir(d);
    free(seen);
    return 0;
}

int xfer_fs_open_read(const xfer_path_t *p, FILE **out, int64_t *size)
{
    *out = NULL;
    if (p->root == XFER_ROOT_NONE || is_mount(p)) return EISDIR;
    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(p, real, sizeof real)) return ENAMETOOLONG;

    struct stat st;
    if (stat(real, &st) != 0) return ENOENT;
    if (S_ISDIR(st.st_mode)) return EISDIR;
    errno = 0;
    FILE *f = fopen(real, "rb");
    if (!f) return failed(EIO);
    *out = f;
    *size = (int64_t)st.st_size;
    return 0;
}

/* Whether the folder `p` is in is there. */
static bool parent_exists(const xfer_path_t *p)
{
    if (xfer_fs_flat(p->root)) return true;
    const char *slash = strrchr(xfer_path_rest(p), '/');
    if (!slash || slash == xfer_path_rest(p)) return true;   /* at the top of the root */

    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(p, real, sizeof real)) return false;
    real[strlen(xfer_fs_mount(p->root)) + (size_t)(slash - xfer_path_rest(p))] = '\0';
    struct stat st;
    return stat(real, &st) == 0 && S_ISDIR(st.st_mode);
}

int xfer_fs_upload_begin(xfer_upload_t *up, const xfer_path_t *p, bool overwrite)
{
    memset(up, 0, sizeof *up);
    if (p->root == XFER_ROOT_NONE || is_mount(p)) return EINVAL;
    if (strcmp(xfer_path_rest(p) + 1, XFER_UPLOAD_TMP) == 0) return EINVAL;
    if (!xfer_fs_real(p, up->dest, sizeof up->dest) || !tmp_path(p->root, up->tmp, sizeof up->tmp)) {
        return ENAMETOOLONG;
    }

    struct stat st;
    if (stat(up->dest, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return EISDIR;
        if (!overwrite) return EEXIST;
    } else if (!parent_exists(p)) {
        return ENOENT;
    }

    unlink(up->tmp);
    errno = 0;
    up->file = fopen(up->tmp, "wb");
    if (!up->file) return failed(EIO);
    return 0;
}

int xfer_fs_upload_write(xfer_upload_t *up, const void *data, size_t len)
{
    if (!up->file) return EBADF;
    errno = 0;
    if (len && fwrite(data, 1, len, up->file) != len) return failed(ENOSPC);
    return 0;
}

int xfer_fs_upload_commit(xfer_upload_t *up)
{
    if (!up->file) return EBADF;
    int err = 0;
    errno = 0;
    if (fflush(up->file) != 0 || ferror(up->file)) err = failed(EIO);
    if (fclose(up->file) != 0 && !err) err = failed(EIO);
    up->file = NULL;
    if (err) {
        unlink(up->tmp);
        return err;
    }

    /* Neither FAT nor SPIFFS renames over a file that is there. */
    struct stat st;
    errno = 0;
    if (stat(up->dest, &st) == 0 && unlink(up->dest) != 0) {
        err = failed(EIO);
        unlink(up->tmp);
        return err;
    }
    errno = 0;
    if (rename(up->tmp, up->dest) != 0) {
        err = failed(EIO);
        unlink(up->tmp);
        return err;
    }
    return 0;
}

void xfer_fs_upload_abort(xfer_upload_t *up)
{
    if (up->file) {
        fclose(up->file);
        up->file = NULL;
    }
    if (up->tmp[0]) unlink(up->tmp);
}

/* Whether a flat root has a file under the folder `p`. */
static bool flat_has_files(const xfer_path_t *p)
{
    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(p, real, sizeof real)) return false;
    DIR *d = opendir(real);
    if (!d) return false;
    bool any = readdir(d) != NULL;
    closedir(d);
    return any;
}

int xfer_fs_delete(const xfer_path_t *p)
{
    if (p->root == XFER_ROOT_NONE || is_mount(p)) return EINVAL;
    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(p, real, sizeof real)) return ENAMETOOLONG;

    struct stat st;
    if (stat(real, &st) != 0) {
        if (xfer_fs_flat(p->root) && flat_has_files(p)) return ENOTEMPTY;
        return ENOENT;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = 0;
        if (rmdir(real) == 0) return 0;
        int err = failed(EIO);
        /* FAT says EACCES, or EIO, for a folder that is not empty. */
        DIR *d = opendir(real);
        bool empty = true;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
                    empty = false;
                    break;
                }
            }
            closedir(d);
        }
        return empty ? err : ENOTEMPTY;
    }
    errno = 0;
    return unlink(real) == 0 ? 0 : failed(EIO);
}

int xfer_fs_mkdir(const xfer_path_t *p)
{
    if (p->root == XFER_ROOT_NONE || is_mount(p)) return EEXIST;
    if (xfer_fs_flat(p->root)) return 0;
    char real[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(p, real, sizeof real)) return ENAMETOOLONG;

    struct stat st;
    if (stat(real, &st) == 0) return EEXIST;
    if (!parent_exists(p)) return ENOENT;
    errno = 0;
    return mkdir(real, 0755) == 0 ? 0 : failed(EIO);
}

int xfer_fs_rename(const xfer_path_t *from, const char *name, xfer_path_t *to)
{
    if (from->root == XFER_ROOT_NONE || is_mount(from)) return EINVAL;
    if (!xfer_name_ok(name) || strcmp(name, XFER_UPLOAD_TMP) == 0) return EINVAL;

    char want[XFER_PATH_MAX];
    const char *slash = strrchr(from->path, '/');
    int n = snprintf(want, sizeof want, "%.*s/%s", (int)(slash - from->path), from->path, name);
    if (n < 0 || (size_t)n >= sizeof want || !xfer_path_parse(want, to)) return EINVAL;

    char src[XFER_PATH_MAX + 128], dst[XFER_PATH_MAX + 128];
    if (!xfer_fs_real(from, src, sizeof src) || !xfer_fs_real(to, dst, sizeof dst)) {
        return ENAMETOOLONG;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        return xfer_fs_flat(from->root) && flat_has_files(from) ? ENOTSUP : ENOENT;
    }
    if (strcmp(src, dst) == 0) return 0;
    /* A change of case only is the same name to FAT: no clash with itself. */
    if (strcasecmp(src, dst) != 0 && stat(dst, &st) == 0) return EEXIST;
    errno = 0;
    return rename(src, dst) == 0 ? 0 : failed(EIO);
}
