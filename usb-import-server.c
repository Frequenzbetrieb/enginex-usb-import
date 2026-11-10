/*
 * USB Import Daemon v7.0 for EngineX OS
 * Listens on 127.0.0.1:8082, polls /media/ for USB drives.
 * MIT License — DJ MR. BROWN
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>

#include "sqlite3.h"

/* ── Configuration ── */

#define PORT 8082
#define POLL_INTERVAL 3
#define MAX_KNOWN 32
#define MAX_BACKUPS 3
#define MAX_RECURSE_DEPTH 64
#define CONFIG_PATH "/data/enginex/mods/usb-import/config"

/* ── Copied file tracking ── */

typedef struct {
    char *rel_path;   /* e.g. "Music/Artist/song.mp3" */
    char *file_type;  /* e.g. "mp3" */
    int   skipped;    /* 1 if file already existed */
} CopiedFile;

static CopiedFile *g_copied = NULL;
static int g_copied_count = 0;
static int g_copied_cap = 0;

/* ── Global state (protected by g_lock) ── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int  g_importing;
static int  g_import_done;
static int  g_restart_pending;
static int  g_files_copied;
static int  g_files_skipped;
static int  g_files_failed;
static int  g_files_total;
static long g_bytes_copied;
static char g_current_file[512];
static char g_error_msg[512];
static char g_backup_path[512];
static int  g_done_acked;  /* 1 after client has seen done result */

/* Pending drive for import dialog */
static char g_pending_drive[256];
static char g_pending_path[1024];

/* Internal drive (selected by user or auto) */
static char g_internal_root[1024];  /* e.g. "/media/Denon" */
static char g_saved_target[256];    /* saved target drive name */

/* Drive candidates for selection dialog */
#define MAX_CANDIDATES 8
static char g_candidates[MAX_CANDIDATES][256];
static int  g_candidate_count;
static int  g_needs_target_selection;

/* Known drives (already seen/imported) */
static char g_known[MAX_KNOWN][256];
static int  g_known_count;

/* ── Helpers ── */

static void json_escape(char *dst, int dsz, const char *src) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (*src && i < dsz - 6) {
        if (*src == '"' || *src == '\\') {
            dst[i++] = '\\'; dst[i++] = *src;
        } else if (*src == '\n') {
            dst[i++] = '\\'; dst[i++] = 'n';
        } else if ((unsigned char)*src < 0x20) {
            i += snprintf(dst + i, dsz - i, "\\u%04x", (unsigned char)*src);
        } else {
            dst[i++] = *src;
        }
        src++;
    }
    dst[i] = 0;
}

static int ends_with_ci(const char *s, const char *suffix) {
    int sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    for (int i = 0; i < xl; i++)
        if (tolower((unsigned char)s[sl - xl + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static int is_music_file(const char *name) {
    static const char *exts[] = {
        ".mp3",".flac",".wav",".m4a",".ogg",".aac",".aiff",".aif", NULL
    };
    for (const char **e = exts; *e; e++)
        if (ends_with_ci(name, *e)) return 1;
    return 0;
}

static const char *get_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static void add_known(const char *name) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_known_count; i++)
        if (strcmp(g_known[i], name) == 0) { pthread_mutex_unlock(&g_lock); return; }
    if (g_known_count < MAX_KNOWN)
        snprintf(g_known[g_known_count++], 256, "%s", name);
    pthread_mutex_unlock(&g_lock);
}

static int is_known(const char *name) {
    for (int i = 0; i < g_known_count; i++)
        if (strcmp(g_known[i], name) == 0) return 1;
    return 0;
}

/* Remove drives that are no longer mounted */
static void purge_unmounted_known(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_known_count; ) {
        char path[512];
        snprintf(path, sizeof(path), "/media/%s", g_known[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            g_known_count--;
            memmove(g_known[i], g_known[g_known_count], 256);
            /* don't increment i */
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ── Saved target config ── */

static void load_saved_target(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) { g_saved_target[0] = 0; return; }
    char line[512];
    g_saved_target[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "target=", 7) == 0) {
            snprintf(g_saved_target, sizeof(g_saved_target), "%s", line + 7);
        }
    }
    fclose(f);
}

static void save_target(const char *name) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) { fprintf(stderr, "Cannot save config: %s\n", strerror(errno)); return; }
    fprintf(f, "target=%s\n", name);
    fclose(f);
    snprintf(g_saved_target, sizeof(g_saved_target), "%s", name);
    fprintf(stderr, "Saved target drive: %s\n", name);
}

/* ── Drive detection ── */

/* Check if a block device path contains /usb (indicating USB attachment) */
static int is_usb_device(const char *devname) {
    char syspath[512];
    /* Strip partition number to get base device */
    char base[64];
    snprintf(base, sizeof(base), "%s", devname);
    /* Remove trailing digits for partition (e.g. sda1 -> sda) */
    int len = strlen(base);
    while (len > 0 && base[len-1] >= '0' && base[len-1] <= '9') len--;
    base[len] = 0;

    /* Check /sys/block/<dev>/removable */
    snprintf(syspath, sizeof(syspath), "/sys/block/%s/removable", base);
    FILE *f = fopen(syspath, "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1 && val == 1) {
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    /* Resolve the sysfs path and check if it contains /usb */
    snprintf(syspath, sizeof(syspath), "/sys/block/%s", base);
    char resolved[1024];
    if (realpath(syspath, resolved)) {
        if (strstr(resolved, "/usb")) return 1;
    }

    return 0;
}

/* Find the device name for a mount point from /proc/mounts */
static int get_mount_device(const char *mountpoint, char *devname, int devsize) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mp[512];
        if (sscanf(line, "%255s %511s", dev, mp) == 2) {
            if (strcmp(mp, mountpoint) == 0) {
                /* Extract base device name from /dev/sdXN */
                const char *base = strrchr(dev, '/');
                base = base ? base + 1 : dev;
                snprintf(devname, devsize, "%s", base);
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

/* Scan for drives with Engine Library DB, return candidates */
static int find_engine_drives(char candidates[][256], int max) {
    int count = 0;
    DIR *d = opendir("/media");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < max) {
        if (e->d_name[0] == '.') continue;
        char dbpath[1024];
        snprintf(dbpath, sizeof(dbpath), "/media/%s/Engine Library/Database2/m.db", e->d_name);
        struct stat st;
        if (stat(dbpath, &st) == 0 && st.st_size > 0) {
            snprintf(candidates[count++], 256, "%s", e->d_name);
        }
    }
    closedir(d);
    return count;
}

/* Filter out USB-attached drives from candidate list */
static int filter_non_usb(char candidates[][256], int count) {
    int out = 0;
    for (int i = 0; i < count; i++) {
        char mp[512], devname[64];
        snprintf(mp, sizeof(mp), "/media/%s", candidates[i]);
        if (get_mount_device(mp, devname, sizeof(devname)) && is_usb_device(devname)) {
            fprintf(stderr, "Excluding USB device: %s (%s)\n", candidates[i], devname);
            continue;
        }
        if (i != out) snprintf(candidates[out], 256, "%s", candidates[i]);
        out++;
    }
    return out;
}

/* ── Validate drive parameter ── */

static int validate_drive_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '/')) return 0;
    for (const char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20) return 0;
    return 1;
}

/* ── File copying ── */

static int copy_file_safe(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;

    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.usb-import-tmp", dst);

    int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { close(sfd); return -1; }

    char buf[65536];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(dfd, buf + w, n - w);
            if (r <= 0) { close(sfd); close(dfd); unlink(tmp); return -1; }
            w += r;
        }
    }
    if (n < 0) { close(sfd); close(dfd); unlink(tmp); return -1; }

    close(sfd);
    if (fsync(dfd) != 0) { close(dfd); unlink(tmp); return -1; }
    close(dfd);

    if (rename(tmp, dst) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Create parent directories */
static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* Generate unique destination, checking content via size comparison */
static void unique_dest(char *dst, int dsz, const char *base_dir,
                        const char *rel, off_t src_size) {
    snprintf(dst, dsz, "%s/%s", base_dir, rel);
    struct stat st;
    if (lstat(dst, &st) != 0) return; /* does not exist, use as-is */
    if (st.st_size == src_size) return; /* same size, treat as same file */

    /* Find stem and extension */
    const char *dot = strrchr(rel, '.');
    char stem[512], ext[64];
    if (dot) {
        int sl = dot - rel;
        snprintf(stem, sizeof(stem), "%.*s", sl, rel);
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(stem, sizeof(stem), "%s", rel);
        ext[0] = 0;
    }

    for (int n = 2; n < 1000; n++) {
        snprintf(dst, dsz, "%s/%s (%d)%s", base_dir, stem, n, ext);
        if (lstat(dst, &st) != 0) return;
        if (st.st_size == src_size) return; /* existing (N) copy matches */
    }
}

/* Recursively scan and copy music files */
static void copy_music_recursive(const char *src_base, const char *dst_base,
                                 const char *rel, int depth) {
    if (depth > MAX_RECURSE_DEPTH) return;

    char srcdir[1024];
    if (rel[0])
        snprintf(srcdir, sizeof(srcdir), "%s/%s", src_base, rel);
    else
        snprintf(srcdir, sizeof(srcdir), "%s", src_base);

    DIR *d = opendir(srcdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char relpath[1024];
        if (rel[0])
            snprintf(relpath, sizeof(relpath), "%s/%s", rel, e->d_name);
        else
            snprintf(relpath, sizeof(relpath), "%s", e->d_name);

        char fullsrc[1024];
        snprintf(fullsrc, sizeof(fullsrc), "%s/%s", src_base, relpath);

        struct stat st;
        if (lstat(fullsrc, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            copy_music_recursive(src_base, dst_base, relpath, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (!is_music_file(e->d_name)) continue;

        pthread_mutex_lock(&g_lock);
        g_files_total++;
        snprintf(g_current_file, sizeof(g_current_file), "%s", e->d_name);
        pthread_mutex_unlock(&g_lock);

        /* Destination under Engine Library/Music/ */
        char music_rel[1024];
        snprintf(music_rel, sizeof(music_rel), "Music/%s", relpath);

        char dst[1024];
        unique_dest(dst, sizeof(dst),
                    dst_base, /* e.g. "/media/Denon/Engine Library" */
                    music_rel, st.st_size);

        /* Track the file for DB registration */
        /* Compute the relative path from Engine Library dir */
        const char *db_rel = dst + strlen(dst_base) + 1; /* skip base + / */

        int skipped = 0;
        struct stat dst_st;
        if (lstat(dst, &dst_st) == 0 && dst_st.st_size == st.st_size) {
            /* File already exists with same size — skip copy but register */
            skipped = 1;
            pthread_mutex_lock(&g_lock);
            g_files_skipped++;
            pthread_mutex_unlock(&g_lock);
        } else {
            /* Create parent dirs and copy */
            mkdirs(dst);
            if (copy_file_safe(fullsrc, dst) != 0) {
                pthread_mutex_lock(&g_lock);
                g_files_failed++;
                pthread_mutex_unlock(&g_lock);
                fprintf(stderr, "Copy failed: %s\n", relpath);
                continue; /* don't register failed copies */
            }
            pthread_mutex_lock(&g_lock);
            g_files_copied++;
            g_bytes_copied += st.st_size;
            pthread_mutex_unlock(&g_lock);
        }

        /* Add to copied list for DB registration (both copied and skipped) */
        pthread_mutex_lock(&g_lock);
        if (g_copied_count >= g_copied_cap) {
            int newcap = g_copied_cap ? g_copied_cap * 2 : 256;
            CopiedFile *tmp = realloc(g_copied, newcap * sizeof(CopiedFile));
            if (tmp) { g_copied = tmp; g_copied_cap = newcap; }
        }
        if (g_copied_count < g_copied_cap) {
            g_copied[g_copied_count].rel_path = strdup(db_rel);
            g_copied[g_copied_count].file_type = strdup(get_extension(e->d_name));
            g_copied[g_copied_count].skipped = skipped;
            g_copied_count++;
        }
        pthread_mutex_unlock(&g_lock);
    }
    closedir(d);
}

/* ── Backup with write lock ── */

static int backup_database(sqlite3 *db, const char *db_path, char *backup_out, int bsz) {
    /* Use SQLite backup API while holding the write lock */
    char ts[64];
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", t);

    char backup_path[1024];
    snprintf(backup_path, sizeof(backup_path), "%s.usb-import-backup-%s", db_path, ts);
    snprintf(backup_out, bsz, "%s", backup_path);

    sqlite3 *dst_db = NULL;
    if (sqlite3_open(backup_path, &dst_db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open backup dest: %s\n", sqlite3_errmsg(dst_db));
        if (dst_db) sqlite3_close(dst_db);
        return -1;
    }

    sqlite3_backup *bk = sqlite3_backup_init(dst_db, "main", db, "main");
    if (!bk) {
        fprintf(stderr, "Backup init failed: %s\n", sqlite3_errmsg(dst_db));
        sqlite3_close(dst_db);
        unlink(backup_path);
        return -1;
    }

    int rc = sqlite3_backup_step(bk, -1);
    sqlite3_backup_finish(bk);
    sqlite3_close(dst_db);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Backup step failed: rc=%d\n", rc);
        unlink(backup_path);
        return -1;
    }

    fprintf(stderr, "Backup created: %s\n", backup_path);
    return 0;
}

/* Remove old backups, keep only the latest N */
static void rotate_backups(const char *db_path, int keep) {
    char dir[1024], base[256];
    snprintf(dir, sizeof(dir), "%s", db_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    snprintf(base, sizeof(base), "%s", slash + 1);

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s.usb-import-backup-", base);
    int plen = strlen(prefix);

    /* Collect backup filenames */
    char backups[32][256];
    int count = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && count < 32) {
        if (strncmp(e->d_name, prefix, plen) == 0) {
            snprintf(backups[count++], 256, "%s", e->d_name);
        }
    }
    closedir(d);

    if (count <= keep) return;

    /* Sort by name (timestamp is in the name, so lexicographic = chronological) */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(backups[i], backups[j]) > 0) {
                char tmp[256];
                memcpy(tmp, backups[i], 256);
                memcpy(backups[i], backups[j], 256);
                memcpy(backups[j], tmp, 256);
            }

    /* Remove oldest, keep the last 'keep' */
    for (int i = 0; i < count - keep; i++) {
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, backups[i]);
        fprintf(stderr, "Removing old backup: %s\n", backups[i]);
        unlink(fullpath);
    }
}

/* ── Import thread ── */

static void *import_thread(void *arg) {
    char *drive_name = (char *)arg;

    char usb_path[1024];
    snprintf(usb_path, sizeof(usb_path), "/media/%s", drive_name);

    /* Determine internal drive */
    pthread_mutex_lock(&g_lock);
    char internal[1024];
    snprintf(internal, sizeof(internal), "%s", g_internal_root);
    pthread_mutex_unlock(&g_lock);

    if (!internal[0]) {
        pthread_mutex_lock(&g_lock);
        g_importing = 0;
        g_import_done = 1;
        snprintf(g_error_msg, sizeof(g_error_msg), "No target drive configured.");
        pthread_mutex_unlock(&g_lock);
        free(drive_name);
        return NULL;
    }

    char el_dir[1024];
    snprintf(el_dir, sizeof(el_dir), "%s/Engine Library", internal);
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/Database2/m.db", el_dir);

    /* Check free space */
    struct statvfs sv;
    if (statvfs(internal, &sv) == 0) {
        unsigned long long avail = (unsigned long long)sv.f_bavail * sv.f_bsize;
        if (avail < 100 * 1024 * 1024) {
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "Not enough free space on internal drive (%llu MB).",
                avail / (1024 * 1024));
            pthread_mutex_unlock(&g_lock);
            free(drive_name);
            return NULL;
        }
    }

    /* Copy files first */
    pthread_mutex_lock(&g_lock);
    g_copied_count = 0;
    pthread_mutex_unlock(&g_lock);

    copy_music_recursive(usb_path, el_dir, "", 0);

    pthread_mutex_lock(&g_lock);
    int total_for_db = g_copied_count;
    int copied = g_files_copied;
    int skipped = g_files_skipped;
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr, "Copy done: %d copied, %d skipped, %d for DB\n",
            copied, skipped, total_for_db);

    /* If nothing to register, we're done */
    if (total_for_db == 0) {
        pthread_mutex_lock(&g_lock);
        g_importing = 0;
        g_import_done = 1;
        if (g_files_failed > 0)
            snprintf(g_error_msg, sizeof(g_error_msg), "All files failed to copy.");
        /* else: no music files found — not an error, just nothing to do */
        pthread_mutex_unlock(&g_lock);
        add_known(drive_name);
        free(drive_name);
        return NULL;
    }

    /* ── Database phase ── */
    {
        sqlite3 *db = NULL;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg), "Cannot open database.");
            pthread_mutex_unlock(&g_lock);
            free(drive_name);
            return NULL;
        }

        sqlite3_busy_timeout(db, 10000);
        sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);

        /* Take write lock with BEGIN IMMEDIATE */
        char *errmsg = NULL;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg), "Database locked: %s",
                     errmsg ? errmsg : "unknown");
            pthread_mutex_unlock(&g_lock);
            sqlite3_free(errmsg);
            sqlite3_close(db);
            free(drive_name);
            return NULL;
        }

        /* Backup AFTER acquiring write lock */
        char backup[1024] = "";
        if (backup_database(db, db_path, backup, sizeof(backup)) != 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "Backup failed — aborting import to protect your database.");
            pthread_mutex_unlock(&g_lock);
            sqlite3_close(db);
            free(drive_name);
            return NULL;
        }
        pthread_mutex_lock(&g_lock);
        snprintf(g_backup_path, sizeof(g_backup_path), "%s", backup);
        pthread_mutex_unlock(&g_lock);

        /* Rotate old backups */
        rotate_backups(db_path, MAX_BACKUPS);

        /* Get originDatabaseUuid */
        char uuid[128] = "";
        {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                "SELECT originDatabaseUuid FROM Track GROUP BY originDatabaseUuid ORDER BY COUNT(*) DESC LIMIT 1",
                -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const char *val = (const char *)sqlite3_column_text(st, 0);
                    if (val) snprintf(uuid, sizeof(uuid), "%s", val);
                }
                sqlite3_finalize(st);
            }
        }

        if (!uuid[0]) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "Cannot determine originDatabaseUuid — is the Engine Library empty?");
            pthread_mutex_unlock(&g_lock);
            sqlite3_close(db);
            free(drive_name);
            return NULL;
        }

        /* Get max originTrackId */
        int max_otid = 0;
        {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                "SELECT MAX(CAST(originTrackId AS INTEGER)) FROM Track",
                -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    max_otid = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
        }

        /* Insert tracks */
        int db_error = 0;
        int tracks_registered = 0;
        int next_otid = max_otid + 1;

        for (int i = 0; i < total_for_db && !db_error; i++) {
            char esc_path[1024];
            json_escape(esc_path, sizeof(esc_path), g_copied[i].rel_path);

            char sql[2048];
            snprintf(sql, sizeof(sql),
                "INSERT OR IGNORE INTO Track "
                "(path, filename, fileType, isAvailable, "
                "originDatabaseUuid, originTrackId) "
                "VALUES ('%s', '%s', '%s', 1, '%s', '%d')",
                g_copied[i].rel_path,
                strrchr(g_copied[i].rel_path, '/') ?
                    strrchr(g_copied[i].rel_path, '/') + 1 : g_copied[i].rel_path,
                g_copied[i].file_type,
                uuid, next_otid);

            /* Use proper parameter binding to avoid SQL injection */
            sqlite3_stmt *st = NULL;
            const char *insert_sql =
                "INSERT OR IGNORE INTO Track "
                "(path, filename, fileType, isAvailable, "
                "originDatabaseUuid, originTrackId) "
                "VALUES (?, ?, ?, 1, ?, ?)";
            if (sqlite3_prepare_v2(db, insert_sql, -1, &st, NULL) != SQLITE_OK) {
                fprintf(stderr, "Prepare INSERT Track failed: %s\n", sqlite3_errmsg(db));
                db_error = 1;
                break;
            }

            const char *filename = strrchr(g_copied[i].rel_path, '/');
            filename = filename ? filename + 1 : g_copied[i].rel_path;

            sqlite3_bind_text(st, 1, g_copied[i].rel_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, filename, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, g_copied[i].file_type, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, uuid, -1, SQLITE_STATIC);
            sqlite3_bind_int(st, 5, next_otid);

            int rc = sqlite3_step(st);
            sqlite3_finalize(st);

            if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
                fprintf(stderr, "INSERT Track failed: %s\n", sqlite3_errmsg(db));
                db_error = 1;
                break;
            }
            if (rc == SQLITE_DONE) {
                tracks_registered++;
                next_otid++;
            }
            /* SQLITE_CONSTRAINT = already exists, fine */
        }

        /* Create or find "USB Import" playlist */
        int playlist_id = 0;
        if (!db_error && tracks_registered > 0) {
            /* Check if playlist exists */
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                "SELECT id FROM Playlist WHERE title='USB Import' LIMIT 1",
                -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    playlist_id = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }

            if (!playlist_id) {
                /* Find last playlist in linked list */
                int last_id = 0;
                if (sqlite3_prepare_v2(db,
                    "SELECT id FROM Playlist WHERE nextListId=0 ORDER BY id DESC LIMIT 1",
                    -1, &st, NULL) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW)
                        last_id = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                }

                /* Insert new playlist */
                const char *pl_sql = "INSERT INTO Playlist (title, nextListId) VALUES ('USB Import', 0)";
                if (sqlite3_exec(db, pl_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
                    fprintf(stderr, "INSERT Playlist failed: %s\n", errmsg ? errmsg : "?");
                    sqlite3_free(errmsg); errmsg = NULL;
                    db_error = 1;
                }
                if (!db_error) {
                    playlist_id = (int)sqlite3_last_insert_rowid(db);

                    /* Link into chain */
                    if (last_id > 0) {
                        char link_sql[256];
                        snprintf(link_sql, sizeof(link_sql),
                            "UPDATE Playlist SET nextListId=%d WHERE id=%d",
                            playlist_id, last_id);
                        if (sqlite3_exec(db, link_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
                            fprintf(stderr, "UPDATE Playlist chain failed: %s\n", errmsg ? errmsg : "?");
                            sqlite3_free(errmsg); errmsg = NULL;
                            db_error = 1;
                        }
                    }
                }
            }
        }

        /* Add tracks to playlist */
        if (!db_error && playlist_id > 0 && tracks_registered > 0) {
            /* Find last entity in this playlist */
            int last_entity = 0;
            {
                sqlite3_stmt *st = NULL;
                char q[256];
                snprintf(q, sizeof(q),
                    "SELECT id FROM PlaylistEntity WHERE listId=%d AND nextEntityId=0 ORDER BY id DESC LIMIT 1",
                    playlist_id);
                if (sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW)
                        last_entity = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                }
            }

            /* Get originTrackIds for our newly inserted tracks */
            for (int i = 0; i < total_for_db && !db_error; i++) {
                /* Look up the track's originTrackId by path */
                sqlite3_stmt *st = NULL;
                int otid = 0;
                if (sqlite3_prepare_v2(db,
                    "SELECT originTrackId FROM Track WHERE path=?",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, g_copied[i].rel_path, -1, SQLITE_STATIC);
                    if (sqlite3_step(st) == SQLITE_ROW)
                        otid = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                }
                if (!otid) continue; /* track not in DB (shouldn't happen) */

                /* Check if already in playlist */
                int already = 0;
                if (sqlite3_prepare_v2(db,
                    "SELECT 1 FROM PlaylistEntity WHERE listId=? AND trackId=?",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(st, 1, playlist_id);
                    sqlite3_bind_int(st, 2, otid);
                    if (sqlite3_step(st) == SQLITE_ROW) already = 1;
                    sqlite3_finalize(st);
                }
                if (already) continue;

                /* Insert playlist entity */
                if (sqlite3_prepare_v2(db,
                    "INSERT INTO PlaylistEntity (listId, trackId, nextEntityId) VALUES (?, ?, 0)",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(st, 1, playlist_id);
                    sqlite3_bind_int(st, 2, otid);
                    int rc = sqlite3_step(st);
                    int new_id = (int)sqlite3_last_insert_rowid(db);
                    sqlite3_finalize(st);

                    if (rc != SQLITE_DONE) {
                        fprintf(stderr, "INSERT PlaylistEntity failed: %s\n", sqlite3_errmsg(db));
                        db_error = 1;
                        break;
                    }

                    /* Link into chain */
                    if (last_entity > 0) {
                        char link_sql[256];
                        snprintf(link_sql, sizeof(link_sql),
                            "UPDATE PlaylistEntity SET nextEntityId=%d WHERE id=%d",
                            new_id, last_entity);
                        if (sqlite3_exec(db, link_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
                            fprintf(stderr, "UPDATE PlaylistEntity chain failed: %s\n", errmsg ? errmsg : "?");
                            sqlite3_free(errmsg); errmsg = NULL;
                            db_error = 1;
                            break;
                        }
                    }
                    last_entity = new_id;
                } else {
                    fprintf(stderr, "Prepare PlaylistEntity failed: %s\n", sqlite3_errmsg(db));
                    db_error = 1;
                    break;
                }
            }
        }

        /* Commit or rollback */
        if (db_error) {
            fprintf(stderr, "Rolling back transaction\n");
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "Database error — rolled back. Backup at: %s", backup);
            pthread_mutex_unlock(&g_lock);
            sqlite3_close(db);
            add_known(drive_name);
            free(drive_name);
            return NULL;
        }

        if (sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
            fprintf(stderr, "COMMIT failed: %s\n", errmsg ? errmsg : "?");
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "COMMIT failed: %s — rolled back. Backup at: %s",
                errmsg ? errmsg : "unknown", backup);
            pthread_mutex_unlock(&g_lock);
            sqlite3_free(errmsg);
            sqlite3_close(db);
            add_known(drive_name);
            free(drive_name);
            return NULL;
        }

        sqlite3_close(db);
        fprintf(stderr, "Database updated: %d tracks registered\n", tracks_registered);
    }

    /* Signal completion */
    pthread_mutex_lock(&g_lock);
    g_importing = 0;
    g_import_done = 1;
    g_restart_pending = 1;
    g_done_acked = 0;
    pthread_mutex_unlock(&g_lock);

    /* Free copied file list */
    for (int i = 0; i < g_copied_count; i++) {
        free(g_copied[i].rel_path);
        free(g_copied[i].file_type);
    }
    g_copied_count = 0;

    add_known(drive_name);
    free(drive_name);
    fprintf(stderr, "Import complete, waiting for user to confirm restart\n");
    return NULL;
}

/* ── HTTP helpers ── */

static void send_response(int fd, int code, const char *body) {
    char hdr[512];
    int blen = strlen(body);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        code, blen);
    write(fd, hdr, hlen);
    write(fd, body, blen);
}

/* ── Request handler ── */

static void handle_request(int fd) {
    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = 0;

    if (strstr(buf, "GET /api/status")) {
        char resp[4096];
        pthread_mutex_lock(&g_lock);

        if (g_needs_target_selection && g_candidate_count > 0) {
            /* Drive selection needed */
            int pos = snprintf(resp, sizeof(resp),
                "{\"select_target\":true,\"candidates\":[");
            for (int i = 0; i < g_candidate_count; i++) {
                char esc[512];
                json_escape(esc, sizeof(esc), g_candidates[i]);
                pos += snprintf(resp + pos, sizeof(resp) - pos,
                    "%s\"%s\"", i ? "," : "", esc);
            }
            pos += snprintf(resp + pos, sizeof(resp) - pos,
                "],\"saved_target\":\"%s\"}", g_saved_target);
            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, resp);

        } else if (g_import_done) {
            char esc_err[512], esc_backup[512];
            json_escape(esc_err, sizeof(esc_err), g_error_msg);
            json_escape(esc_backup, sizeof(esc_backup), g_backup_path);
            snprintf(resp, sizeof(resp),
                "{\"done\":true,\"files_copied\":%d,\"files_skipped\":%d,\"files_failed\":%d,"
                "\"restart_pending\":%s,\"error\":\"%s\",\"backup\":\"%s\"}",
                g_files_copied, g_files_skipped, g_files_failed,
                g_restart_pending ? "true" : "false", esc_err, esc_backup);

            /* Keep the done result until acknowledged */
            g_done_acked = 1;

            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, resp);

        } else if (g_importing) {
            char esc_file[512];
            json_escape(esc_file, sizeof(esc_file), g_current_file);
            snprintf(resp, sizeof(resp),
                "{\"importing\":true,\"files_copied\":%d,\"files_skipped\":%d,"
                "\"files_failed\":%d,\"files_total\":%d,\"current_file\":\"%s\"}",
                g_files_copied, g_files_skipped, g_files_failed,
                g_files_total, esc_file);
            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, resp);

        } else if (g_pending_drive[0]) {
            char esc[512];
            json_escape(esc, sizeof(esc), g_pending_drive);
            snprintf(resp, sizeof(resp), "{\"new_drive\":\"%s\"}", esc);
            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, resp);

        } else {
            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, "{\"idle\":true}");
        }

    } else if (strstr(buf, "POST /api/import")) {
        /* Extract drive name */
        char *dj = strstr(buf, "\"drive\"");
        char drive[256] = "";
        if (dj) {
            char *q1 = strchr(dj + 7, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2 && q2 - q1 - 1 < (int)sizeof(drive)) {
                    memcpy(drive, q1 + 1, q2 - q1 - 1);
                    drive[q2 - q1 - 1] = 0;
                }
            }
        }

        if (!validate_drive_name(drive)) {
            send_response(fd, 400, "{\"error\":\"invalid drive name\"}");
        } else {
            pthread_mutex_lock(&g_lock);
            /* Check that this is the currently pending drive */
            if (strcmp(drive, g_pending_drive) != 0) {
                pthread_mutex_unlock(&g_lock);
                send_response(fd, 400, "{\"error\":\"not the pending drive\"}");
            } else if (g_importing) {
                pthread_mutex_unlock(&g_lock);
                send_response(fd, 409, "{\"error\":\"import already in progress\"}");
            } else {
                g_importing = 1;
                g_import_done = 0;
                g_files_copied = 0;
                g_files_skipped = 0;
                g_files_failed = 0;
                g_files_total = 0;
                g_bytes_copied = 0;
                g_current_file[0] = 0;
                g_error_msg[0] = 0;
                g_backup_path[0] = 0;
                g_restart_pending = 0;
                g_done_acked = 0;
                char *dname = strdup(drive);
                g_pending_drive[0] = 0;
                g_pending_path[0] = 0;
                pthread_mutex_unlock(&g_lock);

                pthread_t th;
                pthread_create(&th, NULL, import_thread, dname);
                pthread_detach(th);
                send_response(fd, 200, "{\"ok\":true}");
            }
        }

    } else if (strstr(buf, "POST /api/dismiss")) {
        pthread_mutex_lock(&g_lock);
        if (g_pending_drive[0]) {
            add_known(g_pending_drive);
            g_pending_drive[0] = 0;
            g_pending_path[0] = 0;
        }
        pthread_mutex_unlock(&g_lock);
        send_response(fd, 200, "{\"ok\":true}");

    } else if (strstr(buf, "POST /api/ack")) {
        /* Client acknowledges seeing the done result — clear state */
        pthread_mutex_lock(&g_lock);
        if (g_import_done && g_done_acked) {
            g_import_done = 0;
            g_files_copied = 0;
            g_files_skipped = 0;
            g_files_failed = 0;
            g_files_total = 0;
            g_bytes_copied = 0;
            g_current_file[0] = 0;
            g_error_msg[0] = 0;
        }
        pthread_mutex_unlock(&g_lock);
        send_response(fd, 200, "{\"ok\":true}");

    } else if (strstr(buf, "POST /api/restart")) {
        pthread_mutex_lock(&g_lock);
        if (g_restart_pending) {
            g_restart_pending = 0;
            g_import_done = 0;
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "User confirmed restart, killing Engine\n");
            system("killall Engine");
            send_response(fd, 200, "{\"ok\":true}");
        } else {
            pthread_mutex_unlock(&g_lock);
            send_response(fd, 200, "{\"ok\":false,\"reason\":\"no restart pending\"}");
        }

    } else if (strstr(buf, "POST /api/later")) {
        /* User chose "Later" — clear restart_pending so scanning resumes */
        pthread_mutex_lock(&g_lock);
        g_restart_pending = 0;
        g_import_done = 0;
        pthread_mutex_unlock(&g_lock);
        send_response(fd, 200, "{\"ok\":true}");

    } else if (strstr(buf, "POST /api/select-target")) {
        /* User selected their target drive */
        char *dj = strstr(buf, "\"drive\"");
        char drive[256] = "";
        int remember = 0;
        if (dj) {
            char *q1 = strchr(dj + 7, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2 && q2 - q1 - 1 < (int)sizeof(drive)) {
                    memcpy(drive, q1 + 1, q2 - q1 - 1);
                    drive[q2 - q1 - 1] = 0;
                }
            }
        }
        if (strstr(buf, "\"remember\"") && strstr(buf, "true"))
            remember = 1;

        if (!validate_drive_name(drive)) {
            send_response(fd, 400, "{\"error\":\"invalid drive name\"}");
        } else {
            pthread_mutex_lock(&g_lock);
            snprintf(g_internal_root, sizeof(g_internal_root), "/media/%s", drive);
            g_needs_target_selection = 0;
            g_candidate_count = 0;
            pthread_mutex_unlock(&g_lock);

            if (remember) save_target(drive);

            fprintf(stderr, "Target drive selected: %s (remember=%d)\n", drive, remember);
            send_response(fd, 200, "{\"ok\":true}");
        }

    } else if (strstr(buf, "POST /api/reset-target")) {
        /* Reset saved target — next scan will trigger selection */
        unlink(CONFIG_PATH);
        pthread_mutex_lock(&g_lock);
        g_saved_target[0] = 0;
        g_internal_root[0] = 0;
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "Target drive config reset\n");
        send_response(fd, 200, "{\"ok\":true}");

    } else {
        send_response(fd, 404, "{\"error\":\"not found\"}");
    }
    close(fd);
}

/* ── Poll thread ── */

static void *poll_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(POLL_INTERVAL);
        pthread_mutex_lock(&g_lock);
        int busy = g_importing || g_pending_drive[0] ||
                   (g_import_done && !g_done_acked) || g_needs_target_selection;
        pthread_mutex_unlock(&g_lock);
        if (busy) continue;

        purge_unmounted_known();

        /* Find drives with Engine Library */
        char candidates[MAX_CANDIDATES][256];
        int ncan = find_engine_drives(candidates, MAX_CANDIDATES);

        /* Filter USB drives */
        int non_usb = filter_non_usb(candidates, ncan);

        /* Determine internal drive */
        pthread_mutex_lock(&g_lock);

        if (g_saved_target[0]) {
            /* Check if saved target is still mounted */
            char check[1024];
            snprintf(check, sizeof(check), "/media/%s/Engine Library/Database2/m.db", g_saved_target);
            struct stat st;
            if (stat(check, &st) == 0) {
                snprintf(g_internal_root, sizeof(g_internal_root), "/media/%s", g_saved_target);
            } else {
                /* Saved target gone — need reselection */
                g_internal_root[0] = 0;
                if (non_usb > 0) {
                    g_needs_target_selection = 1;
                    g_candidate_count = non_usb;
                    for (int i = 0; i < non_usb && i < MAX_CANDIDATES; i++)
                        snprintf(g_candidates[i], 256, "%s", candidates[i]);
                    pthread_mutex_unlock(&g_lock);
                    continue;
                }
            }
        } else if (non_usb == 1) {
            /* Exactly one non-USB drive — use it automatically */
            snprintf(g_internal_root, sizeof(g_internal_root), "/media/%s", candidates[0]);
        } else if (non_usb > 1) {
            /* Multiple candidates — ask user to choose */
            g_needs_target_selection = 1;
            g_candidate_count = non_usb;
            for (int i = 0; i < non_usb && i < MAX_CANDIDATES; i++)
                snprintf(g_candidates[i], 256, "%s", candidates[i]);
            pthread_mutex_unlock(&g_lock);
            continue;
        } else if (non_usb == 0 && ncan > 0) {
            /* All drives are USB — still ask, maybe user wants one */
            g_needs_target_selection = 1;
            g_candidate_count = ncan;
            for (int i = 0; i < ncan && i < MAX_CANDIDATES; i++)
                snprintf(g_candidates[i], 256, "%s", candidates[i]);
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        char internal[1024];
        snprintf(internal, sizeof(internal), "%s", g_internal_root);
        pthread_mutex_unlock(&g_lock);

        if (!internal[0]) continue;

        /* Now scan for USB drives to offer import */
        DIR *d = opendir("/media");
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;

            char mp[512];
            snprintf(mp, sizeof(mp), "/media/%s", e->d_name);

            /* Skip the internal drive */
            if (strcmp(mp, internal) == 0) continue;

            /* Skip known drives */
            if (is_known(e->d_name)) continue;

            /* Check if this drive has music files (quick scan: just check top level and one level deep) */
            /* For now, any mounted drive that isn't the internal drive is offered */
            struct stat st;
            if (stat(mp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            pthread_mutex_lock(&g_lock);
            snprintf(g_pending_drive, sizeof(g_pending_drive), "%s", e->d_name);
            snprintf(g_pending_path, sizeof(g_pending_path), "%s", mp);
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "New USB drive detected: %s\n", e->d_name);
            break;
        }
        closedir(d);
    }
    return NULL;
}

/* ── Main ── */

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "USB Import daemon v7.0 on port %d\n", PORT);

    /* Load saved target preference */
    load_saved_target();
    if (g_saved_target[0])
        fprintf(stderr, "Saved target drive: %s\n", g_saved_target);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    listen(sock, 8);
    fprintf(stderr, "Listening on 127.0.0.1:%d\n", PORT);

    pthread_t pt;
    pthread_create(&pt, NULL, poll_thread, NULL);

    while (1) {
        int fd = accept(sock, NULL, NULL);
        if (fd < 0) continue;
        handle_request(fd);
    }
}
