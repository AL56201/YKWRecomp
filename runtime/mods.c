// RomFS mods. Each folder in mods/ (YKW_MODS_DIR overrides) is one mod; its RomFS files are found in, first match:
//   mods/<name>/romfs/                                   (GameBanana "FA" zips: a top-level romfs folder)
//   mods/<name>/contents/0100C0000CEEA000/romfs/          (Atmosphere layout)
//   mods/<name>/atmosphere/contents/0100C0000CEEA000/romfs/
// Mods load in name order; a later one wins when two replace the same file. An empty file named "disabled" in a mod's
// folder turns it off; YKW_NO_MODS=1 turns them all off (test runs). Nothing in a mod is changed or copied.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle.h"
#include "mods.h"

#define MODS_MAX 64
static struct { char name[256]; char root[1200]; } g_mods[MODS_MAX];
static int g_nmods = -1;

static bool is_dir(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

void mods_scan(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    const char *off = getenv("YKW_NO_MODS");
    if (off && *off && strcmp(off, "0")) { LOG("[mods] off (YKW_NO_MODS)\n"); return; }
    const char *dir = getenv("YKW_MODS_DIR");
    if (!dir || !*dir) dir = "mods";
    DIR *d = opendir(dir);
    if (!d) { LOG("[mods] no %s folder: playing without mods\n", dir); return; }
    char names[MODS_MAX][256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MODS_MAX) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) strlcpy(names[n++], e->d_name, sizeof names[0]);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], name_cmp);
    static const char *layouts[] = {"romfs", "contents/" YKW_TITLE_ID "/romfs",
                                    "atmosphere/contents/" YKW_TITLE_ID "/romfs"};
    for (int i = 0; i < n; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s/disabled", dir, names[i]);
        struct stat sb;
        if (stat(p, &sb) == 0) { LOG("[mods] %s: disabled\n", names[i]); continue; }
        bool found = false;
        for (size_t k = 0; k < sizeof layouts / sizeof *layouts && !found; k++) {
            snprintf(p, sizeof p, "%s/%s/%s", dir, names[i], layouts[k]);
            if (!is_dir(p)) continue;
            strlcpy(g_mods[g_nmods].name, names[i], sizeof g_mods[0].name);
            strlcpy(g_mods[g_nmods].root, p, sizeof g_mods[0].root);
            g_nmods++;
            found = true;
            LOG("[mods] %s: romfs at %s\n", names[i], p);
        }
        if (!found) LOG("[mods] %s: no romfs folder (romfs/ or contents/%s/romfs/), skipped\n", names[i], YKW_TITLE_ID);
    }
}

int mods_count(void) { mods_scan(); return g_nmods; }
const char *mods_romfs_root(int i) { return g_mods[i].root; }
const char *mods_name(int i) { return g_mods[i].name; }

bool mods_romfs_override(const char *path, char *host, size_t cap) {
    mods_scan();
    for (int i = g_nmods - 1; i >= 0; i--) {
        snprintf(host, cap, "%s/%s", g_mods[i].root, path);
        struct stat sb;
        if (stat(host, &sb) == 0 && S_ISREG(sb.st_mode)) return true;
    }
    return false;
}

RT_ON_RESET(mods_reset) { g_nmods = -1; }

// (services/fs.c: each RomFS file as the game first reads it; YKW_ROMFS_LOG=1 logs them)
void romfs_file_read_hook(const char *path) {
    if (ENV("YKW_ROMFS_LOG")) LOG("[romfs] %s\n", path);
}
