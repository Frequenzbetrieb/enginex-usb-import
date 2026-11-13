#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sqlite3.h"

#define EL_BASE "/media/Denon/Engine Library/"

static void rmdir_if_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    int has = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        has = 1; break;
    }
    closedir(d);
    if (!has && rmdir(path) == 0)
        printf("  rmdir: %s\n", path);
}

int main(void) {
    sqlite3 *db;
    if (sqlite3_open("/media/Denon/Engine Library/Database2/m.db", &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB\n"); return 1;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_stmt *st;
    char *err = NULL;

    /* Count before */
    int total = 0, usb = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM Track", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM Track WHERE id > 16104", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) usb = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    printf("Total tracks: %d, USB-imported (id>16104): %d\n", total, usb);

    /* Delete files for USB-imported tracks */
    printf("\n=== Deleting files ===\n");
    int deleted = 0, missing = 0;
    if (sqlite3_prepare_v2(db, "SELECT path FROM Track WHERE id > 16104", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *rel = (const char*)sqlite3_column_text(st, 0);
            if (!rel) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s%s", EL_BASE, rel);
            if (unlink(full) == 0) {
                deleted++;
                char *sl = strrchr(full, '/');
                if (sl) { *sl = 0; rmdir_if_empty(full); }
            } else {
                missing++;
                if (missing <= 3) printf("  missing: %s\n", full);
            }
        }
        sqlite3_finalize(st);
    }
    printf("Files: %d deleted, %d missing\n", deleted, missing);

    /* Delete AlbumArt only used by USB tracks */
    sqlite3_exec(db,
        "DELETE FROM AlbumArt WHERE id IN ("
        "  SELECT DISTINCT a.id FROM AlbumArt a "
        "  JOIN Track t ON t.albumArtId = a.id "
        "  WHERE t.id > 16104 AND a.id NOT IN ("
        "    SELECT albumArtId FROM Track WHERE id <= 16104 AND albumArtId IS NOT NULL"
        "  ))", NULL, NULL, &err);
    if (err) { fprintf(stderr, "AlbumArt: %s\n", err); sqlite3_free(err); err = NULL; }

    /* Delete playlist entities */
    sqlite3_exec(db, "DELETE FROM PlaylistEntity WHERE listId IN (SELECT id FROM Playlist WHERE title='USB Import')", NULL, NULL, &err);
    if (err) { fprintf(stderr, "PE: %s\n", err); sqlite3_free(err); err = NULL; }

    /* Delete playlist */
    sqlite3_exec(db, "DELETE FROM Playlist WHERE title='USB Import'", NULL, NULL, &err);
    if (err) { fprintf(stderr, "PL: %s\n", err); sqlite3_free(err); err = NULL; }

    /* Delete USB tracks */
    sqlite3_exec(db, "DELETE FROM Track WHERE id > 16104", NULL, NULL, &err);
    if (err) { fprintf(stderr, "TR: %s\n", err); sqlite3_free(err); err = NULL; }

    int remaining = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM Track", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) remaining = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    printf("\nDone. Remaining tracks: %d\n", remaining);

    sqlite3_close(db);

    /* Reset config */
    unlink("/data/enginex/mods/usb-import/config");
    printf("Config reset.\n");
    return 0;
}
