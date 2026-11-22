// NSP merger: puts several NSP files (a game's base, its update and its DLC packs) into ONE multi-content NSP, the kind
// installers such as DBI, Tinfoil or Awoo take: every title stays its own title (the update is not melted into the base game:
// that would need new encrypted and signed files), the file just holds all of them. An NSP is a PFS0 archive of the title's
// files (.nca, .tik, .cert, ...), so this only reads the files of each NSP and writes them into a new PFS0. No keys needed,
// nothing is decrypted, and the NCAs, tickets and certificates are copied byte for byte.
//
//   nsp-merge [--out final.nsp] base.nsp update.nsp dlc1.nsp dlc2.nsp ...
//
// Drag all the NSP files onto the program, or put them in its folder and open it. The result is written beside the first
// file (its name without the [ids], and " [FULL].nsp"): about the sum of the sizes, so a drive that is not FAT32 (a FAT32
// drive can't hold a file over 4 GB).
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
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

#define MAX_SRC 64
#define MAX_ENT 8192
#define HDR_ALIGN 0x4000  // (the PFS0 header, string table padding included, ends on this boundary: as official NSPs do)

typedef struct {
    char path[1100];
    FILE *f;
    uint64_t size;
} Src;
typedef struct {
    char name[256];
    uint64_t off, size;  // in the source file
    int src;
} Ent;

static Src g_src[MAX_SRC];
static int g_nsrc;
static Ent *g_ent;
static int g_nent;

static char g_err[1500];
static bool fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return false;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != suffix[i]) return false;
    return true;
}
static bool is_nsp(const char *p) { return ends_with(p, ".nsp"); }

// the 16-hex-digit title id in a name like "Game [01006A800016E000][v0].nsp", or 0
static uint64_t id_in_name(const char *name) {
    for (const char *p = name; (p = strchr(p, '[')); p++) {
        int i = 0;
        uint64_t v = 0;
        while (i < 16 && isxdigit((unsigned char)p[1 + i])) v = v << 4 | (uint64_t)(isdigit((unsigned char)p[1 + i]) ? p[1 + i] - '0' : tolower((unsigned char)p[1 + i]) - 'a' + 10), i++;
        if (i == 16 && p[17] == ']') return v;
    }
    return 0;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > s) s = b;
    return s ? s + 1 : path;
}

// one NSP's PFS0 file table into g_ent
static bool read_pfs0(int si) {
    Src *s = &g_src[si];
    uint8_t h[16];
    if (fseeko(s->f, 0, SEEK_SET) || fread(h, 1, 16, s->f) != 16 || memcmp(h, "PFS0", 4))
        return fail("%s is not an NSP (it has no PFS0 header: an XCI, an NSZ or another file?).", s->path);
    uint32_t n = le32(h + 4), strsz = le32(h + 8);
    if (n == 0 || n > 4096 || strsz > (1u << 20)) return fail("%s has a damaged file table.", s->path);
    size_t tab = 16 + (size_t)n * 24;
    uint8_t *raw = malloc(tab + strsz + 1);
    if (!raw) return fail("Out of memory.");
    if (fseeko(s->f, 0, SEEK_SET) || fread(raw, 1, tab + strsz, s->f) != tab + strsz) { free(raw); return fail("%s is cut off.", s->path); }
    raw[tab + strsz] = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = raw + 16 + (size_t)i * 24;
        uint32_t so = le32(e + 16);
        uint64_t off = tab + strsz + le64(e), size = le64(e + 8);
        if (so >= strsz || off + size < off || off + size > s->size) {
            free(raw);
            return fail("%s is cut off or damaged: a file in it goes past its end (an incomplete copy or download?).", s->path);
        }
        const char *name = (const char *)raw + tab + so;
        if (strchr(name, '/') || strchr(name, '\\') || !name[0] || strlen(name) > 255) { free(raw); return fail("%s has a file name this won't take.", s->path); }
        if (ends_with(name, ".ncz") || ends_with(name, ".nsz")) {
            free(raw);
            return fail("%s is compressed (NSZ): extract it to a normal NSP first (the nsz tool does).", s->path);
        }
        // the same file in two NSPs (a certificate or ticket both carry): kept once; the same name with another size is a problem
        int dup = -1;
        for (int j = 0; j < g_nent; j++)
            if (!strcmp(g_ent[j].name, name)) { dup = j; break; }
        if (dup >= 0) {
            if (g_ent[dup].size != size) {
                bool r = fail("%s and %s both have a file called %s, but they are different. They can't both go into one NSP.",
                              g_src[g_ent[dup].src].path, s->path, name);
                free(raw);
                return r;
            }
            continue;
        }
        if (g_nent >= MAX_ENT) { free(raw); return fail("Too many files."); }
        snprintf(g_ent[g_nent].name, sizeof g_ent[g_nent].name, "%s", name);
        g_ent[g_nent].off = off;
        g_ent[g_nent].size = size;
        g_ent[g_nent].src = si;
        g_nent++;
    }
    free(raw);
    return true;
}

static double free_gb(const char *folder) {
    char p[1600];
    snprintf(p, sizeof p, "%s", folder[0] ? folder : ".");
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

static int done_with(int code) {
#ifdef _WIN32
    printf("\nPress Enter to close this window.");
    fflush(stdout);
    getchar();
#endif
    return code;
}

// the merged NSP: header, then the files one after the other, in the order the sources gave them. Written as name.part and
// renamed when it is whole, so a stopped run leaves no half file looking like a finished one.
static bool write_merged(const char *out) {
    size_t tab = 16 + (size_t)g_nent * 24, strsz = 0;
    for (int i = 0; i < g_nent; i++) strsz += strlen(g_ent[i].name) + 1;
    size_t hdr = tab + strsz;
    size_t padded = (hdr + HDR_ALIGN - 1) / HDR_ALIGN * HDR_ALIGN;
    strsz += padded - hdr;
    uint8_t *h = calloc(1, padded);
    if (!h) return fail("Out of memory.");
    memcpy(h, "PFS0", 4);
    put32(h + 4, (uint32_t)g_nent);
    put32(h + 8, (uint32_t)strsz);
    uint64_t off = 0, total = 0;
    size_t so = 0;
    for (int i = 0; i < g_nent; i++) {
        uint8_t *e = h + 16 + (size_t)i * 24;
        put64(e, off);
        put64(e + 8, g_ent[i].size);
        put32(e + 16, (uint32_t)so);
        memcpy(h + tab + so, g_ent[i].name, strlen(g_ent[i].name));
        so += strlen(g_ent[i].name) + 1;
        off += g_ent[i].size;
    }
    total = off;
    char part[1500];
    if (snprintf(part, sizeof part, "%s.part", out) >= (int)sizeof part) { free(h); return fail("The path is too long."); }
    FILE *f = fopen(part, "wb");
    if (!f) { free(h); return fail("The file %s could not be written (%s).", part, strerror(errno)); }
    bool ok = fwrite(h, 1, padded, f) == padded;
    free(h);
    uint8_t *buf = malloc(4u << 20);
    if (!buf) ok = fail("Out of memory.");
    uint64_t done = 0, last = 0;
    for (int i = 0; ok && i < g_nent; i++) {
        Src *s = &g_src[g_ent[i].src];
        if (fseeko(s->f, (off_t)g_ent[i].off, SEEK_SET)) { ok = fail("Reading %s failed.", s->path); break; }
        for (uint64_t left = g_ent[i].size; left;) {
            size_t take = left < (4u << 20) ? (size_t)left : (4u << 20);
            if (fread(buf, 1, take, s->f) != take) { ok = fail("Reading %s failed: is it damaged or cut off?", s->path); break; }
            if (fwrite(buf, 1, take, f) != take) {
                ok = fail("Writing %s failed (%s). Is the disk full, or is it a FAT32 drive (files over 4 GB don't fit)?", part, strerror(errno));
                break;
            }
            left -= take;
            done += take;
            if (done - last >= (64u << 20) || done == total) {
                printf("\r  %llu / %llu MB   ", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
                fflush(stdout);
                last = done;
            }
        }
    }
    free(buf);
    printf("\n");
    if (fclose(f) && ok) ok = fail("Writing %s failed (%s): is the disk full?", part, strerror(errno));
    if (!ok) { remove(part); return false; }
    // checked: it reads back as the PFS0 it should be, with the size it should have
    f = fopen(part, "rb");
    uint8_t chk[16];
    bool good = f && fread(chk, 1, 16, f) == 16 && !memcmp(chk, "PFS0", 4) && le32(chk + 4) == (uint32_t)g_nent;
    if (good) good = fseeko(f, 0, SEEK_END) == 0 && (uint64_t)ftello(f) == padded + total;
    if (f) fclose(f);
    if (!good) { remove(part); return fail("The finished file did not check out (its size or header is wrong). Nothing was kept."); }
    if (rename(part, out)) { remove(part); return fail("The file could not be named %s (%s).", out, strerror(errno)); }
    return true;
}

int main(int argc, char **argv) {
    const char *out_arg = NULL;
    char *given[MAX_SRC];
    int ng = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_arg = argv[++i];
        else if (is_nsp(argv[i]) && ng < MAX_SRC) given[ng++] = argv[i];
        else {
            printf("Usage: %s [--out final.nsp] base.nsp update.nsp dlc.nsp ...\n"
                   "Or drag all the NSP files (the base game, its update and its DLC) onto this program.\n", argv[0]);
            return done_with(2);
        }
    }
    printf("NSP merger: the base game, its update and its DLC in one NSP\n\n");

    char home[1100] = ".", found[MAX_SRC][1100];
    // (only when nothing was given: names given on the command line are relative to where it was started)
    if (!ng && exe_dir(home, sizeof home) && CHDIR(home) != 0) snprintf(home, sizeof home, ".");
    if (!ng) {  // nothing dragged on it: the NSP files beside it (those it made are left out)
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("*.nsp", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (ng < MAX_SRC && !ends_with(fd.cFileName, "[full].nsp")) { snprintf(found[ng], sizeof found[ng], "%s", fd.cFileName); given[ng] = found[ng]; ng++; }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(".");
        for (struct dirent *e; d && (e = readdir(d));)
            if (is_nsp(e->d_name) && !ends_with(e->d_name, "[full].nsp") && ng < MAX_SRC) { snprintf(found[ng], sizeof found[ng], "%s", e->d_name); given[ng] = found[ng]; ng++; }
        if (d) closedir(d);
#endif
        // (the order the system lists them in means nothing: by name, so the base game's lower id comes first)
        for (int i = 1; i < ng; i++)
            for (int j = i; j > 0 && strcmp(given[j - 1], given[j]) > 0; j--) { char *t = given[j]; given[j] = given[j - 1]; given[j - 1] = t; }
    }
    if (ng < 2) {
        printf(ng ? "Found only one NSP file: give the base game's, its update's and its DLC.\n"
                  : "No NSP files found.\n");
        printf("\nDrag all the NSP files (the base game, its update and its DLC) onto this program, or put them in this\nfolder (%s) and open it again.\n", home);
        return done_with(1);
    }

    g_ent = calloc(MAX_ENT, sizeof *g_ent);
    if (!g_ent) { printf("Out of memory.\n"); return done_with(1); }
    uint64_t sum = 0;
    int rc = 1;
    bool ok = true;
    for (int i = 0; ok && i < ng; i++) {
        Src *s = &g_src[g_nsrc];
        snprintf(s->path, sizeof s->path, "%s", given[i]);
        s->f = fopen(s->path, "rb");
        if (!s->f) { ok = fail("The file %s could not be opened (%s).", s->path, strerror(errno)); break; }
        g_nsrc++;
        if (fseeko(s->f, 0, SEEK_END)) { ok = fail("The file %s could not be read.", s->path); break; }
        s->size = (uint64_t)ftello(s->f);
        sum += s->size;
        ok = read_pfs0(g_nsrc - 1);
    }
    // a name with a title id of another game among them: said, not forbidden (names are the player's)
    if (ok) {
        uint64_t first = 0;
        for (int i = 0; i < g_nsrc; i++) {
            uint64_t id = id_in_name(base_name(g_src[i].path));
            if (!id) continue;
            if (!first) first = id;
            else if ((id >> 16) != (first >> 16))
                printf("WARNING: %s has the title id %016llX, which looks like another game than %016llX. They go in anyway; check the\n"
                       "file names if that is not what you meant.\n\n", base_name(g_src[i].path), (unsigned long long)id, (unsigned long long)first);
        }
    }
    if (ok) {
        char outpath[1500];
        if (out_arg) snprintf(outpath, sizeof outpath, "%s", out_arg);
        else {
            // beside the first file: its name up to the first [ (the ids), then " [FULL].nsp"
            const char *p = g_src[0].path, *name = base_name(p);
            char stem[300];
            snprintf(stem, sizeof stem, "%s", name);
            char *br = strchr(stem, '[');
            if (br && br > stem) *br = 0;
            else if (ends_with(stem, ".nsp")) stem[strlen(stem) - 4] = 0;
            for (size_t n = strlen(stem); n && (stem[n - 1] == ' ' || stem[n - 1] == '_'); n--) stem[n - 1] = 0;
            if (!stem[0]) snprintf(stem, sizeof stem, "merged");
            size_t dirlen = (size_t)(name - p);
            snprintf(outpath, sizeof outpath, "%.*s%s [FULL].nsp", (int)dirlen, p, stem);
        }
        printf("%d NSP files, %d files inside, about %.1f GB:\n", g_nsrc, g_nent, (double)sum / 1073741824.0);
        for (int i = 0; i < g_nsrc; i++) printf("  %s\n", base_name(g_src[i].path));
        printf("Into: %s\n", outpath);
        struct stat st;
        char outdir[1500];
        snprintf(outdir, sizeof outdir, "%s", outpath);
        char *sl = strrchr(outdir, '/'), *bs = strrchr(outdir, '\\');
        if (bs > sl) sl = bs;
        if (sl) *sl = 0; else snprintf(outdir, sizeof outdir, ".");
        if (stat(outpath, &st) == 0) ok = fail("%s already exists: delete it, or name another with --out.", outpath);
        double gb = ok ? free_gb(outdir) : -1;
        if (ok && gb >= 0 && gb * 1073741824.0 < (double)sum * 1.01)
            printf("\nWARNING: only %.1f GB are free there, and about %.1f GB are needed. If it runs out of room it stops and nothing\n"
                   "half-written stays behind; use --out to name a file on a drive with more room.\n", gb, (double)sum / 1073741824.0);
        if (ok) {
            printf("\n");
            ok = write_merged(outpath);
            if (ok) {
                printf("\nDone. %s\n", outpath);
                if (sum > 0xFFFFFFFFull) printf("It is over 4 GB: keep it on an exFAT or NTFS drive (FAT32 can't hold it).\n");
                rc = 0;
            }
        }
    }
    if (!ok) printf("\nIt did not work:\n\n%s\n", g_err);
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].f) fclose(g_src[i].f);
    free(g_ent);
    return done_with(rc);
}
