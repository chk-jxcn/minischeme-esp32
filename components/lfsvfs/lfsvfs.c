/**
 * @file lfsvfs.c
 * @brief LittleFS block device on an ESP-IDF partition plus the VFS driver
 *        (see lfsvfs.h).
 *
 * littlefs is not thread-safe: every operation holds one mutex (the HTTP
 * server, the serial service and the VM task all use files).
 */

#include "lfsvfs.h"
#include "lfs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LFSV_BLOCK     4096
#define LFSV_CACHE     256
#define LFSV_LOOKAHEAD 32

static const char *TAG = "lfsvfs";

typedef struct {
    bool used;
    lfs_file_t file;
    struct lfs_file_config cfg;
    uint8_t buf[LFSV_CACHE];
} LfsFd;

typedef struct {
    DIR dir;            /* first: the VFS fills in its index */
    lfs_dir_t ld;
    struct dirent de;
} LfsDir;

static lfs_t s_lfs;
static struct lfs_config s_cfg;
static const esp_partition_t *s_part = NULL;
static SemaphoreHandle_t s_mx = NULL;
static LfsFd *s_fds = NULL;
static int s_nfd = 0;
static bool s_mounted = false;
static char s_base[16];
static uint8_t s_rbuf[LFSV_CACHE], s_pbuf[LFSV_CACHE], s_look[LFSV_LOOKAHEAD];

#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

/* ------------------------------------------------------------------------ */
/* Block device                                                             */
/* ------------------------------------------------------------------------ */

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c;
    return esp_partition_read(s_part, (size_t)b * LFSV_BLOCK + off, buf, size) == ESP_OK ? 0 : LFS_ERR_IO;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c;
    return esp_partition_write(s_part, (size_t)b * LFSV_BLOCK + off, buf, size) == ESP_OK ? 0 : LFS_ERR_IO;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    (void)c;
    return esp_partition_erase_range(s_part, (size_t)b * LFSV_BLOCK, LFSV_BLOCK) == ESP_OK ? 0 : LFS_ERR_IO;
}

static int bd_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

static esp_err_t setup(const char *label) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!s_part) return ESP_ERR_NOT_FOUND;
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_mx) return ESP_ERR_NO_MEM;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.read = bd_read;
    s_cfg.prog = bd_prog;
    s_cfg.erase = bd_erase;
    s_cfg.sync = bd_sync;
    s_cfg.read_size = 16;
    s_cfg.prog_size = 16;
    s_cfg.block_size = LFSV_BLOCK;
    s_cfg.block_count = s_part->size / LFSV_BLOCK;
    s_cfg.block_cycles = 500;
    s_cfg.cache_size = LFSV_CACHE;
    s_cfg.lookahead_size = LFSV_LOOKAHEAD;
    s_cfg.read_buffer = s_rbuf;
    s_cfg.prog_buffer = s_pbuf;
    s_cfg.lookahead_buffer = s_look;
    return ESP_OK;
}

static int to_errno(int e) {
    switch (e) {
        case LFS_ERR_NOENT: return ENOENT;
        case LFS_ERR_EXIST: return EEXIST;
        case LFS_ERR_NOTDIR: return ENOTDIR;
        case LFS_ERR_ISDIR: return EISDIR;
        case LFS_ERR_NOTEMPTY: return ENOTEMPTY;
        case LFS_ERR_BADF: return EBADF;
        case LFS_ERR_FBIG: return EFBIG;
        case LFS_ERR_INVAL: return EINVAL;
        case LFS_ERR_NOSPC: return ENOSPC;
        case LFS_ERR_NOMEM: return ENOMEM;
        case LFS_ERR_NAMETOOLONG: return ENAMETOOLONG;
        default: return EIO;
    }
}

static int fail(int e) {
    errno = to_errno(e);
    return -1;
}

/* Missing parent directories of path, as SPIFFS never needed any */
static void make_parents(const char *path) {
    char tmp[LFS_NAME_MAX + 1];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        lfs_mkdir(&s_lfs, tmp);
        tmp[i] = '/';
    }
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

static int vfs_open(void *ctx, const char *path, int flags, int mode) {
    (void)ctx; (void)mode;
    int lflags = 0;
    switch (flags & O_ACCMODE) {
        case O_RDONLY: lflags = LFS_O_RDONLY; break;
        case O_WRONLY: lflags = LFS_O_WRONLY; break;
        default: lflags = LFS_O_RDWR; break;
    }
    if (flags & O_CREAT) lflags |= LFS_O_CREAT;
    if (flags & O_EXCL) lflags |= LFS_O_EXCL;
    if (flags & O_TRUNC) lflags |= LFS_O_TRUNC;
    if (flags & O_APPEND) lflags |= LFS_O_APPEND;
    LOCK();
    int fd = -1;
    for (int i = 0; i < s_nfd; i++) {
        if (!s_fds[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        UNLOCK();
        errno = ENFILE;
        return -1;
    }
    LfsFd *f = &s_fds[fd];
    memset(&f->cfg, 0, sizeof(f->cfg));
    f->cfg.buffer = f->buf;
    int e = lfs_file_opencfg(&s_lfs, &f->file, path, lflags, &f->cfg);
    if (e == LFS_ERR_NOENT && (flags & O_CREAT)) {
        make_parents(path);
        e = lfs_file_opencfg(&s_lfs, &f->file, path, lflags, &f->cfg);
    }
    if (e == 0) f->used = true;
    UNLOCK();
    return e < 0 ? fail(e) : fd;
}

static LfsFd *fd_get(int fd) {
    return fd >= 0 && fd < s_nfd && s_fds[fd].used ? &s_fds[fd] : NULL;
}

static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    lfs_ssize_t n = f ? lfs_file_write(&s_lfs, &f->file, data, size) : LFS_ERR_BADF;
    UNLOCK();
    return n < 0 ? fail((int)n) : (ssize_t)n;
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    lfs_ssize_t n = f ? lfs_file_read(&s_lfs, &f->file, dst, size) : LFS_ERR_BADF;
    UNLOCK();
    return n < 0 ? fail((int)n) : (ssize_t)n;
}

static off_t vfs_lseek(void *ctx, int fd, off_t off, int mode) {
    (void)ctx;
    int whence = mode == SEEK_CUR ? LFS_SEEK_CUR : (mode == SEEK_END ? LFS_SEEK_END : LFS_SEEK_SET);
    LOCK();
    LfsFd *f = fd_get(fd);
    lfs_soff_t r = f ? lfs_file_seek(&s_lfs, &f->file, (lfs_soff_t)off, whence) : LFS_ERR_BADF;
    UNLOCK();
    return r < 0 ? fail((int)r) : (off_t)r;
}

static int vfs_close(void *ctx, int fd) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    int e = f ? lfs_file_close(&s_lfs, &f->file) : LFS_ERR_BADF;
    if (f) f->used = false;
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    lfs_soff_t size = f ? lfs_file_size(&s_lfs, &f->file) : LFS_ERR_BADF;
    UNLOCK();
    if (size < 0) return fail((int)size);
    memset(st, 0, sizeof(*st));
    st->st_size = size;
    st->st_mode = S_IFREG | 0666;
    st->st_blksize = LFSV_BLOCK;
    return 0;
}

static int vfs_fsync(void *ctx, int fd) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    int e = f ? lfs_file_sync(&s_lfs, &f->file) : LFS_ERR_BADF;
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static int vfs_ftruncate(void *ctx, int fd, off_t length) {
    (void)ctx;
    LOCK();
    LfsFd *f = fd_get(fd);
    int e = f ? lfs_file_truncate(&s_lfs, &f->file, (lfs_off_t)length) : LFS_ERR_BADF;
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

/* ------------------------------------------------------------------------ */
/* Names and directories                                                    */
/* ------------------------------------------------------------------------ */

static int vfs_stat(void *ctx, const char *path, struct stat *st) {
    (void)ctx;
    struct lfs_info info;
    LOCK();
    int e = lfs_stat(&s_lfs, path, &info);
    UNLOCK();
    if (e < 0) return fail(e);
    memset(st, 0, sizeof(*st));
    st->st_size = info.type == LFS_TYPE_REG ? info.size : 0;
    st->st_mode = info.type == LFS_TYPE_DIR ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_blksize = LFSV_BLOCK;
    return 0;
}

static int vfs_unlink(void *ctx, const char *path) {
    (void)ctx;
    struct lfs_info info;
    LOCK();
    int e = lfs_stat(&s_lfs, path, &info);
    if (e == 0 && info.type == LFS_TYPE_DIR) e = LFS_ERR_ISDIR;
    if (e == 0) e = lfs_remove(&s_lfs, path);
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static int vfs_rename(void *ctx, const char *src, const char *dst) {
    (void)ctx;
    LOCK();
    int e = lfs_rename(&s_lfs, src, dst);
    if (e == LFS_ERR_NOENT) {
        struct lfs_info info;
        if (lfs_stat(&s_lfs, src, &info) == 0) {
            make_parents(dst);
            e = lfs_rename(&s_lfs, src, dst);
        }
    }
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static DIR *vfs_opendir(void *ctx, const char *name) {
    (void)ctx;
    LfsDir *d = (LfsDir *)calloc(1, sizeof(LfsDir));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    LOCK();
    int e = lfs_dir_open(&s_lfs, &d->ld, name[0] ? name : "/");
    UNLOCK();
    if (e < 0) {
        free(d);
        fail(e);
        return NULL;
    }
    return &d->dir;
}

static struct dirent *vfs_readdir(void *ctx, DIR *pdir) {
    (void)ctx;
    LfsDir *d = (LfsDir *)pdir;
    struct lfs_info info;
    for (;;) {
        LOCK();
        int r = lfs_dir_read(&s_lfs, &d->ld, &info);
        UNLOCK();
        if (r <= 0) {
            if (r < 0) fail(r);
            return NULL;
        }
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
        d->de.d_ino = 0;
        d->de.d_type = info.type == LFS_TYPE_DIR ? DT_DIR : DT_REG;
        strlcpy(d->de.d_name, info.name, sizeof(d->de.d_name));
        return &d->de;
    }
}

static int vfs_readdir_r(void *ctx, DIR *pdir, struct dirent *entry, struct dirent **out) {
    struct dirent *e = vfs_readdir(ctx, pdir);
    if (!e) {
        *out = NULL;
        return errno == 0 ? 0 : errno;
    }
    memcpy(entry, e, sizeof(*entry));
    *out = entry;
    return 0;
}

static long vfs_telldir(void *ctx, DIR *pdir) {
    (void)ctx;
    LOCK();
    lfs_soff_t r = lfs_dir_tell(&s_lfs, &((LfsDir *)pdir)->ld);
    UNLOCK();
    return r < 0 ? fail((int)r) : (long)r;
}

static void vfs_seekdir(void *ctx, DIR *pdir, long offset) {
    (void)ctx;
    LOCK();
    lfs_dir_seek(&s_lfs, &((LfsDir *)pdir)->ld, (lfs_off_t)offset);
    UNLOCK();
}

static int vfs_closedir(void *ctx, DIR *pdir) {
    (void)ctx;
    LfsDir *d = (LfsDir *)pdir;
    LOCK();
    int e = lfs_dir_close(&s_lfs, &d->ld);
    UNLOCK();
    free(d);
    return e < 0 ? fail(e) : 0;
}

static int vfs_mkdir(void *ctx, const char *name, mode_t mode) {
    (void)ctx; (void)mode;
    LOCK();
    int e = lfs_mkdir(&s_lfs, name);
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static int vfs_rmdir(void *ctx, const char *name) {
    (void)ctx;
    struct lfs_info info;
    LOCK();
    int e = lfs_stat(&s_lfs, name, &info);
    if (e == 0 && info.type != LFS_TYPE_DIR) e = LFS_ERR_NOTDIR;
    if (e == 0) e = lfs_remove(&s_lfs, name);
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static int vfs_access(void *ctx, const char *path, int amode) {
    (void)amode;
    struct stat st;
    return vfs_stat(ctx, path, &st);
}

static int vfs_truncate(void *ctx, const char *path, off_t length) {
    (void)ctx;
    lfs_file_t file;
    LOCK();
    int e = lfs_file_open(&s_lfs, &file, path, LFS_O_WRONLY);
    if (e == 0) {
        e = lfs_file_truncate(&s_lfs, &file, (lfs_off_t)length);
        int c = lfs_file_close(&s_lfs, &file);
        if (e == 0) e = c;
    }
    UNLOCK();
    return e < 0 ? fail(e) : 0;
}

static const esp_vfs_dir_ops_t s_dir_ops = {
    .stat_p = vfs_stat,
    .unlink_p = vfs_unlink,
    .rename_p = vfs_rename,
    .opendir_p = vfs_opendir,
    .readdir_p = vfs_readdir,
    .readdir_r_p = vfs_readdir_r,
    .telldir_p = vfs_telldir,
    .seekdir_p = vfs_seekdir,
    .closedir_p = vfs_closedir,
    .mkdir_p = vfs_mkdir,
    .rmdir_p = vfs_rmdir,
    .access_p = vfs_access,
    .truncate_p = vfs_truncate,
    .ftruncate_p = vfs_ftruncate,
};

static const esp_vfs_fs_ops_t s_ops = {
    .write_p = vfs_write,
    .lseek_p = vfs_lseek,
    .read_p = vfs_read,
    .open_p = vfs_open,
    .close_p = vfs_close,
    .fstat_p = vfs_fstat,
    .fsync_p = vfs_fsync,
    .dir = &s_dir_ops,
};

/* ------------------------------------------------------------------------ */
/* Mount                                                                    */
/* ------------------------------------------------------------------------ */

esp_err_t lfsvfs_format(const char *partition_label) {
    if (s_mounted) return ESP_ERR_INVALID_STATE;
    esp_err_t r = setup(partition_label);
    if (r != ESP_OK) return r;
    return lfs_format(&s_lfs, &s_cfg) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t lfsvfs_mount(const char *base_path, const char *partition_label, bool format_if_failed, int max_files) {
    if (s_mounted) return ESP_ERR_INVALID_STATE;
    esp_err_t r = setup(partition_label);
    if (r != ESP_OK) return r;
    int e = lfs_mount(&s_lfs, &s_cfg);
    if (e != 0 && format_if_failed) {
        ESP_LOGW(TAG, "no LittleFS on '%s': formatting", partition_label);
        if (lfs_format(&s_lfs, &s_cfg) == 0) e = lfs_mount(&s_lfs, &s_cfg);
    }
    if (e != 0) return ESP_FAIL;
    s_nfd = max_files > 0 ? max_files : 8;
    s_fds = (LfsFd *)calloc((size_t)s_nfd, sizeof(LfsFd));
    if (!s_fds) {
        lfs_unmount(&s_lfs);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_base, base_path, sizeof(s_base));
    r = esp_vfs_register_fs(s_base, &s_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
    if (r != ESP_OK) {
        free(s_fds);
        s_fds = NULL;
        lfs_unmount(&s_lfs);
        return r;
    }
    s_mounted = true;
    return ESP_OK;
}

void lfsvfs_unmount(void) {
    if (!s_mounted) return;
    esp_vfs_unregister(s_base);
    LOCK();
    for (int i = 0; i < s_nfd; i++) {
        if (s_fds[i].used) lfs_file_close(&s_lfs, &s_fds[i].file);
    }
    lfs_unmount(&s_lfs);
    UNLOCK();
    free(s_fds);
    s_fds = NULL;
    s_mounted = false;
}

bool lfsvfs_mounted(void) {
    return s_mounted;
}

esp_err_t lfsvfs_info(size_t *total, size_t *used) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    LOCK();
    lfs_ssize_t blocks = lfs_fs_size(&s_lfs);
    UNLOCK();
    if (total) *total = (size_t)s_cfg.block_count * LFSV_BLOCK;
    if (used) *used = blocks > 0 ? (size_t)blocks * LFSV_BLOCK : 0;
    return blocks < 0 ? ESP_FAIL : ESP_OK;
}
