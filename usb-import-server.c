/*
 * USB Import Daemon v8.0 for EngineX OS
 * Listens on 127.0.0.1:8082, polls /media/ for USB drives.
 * MIT License — DJ MR. BROWN
 *
 * Flow: USB detected → "Import?" → target selection (if needed) → copy → DB → restart
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
    long long file_bytes;
    /* ID3 metadata */
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    int  year;
    int  bitrate;     /* kbps */
    int  duration;    /* seconds */
    /* Cover art */
    unsigned char *cover_data;
    int cover_size;
    unsigned char cover_hash[20];
} CopiedFile;

/* ── ID3 Tag Reader ── */

static unsigned int id3_syncsafe(const unsigned char *b) {
    return ((unsigned int)b[0] << 21) | ((unsigned int)b[1] << 14) |
           ((unsigned int)b[2] << 7)  | (unsigned int)b[3];
}

static unsigned int id3_be32(const unsigned char *b) {
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) |
           ((unsigned int)b[2] << 8)  | (unsigned int)b[3];
}

static void cover_hash(const unsigned char *data, int size, unsigned char out[20]) {
    /* Simple hash: DJB2 spread across 20 bytes */
    memset(out, 0, 20);
    if (!data || size <= 0) return;
    unsigned long h = 5381;
    for (int i = 0; i < size; i++) {
        h = ((h << 5) + h) + data[i];
        out[i % 20] ^= (unsigned char)(h & 0xFF);
    }
}

static void id3v2_read_text(const unsigned char *data, int size, char *out, int outsz) {
    if (size < 1) return;
    int enc = data[0];
    const unsigned char *txt = data + 1;
    int txtlen = size - 1;
    if (txtlen <= 0) return;

    if (enc == 0 || enc == 3) {
        /* ISO-8859-1 or UTF-8 */
        int n = txtlen < outsz - 1 ? txtlen : outsz - 1;
        memcpy(out, txt, n);
        out[n] = 0;
    } else if (enc == 1 || enc == 2) {
        /* UTF-16 with or without BOM — extract ASCII portion */
        int start = 0;
        if (txtlen >= 2 && ((txt[0] == 0xFF && txt[1] == 0xFE) || (txt[0] == 0xFE && txt[1] == 0xFF)))
            start = 2;
        int le = (start >= 2 && txt[0] == 0xFF);
        int j = 0;
        for (int i = start; i + 1 < txtlen && j < outsz - 1; i += 2) {
            unsigned char lo = le ? txt[i] : txt[i+1];
            unsigned char hi = le ? txt[i+1] : txt[i];
            if (lo == 0 && hi == 0) break;
            if (hi == 0 && lo >= 0x20)
                out[j++] = (char)lo;
            else if (hi == 0)
                break;
        }
        out[j] = 0;
    }
}

static void id3v2_read_apic(const unsigned char *data, int size,
                            unsigned char **out_data, int *out_size) {
    if (size < 4) return;
    int enc = data[0];
    (void)enc;
    const unsigned char *p = data + 1;
    const unsigned char *end = data + size;
    /* Skip MIME type (null-terminated) */
    while (p < end && *p) p++;
    if (p >= end) return;
    p++; /* skip null */
    if (p >= end) return;
    /* Picture type byte */
    p++;
    if (p >= end) return;
    /* Skip description (null-terminated, encoding-dependent) */
    while (p < end && *p) p++;
    if (p >= end) return;
    p++; /* skip null */
    int img_size = (int)(end - p);
    if (img_size <= 0) return;
    *out_data = malloc(img_size);
    if (*out_data) {
        memcpy(*out_data, p, img_size);
        *out_size = img_size;
    }
}

/* MP3 bitrate table for MPEG1 Layer3 */
static const int mp3_bitrates[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const int mp3_samplerates[4] = { 44100, 48000, 32000, 0 };

static void read_id3_tags(const char *filepath, CopiedFile *cf) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    unsigned char hdr[10];
    if (fread(hdr, 1, 10, f) != 10) { fclose(f); return; }

    /* ID3v2 header */
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        unsigned int tag_size = id3_syncsafe(hdr + 6);
        unsigned char *tag_data = malloc(tag_size);
        if (!tag_data) { fclose(f); return; }
        if (fread(tag_data, 1, tag_size, f) != tag_size) {
            free(tag_data); fclose(f); return;
        }
        int ver = hdr[3]; /* 3 = ID3v2.3, 4 = ID3v2.4 */
        unsigned int pos = 0;
        while (pos + 10 <= tag_size) {
            char fid[5] = {0};
            memcpy(fid, tag_data + pos, 4);
            if (fid[0] == 0) break;
            unsigned int fsize;
            if (ver >= 4)
                fsize = id3_syncsafe(tag_data + pos + 4);
            else
                fsize = id3_be32(tag_data + pos + 4);
            /* unsigned int flags = (tag_data[pos+8] << 8) | tag_data[pos+9]; */
            pos += 10;
            if (fsize == 0 || pos + fsize > tag_size) break;

            if (strcmp(fid, "TIT2") == 0)
                id3v2_read_text(tag_data + pos, fsize, cf->title, sizeof(cf->title));
            else if (strcmp(fid, "TPE1") == 0)
                id3v2_read_text(tag_data + pos, fsize, cf->artist, sizeof(cf->artist));
            else if (strcmp(fid, "TALB") == 0)
                id3v2_read_text(tag_data + pos, fsize, cf->album, sizeof(cf->album));
            else if (strcmp(fid, "TCON") == 0)
                id3v2_read_text(tag_data + pos, fsize, cf->genre, sizeof(cf->genre));
            else if (strcmp(fid, "TDRC") == 0 || strcmp(fid, "TYER") == 0) {
                char ybuf[32] = {0};
                id3v2_read_text(tag_data + pos, fsize, ybuf, sizeof(ybuf));
                cf->year = atoi(ybuf);
            } else if (strcmp(fid, "APIC") == 0 && !cf->cover_data) {
                id3v2_read_apic(tag_data + pos, fsize,
                               &cf->cover_data, &cf->cover_size);
            }
            pos += fsize;
        }

        /* Try to read MP3 frame header for bitrate/duration after ID3v2 tag */
        long id3end = 10 + (long)tag_size;
        fseek(f, id3end, SEEK_SET);
        unsigned char fb[4];
        if (fread(fb, 1, 4, f) == 4 && fb[0] == 0xFF && (fb[1] & 0xE0) == 0xE0) {
            int br_idx = (fb[2] >> 4) & 0x0F;
            int sr_idx = (fb[2] >> 2) & 0x03;
            if (br_idx > 0 && br_idx < 15 && sr_idx < 3) {
                cf->bitrate = mp3_bitrates[br_idx];
                fseek(f, 0, SEEK_END);
                long fsize_l = ftell(f);
                long audio_size = fsize_l - id3end;
                if (audio_size > 0 && cf->bitrate > 0)
                    cf->duration = (int)(audio_size / (cf->bitrate * 125));
            }
        }
        free(tag_data);
    } else {
        /* Try ID3v1 at end of file */
        fseek(f, -128, SEEK_END);
        unsigned char v1[128];
        if (fread(v1, 1, 128, f) == 128 && v1[0] == 'T' && v1[1] == 'A' && v1[2] == 'G') {
            if (!cf->title[0]) {
                int n = 30; while (n > 0 && (v1[3+n-1] == ' ' || v1[3+n-1] == 0)) n--;
                if (n > 0) { memcpy(cf->title, v1+3, n); cf->title[n] = 0; }
            }
            if (!cf->artist[0]) {
                int n = 30; while (n > 0 && (v1[33+n-1] == ' ' || v1[33+n-1] == 0)) n--;
                if (n > 0) { memcpy(cf->artist, v1+33, n); cf->artist[n] = 0; }
            }
            if (!cf->album[0]) {
                int n = 30; while (n > 0 && (v1[63+n-1] == ' ' || v1[63+n-1] == 0)) n--;
                if (n > 0) { memcpy(cf->album, v1+63, n); cf->album[n] = 0; }
            }
            if (!cf->year) {
                char ybuf[5] = {0};
                memcpy(ybuf, v1+93, 4);
                cf->year = atoi(ybuf);
            }
        }
        /* Try to get bitrate from MP3 header at start */
        fseek(f, 0, SEEK_SET);
        unsigned char fb[4];
        if (fread(fb, 1, 4, f) == 4 && fb[0] == 0xFF && (fb[1] & 0xE0) == 0xE0) {
            int br_idx = (fb[2] >> 4) & 0x0F;
            int sr_idx = (fb[2] >> 2) & 0x03;
            if (br_idx > 0 && br_idx < 15 && sr_idx < 3) {
                cf->bitrate = mp3_bitrates[br_idx];
                fseek(f, 0, SEEK_END);
                long fsize_l = ftell(f);
                if (fsize_l > 0 && cf->bitrate > 0)
                    cf->duration = (int)(fsize_l / (cf->bitrate * 125));
            }
        }
    }

    /* Fallback: use filename as title */
    if (!cf->title[0] && cf->rel_path) {
        const char *fn = strrchr(cf->rel_path, '/');
        fn = fn ? fn + 1 : cf->rel_path;
        const char *dot = strrchr(fn, '.');
        int n = dot ? (int)(dot - fn) : (int)strlen(fn);
        if (n > (int)sizeof(cf->title) - 1) n = sizeof(cf->title) - 1;
        memcpy(cf->title, fn, n);
        cf->title[n] = 0;
    }

    if (cf->cover_data && cf->cover_size > 0)
        cover_hash(cf->cover_data, cf->cover_size, cf->cover_hash);

    fclose(f);
}

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
static int  g_done_acked;

/* Pending drive for import dialog */
static char g_pending_drive[256];
static char g_pending_path[1024];

/* Internal drive (selected by user or auto) */
static char g_internal_root[1024];  /* e.g. "/media/Denon" */
static char g_saved_target[256];    /* saved target drive name */

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

static void purge_unmounted_known(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_known_count; ) {
        char path[512];
        snprintf(path, sizeof(path), "/media/%s", g_known[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            g_known_count--;
            memmove(g_known[i], g_known[g_known_count], 256);
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
        if (strncmp(line, "target=", 7) == 0)
            snprintf(g_saved_target, sizeof(g_saved_target), "%s", line + 7);
    }
    fclose(f);
}

static void save_target(const char *name) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "target=%s\n", name);
    fclose(f);
    snprintf(g_saved_target, sizeof(g_saved_target), "%s", name);
    fprintf(stderr, "Saved target drive: %s\n", name);
}

/* ── Drive detection ── */

static int is_usb_device(const char *devname) {
    char base[64];
    snprintf(base, sizeof(base), "%s", devname);
    int len = strlen(base);
    while (len > 0 && base[len-1] >= '0' && base[len-1] <= '9') len--;
    base[len] = 0;

    char syspath[512];
    snprintf(syspath, sizeof(syspath), "/sys/block/%s/removable", base);
    FILE *f = fopen(syspath, "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1 && val == 1) { fclose(f); return 1; }
        fclose(f);
    }

    snprintf(syspath, sizeof(syspath), "/sys/block/%s", base);
    char resolved[1024];
    if (realpath(syspath, resolved))
        if (strstr(resolved, "/usb")) return 1;

    return 0;
}

static int get_mount_device(const char *mountpoint, char *devname, int devsize) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mp[512];
        if (sscanf(line, "%255s %511s", dev, mp) == 2) {
            if (strcmp(mp, mountpoint) == 0) {
                const char *b = strrchr(dev, '/');
                b = b ? b + 1 : dev;
                snprintf(devname, devsize, "%s", b);
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

#define MAX_CANDIDATES 8

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
        if (stat(dbpath, &st) == 0 && st.st_size > 0)
            snprintf(candidates[count++], 256, "%s", e->d_name);
    }
    closedir(d);
    return count;
}

static int filter_non_usb(char candidates[][256], int count) {
    int out = 0;
    for (int i = 0; i < count; i++) {
        char mp[512], devname[64];
        snprintf(mp, sizeof(mp), "/media/%s", candidates[i]);
        if (get_mount_device(mp, devname, sizeof(devname)) && is_usb_device(devname))
            continue;
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

static void unique_dest(char *dst, int dsz, const char *base_dir,
                        const char *rel, off_t src_size) {
    snprintf(dst, dsz, "%s/%s", base_dir, rel);
    struct stat st;
    if (lstat(dst, &st) != 0) return;
    if (st.st_size == src_size) return;

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
        if (st.st_size == src_size) return;
    }
}

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
        unique_dest(dst, sizeof(dst), dst_base, music_rel, st.st_size);

        const char *db_rel = dst + strlen(dst_base) + 1;

        int skipped = 0;
        struct stat dst_st;
        if (lstat(dst, &dst_st) == 0 && dst_st.st_size == st.st_size) {
            skipped = 1;
            pthread_mutex_lock(&g_lock);
            g_files_skipped++;
            pthread_mutex_unlock(&g_lock);
        } else {
            mkdirs(dst);
            if (copy_file_safe(fullsrc, dst) != 0) {
                pthread_mutex_lock(&g_lock);
                g_files_failed++;
                pthread_mutex_unlock(&g_lock);
                fprintf(stderr, "Copy failed: %s\n", relpath);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            g_files_copied++;
            g_bytes_copied += st.st_size;
            pthread_mutex_unlock(&g_lock);
        }

        /* Add to copied list for DB registration */
        pthread_mutex_lock(&g_lock);
        if (g_copied_count >= g_copied_cap) {
            int newcap = g_copied_cap ? g_copied_cap * 2 : 256;
            CopiedFile *tmp = realloc(g_copied, newcap * sizeof(CopiedFile));
            if (tmp) { g_copied = tmp; g_copied_cap = newcap; }
        }
        if (g_copied_count < g_copied_cap) {
            memset(&g_copied[g_copied_count], 0, sizeof(CopiedFile));
            g_copied[g_copied_count].rel_path = strdup(db_rel);
            g_copied[g_copied_count].file_type = strdup(get_extension(e->d_name));
            g_copied[g_copied_count].skipped = skipped;
            g_copied[g_copied_count].file_bytes = (long long)st.st_size;
            pthread_mutex_unlock(&g_lock);
            read_id3_tags(skipped ? fullsrc : dst, &g_copied[g_copied_count]);
            pthread_mutex_lock(&g_lock);
            g_copied_count++;
        }
        pthread_mutex_unlock(&g_lock);
    }
    closedir(d);
}

/* ── Backup with write lock ── */

static int backup_database(sqlite3 *db, const char *db_path, char *backup_out, int bsz) {
    char ts[64];
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", t);

    char backup_path[1024];
    snprintf(backup_path, sizeof(backup_path), "%s.usb-import-backup-%s", db_path, ts);
    snprintf(backup_out, bsz, "%s", backup_path);

    sqlite3 *dst_db = NULL;
    if (sqlite3_open(backup_path, &dst_db) != SQLITE_OK) {
        if (dst_db) sqlite3_close(dst_db);
        return -1;
    }

    sqlite3_backup *bk = sqlite3_backup_init(dst_db, "main", db, "main");
    if (!bk) {
        sqlite3_close(dst_db);
        unlink(backup_path);
        return -1;
    }

    int rc = sqlite3_backup_step(bk, -1);
    sqlite3_backup_finish(bk);
    sqlite3_close(dst_db);

    if (rc != SQLITE_DONE) {
        unlink(backup_path);
        return -1;
    }

    fprintf(stderr, "Backup created: %s\n", backup_path);
    return 0;
}

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

    char backups[32][256];
    int count = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && count < 32) {
        if (strncmp(e->d_name, prefix, plen) == 0)
            snprintf(backups[count++], 256, "%s", e->d_name);
    }
    closedir(d);

    if (count <= keep) return;

    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(backups[i], backups[j]) > 0) {
                char tmp[256];
                memcpy(tmp, backups[i], 256);
                memcpy(backups[i], backups[j], 256);
                memcpy(backups[j], tmp, 256);
            }

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
                "Not enough free space (%llu MB).",
                avail / (1024 * 1024));
            pthread_mutex_unlock(&g_lock);
            free(drive_name);
            return NULL;
        }
    }

    /* Copy files */
    pthread_mutex_lock(&g_lock);
    g_copied_count = 0;
    pthread_mutex_unlock(&g_lock);

    /* If USB has Engine Library/Music/, scan from there to avoid double-nesting */
    char usb_music[1024];
    snprintf(usb_music, sizeof(usb_music), "%s/Engine Library/Music", usb_path);
    struct stat usb_music_st;
    const char *scan_src = usb_path;
    if (stat(usb_music, &usb_music_st) == 0 && S_ISDIR(usb_music_st.st_mode)) {
        scan_src = usb_music;
        fprintf(stderr, "USB has Engine Library/Music/ — scanning from there\n");
    }
    copy_music_recursive(scan_src, el_dir, "", 0);

    pthread_mutex_lock(&g_lock);
    int total_for_db = g_copied_count;
    int copied = g_files_copied;
    int skipped = g_files_skipped;
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr, "Copy done: %d copied, %d skipped, %d for DB\n",
            copied, skipped, total_for_db);

    if (total_for_db == 0) {
        pthread_mutex_lock(&g_lock);
        g_importing = 0;
        g_import_done = 1;
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

        /* Backup */
        char backup[1024] = "";
        if (backup_database(db, db_path, backup, sizeof(backup)) != 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "Backup failed — aborting to protect your database.");
            pthread_mutex_unlock(&g_lock);
            sqlite3_close(db);
            free(drive_name);
            return NULL;
        }
        pthread_mutex_lock(&g_lock);
        snprintf(g_backup_path, sizeof(g_backup_path), "%s", backup);
        pthread_mutex_unlock(&g_lock);

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

        /* Insert tracks with full metadata */
        int db_error = 0;
        int tracks_registered = 0;
        int next_otid = max_otid + 1;

        for (int i = 0; i < total_for_db && !db_error; i++) {
            /* Insert cover art if present */
            int album_art_id = 0;
            if (g_copied[i].cover_data && g_copied[i].cover_size > 0) {
                /* Check if this cover hash already exists */
                sqlite3_stmt *st = NULL;
                if (sqlite3_prepare_v2(db, "SELECT id FROM AlbumArt WHERE hash=?",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_blob(st, 1, g_copied[i].cover_hash, 20, SQLITE_STATIC);
                    if (sqlite3_step(st) == SQLITE_ROW)
                        album_art_id = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                }
                if (!album_art_id) {
                    if (sqlite3_prepare_v2(db,
                        "INSERT INTO AlbumArt (hash, albumArt) VALUES (?, ?)",
                        -1, &st, NULL) == SQLITE_OK) {
                        sqlite3_bind_blob(st, 1, g_copied[i].cover_hash, 20, SQLITE_STATIC);
                        sqlite3_bind_blob(st, 2, g_copied[i].cover_data, g_copied[i].cover_size, SQLITE_STATIC);
                        if (sqlite3_step(st) == SQLITE_DONE)
                            album_art_id = (int)sqlite3_last_insert_rowid(db);
                        sqlite3_finalize(st);
                    }
                }
            }

            /* Insert track with all metadata */
            sqlite3_stmt *st = NULL;
            const char *insert_sql =
                "INSERT OR IGNORE INTO Track "
                "(path, filename, title, artist, album, genre, year, "
                "length, bitrate, fileBytes, fileType, albumArtId, "
                "isAnalyzed, isAvailable, isMetadataImported, "
                "dateCreated, dateAdded, lastEditTime, "
                "originDatabaseUuid, originTrackId) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                "0, 1, 1, strftime('%s','now'), strftime('%s','now'), "
                "strftime('%s','now'), ?, ?)";
            if (sqlite3_prepare_v2(db, insert_sql, -1, &st, NULL) != SQLITE_OK) {
                fprintf(stderr, "Prepare INSERT Track failed: %s\n", sqlite3_errmsg(db));
                db_error = 1;
                break;
            }

            const char *filename = strrchr(g_copied[i].rel_path, '/');
            filename = filename ? filename + 1 : g_copied[i].rel_path;

            sqlite3_bind_text(st, 1, g_copied[i].rel_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, filename, -1, SQLITE_STATIC);

            if (g_copied[i].title[0])
                sqlite3_bind_text(st, 3, g_copied[i].title, -1, SQLITE_STATIC);
            else
                sqlite3_bind_text(st, 3, filename, -1, SQLITE_STATIC);

            if (g_copied[i].artist[0])
                sqlite3_bind_text(st, 4, g_copied[i].artist, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(st, 4);

            if (g_copied[i].album[0])
                sqlite3_bind_text(st, 5, g_copied[i].album, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(st, 5);

            if (g_copied[i].genre[0])
                sqlite3_bind_text(st, 6, g_copied[i].genre, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(st, 6);

            if (g_copied[i].year > 0)
                sqlite3_bind_int(st, 7, g_copied[i].year);
            else
                sqlite3_bind_null(st, 7);

            if (g_copied[i].duration > 0)
                sqlite3_bind_int(st, 8, g_copied[i].duration);
            else
                sqlite3_bind_null(st, 8);

            if (g_copied[i].bitrate > 0)
                sqlite3_bind_int(st, 9, g_copied[i].bitrate);
            else
                sqlite3_bind_null(st, 9);

            sqlite3_bind_int64(st, 10, g_copied[i].file_bytes);
            sqlite3_bind_text(st, 11, g_copied[i].file_type, -1, SQLITE_STATIC);

            if (album_art_id > 0)
                sqlite3_bind_int(st, 12, album_art_id);
            else
                sqlite3_bind_null(st, 12);

            sqlite3_bind_text(st, 13, uuid, -1, SQLITE_STATIC);
            sqlite3_bind_int(st, 14, next_otid);

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
        }

        /* Create or find "USB Import" playlist */
        int playlist_id = 0;
        if (!db_error && total_for_db > 0) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                "SELECT id FROM Playlist WHERE title='USB Import' LIMIT 1",
                -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    playlist_id = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }

            if (!playlist_id) {
                int last_id = 0;
                if (sqlite3_prepare_v2(db,
                    "SELECT id FROM Playlist WHERE nextListId=0 AND parentListId=0 ORDER BY id DESC LIMIT 1",
                    -1, &st, NULL) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW)
                        last_id = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                }

                const char *pl_sql =
                    "INSERT INTO Playlist (title, parentListId, nextListId, isPersisted, "
                    "lastEditTime, isExplicitlyExported) "
                    "VALUES ('USB Import', 0, 0, 1, strftime('%s','now'), 1)";
                if (sqlite3_exec(db, pl_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
                    fprintf(stderr, "INSERT Playlist failed: %s\n", errmsg ? errmsg : "?");
                    sqlite3_free(errmsg); errmsg = NULL;
                    db_error = 1;
                }
                if (!db_error) {
                    playlist_id = (int)sqlite3_last_insert_rowid(db);
                    if (last_id > 0) {
                        char link_sql[256];
                        snprintf(link_sql, sizeof(link_sql),
                            "UPDATE Playlist SET nextListId=%d WHERE id=%d",
                            playlist_id, last_id);
                        sqlite3_exec(db, link_sql, NULL, NULL, NULL);
                    }
                }
            }
        }

        /* Add tracks to playlist */
        if (!db_error && playlist_id > 0 && total_for_db > 0) {
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

            for (int i = 0; i < total_for_db && !db_error; i++) {
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
                if (!otid) continue;

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

                if (sqlite3_prepare_v2(db,
                    "INSERT INTO PlaylistEntity (listId, trackId, databaseUuid, nextEntityId, membershipReference) "
                    "VALUES (?, ?, ?, 0, 1)",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(st, 1, playlist_id);
                    sqlite3_bind_int(st, 2, otid);
                    sqlite3_bind_text(st, 3, uuid, -1, SQLITE_STATIC);
                    int rc = sqlite3_step(st);
                    int new_id = (int)sqlite3_last_insert_rowid(db);
                    sqlite3_finalize(st);

                    if (rc != SQLITE_DONE) {
                        db_error = 1;
                        break;
                    }

                    if (last_entity > 0) {
                        char link_sql[256];
                        snprintf(link_sql, sizeof(link_sql),
                            "UPDATE PlaylistEntity SET nextEntityId=%d WHERE id=%d",
                            new_id, last_entity);
                        sqlite3_exec(db, link_sql, NULL, NULL, NULL);
                    }
                    last_entity = new_id;
                }
            }
        }

        /* Commit or rollback */
        if (db_error) {
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
            /* Free copied list */
            for (int i = 0; i < g_copied_count; i++) {
                free(g_copied[i].rel_path);
                free(g_copied[i].file_type);
                free(g_copied[i].cover_data);
            }
            g_copied_count = 0;
            return NULL;
        }

        if (sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            pthread_mutex_lock(&g_lock);
            g_importing = 0;
            g_import_done = 1;
            snprintf(g_error_msg, sizeof(g_error_msg),
                "COMMIT failed: %s", errmsg ? errmsg : "unknown");
            pthread_mutex_unlock(&g_lock);
            sqlite3_free(errmsg);
            sqlite3_close(db);
            add_known(drive_name);
            free(drive_name);
            for (int i = 0; i < g_copied_count; i++) {
                free(g_copied[i].rel_path);
                free(g_copied[i].file_type);
                free(g_copied[i].cover_data);
            }
            g_copied_count = 0;
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
        free(g_copied[i].cover_data);
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

        if (g_import_done) {
            char esc_err[512], esc_backup[512];
            json_escape(esc_err, sizeof(esc_err), g_error_msg);
            json_escape(esc_backup, sizeof(esc_backup), g_backup_path);
            snprintf(resp, sizeof(resp),
                "{\"done\":true,\"files_copied\":%d,\"files_skipped\":%d,\"files_failed\":%d,"
                "\"restart_pending\":%s,\"error\":\"%s\",\"backup\":\"%s\"}",
                g_files_copied, g_files_skipped, g_files_failed,
                g_restart_pending ? "true" : "false", esc_err, esc_backup);
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
            int has_target = g_internal_root[0] != 0;
            pthread_mutex_unlock(&g_lock);

            if (!has_target) {
                /* Need target selection — return candidates */
                char candidates[MAX_CANDIDATES][256];
                int ncan = find_engine_drives(candidates, MAX_CANDIDATES);
                int non_usb = filter_non_usb(candidates, ncan);

                char resp[4096];
                int pos = snprintf(resp, sizeof(resp),
                    "{\"needs_target\":true,\"candidates\":[");
                /* Show non-USB drives first, then all if none */
                int show_count = non_usb > 0 ? non_usb : ncan;
                char (*show)[256] = candidates;
                for (int i = 0; i < show_count; i++) {
                    char esc[512];
                    json_escape(esc, sizeof(esc), show[i]);
                    pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "%s\"%s\"", i ? "," : "", esc);
                }
                pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
                send_response(fd, 200, resp);
            } else if (g_importing) {
                send_response(fd, 409, "{\"error\":\"import already in progress\"}");
            } else {
                /* Start import */
                pthread_mutex_lock(&g_lock);
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
            send_response(fd, 200, "{\"ok\":false}");
        }

    } else if (strstr(buf, "POST /api/later")) {
        pthread_mutex_lock(&g_lock);
        g_restart_pending = 0;
        g_import_done = 0;
        pthread_mutex_unlock(&g_lock);
        send_response(fd, 200, "{\"ok\":true}");

    } else if (strstr(buf, "POST /api/select-target")) {
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
            pthread_mutex_unlock(&g_lock);

            if (remember) save_target(drive);

            fprintf(stderr, "Target drive selected: %s (remember=%d)\n", drive, remember);
            send_response(fd, 200, "{\"ok\":true}");
        }

    } else if (strstr(buf, "POST /api/reset-target")) {
        unlink(CONFIG_PATH);
        pthread_mutex_lock(&g_lock);
        g_saved_target[0] = 0;
        g_internal_root[0] = 0;
        pthread_mutex_unlock(&g_lock);
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
                   (g_import_done && !g_done_acked);
        pthread_mutex_unlock(&g_lock);
        if (busy) continue;

        purge_unmounted_known();

        /* Auto-detect internal drive if saved target configured */
        pthread_mutex_lock(&g_lock);
        if (g_saved_target[0]) {
            char check[1024];
            snprintf(check, sizeof(check), "/media/%s/Engine Library/Database2/m.db", g_saved_target);
            struct stat st;
            if (stat(check, &st) == 0)
                snprintf(g_internal_root, sizeof(g_internal_root), "/media/%s", g_saved_target);
            else
                g_internal_root[0] = 0;
        } else if (!g_internal_root[0]) {
            /* Auto-detect if only one non-USB Engine drive exists */
            char candidates[MAX_CANDIDATES][256];
            pthread_mutex_unlock(&g_lock);
            int ncan = find_engine_drives(candidates, MAX_CANDIDATES);
            int non_usb = filter_non_usb(candidates, ncan);
            pthread_mutex_lock(&g_lock);
            if (non_usb == 1)
                snprintf(g_internal_root, sizeof(g_internal_root), "/media/%s", candidates[0]);
        }
        char internal[1024];
        snprintf(internal, sizeof(internal), "%s", g_internal_root);
        pthread_mutex_unlock(&g_lock);

        /* Scan for USB drives to offer import */
        DIR *d = opendir("/media");
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;

            char mp[512];
            snprintf(mp, sizeof(mp), "/media/%s", e->d_name);

            /* Skip the internal drive */
            if (internal[0] && strcmp(mp, internal) == 0) continue;

            if (is_known(e->d_name)) continue;

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

    fprintf(stderr, "USB Import daemon v8.0 on port %d\n", PORT);

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
