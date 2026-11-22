// Super Smash Bros. Ultimate NSP extractor: a small program on its own that makes the game's exefs and romfs folders from
// the base game's NSP and the update's NSP (the update is a patch of the base game's RomFS: both are needed). Put it in any
// folder and drag both NSP files onto it, or put both beside it and open it: it writes smash-ultimate\exefs and
// smash-ultimate\romfs next to itself (the game is about 16 GB or more). Needs the player's own prod.keys (next to it, in
// keys\, in ~/.switch, or where Ryujinx / yuzu keep it, or named with --keys).
//
//   smash-nsp-extract [--keys prod.keys] [--out folder] [base.nsp update.nsp]
//
// The work is in runtime/nspimport.c, the same code the MHGU extractor uses.
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define CHDIR _chdir
#else
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define CHDIR chdir
#endif
#include "../runtime/nspimport.h"

#define APP_SMASH 0x01006A800016E000ull  // Super Smash Bros. Ultimate (its update's id is 01006A800016E800)
static const uint64_t k_games[1] = {APP_SMASH};
#define GAME_TEXT "Super Smash Bros. Ultimate (01006A800016E000)"
#define NEED_GB 20  // (room to ask for: the game's own size is about 16-18 GB, with the update merged)

static bool is_nsp(const char *p) {
    size_t n = strlen(p);
    return n > 4 && tolower((unsigned char)p[n - 4]) == '.' && tolower((unsigned char)p[n - 3]) == 'n' &&
           tolower((unsigned char)p[n - 2]) == 's' && tolower((unsigned char)p[n - 1]) == 'p';
}

// the program's own folder, into dir; false when it can't be told
static bool exe_dir(char *dir, size_t size) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)size);
    if (!n || n >= size) return false;
#else
    ssize_t n = readlink("/proc/self/exe", dir, size - 1);
    if (n <= 0) return false;
    dir[n] = 0;
#endif
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (!s) return false;
    *s = 0;
    return true;
}

// free bytes on the drive that holds folder (which may not exist yet: its nearest parent), or -1 when it can't be told
static double free_gb(const char *folder) {
    char p[1300];
    snprintf(p, sizeof p, "%s", folder);
    for (;;) {
        struct stat st;
        if (stat(p, &st) == 0) break;
        char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
        if (b > s) s = b;
        if (!s) { snprintf(p, sizeof p, "."); break; }
        *s = 0;
        if (!p[0]) { snprintf(p, sizeof p, "/"); break; }
    }
#ifdef _WIN32
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(p, &avail, NULL, NULL)) return -1;
    return (double)avail.QuadPart / (1024.0 * 1024.0 * 1024.0);
#else
    struct statvfs v;
    if (statvfs(p, &v)) return -1;
    return (double)v.f_bavail * (double)v.f_frsize / (1024.0 * 1024.0 * 1024.0);
#endif
}

static void progress(void *user, const char *what, uint64_t done, uint64_t total) {
    (void)user;
    if (!total) { printf("%s...\n", what); fflush(stdout); return; }
    printf("\r  %s: %llu / %llu MB   ", what, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    if (done >= total) printf("\n");
    fflush(stdout);
}

// a window that closes the moment the program ends (dragging a file onto it) must not hide what it said
static int done_with(int code) {
#ifdef _WIN32
    printf("\nPress Enter to close this window.");
    fflush(stdout);
    getchar();
#endif
    return code;
}

int main(int argc, char **argv) {
    const char *keys = NULL, *out = NULL;
    char *nsps[16];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (is_nsp(argv[i]) && n < 16) nsps[n++] = argv[i];
        else {
            printf("Usage: %s [--keys prod.keys] [--out folder] base.nsp update.nsp\n"
                   "Or drag the base game's NSP and the update's NSP onto this program.\n", argv[0]);
            return done_with(2);
        }
    }
    printf("Super Smash Bros. Ultimate: extract the game from its NSP files\n\n");

    // the program's folder: the keys are looked for there, and the game's folder goes there
    char home[1100] = ".", dflt[1200], found[4][1100];
    char *list[4];
    if (exe_dir(home, sizeof home) && CHDIR(home) != 0) snprintf(home, sizeof home, ".");
    snprintf(dflt, sizeof dflt, "%s/smash-ultimate", home);
    if (!out) out = dflt;

    if (!n) {  // nothing dragged on it: the NSP files beside it
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("*.nsp", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (n < 4) { snprintf(found[n], sizeof found[n], "%s", fd.cFileName); nsps[n] = found[n]; n++; }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(".");
        for (struct dirent *e; d && (e = readdir(d));)
            if (is_nsp(e->d_name) && n < 4) { snprintf(found[n], sizeof found[n], "%s", e->d_name); nsps[n] = found[n]; n++; }
        if (d) closedir(d);
#endif
    }
    if (n != 2) {
        printf(n ? "Found %d NSP files: two are needed, the base game's and the update's.\n\n" : "No NSP files found.\n\n", n);
        printf("Drag the base game's NSP and the update's NSP onto this program, or put both in this folder\n(%s)\n"
               "and open it again.\n", home);
        return done_with(1);
    }
    for (int i = 0; i < 4; i++) list[i] = i < n ? nsps[i] : NULL;

    NspImport job = {.nsp_paths = (const char *const *)list, .nsp_count = n, .keys_path = keys, .out_dir = out,
                     .progress = progress, .app_ids = k_games, .app_id_count = 1, .games = GAME_TEXT};
    char err[1100];
    uint64_t app = 0;
    if (!nsp_identify(&job, &app, err, sizeof err)) {
        printf("It did not work:\n\n%s\n", err);
        return done_with(1);
    }
    printf("Base/update: %s\n             %s\nInto:        %s\n", nsps[0], nsps[1], out);
    double gb = free_gb(out);
    if (gb >= 0 && gb < NEED_GB)
        printf("\nWARNING: only %.1f GB are free there and the game needs about %d GB. If it runs out of room it stops with a\n"
               "message and nothing half-written stays behind; choose a drive with more room with --out <folder>.\n", gb, NEED_GB);
    printf("(a few minutes)\n\n");
    if (!nsp_import(&job, err, sizeof err)) {
        printf("\nIt did not work:\n\n%s\n", err);
        return done_with(1);
    }
    printf("\nDone. The game's exefs and romfs folders are in %s\n", out);
    return done_with(0);
}
