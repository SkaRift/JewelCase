/* Jewel Case - a full-screen ROM launcher for jailbroken PS5 consoles.
 *
 * Scans ROM folders, groups games by system, shows box art and starts each
 * game in the right emulator through ps5-payload-websrv (/hbldr or /launch).
 *
 * Builds for the PS5 (ps5-payload-sdk + SDL2) and for Linux (testing).
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "cJSON.h"

/* alloc.c: large blocks come from direct memory (the native heap is small) */
void *gs_malloc(size_t size);
void *gs_calloc(size_t n, size_t size);
void *gs_realloc(void *p, size_t size);
void gs_free(void *p);
#define STBI_MALLOC(sz) gs_malloc(sz)
#define STBI_REALLOC(p, newsz) gs_realloc(p, newsz)
#define STBI_FREE(p) gs_free(p)
#define STBTT_malloc(x, u) ((void)(u), gs_malloc(x))
#define STBTT_free(x, u) ((void)(u), gs_free(x))
#define STBIW_MALLOC(sz) gs_malloc(sz)
#define STBIW_REALLOC(p, newsz) gs_realloc(p, newsz)
#define STBIW_FREE(p) gs_free(p)

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_FAILURE_STRINGS
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO_FAILURE_STRINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "fonts.h"
#include "logo.h"
#include "media.h"
#include "splash.h"

/* everything below allocates through alloc.c */
#define malloc(n) gs_malloc(n)
#define calloc(n, s) gs_calloc(n, s)
#define realloc(p, n) gs_realloc(p, n)
#define free(p) gs_free(p)

int music_start(char (*paths)[1024], int n);
void music_set_volume(int pct);
void music_stop(void);
int music_playing(void);
const char *music_now(void);

#define GS_VERSION "1.0"
#define GS_TITLE_ID "PPSA75837"

#ifdef GS_NATIVE
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int gs_missing_imports(char *buf, size_t cap);
void gs_notify(const char *text);
const char *gs_vout_init(void);
void gs_vout_present(const void *pixels, int pitch);
static SDL_Surface *g_screen;   /* Jewel Case draws here; vout.c puts it on screen */

/* A native title must never return from main or call exit(): say why and close. */
static void gs_fatal(const char *what, const char *detail) {
    char msg[600];
    snprintf(msg, sizeof(msg), "Jewel Case can't start: %s%s%s", what, detail && detail[0] ? ": " : "",
             detail ? detail : "");
    printf("%s\n", msg);
    gs_notify(msg);
    sleep(5);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}
#endif
#define SCREEN_W 1920
#define SCREEN_H 1080
#define WEBSRV_PORT 8080

/* ------------------------------------------------------------------------ */
/* Small utilities                                                           */
/* ------------------------------------------------------------------------ */

static char g_fsroot[512] = "";   /* host testing: prefix for absolute paths */
#ifdef GS_NATIVE
static char g_appdir[512] = "/app0";
#else
static char g_appdir[512] = "/data/homebrew/JewelCase";
#endif
/* what Jewel Case saves (recent, favorites, settings) lives outside the app
 * folder, so installing an update never touches it */
#define USER_DIR "/data/jewelcase"

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) {
        logf_("out of memory");
        exit(1);
    }
    return p;
}

static void scopy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

static void stem_of(const char *name, char *out, size_t cap) {
    scopy(out, cap, name);
    char *d = strrchr(out, '.');
    if (d) *d = 0;
}

/* "Super Mario World (USA) [!]" -> "Super Mario World" */
static void pretty_name(const char *stem, char *out, size_t cap) {
    size_t o = 0;
    int depth = 0;
    for (const char *p = stem; *p && o + 1 < cap; p++) {
        if (*p == '(' || *p == '[') { depth++; continue; }
        if (*p == ')' || *p == ']') { if (depth) depth--; continue; }
        if (depth) continue;
        out[o++] = (*p == '_') ? ' ' : *p;
    }
    out[o] = 0;
    /* collapse spaces / trim */
    char *r = out, *w = out;
    int sp = 1;
    for (; *r; r++) {
        if (*r == ' ') {
            if (!sp) *w++ = ' ';
            sp = 1;
        } else {
            *w++ = *r;
            sp = 0;
        }
    }
    while (w > out && w[-1] == ' ') w--;
    *w = 0;
    if (!out[0]) scopy(out, cap, stem);
    /* "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past" */
    static const char *arts[] = {", The", ", A", ", An"};
    for (int i = 0; i < 3; i++) {
        size_t al = strlen(arts[i]);
        char *hit = strstr(out, arts[i]);
        if (!hit || (hit[al] && strncmp(hit + al, " - ", 3) && strncmp(hit + al, ": ", 2))) continue;
        char moved[512];
        size_t head = (size_t)(hit - out);
        if (head + strlen(out) + 8 >= sizeof(moved)) break;
        snprintf(moved, sizeof(moved), "%s %.*s%s", arts[i] + 2, (int)head, out, hit + al);
        scopy(out, cap, moved);
        break;
    }
}

static int disc_number(const char *stem) {
    char low[512];
    scopy(low, sizeof(low), stem);
    lower(low);
    const char *keys[] = {"(disc", "(disk", "(cd"};
    for (int k = 0; k < 3; k++) {
        const char *p = strstr(low, keys[k]);
        if (p) {
            p += strlen(keys[k]);
            while (*p == ' ') p++;
            if (isdigit((unsigned char)*p)) return atoi(p);
        }
    }
    return 0;
}

/* host testing maps /data/... into a fake tree */
static void real_path(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", g_fsroot, path);
}

/* folder index written by jc-scan.elf (native: the sandbox can't list folders) */
static int g_ix_on;
static int ix_type(const char *path);   /* 'D', 'F' or 0 */

static DIR *open_dir(const char *path) {
    char rp[1024];
    real_path(path, rp, sizeof(rp));
    return opendir(rp);
}

static int is_dir(const char *path) {
    if (g_ix_on) return ix_type(path) == 'D';
    char rp[1024];
    struct stat st;
    real_path(path, rp, sizeof(rp));
    return stat(rp, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path) {
    if (g_ix_on) return ix_type(path) == 'F';
    char rp[1024];
    struct stat st;
    real_path(path, rp, sizeof(rp));
    return stat(rp, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file(const char *path, size_t *len) {
    char rp[1024];
    real_path(path, rp, sizeof(rp));
    FILE *f = fopen(rp, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

static int write_file(const char *path, const char *data) {
    char rp[1024];
    real_path(path, rp, sizeof(rp));
    FILE *f = fopen(rp, "wb");
    if (!f) return -1;
    fputs(data, f);
    fclose(f);
    return 0;
}

typedef struct {
    char name[256];
    int dir;
} DirEnt;

/* ---- folder index ------------------------------------------------------- */
static char *g_ix_buf;
static char **g_ix_path;
static char *g_ix_kind;
static int g_ix_n;
typedef struct { char *tid, *dir, *name; } IxTitle;
static IxTitle *g_ix_titles;
static int g_ix_ntitles;

static int ix_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int ix_lower_bound(const char *p) {
    int lo = 0, hi = g_ix_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(g_ix_path[mid], p) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int ix_type(const char *path) {
    int i = ix_lower_bound(path);
    if (i < g_ix_n && !strcmp(g_ix_path[i], path)) return g_ix_path[i][-2];
    return 0;
}

/* Parses an index; takes ownership of buf. Returns entries, or -1 if incomplete. */
static int ix_load(char *buf) {
    if (strncmp(buf, "jewelcase-index 1\n", strlen("jewelcase-index 1\n"))) return -1;
    size_t len = strlen(buf);
    if (len < 4 || strcmp(buf + len - 4, "end\n")) return -1;
    if (g_ix_buf) {   /* a rescan replaces the previous list */
        free(g_ix_path);
        free(g_ix_titles);
        free(g_ix_buf);
        g_ix_buf = NULL;
    }
    int lines = 0;
    for (char *c = buf; *c; c++) if (*c == '\n') lines++;
    g_ix_path = xcalloc((size_t)lines + 1, sizeof(char *));
    g_ix_titles = xcalloc((size_t)lines + 1, sizeof(IxTitle));
    g_ix_n = g_ix_ntitles = 0;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        if ((ln[0] == 'D' || ln[0] == 'F') && ln[1] == '\t') {
            g_ix_path[g_ix_n++] = ln + 2;          /* kind stays at path[-2] */
        } else if (ln[0] == 'T' && ln[1] == '\t') {
            char *a = ln + 2, *b = strchr(a, '\t');
            if (!b) continue;
            *b++ = 0;
            char *c = strchr(b, '\t');
            if (!c) continue;
            *c++ = 0;
            g_ix_titles[g_ix_ntitles++] = (IxTitle){a, b, c};
        }
    }
    qsort(g_ix_path, (size_t)g_ix_n, sizeof(char *), ix_cmp);
    g_ix_buf = buf;
    g_ix_on = 1;
    return g_ix_n;
}

/* list a directory; returns count, entries in *out (caller frees) */
static int list_dir(const char *path, DirEnt **out) {
    *out = NULL;
    if (g_ix_on) {
        if (ix_type(path) != 'D') return -1;
        char prefix[1100];
        snprintf(prefix, sizeof(prefix), "%s/", path);
        size_t pl = strlen(prefix);
        int cap = 64, n = 0;
        DirEnt *v = xcalloc((size_t)cap, sizeof(DirEnt));
        for (int i = ix_lower_bound(prefix); i < g_ix_n && !strncmp(g_ix_path[i], prefix, pl); i++) {
            const char *rest = g_ix_path[i] + pl;
            if (!*rest || strchr(rest, '/') || rest[0] == '.') continue;
            if (n && !strcmp(v[n - 1].name, rest)) continue;   /* listed twice */
            if (n == cap) {
                cap *= 2;
                v = realloc(v, (size_t)cap * sizeof(DirEnt));
                if (!v) exit(1);
            }
            scopy(v[n].name, sizeof(v[n].name), rest);
            v[n].dir = g_ix_path[i][-2] == 'D';
            n++;
        }
        *out = v;
        return n;
    }
    DIR *d = open_dir(path);
    if (!d) return -1;
    int cap = 64, n = 0;
    DirEnt *v = xcalloc((size_t)cap, sizeof(DirEnt));
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap *= 2;
            v = realloc(v, (size_t)cap * sizeof(DirEnt));
            if (!v) exit(1);
        }
        scopy(v[n].name, sizeof(v[n].name), e->d_name);
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR) v[n].dir = 1;
        else if (e->d_type == DT_REG) v[n].dir = 0;
        else v[n].dir = is_dir(full);
        n++;
    }
    closedir(d);
    *out = v;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Data model                                                                */
/* ------------------------------------------------------------------------ */

#define MAX_LIST 16
#define MAX_CORES 96

typedef struct {
    char id[32];
    char name[64];
    int is_title;               /* 0 = homebrew ELF via /hbldr, 1 = installed app via /launch */
    char path[512];             /* ELF path (explicit or detected) */
    char dir[512];
    char title_id[16];
    char match[MAX_LIST][48];  int nmatch;
    char exclude[MAX_LIST][48]; int nexclude;
    char elf[MAX_LIST][64];    int nelf;
    char args[MAX_LIST][256];  int nargs;
    char envk[MAX_LIST][64];
    char envv[MAX_LIST][256];  int nenv;
    char cores_dir[512];
    char cores[MAX_CORES][64]; int ncores;
    char core_files[MAX_CORES][96];
    char title_match[48];       /* installed app: match its titleName ("a|b": either) */
    char title_ids[64];         /* installed app: known title IDs ("PPSA1|PPSA2") */
    int ps5ra;                  /* PS5 RetroArch title with game mode (request file) */
    int found;
} Emu;

typedef struct {
    char emu[32];
    char cores[MAX_LIST][48];
    int ncores;
} EmuChoice;

typedef struct Game {
    char title[160];
    char path[1024];
    char dir[1024];
    char art[1024];
    int sys;                    /* index into systems */
    char stem[256];             /* file (or folder) name without extension */
    /* carousel image: the game's cartridge / disc with its art, and its floor reflection */
    SDL_Texture *media, *refl;
    SDL_Texture *back;          /* its back, made when it's first turned around */
    int media_w, media_h, refl_h;
    int media_pending;          /* built without art while its cover is downloading */
    int extra_pending;          /* disc: its logo / disc scan not looked for yet */
    int extra_state;            /* what extras_state() said when the image was built */
    Uint32 cover_check;         /* last time a pending cover was looked for */
} Game;

typedef struct {
    char id[32];
    char name[64];
    char shortname[16];
    Uint32 color;
    char folders[MAX_LIST][32]; int nfolders;
    char exts[MAX_LIST][8];     int nexts;
    EmuChoice emus[6];          int nemus;
    int folder_games;
    /* resolved */
    Game *games; int ngames;
    int emu;                    /* index into emus array, -1 none */
    char core_path[640];
    char core_name[64];
} System;

static Emu g_emus[32];
static int g_nemus;
static System g_sys[64];
static int g_nsys;
static char g_roots[48][256];
static int g_nroots;
static int g_hide_empty = 1;
static char g_rom_folder[64] = "/data/roms";   /* where Create system folders makes them */

/* recent list (newest first) and favourites */
#define MAX_RECENT 30
static char g_recent[MAX_RECENT][1024];
static long g_recent_time[MAX_RECENT];
static int g_nrecent;
#define MAX_FAVS 200
static char (*g_favs)[1024];
static int g_nfavs;

static const char *HOMEBREW_ROOTS[] = {
    "/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew"
};

static const char *DEFAULT_ROOTS[] = {
    "/data/roms",
    "/mnt/usb0/roms", "/mnt/usb1/roms", "/mnt/usb2/roms", "/mnt/usb3/roms",
    "/mnt/usb4/roms", "/mnt/usb5/roms", "/mnt/usb6/roms", "/mnt/usb7/roms",
    "/mnt/ext0/roms", "/mnt/ext1/roms",
    "/data/homebrew/roms",
    "/mnt/usb0/homebrew/roms", "/mnt/usb1/homebrew/roms",
    "/mnt/ext0/homebrew/roms", "/mnt/ext1/homebrew/roms"
};

static void split_into(const char *s, char sep, char (*dst)[48], int *n, int max) {
    *n = 0;
    const char *p = s;
    while (*p && *n < max) {
        const char *q = strchr(p, sep);
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len >= 48) len = 47;
        memcpy(dst[*n], p, len);
        dst[*n][len] = 0;
        lower(dst[*n]);
        if (dst[*n][0]) (*n)++;
        if (!q) break;
        p = q + 1;
    }
}

static Emu *emu_add(const char *id, const char *name, const char *match,
                    const char *exclude, const char *elfs, const char *args) {
    Emu *e = &g_emus[g_nemus++];
    memset(e, 0, sizeof(*e));
    scopy(e->id, sizeof(e->id), id);
    scopy(e->name, sizeof(e->name), name);
    split_into(match, '|', e->match, &e->nmatch, MAX_LIST);
    split_into(exclude, '|', e->exclude, &e->nexclude, MAX_LIST);
    /* elf names: keep case */
    e->nelf = 0;
    {
        const char *p = elfs;
        while (*p && e->nelf < MAX_LIST) {
            const char *q = strchr(p, '|');
            size_t len = q ? (size_t)(q - p) : strlen(p);
            if (len >= 64) len = 63;
            memcpy(e->elf[e->nelf], p, len);
            e->elf[e->nelf][len] = 0;
            e->nelf++;
            if (!q) break;
            p = q + 1;
        }
    }
    /* args: separated by \n */
    e->nargs = 0;
    {
        const char *p = args;
        while (*p && e->nargs < MAX_LIST) {
            const char *q = strchr(p, '\n');
            size_t len = q ? (size_t)(q - p) : strlen(p);
            if (len >= 256) len = 255;
            memcpy(e->args[e->nargs], p, len);
            e->args[e->nargs][len] = 0;
            e->nargs++;
            if (!q) break;
            p = q + 1;
        }
    }
    return e;
}

static void emu_env(Emu *e, const char *k, const char *v) {
    if (e->nenv >= MAX_LIST) return;
    scopy(e->envk[e->nenv], 64, k);
    scopy(e->envv[e->nenv], 256, v);
    e->nenv++;
}

static Emu *emu_find(const char *id) {
    for (int i = 0; i < g_nemus; i++)
        if (!strcmp(g_emus[i].id, id)) return &g_emus[i];
    return NULL;
}

static Emu *app_emu(const char *id, const char *name, const char *ids, const char *names, const char *args) {
    Emu *e = emu_add(id, name, "", "", "", args);
    e->is_title = 1;
    scopy(e->title_ids, sizeof(e->title_ids), ids);
    scopy(e->title_match, sizeof(e->title_match), names);
    return e;
}

static void default_emulators(void) {
    Emu *e;
    g_nemus = 0;
    e = emu_add("retroarch", "RetroArch (homebrew)", "retroarch", "", "retroarch.elf|eboot.elf",
                "-f\n-c\n{dir}/retroarch.cfg\n-L\n{core}\n{rom}");
    emu_env(e, "HOME", "{dir}");
    emu_env(e, "LD_LIBRARY_PATH", "{dir}");
    scopy(e->cores_dir, sizeof(e->cores_dir), "{dir}/.config/retroarch/cores");
    e = emu_add("retroarch_app", "RetroArch", "", "", "", "-L\n{core}\n{rom}");
    e->is_title = 1;
    scopy(e->title_match, sizeof(e->title_match), "retroarch");
    scopy(e->title_ids, sizeof(e->title_ids), "PPSA99169");
    scopy(e->cores_dir, sizeof(e->cores_dir), "auto");
    /* standalone emulator apps (their own home icon), found by title ID or name.
     * "--rom <file> --exit-after-game" is the PS5 forwarder format; others take the
     * file as their first argument; a few can only be opened (pick the game there). */
    app_emu("eden_app", "ProsperoEden", "PPSA99008", "prosperoeden", "--rom\n{rom}\n--exit-after-game");
    app_emu("porpoise", "Porpoise", "PPSA99764", "porpoise", "--rom\n{rom}\n--exit-after-game");
    app_emu("cemu_har", "PS5CEMU-HAR", "PPSA99360", "cemu", "--rom\n{rom}\n--exit-after-game");
    app_emu("ps5x360", "PS5X360", "PPSA50011", "x360|xenia", "{rom}");
    /* Mupen64Plus reads the game from argv[1]; the launch list may start at argv[0],
     * so the game goes in both (it ignores anything after argv[1]) */
    app_emu("mupen_app", "Mupen64Plus PS5", "PPSA99064", "mupen", "{rom}\n{rom}");
    app_emu("psflycast", "PSFlycast", "PPSA99247", "flycast", "{rom}");
    /* no launch arguments yet: these open the app (PS5SX2's forwarder support is a pending PR) */
    app_emu("ps5sx2_app", "PS5SX2", "PPSA99203", "ps5sx2", "--rom\n{rom}\n--exit-after-game");
    app_emu("xpsemu_app", "XPSemu", "PPSA97358", "xpsemu|xemu", "");
    app_emu("psxs5", "PSXS5", "PPSA97510", "psxs5", "");
    e = emu_add("mednafen", "Mednafen", "mednafen", "", "mednafen.elf|eboot.elf", "{rom}");
    emu_env(e, "MEDNAFEN_HOME", "{dir}");
    emu_add("ps2", "PS2 emulator", "ps5sx2|pcsx2|ps2", "", "eboot.elf", "{rom}");
    emu_add("ps3", "PS3 emulator", "rpcs3|ps3", "", "eboot.elf", "{rom}");
    emu_add("switch", "Switch emulator", "prosperoeden|eden|yuzu|switch", "", "eboot.elf", "{rom}");
    emu_add("xbox", "Xbox emulator", "xpsemu|xemu|xbox", "360", "eboot.elf", "{rom}");
    emu_add("xbox360", "Xbox 360 emulator", "xenia|360", "", "eboot.elf", "{rom}");
}

static void list_set(char (*dst)[32], int *n, const char *csv) {
    *n = 0;
    const char *p = csv;
    while (*p && *n < MAX_LIST) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len >= 32) len = 31;
        memcpy(dst[*n], p, len);
        dst[*n][len] = 0;
        (*n)++;
        if (!q) break;
        p = q + 1;
    }
}

static void exts_set(System *s, const char *csv) {
    s->nexts = 0;
    const char *p = csv;
    while (*p && s->nexts < MAX_LIST) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len >= 8) len = 7;
        memcpy(s->exts[s->nexts], p, len);
        s->exts[s->nexts][len] = 0;
        lower(s->exts[s->nexts]);
        s->nexts++;
        if (!q) break;
        p = q + 1;
    }
}

/* emus spec: "retroarch:mesen,nestopia;mednafen" */
static void emus_set(System *s, const char *spec) {
    s->nemus = 0;
    char buf[512];
    scopy(buf, sizeof(buf), spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ";", &save); tok && s->nemus < 6; tok = strtok_r(NULL, ";", &save)) {
        EmuChoice *c = &s->emus[s->nemus++];
        memset(c, 0, sizeof(*c));
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = 0;
            split_into(colon + 1, ',', c->cores, &c->ncores, MAX_LIST);
        }
        scopy(c->emu, sizeof(c->emu), tok);
    }
}

static System *sys_add(const char *id, const char *name, const char *shortname, Uint32 color,
                       const char *folders, const char *exts, const char *emus, int folder_games) {
    System *s = &g_sys[g_nsys++];
    memset(s, 0, sizeof(*s));
    scopy(s->id, sizeof(s->id), id);
    scopy(s->name, sizeof(s->name), name);
    scopy(s->shortname, sizeof(s->shortname), shortname);
    s->color = color;
    list_set(s->folders, &s->nfolders, folders);
    exts_set(s, exts);
    emus_set(s, emus);
    s->folder_games = folder_games;
    s->emu = -1;
    return s;
}

static void default_systems(void) {
    g_nsys = 0;
    sys_add("nes", "NES", "NES", 0xd6453d, "nes,famicom,fc", "nes,fds,unf,zip,7z",
            "retroarch:mesen,nestopia,fceumm,quicknes;mednafen", 0);
    sys_add("snes", "Super Nintendo", "SNES", 0x7b6cf0, "snes,sfc,superfamicom", "sfc,smc,fig,swc,bs,zip,7z",
            "retroarch:snes9x,bsnes,snes9x2010;mednafen", 0);
    sys_add("n64", "Nintendo 64", "N64", 0x2f9e58, "n64", "z64,n64,v64,zip,7z",
            "mupen_app;retroarch:mupen64plus_next,parallel_n64", 0);
    sys_add("gb", "Game Boy", "GB", 0x8b9a46, "gb", "gb,zip,7z",
            "retroarch:gambatte,sameboy,mgba;mednafen", 0);
    sys_add("gbc", "Game Boy Color", "GBC", 0x5d3fd3, "gbc", "gbc,zip,7z",
            "retroarch:gambatte,sameboy,mgba;mednafen", 0);
    sys_add("gba", "Game Boy Advance", "GBA", 0x4d4dd8, "gba", "gba,zip,7z",
            "retroarch:mgba,mednafen_gba,gpsp,vbam;mednafen", 0);
    sys_add("nds", "Nintendo DS", "DS", 0x9aa3ad, "nds,ds", "nds,zip,7z",
            "cemu_har;retroarch:melonds,melondsds,desmume", 0);
    sys_add("genesis", "Genesis / Mega Drive", "MD", 0x4a4f63, "genesis,megadrive,md", "md,gen,smd,bin,zip,7z",
            "retroarch:genesis_plus_gx,picodrive;mednafen", 0);
    sys_add("sms", "Master System", "SMS", 0xc8302f, "sms,mastersystem", "sms,zip,7z",
            "retroarch:genesis_plus_gx,picodrive;mednafen", 0);
    sys_add("gg", "Game Gear", "GG", 0x2b5fb5, "gg,gamegear", "gg,zip,7z",
            "retroarch:genesis_plus_gx;mednafen", 0);
    sys_add("pce", "PC Engine / TG-16", "PCE", 0xe08a2c, "pce,pcengine,tg16", "pce,cue,chd,zip",
            "retroarch:mednafen_pce_fast,mednafen_pce;mednafen", 0);
    sys_add("arcade", "Arcade", "ARC", 0xe0b12c, "arcade,fbneo,mame,neogeo", "zip,7z",
            "retroarch:fbneo,mame2003_plus,mame2010", 0);
    sys_add("psx", "PlayStation", "PS1", 0x8f96a3, "psx,ps1,playstation", "m3u,cue,chd,pbp,iso",
            "psxs5;retroarch:swanstation,pcsx_rearmed,mednafen_psx_hw,mednafen_psx;mednafen", 0);
    sys_add("psp", "PSP", "PSP", 0x3e4a5c, "psp", "iso,cso,chd,pbp", "retroarch:ppsspp", 0);
    sys_add("ps2", "PlayStation 2", "PS2", 0x1f4fa8, "ps2", "iso,chd,cso,bin", "ps5sx2_app;ps2;retroarch:pcsx2,ps5sx2,lrps2", 0);
    sys_add("ps3", "PlayStation 3", "PS3", 0x5b6170, "ps3", "iso", "ps3", 1);
    sys_add("gc", "GameCube", "GC", 0x6a55c9, "gc,gamecube,ngc", "iso,rvz,gcz,ciso,gcm,dol",
            "porpoise;retroarch:dolphin", 0);
    sys_add("wii", "Wii", "WII", 0x9fb2c4, "wii", "iso,rvz,wbfs,wia,ciso,wad", "porpoise;retroarch:dolphin", 0);
    sys_add("dc", "Dreamcast", "DC", 0xe8763a, "dc,dreamcast", "chd,gdi,cdi,m3u", "psflycast;retroarch:flycast", 0);
    sys_add("saturn", "Sega Saturn", "SAT", 0x5a6cb4, "saturn,ss", "chd,cue,m3u,ccd",
            "retroarch:mednafen_saturn,yabasanshiro,yabause", 0);
    sys_add("segacd", "Sega CD", "SCD", 0x8a5cc8, "segacd,megacd", "chd,cue,m3u",
            "retroarch:genesis_plus_gx,picodrive", 0);
    sys_add("32x", "Sega 32X", "32X", 0xb7473f, "32x,sega32x", "32x,zip,7z", "retroarch:picodrive", 0);
    sys_add("3ds", "Nintendo 3DS", "3DS", 0xd0453f, "3ds,n3ds", "3ds,cci,cxi,cia,3dsx,z3ds,zcci", "cemu_har;retroarch:azahar,citra", 0);
    sys_add("wiiu", "Wii U", "WIIU", 0x009ac7, "wiiu,wii u", "wua,wud,wux,rpx", "cemu_har", 1);
    sys_add("pcfx", "PC-FX", "PCFX", 0xc9a227, "pcfx", "chd,cue,ccd", "retroarch:mednafen_pcfx", 0);
    sys_add("vb", "Virtual Boy", "VB", 0xb8202a, "vb,virtualboy", "vb,vboy,zip", "retroarch:mednafen_vb;mednafen", 0);
    sys_add("ngp", "Neo Geo Pocket", "NGP", 0x3b7bbf, "ngp,ngpc", "ngp,ngc,zip", "retroarch:mednafen_ngp;mednafen", 0);
    sys_add("ws", "WonderSwan", "WS", 0x6b7f99, "ws,wsc,wonderswan", "ws,wsc,zip", "retroarch:mednafen_wswan;mednafen", 0);
    sys_add("lynx", "Atari Lynx", "LYNX", 0xd07a2a, "lynx", "lnx,zip", "retroarch:handy,mednafen_lynx;mednafen", 0);
    sys_add("atari2600", "Atari 2600", "2600", 0xb5552b, "atari2600,a2600", "a26,bin,zip", "retroarch:stella,stella2014", 0);
    sys_add("atari5200", "Atari 5200", "5200", 0x6d6d6d, "atari5200,a5200", "a52,bin,zip", "retroarch:a5200", 0);
    sys_add("atari7800", "Atari 7800", "7800", 0x9a3c2f, "atari7800,a7800", "a78,bin,zip", "retroarch:prosystem", 0);
    sys_add("jaguar", "Atari Jaguar", "JAG", 0xc0392b, "jaguar,atarijaguar", "j64,jag,zip", "retroarch:virtualjaguar", 0);
    sys_add("3do", "3DO", "3DO", 0x7a6a3a, "3do", "iso,chd,cue", "retroarch:opera", 0);
    sys_add("neocd", "Neo Geo CD", "NGCD", 0x2d5aa8, "neocd,neogeocd", "chd,cue", "retroarch:neocd", 0);
    sys_add("amiga", "Amiga", "AMI", 0xd8582c, "amiga", "adf,ipf,hdf,lha,zip", "retroarch:puae,puae2021", 0);
    sys_add("c64", "Commodore 64", "C64", 0x6b5e9f, "c64", "d64,t64,prg,crt,zip", "retroarch:vice_x64,vice_x64sc", 0);
    sys_add("dos", "DOS", "DOS", 0x4f5b66, "dos,pc", "zip,dosz,exe,com", "retroarch:dosbox_pure", 0);
    sys_add("pokemini", "Pokemon Mini", "PM", 0xe5c32f, "pokemini", "min,zip", "retroarch:pokemini", 0);
    sys_add("scummvm", "ScummVM", "SCVM", 0x3c9a54, "scummvm", "scummvm,svm", "retroarch:scummvm", 0);
    sys_add("switch", "Nintendo Switch", "NSW", 0xe60012, "switch,nsw", "nsp,xci,nca,nro", "eden_app;switch", 0);
    sys_add("xbox", "Xbox", "XBOX", 0x3c8a2e, "xbox", "iso,xiso", "xpsemu_app;xbox", 0);
    sys_add("xbox360", "Xbox 360", "360", 0x7ab51d, "xbox360,x360,360", "iso,xex,zar", "ps5x360;xbox360", 1);
}

/* ------------------------------------------------------------------------ */
/* Config file (jewelcase.json), same format as the web add-on               */
/* ------------------------------------------------------------------------ */

static void json_strlist(cJSON *arr, char (*dst)[48], int *n) {
    *n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && *n < MAX_LIST) {
            scopy(dst[*n], 48, it->valuestring);
            lower(dst[*n]);
            (*n)++;
        }
    }
}

static void load_config(char *err, size_t errcap) {
    char path[600];
    snprintf(path, sizeof(path), "%s", USER_DIR "/jewelcase.json");
    {
        char rp[1300];
        real_path(path, rp, sizeof(rp));
        FILE *t = fopen(rp, "rb");
        if (t) fclose(t);
        else snprintf(path, sizeof(path), "%s/jewelcase.json", g_appdir);
    }
    size_t len = 0;
    char *txt = read_file(path, &len);
    g_nroots = 0;
    cJSON *root = NULL;
    if (txt) {
        root = cJSON_Parse(txt);
        if (!root) snprintf(err, errcap, "jewelcase.json has a syntax error near: %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        free(txt);
    }

    int replace = 0;
    if (root) {
        cJSON *rr = cJSON_GetObjectItem(root, "romRoots"), *it;
        cJSON_ArrayForEach(it, rr) {
            if (cJSON_IsString(it) && g_nroots < 48) scopy(g_roots[g_nroots++], 256, it->valuestring);
        }
        replace = cJSON_IsTrue(cJSON_GetObjectItem(root, "replaceRomRoots"));
        cJSON *he = cJSON_GetObjectItem(root, "hideEmptySystems");
        if (cJSON_IsBool(he)) g_hide_empty = cJSON_IsTrue(he);
    }
    if (!replace) {
        for (size_t i = 0; i < sizeof(DEFAULT_ROOTS) / sizeof(*DEFAULT_ROOTS) && g_nroots < 48; i++)
            scopy(g_roots[g_nroots++], 256, DEFAULT_ROOTS[i]);
    }
    if (!root) return;

    /* emulators */
    cJSON *emus = cJSON_GetObjectItem(root, "emulators"), *eo;
    cJSON_ArrayForEach(eo, emus) {
        if (!cJSON_IsObject(eo) || !eo->string) continue;
        Emu *e = emu_find(eo->string);
        if (!e) {
            if (g_nemus >= 32) continue;
            e = emu_add(eo->string, eo->string, "", "", "eboot.elf", "{rom}");
        }
        cJSON *v;
        if ((v = cJSON_GetObjectItem(eo, "name")) && cJSON_IsString(v)) scopy(e->name, sizeof(e->name), v->valuestring);
        if ((v = cJSON_GetObjectItem(eo, "type")) && cJSON_IsString(v)) e->is_title = !strcmp(v->valuestring, "title");
        if ((v = cJSON_GetObjectItem(eo, "titleId")) && cJSON_IsString(v)) scopy(e->title_id, sizeof(e->title_id), v->valuestring);
        if ((v = cJSON_GetObjectItem(eo, "path")) && cJSON_IsString(v)) scopy(e->path, sizeof(e->path), v->valuestring);
        if ((v = cJSON_GetObjectItem(eo, "titleName")) && cJSON_IsString(v)) scopy(e->title_match, sizeof(e->title_match), v->valuestring);
        if ((v = cJSON_GetObjectItem(eo, "match")) && cJSON_IsString(v)) split_into(v->valuestring, '|', e->match, &e->nmatch, MAX_LIST);
        if ((v = cJSON_GetObjectItem(eo, "exclude")) && cJSON_IsString(v)) split_into(v->valuestring, '|', e->exclude, &e->nexclude, MAX_LIST);
        if ((v = cJSON_GetObjectItem(eo, "coresDir")) && cJSON_IsString(v)) scopy(e->cores_dir, sizeof(e->cores_dir), v->valuestring);
        if ((v = cJSON_GetObjectItem(eo, "elf")) && cJSON_IsArray(v)) {
            e->nelf = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) if (cJSON_IsString(it) && e->nelf < MAX_LIST) scopy(e->elf[e->nelf++], 64, it->valuestring);
        }
        if ((v = cJSON_GetObjectItem(eo, "args")) && cJSON_IsArray(v)) {
            e->nargs = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) if (cJSON_IsString(it) && e->nargs < MAX_LIST) scopy(e->args[e->nargs++], 256, it->valuestring);
        }
        if ((v = cJSON_GetObjectItem(eo, "env")) && cJSON_IsObject(v)) {
            e->nenv = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) if (cJSON_IsString(it) && it->string) emu_env(e, it->string, it->valuestring);
        }
    }

    /* systems */
    cJSON *systems = cJSON_GetObjectItem(root, "systems"), *so;
    cJSON_ArrayForEach(so, systems) {
        if (!cJSON_IsObject(so) || !so->string) continue;
        System *s = NULL;
        for (int i = 0; i < g_nsys; i++) if (!strcmp(g_sys[i].id, so->string)) s = &g_sys[i];
        if (!s) {
            if (g_nsys >= 64) continue;
            s = sys_add(so->string, so->string, so->string, 0x5a6378, so->string, "", "", 0);
        }
        cJSON *v;
        if ((v = cJSON_GetObjectItem(so, "name")) && cJSON_IsString(v)) scopy(s->name, sizeof(s->name), v->valuestring);
        if ((v = cJSON_GetObjectItem(so, "short")) && cJSON_IsString(v)) scopy(s->shortname, sizeof(s->shortname), v->valuestring);
        if ((v = cJSON_GetObjectItem(so, "folderGames")) && cJSON_IsBool(v)) s->folder_games = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(so, "folders")) && cJSON_IsArray(v)) {
            s->nfolders = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) if (cJSON_IsString(it) && s->nfolders < MAX_LIST) scopy(s->folders[s->nfolders++], 32, it->valuestring);
        }
        if ((v = cJSON_GetObjectItem(so, "exts")) && cJSON_IsArray(v)) {
            s->nexts = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) if (cJSON_IsString(it) && s->nexts < MAX_LIST) {
                scopy(s->exts[s->nexts], 8, it->valuestring);
                lower(s->exts[s->nexts]);
                s->nexts++;
            }
        }
        if ((v = cJSON_GetObjectItem(so, "emus")) && cJSON_IsArray(v)) {
            s->nemus = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, v) {
                if (!cJSON_IsObject(it) || s->nemus >= 6) continue;
                cJSON *ev = cJSON_GetObjectItem(it, "emu");
                if (!cJSON_IsString(ev)) continue;
                EmuChoice *c = &s->emus[s->nemus++];
                memset(c, 0, sizeof(*c));
                scopy(c->emu, sizeof(c->emu), ev->valuestring);
                json_strlist(cJSON_GetObjectItem(it, "cores"), c->cores, &c->ncores);
            }
        }
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------------ */
/* Emulator discovery                                                        */
/* ------------------------------------------------------------------------ */

static void fill_tokens(const char *in, char *out, size_t cap, const char *rom, const char *dir,
                        const char *core, const char *romdir) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap;) {
        const char *rep = NULL;
        size_t skip = 0;
        if (!strncmp(p, "{rom}", 5)) { rep = rom; skip = 5; }
        else if (!strncmp(p, "{dir}", 5)) { rep = dir; skip = 5; }
        else if (!strncmp(p, "{core}", 6)) { rep = core; skip = 6; }
        else if (!strncmp(p, "{romdir}", 8)) { rep = romdir; skip = 8; }
        if (rep) {
            for (const char *r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
            p += skip;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

static int name_matches(Emu *e, const char *folder) {
    char low[256];
    scopy(low, sizeof(low), folder);
    lower(low);
    for (int i = 0; i < e->nexclude; i++) if (strstr(low, e->exclude[i])) return 0;
    for (int i = 0; i < e->nmatch; i++) if (strstr(low, e->match[i])) return 1;
    return 0;
}

/* collect *_libretro.{so,prx,sprx,elf} from dir; returns count */
static int read_cores(Emu *e, const char *dir) {
    static const char *suffixes[] = {"_libretro.so", "_libretro.prx", "_libretro.sprx", "_libretro.elf"};
    DirEnt *ents;
    int n = list_dir(dir, &ents);
    e->ncores = 0;
    for (int i = 0; i < n && e->ncores < MAX_CORES; i++) {
        if (ents[i].dir) continue;
        size_t ln = strlen(ents[i].name);
        for (int k = 0; k < 4; k++) {
            size_t ls = strlen(suffixes[k]);
            if (ln > ls && !strcasecmp(ents[i].name + ln - ls, suffixes[k])) {
                size_t bl = ln - ls < 63 ? ln - ls : 63;
                memcpy(e->cores[e->ncores], ents[i].name, bl);
                e->cores[e->ncores][bl] = 0;
                lower(e->cores[e->ncores]);
                scopy(e->core_files[e->ncores], 96, ents[i].name);
                e->ncores++;
                break;
            }
        }
    }
    free(ents);
    return e->ncores;
}

/* installed apps: look for cores in the usual places */
static void find_cores_auto(Emu *e) {
    char cand[16][600];
    int nc = 0;
    const char *tid = e->title_id;
    if (e->dir[0]) {
        snprintf(cand[nc++], 600, "%s/cores", e->dir);
        snprintf(cand[nc++], 600, "%s/app0/cores", e->dir);
        snprintf(cand[nc++], 600, "%s/retroarch/cores", e->dir);
    }
    snprintf(cand[nc++], 600, "/mnt/sandbox/pfsmnt/%s-app0/cores", tid);
    snprintf(cand[nc++], 600, "/system_ex/app/%s/cores", tid);
    snprintf(cand[nc++], 600, "/user/app/%s/cores", tid);
    snprintf(cand[nc++], 600, "/data/retroarch/cores");
    snprintf(cand[nc++], 600, "/data/RetroArch/cores");
    snprintf(cand[nc++], 600, "/data/retroarch/.config/retroarch/cores");
    snprintf(cand[nc++], 600, "/data/%s/cores", tid);
    for (int i = 0; i < nc; i++) {
        if (read_cores(e, cand[i]) > 0) {
            scopy(e->cores_dir, sizeof(e->cores_dir), cand[i]);
            return;
        }
    }
    /* one level below the app folder: any "cores" directory */
    if (e->dir[0]) {
        DirEnt *ents;
        int n = list_dir(e->dir, &ents);
        for (int i = 0; i < n; i++) {
            if (!ents[i].dir) continue;
            char sub[700];
            snprintf(sub, sizeof(sub), "%s/%s/cores", e->dir, ents[i].name);
            if (read_cores(e, sub) > 0) {
                scopy(e->cores_dir, sizeof(e->cores_dir), sub);
                free(ents);
                return;
            }
        }
        free(ents);
    }
    e->ncores = 0;
    scopy(e->cores_dir, sizeof(e->cores_dir), "not found");
}

/* read titleId / titleName from an app's sce_sys/param.json */
static int read_param(const char *appdir, char *tid, size_t tcap, char *name, size_t ncap) {
    char p[700];
    snprintf(p, sizeof(p), "%s/sce_sys/param.json", appdir);
    char *txt = read_file(p, NULL);
    if (!txt) return -1;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    if (!j) return -1;
    cJSON *t = cJSON_GetObjectItem(j, "titleId");
    if (cJSON_IsString(t)) scopy(tid, tcap, t->valuestring);
    name[0] = 0;
    cJSON *lp = cJSON_GetObjectItem(j, "localizedParameters");
    if (lp) {
        cJSON *def = cJSON_GetObjectItem(lp, "defaultLanguage");
        cJSON *loc = cJSON_IsString(def) ? cJSON_GetObjectItem(lp, def->valuestring) : NULL;
        if (!loc) loc = cJSON_GetObjectItem(lp, "en-US");
        cJSON *tn = loc ? cJSON_GetObjectItem(loc, "titleName") : NULL;
        if (cJSON_IsString(tn)) scopy(name, ncap, tn->valuestring);
    }
    cJSON_Delete(j);
    return 0;
}

static const char *APP_ROOTS[] = {"/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew",
                                   "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", "/user/app", "/system_ex/app"};
#define N_APP_ROOTS ((int)(sizeof(APP_ROOTS) / sizeof(*APP_ROOTS)))

static void find_installed_app(Emu *e) {
    e->found = 0;
    if (e->title_id[0]) {
        /* given by jewelcase.json: just locate its folder (optional) */
        int listed = 0;
        for (int t = 0; t < g_ix_ntitles; t++)
            if (!strcmp(g_ix_titles[t].tid, e->title_id)) {
                listed = 1;
                if (!e->dir[0]) scopy(e->dir, sizeof(e->dir), g_ix_titles[t].dir);
            }
        if (g_ix_on && !listed) return;   /* not installed: the next choice takes over */
        for (int r = 0; r < N_APP_ROOTS && !e->dir[0]; r++) {
            char d[600];
            snprintf(d, sizeof(d), "%s/%s", APP_ROOTS[r], e->title_id);
            if (is_dir(d)) scopy(e->dir, sizeof(e->dir), d);
        }
        e->found = 1;
        goto detect_mode;
    }
    if (!e->title_match[0] && !e->title_ids[0]) return;
    char want[48];
    scopy(want, sizeof(want), e->title_match);
    lower(want);
    if (g_ix_on) {
        char ids[80], names[60];
        snprintf(ids, sizeof(ids), "|%s|", e->title_ids);
        for (int t = 0; t < g_ix_ntitles && !e->found; t++) {
            if (!strcmp(g_ix_titles[t].tid, GS_TITLE_ID)) continue;
            char key[24];
            snprintf(key, sizeof(key), "|%s|", g_ix_titles[t].tid);
            int hit = e->title_ids[0] && strstr(ids, key) != NULL;
            if (!hit && e->title_match[0]) {
                char name[256];
                scopy(name, sizeof(name), g_ix_titles[t].name);
                lower(name);
                scopy(names, sizeof(names), e->title_match);
                lower(names);
                char *save = NULL;
                for (char *w = strtok_r(names, "|", &save); w && !hit; w = strtok_r(NULL, "|", &save))
                    if (*w && strstr(name, w)) hit = 1;
            }
            if (!hit) continue;
            scopy(e->title_id, sizeof(e->title_id), g_ix_titles[t].tid);
            scopy(e->dir, sizeof(e->dir), g_ix_titles[t].dir);
            e->found = 1;
        }
        goto detect_mode;
    }
    for (int r = 0; r < N_APP_ROOTS && !e->found; r++) {
        DirEnt *ents;
        int n = list_dir(APP_ROOTS[r], &ents);
        for (int i = 0; i < n && !e->found; i++) {
            if (!ents[i].dir) continue;
            char d[600], tid[16] = "", name[128];
            snprintf(d, sizeof(d), "%s/%s", APP_ROOTS[r], ents[i].name);
            if (read_param(d, tid, sizeof(tid), name, sizeof(name))) continue;
            lower(name);
            if (!strcmp(tid, GS_TITLE_ID)) continue;
            if (strstr(name, want)) {
                scopy(e->title_id, sizeof(e->title_id), tid[0] ? tid : ents[i].name);
                scopy(e->dir, sizeof(e->dir), d);
                e->found = 1;
            }
        }
        free(ents);
    }
detect_mode:
    /* PS5 RetroArch (github.com/mihawk-99/PS5_RetroArch): eboot.bin + cores/ in a
     * title folder. It takes a game through its game mode request file. */
    if (e->found && e->dir[0]) {
        char eb[700], cores[700];
        snprintf(eb, sizeof(eb), "%s/eboot.bin", e->dir);
        snprintf(cores, sizeof(cores), "%s/cores", e->dir);
        e->ps5ra = is_file(eb) && is_dir(cores);
        if (e->ps5ra) {
            /* its own content folder holds games too */
            char content[600];
            snprintf(content, sizeof(content), "%s/content", e->dir);
            if (g_nroots < 48) scopy(g_roots[g_nroots++], 256, content);
        }
    }
}

static void find_emulators(void) {
    /* gather homebrew folders */
    char (*folders)[512] = xcalloc(256, 512);
    char (*fnames)[256] = xcalloc(256, 256);
    int nf = 0;
    for (size_t r = 0; r < sizeof(HOMEBREW_ROOTS) / sizeof(*HOMEBREW_ROOTS); r++) {
        DirEnt *ents;
        int n = list_dir(HOMEBREW_ROOTS[r], &ents);
        for (int i = 0; i < n && nf < 256; i++) {
            if (!ents[i].dir) continue;
            if (!strcasecmp(ents[i].name, "roms")) continue;
            snprintf(folders[nf], 512, "%s/%s", HOMEBREW_ROOTS[r], ents[i].name);
            if (!strcmp(folders[nf], g_appdir)) continue;
            scopy(fnames[nf], 256, ents[i].name);
            nf++;
        }
        free(ents);
    }

    for (int k = 0; k < g_nemus; k++) {
        Emu *e = &g_emus[k];
        e->found = 0;
        if (e->is_title) {
            find_installed_app(e);
        } else if (e->path[0]) {
            if (is_file(e->path)) {
                scopy(e->dir, sizeof(e->dir), e->path);
                char *sl = strrchr(e->dir, '/');
                if (sl) *sl = 0;
                e->found = 1;
            }
        } else {
            for (int f = 0; f < nf && !e->found; f++) {
                if (!name_matches(e, fnames[f])) continue;
                DirEnt *ents;
                int n = list_dir(folders[f], &ents);
                int pick = -1;
                for (int x = 0; x < e->nelf && pick < 0; x++)
                    for (int i = 0; i < n; i++)
                        if (!ents[i].dir && !strcasecmp(ents[i].name, e->elf[x])) { pick = i; break; }
                for (int i = 0; i < n && pick < 0; i++)
                    if (!ents[i].dir && !strcasecmp(ext_of(ents[i].name), "elf")) pick = i;
                if (pick >= 0) {
                    snprintf(e->path, sizeof(e->path), "%s/%s", folders[f], ents[pick].name);
                    scopy(e->dir, sizeof(e->dir), folders[f]);
                    e->found = 1;
                }
                free(ents);
            }
        }
        /* RetroArch cores */
        e->ncores = 0;
        if (e->found && e->cores_dir[0]) {
            if (!strcmp(e->cores_dir, "auto")) {
                find_cores_auto(e);
            } else {
                char cd[640];
                fill_tokens(e->cores_dir, cd, sizeof(cd), "", e->dir, "", "");
                scopy(e->cores_dir, sizeof(e->cores_dir), cd);
                read_cores(e, cd);
            }
        }
        if (e->found) logf_("emulator %s: %s", e->id, e->is_title ? e->title_id : e->path);
    }
    free(folders);
    free(fnames);
}

static int try_choice(System *s, EmuChoice *c, Emu *e) {
    if (!e || !e->found) return 0;
    if (c->ncores) {
        for (int i = 0; i < c->ncores; i++)
            for (int j = 0; j < e->ncores; j++)
                if (!strcmp(c->cores[i], e->cores[j])) {
                    s->emu = (int)(e - g_emus);
                    scopy(s->core_name, sizeof(s->core_name), e->cores[j]);
                    if (e->ps5ra) {
                        /* RetroArch's game mode only accepts /app0/cores/<name>_libretro.so */
                        size_t fl = strlen(e->core_files[j]);
                        if (fl < 12 || strcasecmp(e->core_files[j] + fl - 12, "_libretro.so")) continue;
                        snprintf(s->core_path, sizeof(s->core_path), "/app0/cores/%s", e->core_files[j]);
                    } else {
                        snprintf(s->core_path, sizeof(s->core_path), "%s/%s", e->cores_dir, e->core_files[j]);
                    }
                    return 1;
                }
        return 0;
    }
    s->emu = (int)(e - g_emus);
    return 1;
}

static void pick_emulator(System *s) {
    s->emu = -1;
    s->core_path[0] = 0;
    s->core_name[0] = 0;
    for (int c = 0; c < s->nemus; c++) {
        if (try_choice(s, &s->emus[c], emu_find(s->emus[c].emu))) return;
        /* a RetroArch choice also works with the installed RetroArch app */
        if (!strcmp(s->emus[c].emu, "retroarch") && try_choice(s, &s->emus[c], emu_find("retroarch_app"))) return;
    }
    /* last resort: installed RetroArch app without a known core (opens RetroArch) */
    for (int c = 0; c < s->nemus; c++) {
        if (strcmp(s->emus[c].emu, "retroarch")) continue;
        Emu *e = emu_find("retroarch_app");
        if (e && e->found) {
            s->emu = (int)(e - g_emus);
            scopy(s->core_name, sizeof(s->core_name), "no core found");
            return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* ROM scanning                                                              */
/* ------------------------------------------------------------------------ */

static int has_ext(System *s, const char *name) {
    const char *e = ext_of(name);
    for (int i = 0; i < s->nexts; i++) if (!strcasecmp(e, s->exts[i])) return 1;
    return 0;
}

static int is_img(const char *name) {
    const char *e = ext_of(name);
    return !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg");
}

typedef struct {
    char key[256];
    char path[1024];
} ArtEnt;

static void add_art(ArtEnt **arts, int *na, int *cap, const char *dir, DirEnt *ents, int n) {
    for (int i = 0; i < n; i++) {
        if (ents[i].dir || !is_img(ents[i].name)) continue;
        if (*na == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *arts = realloc(*arts, (size_t)*cap * sizeof(ArtEnt));
            if (!*arts) exit(1);
        }
        ArtEnt *a = &(*arts)[(*na)++];
        stem_of(ents[i].name, a->key, sizeof(a->key));
        lower(a->key);
        snprintf(a->path, sizeof(a->path), "%s/%s", dir, ents[i].name);
    }
}

static const char *find_art(ArtEnt *arts, int na, const char *stem) {
    char key[256];
    scopy(key, sizeof(key), stem);
    lower(key);
    for (int i = 0; i < na; i++) if (!strcmp(arts[i].key, key)) return arts[i].path;
    return "";
}

static int game_cmp(const void *a, const void *b) {
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

static void push_game(System *s, int *cap, const char *title, const char *path, const char *dir, const char *art) {
    for (int i = 0; i < s->ngames; i++)
        if (!strcasecmp(s->games[i].title, title) && !strcasecmp(ext_of(s->games[i].path), ext_of(path))) return;
    if (s->ngames == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        s->games = realloc(s->games, (size_t)*cap * sizeof(Game));
        if (!s->games) exit(1);
    }
    Game *g = &s->games[s->ngames++];
    memset(g, 0, sizeof(*g));
    scopy(g->title, sizeof(g->title), title);
    scopy(g->path, sizeof(g->path), path);
    scopy(g->dir, sizeof(g->dir), dir);
    scopy(g->art, sizeof(g->art), art);
    g->sys = (int)(s - g_sys);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (is_dir(path)) scopy(g->stem, sizeof(g->stem), base);
    else stem_of(base, g->stem, sizeof(g->stem));
}

static void scan_system(System *s) {
    int cap = 0;
    s->games = NULL;
    s->ngames = 0;
    for (int r = 0; r < g_nroots; r++) {
        DirEnt *top;
        int nt = list_dir(g_roots[r], &top);
        if (nt <= 0) {
            free(top);
            continue;
        }
        for (int t = 0; t < nt; t++) {
            if (!top[t].dir) continue;
            int want = 0;
            for (int f = 0; f < s->nfolders; f++) if (!strcasecmp(top[t].name, s->folders[f])) want = 1;
            if (!want) continue;

            char dir[1024];
            snprintf(dir, sizeof(dir), "%s/%s", g_roots[r], top[t].name);
            DirEnt *ents;
            int n = list_dir(dir, &ents);
            if (n <= 0) {
                free(ents);
                continue;
            }

            ArtEnt *arts = NULL;
            int na = 0, acap = 0;
            add_art(&arts, &na, &acap, dir, ents, n);
            const char *artdirs[] = {"media", "boxart", "images", "covers"};
            for (int i = 0; i < n; i++) {
                if (!ents[i].dir) continue;
                for (int k = 0; k < 4; k++) {
                    if (strcasecmp(ents[i].name, artdirs[k])) continue;
                    char sub[1100];
                    snprintf(sub, sizeof(sub), "%s/%s", dir, ents[i].name);
                    DirEnt *se;
                    int sn = list_dir(sub, &se);
                    if (sn > 0) add_art(&arts, &na, &acap, sub, se, sn);
                    free(se);
                }
            }

            int has_m3u = 0;
            for (int i = 0; i < n; i++)
                if (!ents[i].dir && !strcasecmp(ext_of(ents[i].name), "m3u") && has_ext(s, ents[i].name)) has_m3u = 1;

            for (int i = 0; i < n; i++) {
                if (ents[i].dir || !has_ext(s, ents[i].name)) continue;
                char stem[256], title[160], full[1300];
                stem_of(ents[i].name, stem, sizeof(stem));
                const char *ext = ext_of(ents[i].name);
                if (!strcasecmp(ext, "cue")) {
                    char chd[300];
                    snprintf(chd, sizeof(chd), "%s.chd", stem);
                    int dup = 0;
                    for (int j = 0; j < n; j++) if (!strcasecmp(ents[j].name, chd)) dup = 1;
                    if (dup) continue;
                }
                int disc = disc_number(stem);
                if (has_m3u && strcasecmp(ext, "m3u") && disc > 0) continue;
                if (disc > 1) continue;
                pretty_name(stem, title, sizeof(title));
                snprintf(full, sizeof(full), "%s/%s", dir, ents[i].name);
                push_game(s, &cap, title, full, dir, find_art(arts, na, stem));
            }

            if (s->folder_games) {
                for (int i = 0; i < n; i++) {
                    if (!ents[i].dir) continue;
                    int skip = 0;
                    for (int k = 0; k < 4; k++) if (!strcasecmp(ents[i].name, artdirs[k])) skip = 1;
                    if (skip) continue;
                    char title[160], full[1300];
                    pretty_name(ents[i].name, title, sizeof(title));
                    snprintf(full, sizeof(full), "%s/%s", dir, ents[i].name);
                    push_game(s, &cap, title, full, dir, find_art(arts, na, ents[i].name));
                }
            }
            free(arts);
            free(ents);
        }
        free(top);
    }
    if (s->ngames > 1) qsort(s->games, (size_t)s->ngames, sizeof(Game), game_cmp);
}

/* ------------------------------------------------------------------------ */
/* Recent games                                                              */
/* ------------------------------------------------------------------------ */

static void load_recent(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s", USER_DIR "/recent.json");
    char *txt = read_file(path, NULL);
    g_nrecent = 0;
    if (!txt) return;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (g_nrecent >= MAX_RECENT) break;
        if (cJSON_IsString(it)) {   /* older format: paths only */
            scopy(g_recent[g_nrecent], 1024, it->valuestring);
            g_recent_time[g_nrecent++] = 0;
        } else if (cJSON_IsObject(it)) {
            cJSON *p = cJSON_GetObjectItem(it, "path"), *t = cJSON_GetObjectItem(it, "time");
            if (!cJSON_IsString(p)) continue;
            scopy(g_recent[g_nrecent], 1024, p->valuestring);
            g_recent_time[g_nrecent++] = cJSON_IsNumber(t) ? (long)t->valuedouble : 0;
        }
    }
    cJSON_Delete(arr);
}

static void save_recent(const char *played) {
    static char list[MAX_RECENT][1024];
    long times[MAX_RECENT];
    int n = 0;
    scopy(list[n], 1024, played);
    times[n++] = (long)time(NULL);
    for (int i = 0; i < g_nrecent && n < MAX_RECENT; i++)
        if (strcmp(g_recent[i], played)) {
            scopy(list[n], 1024, g_recent[i]);
            times[n++] = g_recent_time[i];
        }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", list[i]);
        cJSON_AddNumberToObject(o, "time", (double)times[i]);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_Print(arr);
    char path[600];
    snprintf(path, sizeof(path), "%s", USER_DIR "/recent.json");
    if (out) {
        write_file(path, out);
        free(out);
    }
    cJSON_Delete(arr);
    for (int i = 0; i < n; i++) {
        scopy(g_recent[i], 1024, list[i]);
        g_recent_time[i] = times[i];
    }
    g_nrecent = n;
}

static long recent_time_of(const char *path) {
    for (int i = 0; i < g_nrecent; i++) if (!strcmp(g_recent[i], path)) return g_recent_time[i] ? g_recent_time[i] : 1;
    return 0;
}

static void load_favs(void) {
    if (!g_favs) g_favs = calloc(MAX_FAVS, 1024);
    if (!g_favs) return;
    char path[600];
    snprintf(path, sizeof(path), "%s", USER_DIR "/favorites.json");
    char *txt = read_file(path, NULL);
    g_nfavs = 0;
    if (!txt) return;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) && g_nfavs < MAX_FAVS) scopy(g_favs[g_nfavs++], 1024, it->valuestring);
    cJSON_Delete(arr);
}

static int is_fav(const char *path) {
    for (int i = 0; i < g_nfavs; i++) if (!strcmp(g_favs[i], path)) return 1;
    return 0;
}

/* returns 1 if it is now a favourite */
static int toggle_fav(const char *path) {
    if (!g_favs) return 0;
    int at = -1;
    for (int i = 0; i < g_nfavs; i++) if (!strcmp(g_favs[i], path)) at = i;
    if (at >= 0) {
        for (int i = at; i + 1 < g_nfavs; i++) scopy(g_favs[i], 1024, g_favs[i + 1]);
        g_nfavs--;
    } else if (g_nfavs < MAX_FAVS) {
        scopy(g_favs[g_nfavs++], 1024, path);
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_nfavs; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(g_favs[i]));
    char *out = cJSON_Print(arr);
    char p[600];
    snprintf(p, sizeof(p), "%s", USER_DIR "/favorites.json");
    if (out) {
        write_file(p, out);
        free(out);
    }
    cJSON_Delete(arr);
    return at < 0;
}

/* ------------------------------------------------------------------------ */
/* Launching through websrv                                                  */
/* ------------------------------------------------------------------------ */

static void url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == '/') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

/* websrv splits args on spaces; a space inside an argument is written "\ " */
static void append_escaped(char *dst, size_t cap, const char *s) {
    size_t o = strlen(dst);
    for (; *s && o + 3 < cap; s++) {
        if (*s == ' ') dst[o++] = '\\';
        dst[o++] = *s;
    }
    dst[o] = 0;
}

typedef struct {
    char target[600];       /* ELF path or title id */
    int is_title;
    char args[4096];        /* websrv format (escaped spaces) */
    char env[2048];
    char cwd[512];
    char pretty[4096];      /* for the info panel */
    char argv_list[MAX_LIST][1400]; int nargv;   /* unescaped, for the native helper */
    char request_path[700]; /* PS5 RetroArch: game-request.txt to write first */
    char request[3000];
} LaunchCmd;

static int build_launch(Game *g, LaunchCmd *lc) {
    System *s = &g_sys[g->sys];
    memset(lc, 0, sizeof(*lc));
    if (s->emu < 0) return -1;
    Emu *e = &g_emus[s->emu];
    lc->is_title = e->is_title;
    if (e->ps5ra && !s->core_path[0]) {
        /* no core of RetroArch's matches this system: just open RetroArch */
        scopy(lc->target, sizeof(lc->target), e->title_id);
        snprintf(lc->pretty, sizeof(lc->pretty), "%s (opens RetroArch: none of its cores matches this system)", e->title_id);
        scopy(lc->cwd, sizeof(lc->cwd), e->dir);
        return 0;
    }
    if (e->ps5ra) {
        /* RetroArch's /app0 is its own folder: map paths inside it */
        char content[1100];
        size_t dl = strlen(e->dir);
        if (!strncmp(g->path, e->dir, dl) && g->path[dl] == '/') snprintf(content, sizeof(content), "/app0%s", g->path + dl);
        else scopy(content, sizeof(content), g->path);
        scopy(lc->target, sizeof(lc->target), e->title_id);
        snprintf(lc->request_path, sizeof(lc->request_path), "%s/game-request.txt", e->dir);
        snprintf(lc->request, sizeof(lc->request),
                 "ps5-game 1\nkind=request\ncore=%s\ncontent=%s\nfrontend=/app0/eboot.bin\nstate=jewelcase\n",
                 s->core_path, content);
        scopy(lc->argv_list[0], sizeof(lc->argv_list[0]), "--ps5-mode=game");
        lc->nargv = 1;
        scopy(lc->args, sizeof(lc->args), "--ps5-mode=game");
        snprintf(lc->pretty, sizeof(lc->pretty), "%s game mode\n  core: %s\n  content: %s",
                 e->title_id, s->core_path[0] ? s->core_path : "(none found)", content);
        scopy(lc->cwd, sizeof(lc->cwd), e->dir);
        return 0;
    }
    scopy(lc->target, sizeof(lc->target), e->is_title ? e->title_id : e->path);
    scopy(lc->cwd, sizeof(lc->cwd), e->dir);
    snprintf(lc->pretty, sizeof(lc->pretty), "%s", lc->target);

    for (int i = 0; i < e->nargs; i++) {
        char a[1400];
        fill_tokens(e->args[i], a, sizeof(a), g->path, e->dir, s->core_path, g->dir);
        /* drop "-L" with an empty core */
        if (!strcmp(a, "-L") && !s->core_path[0]) {
            i++;
            continue;
        }
        if (!a[0]) continue;
        if (lc->args[0]) strncat(lc->args, " ", sizeof(lc->args) - strlen(lc->args) - 1);
        append_escaped(lc->args, sizeof(lc->args), a);
        if (lc->nargv < MAX_LIST) scopy(lc->argv_list[lc->nargv++], sizeof(lc->argv_list[0]), a);
        strncat(lc->pretty, " ", sizeof(lc->pretty) - strlen(lc->pretty) - 1);
        if (strchr(a, ' ')) {
            strncat(lc->pretty, "\"", sizeof(lc->pretty) - strlen(lc->pretty) - 1);
            strncat(lc->pretty, a, sizeof(lc->pretty) - strlen(lc->pretty) - 1);
            strncat(lc->pretty, "\"", sizeof(lc->pretty) - strlen(lc->pretty) - 1);
        } else {
            strncat(lc->pretty, a, sizeof(lc->pretty) - strlen(lc->pretty) - 1);
        }
    }
    for (int i = 0; i < e->nenv; i++) {
        char v[700], kv[800];
        fill_tokens(e->envv[i], v, sizeof(v), g->path, e->dir, s->core_path, g->dir);
        snprintf(kv, sizeof(kv), "%s=%s", e->envk[i], v);
        if (lc->env[0]) strncat(lc->env, " ", sizeof(lc->env) - strlen(lc->env) - 1);
        append_escaped(lc->env, sizeof(lc->env), kv);
    }
    return 0;
}

/* Sends the request. Returns HTTP status, or -1 when websrv isn't reachable.
 * For /hbldr websrv closes Jewel Case before starting the game, so on the PS5
 * this usually never returns. */
/* Native: Jewel Case can't start another app while it runs, so it hands the
 * request to jc-launch.elf through the ELF loader on port 9021.
 * Returns 200 when handed over, -2 when the ELF loader isn't there. */
static int send_to_elfldr(const char *elf_path) {
    size_t len;
    char *elf = read_file(elf_path, &len);
    if (!elf) return -3;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free(elf);
        return -2;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9021);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        free(elf);
        return -2;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, elf + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    free(elf);
    return off == len ? 200 : -2;
}

static int launch_title_native(LaunchCmd *lc) {
    if (lc->request_path[0] && write_file(lc->request_path, lc->request) != 0) return -4;
    static char req[24000];
    snprintf(req, sizeof(req), "jewelcase-launch 1\ntitle=%s\n", lc->target);
    for (int i = 0; i < lc->nargv; i++) {
        strncat(req, "arg=", sizeof(req) - strlen(req) - 1);
        strncat(req, lc->argv_list[i], sizeof(req) - strlen(req) - 1);
        strncat(req, "\n", sizeof(req) - strlen(req) - 1);
    }
    if (write_file("/data/jewelcase-launch.txt", req) != 0) return -5;
    char helper[600];
    snprintf(helper, sizeof(helper), "%s/jc-launch.elf", g_appdir);
#ifdef GS_HOST
    if (getenv("GS_FAKE_LAUNCH")) return 200;
#endif
    return send_to_elfldr(helper);
}

static int send_launch(LaunchCmd *lc) {
    static char req[16384], q[12000], enc[8192];
    q[0] = 0;
#if defined(GS_NATIVE) || defined(GS_HOST)
    if (lc->is_title) {
        logf_("launch title %s via helper", lc->target);
        return launch_title_native(lc);
    }
#endif
    if (lc->is_title) {
        url_encode(lc->target, enc, sizeof(enc));
        snprintf(q, sizeof(q), "/launch?titleId=%s", enc);
        url_encode(lc->args, enc, sizeof(enc));
        strncat(q, "&args=", sizeof(q) - strlen(q) - 1);
        strncat(q, enc, sizeof(q) - strlen(q) - 1);
    } else {
        url_encode(lc->target, enc, sizeof(enc));
        snprintf(q, sizeof(q), "/hbldr?pipe=0&daemon=0&path=%s", enc);
        if (lc->args[0]) {
            url_encode(lc->args, enc, sizeof(enc));
            strncat(q, "&args=", sizeof(q) - strlen(q) - 1);
            strncat(q, enc, sizeof(q) - strlen(q) - 1);
        }
        if (lc->env[0]) {
            url_encode(lc->env, enc, sizeof(enc));
            strncat(q, "&env=", sizeof(q) - strlen(q) - 1);
            strncat(q, enc, sizeof(q) - strlen(q) - 1);
        }
        if (lc->cwd[0]) {
            url_encode(lc->cwd, enc, sizeof(enc));
            strncat(q, "&cwd=", sizeof(q) - strlen(q) - 1);
            strncat(q, enc, sizeof(q) - strlen(q) - 1);
        }
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", q);
    logf_("launch: %s", q);

#ifdef GS_HOST
    if (getenv("GS_FAKE_LAUNCH")) return 200;
#endif

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WEBSRV_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    size_t len = strlen(req), off = 0;
    while (off < len) {
        ssize_t w = write(fd, req + off, len - off);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    char resp[256] = {0};
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (r <= 0) return 0;
    int status = 0;
    sscanf(resp, "HTTP/%*s %d", &status);
    return status;
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static SDL_Renderer *R;

typedef struct {
    float size;
    stbtt_packedchar chars[224];   /* 32..255 */
    SDL_Texture *atlas;
    int ascent, line;
} Font;

static Font F_small, F_body, F_title, F_huge, F_tile;
static SDL_Texture *T_corner;      /* white quarter-circle mask, CORNER_R px */
static SDL_Texture *T_bg;
static SDL_Texture *T_logo;
#define CORNER_R 64

/* Out of memory while building an image: say so once (native: notification). */
static void gs_oom(const char *what) {
    static int told;
    char msg[200];
    snprintf(msg, sizeof(msg), "Jewel Case: not enough memory for the %s", what);
    logf_("%s", msg);
#ifdef GS_NATIVE
    if (!told) gs_notify(msg);
#endif
    told = 1;
}

static int font_init(Font *f, const unsigned char *ttf, float size) {
    const int W = 1024, H = 1024;
    unsigned char *bmp = xcalloc((size_t)W * H, 1);
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, bmp, W, H, 0, 1, NULL)) return -1;
    stbtt_PackSetOversampling(&pc, 2, 2);
    stbtt_PackFontRange(&pc, ttf, 0, size, 32, 224, f->chars);
    stbtt_PackEnd(&pc);

    stbtt_fontinfo info;
    stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    float sc = stbtt_ScaleForPixelHeight(&info, size);
    f->ascent = (int)(asc * sc + 0.5f);
    f->line = (int)((asc - desc + gap) * sc + 0.5f);
    f->size = size;

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        free(bmp);
        gs_oom("font atlas");
        return -1;
    }
    for (int y = 0; y < H; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < W; x++) row[x] = ((Uint32)bmp[y * W + x] << 24) | 0xffffff;
    }
    f->atlas = SDL_CreateTextureFromSurface(R, s);
    SDL_SetTextureBlendMode(f->atlas, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    free(bmp);
    return 0;
}

/* decode one UTF-8 char; returns codepoint and advances *p */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = s[0];
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xe0) == 0xc0 && s[1]) { *p += 2; return ((c & 0x1f) << 6) | (s[1] & 0x3f); }
    if ((c & 0xf0) == 0xe0 && s[1] && s[2]) { *p += 3; return ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); }
    if ((c & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) { *p += 4; return '?'; }
    *p += 1;
    return '?';
}

static int glyph_index(int cp) {
    if (cp == 0x2019 || cp == 0x2018) cp = '\'';
    if (cp == 0x2013 || cp == 0x2014) cp = '-';
    if (cp < 32 || cp > 255) cp = '?';
    return cp - 32;
}

static float text_width(Font *f, const char *s) {
    float w = 0;
    while (*s) {
        int gi = glyph_index(utf8_next(&s));
        w += f->chars[gi].xadvance;
    }
    return w;
}

static void set_color(SDL_Texture *t, Uint32 rgb, Uint8 a) {
    SDL_SetTextureColorMod(t, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    SDL_SetTextureAlphaMod(t, a);
}

static void draw_text(Font *f, float x, float y, const char *s, Uint32 rgb, Uint8 a) {
    if (!f->atlas) return;
    set_color(f->atlas, rgb, a);
    float by = y + f->ascent;
    while (*s) {
        int gi = glyph_index(utf8_next(&s));
        stbtt_aligned_quad q;
        float xx = x, yy = by;
        stbtt_GetPackedQuad(f->chars, 1024, 1024, gi, &xx, &yy, &q, 0);
        SDL_Rect src = {(int)(q.s0 * 1024 + 0.5f), (int)(q.t0 * 1024 + 0.5f),
                        (int)((q.s1 - q.s0) * 1024 + 0.5f), (int)((q.t1 - q.t0) * 1024 + 0.5f)};
        SDL_Rect dst = {(int)(q.x0 + 0.5f), (int)(q.y0 + 0.5f), (int)(q.x1 - q.x0 + 0.5f), (int)(q.y1 - q.y0 + 0.5f)};
        if (src.w > 0 && src.h > 0) SDL_RenderCopy(R, f->atlas, &src, &dst);
        x = xx;
    }
}

/* copy s into out, cutting with "..." so it fits max_w */
static void ellipsize(Font *f, const char *s, float max_w, char *out, size_t cap) {
    if (text_width(f, s) <= max_w) {
        scopy(out, cap, s);
        return;
    }
    float dots = text_width(f, "...");
    const char *p = s;
    float w = 0;
    size_t o = 0;
    while (*p) {
        const char *start = p;
        int gi = glyph_index(utf8_next(&p));
        float cw = f->chars[gi].xadvance;
        if (w + cw + dots > max_w) break;
        size_t n = (size_t)(p - start);
        if (o + n + 4 >= cap) break;
        memcpy(out + o, start, n);
        o += n;
        w += cw;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    memcpy(out + o, "...", 4);
}

/* wrap into up to 2 lines */
static int wrap2(Font *f, const char *s, float max_w, char l1[200], char l2[200]) {
    l1[0] = l2[0] = 0;
    if (text_width(f, s) <= max_w) {
        scopy(l1, 200, s);
        return 1;
    }
    /* find the last space that fits */
    char buf[200];
    scopy(buf, sizeof(buf), s);
    int best = -1;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] != ' ') continue;
        buf[i] = 0;
        if (text_width(f, buf) <= max_w) best = i;
        buf[i] = ' ';
    }
    if (best < 0) {
        ellipsize(f, s, max_w, l1, 200);
        return 1;
    }
    memcpy(l1, s, (size_t)best);
    l1[best] = 0;
    ellipsize(f, s + best + 1, max_w, l2, 200);
    return 2;
}

/* a paragraph into up to maxl lines of max_w; the last one ends in an ellipsis if
 * there is more. Returns the number of lines. */
static int wrap_lines(Font *f, const char *s, float max_w, char lines[][200], int maxl) {
    int n = 0;
    const char *p = s;
    while (*p && n < maxl) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *fit = p, *q = p;
        for (;;) {
            const char *e = q;
            while (*e && *e != ' ') e++;
            int wl = (int)(e - p);
            if (wl >= 199) break;
            char tmp[200];
            memcpy(tmp, p, (size_t)wl);
            tmp[wl] = 0;
            if (text_width(f, tmp) > max_w) break;
            fit = e;
            if (!*e) break;
            q = e + 1;
        }
        if (fit == p) {   /* one word longer than the line */
            ellipsize(f, p, max_w, lines[n++], 200);
            return n;
        }
        int l = (int)(fit - p);
        memcpy(lines[n], p, (size_t)l);
        lines[n][l] = 0;
        n++;
        p = fit;
    }
    while (*p == ' ') p++;
    if (*p && n == maxl) {
        char more[400];
        snprintf(more, sizeof(more), "%s %s", lines[n - 1], p);
        ellipsize(f, more, max_w, lines[n - 1], 200);
    }
    return n;
}

static void fill_rect(int x, int y, int w, int h, Uint32 rgb, Uint8 a) {
    SDL_SetRenderDrawBlendMode(R, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(R, &r);
}

static void round_rect(int x, int y, int w, int h, int r, Uint32 rgb, Uint8 a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0 || !T_corner) {
        fill_rect(x, y, w, h, rgb, a);
        return;
    }
    fill_rect(x + r, y, w - 2 * r, h, rgb, a);
    fill_rect(x, y + r, r, h - 2 * r, rgb, a);
    fill_rect(x + w - r, y + r, r, h - 2 * r, rgb, a);
    set_color(T_corner, rgb, a);
    SDL_Rect src = {0, 0, CORNER_R, CORNER_R};
    SDL_Rect d;
    d = (SDL_Rect){x, y, r, r};
    SDL_RenderCopyEx(R, T_corner, &src, &d, 0, NULL, SDL_FLIP_NONE);
    d = (SDL_Rect){x + w - r, y, r, r};
    SDL_RenderCopyEx(R, T_corner, &src, &d, 0, NULL, SDL_FLIP_HORIZONTAL);
    d = (SDL_Rect){x, y + h - r, r, r};
    SDL_RenderCopyEx(R, T_corner, &src, &d, 0, NULL, SDL_FLIP_VERTICAL);
    d = (SDL_Rect){x + w - r, y + h - r, r, r};
    SDL_RenderCopyEx(R, T_corner, &src, &d, 0, NULL, (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
}

static void make_corner(void) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, CORNER_R, CORNER_R, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        gs_oom("corner image");
        return;
    }
    for (int y = 0; y < CORNER_R; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < CORNER_R; x++) {
            /* coverage of a circle centred at (R,R), 4x4 supersampled */
            int cov = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4.0f - CORNER_R;
                    float py = y + (sy + 0.5f) / 4.0f - CORNER_R;
                    if (px * px + py * py <= (float)CORNER_R * CORNER_R) cov++;
                }
            row[x] = ((Uint32)(cov * 255 / 16) << 24) | 0xffffff;
        }
    }
    T_corner = SDL_CreateTextureFromSurface(R, s);
    SDL_SetTextureBlendMode(T_corner, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
}

static void make_background(void) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        gs_oom("background");
        return;
    }
    for (int y = 0; y < SCREEN_H; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < SCREEN_W; x++) {
            float fy = (float)y / SCREEN_H, fx = (float)x / SCREEN_W;
            /* deep navy with a soft glow top-right */
            float dx = fx - 0.85f, dy = fy - 0.05f;
            float glow = 1.0f - SDL_sqrtf(dx * dx * 1.6f + dy * dy * 3.0f);
            if (glow < 0) glow = 0;
            glow = glow * glow * 0.55f;
            float r = 15 + 8 * fy + glow * 60;
            float g = 17 + 6 * fy + glow * 40;
            float b = 27 + 14 * fy + glow * 110;
            row[x] = 0xff000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
        }
    }
    T_bg = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
}

/* load an image file scaled down to fit max_w x max_h */
static SDL_Texture *load_art(const char *path, int max_w, int max_h, int *ow, int *oh) {
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return NULL;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory((unsigned char *)data, (int)len, &w, &h, &n, 4);
    free(data);
    if (!px) return NULL;
    float sc = SDL_min((float)max_w / w, (float)max_h / h);
    if (sc > 1) sc = 1;
    int tw = SDL_max(1, (int)(w * sc)), th = SDL_max(1, (int)(h * sc));
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst) {
        stbi_image_free(px);
        return NULL;
    }
    /* box-filter downscale */
    for (int y = 0; y < th; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch);
        int y0 = y * h / th, y1 = SDL_max(y0 + 1, (y + 1) * h / th);
        for (int x = 0; x < tw; x++) {
            int x0 = x * w / tw, x1 = SDL_max(x0 + 1, (x + 1) * w / tw);
            unsigned long acc[4] = {0, 0, 0, 0}, cnt = 0;
            int stepy = SDL_max(1, (y1 - y0) / 4), stepx = SDL_max(1, (x1 - x0) / 4);
            for (int yy = y0; yy < y1; yy += stepy)
                for (int xx = x0; xx < x1; xx += stepx) {
                    unsigned char *p = px + ((size_t)yy * w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    cnt++;
                }
            row[x] = ((Uint32)(acc[3] / cnt) << 24) | ((Uint32)(acc[0] / cnt) << 16) |
                     ((Uint32)(acc[1] / cnt) << 8) | (Uint32)(acc[2] / cnt);
        }
    }
    stbi_image_free(px);
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, dst);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(dst);
    *ow = tw;
    *oh = th;
    return t;
}

/* PlayStation face-button glyphs */
static void draw_button(int cx, int cy, char kind) {
    round_rect(cx - 17, cy - 17, 34, 34, 17, 0x2c3145, 255);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
    switch (kind) {
    case 'x':
        SDL_SetRenderDrawColor(R, 0x7f, 0xa8, 0xff, 255);
        for (int t = -1; t <= 1; t++) {
            SDL_RenderDrawLine(R, cx - 7 + t, cy - 7, cx + 7 + t, cy + 7);
            SDL_RenderDrawLine(R, cx + 7 + t, cy - 7, cx - 7 + t, cy + 7);
        }
        break;
    case 'o':
        round_rect(cx - 9, cy - 9, 18, 18, 9, 0xff6b7a, 255);
        round_rect(cx - 6, cy - 6, 12, 12, 6, 0x2c3145, 255);
        break;
    case 't':
        SDL_SetRenderDrawColor(R, 0x4f, 0xd8, 0xb4, 255);
        for (int t = 0; t < 2; t++) {
            SDL_RenderDrawLine(R, cx, cy - 8 + t, cx - 9 + t, cy + 7 - t);
            SDL_RenderDrawLine(R, cx, cy - 8 + t, cx + 9 - t, cy + 7 - t);
            SDL_RenderDrawLine(R, cx - 9 + t, cy + 7 - t, cx + 9 - t, cy + 7 - t);
        }
        break;
    case 's':
        SDL_SetRenderDrawColor(R, 0xe8, 0x8c, 0xd8, 255);
        for (int t = 0; t < 3; t++) {
            SDL_Rect r = {cx - 8 + t, cy - 8 + t, 16 - 2 * t, 16 - 2 * t};
            SDL_RenderDrawRect(R, &r);
        }
        break;
    }
}

static void draw_pill_label(int x, int cy, const char *label) {
    float w = text_width(&F_small, label) + 28;
    round_rect(x, cy - 17, (int)w, 34, 17, 0x2c3145, 255);
    draw_text(&F_small, x + 14, cy - F_small.line / 2.0f, label, 0xcfd5e6, 255);
}

/* ------------------------------------------------------------------------ */
/* Look of each system: media template, tab icon and label, cover collection */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *id;
    const char *tpl;      /* MEDIA_TPL name */
    int icon;             /* ICON_* */
    const char *tab;      /* tab label */
    const char *repo;     /* libretro-thumbnails repository, NULL = none */
} SysLook;

static const SysLook LOOKS[] = {
    {"nes", "tall", ICON_CART_TALL, "NES", "Nintendo_-_Nintendo_Entertainment_System"},
    {"snes", "wide", ICON_CART_WIDE, "SNES", "Nintendo_-_Super_Nintendo_Entertainment_System"},
    {"n64", "chunky", ICON_CART_WIDE, "N64", "Nintendo_-_Nintendo_64"},
    {"gb", "small", ICON_HANDHELD, "GB", "Nintendo_-_Game_Boy"},
    {"gbc", "small_dark", ICON_HANDHELD, "GBC", "Nintendo_-_Game_Boy_Color"},
    {"gba", "slim", ICON_HANDHELD_WIDE, "GBA", "Nintendo_-_Game_Boy_Advance"},
    {"nds", "card", ICON_DUAL, "DS", "Nintendo_-_Nintendo_DS"},
    {"3ds", "card_white", ICON_DUAL, "3DS", "Nintendo_-_Nintendo_3DS"},
    {"genesis", "black", ICON_CART_TALL, "Genesis", "Sega_-_Mega_Drive_-_Genesis"},
    {"sms", "black", ICON_CART_TALL, "SMS", "Sega_-_Master_System_-_Mark_III"},
    {"gg", "small_dark", ICON_HANDHELD_WIDE, "GG", "Sega_-_Game_Gear"},
    {"pce", "black", ICON_CARD, "PCE", "NEC_-_PC_Engine_-_TurboGrafx_16"},
    {"arcade", "case", ICON_STICK, "Arcade", "FBNeo_-_Arcade_Games"},
    {"psx", "disc_black", ICON_DISC, "PS1", "Sony_-_PlayStation"},
    {"psp", "case", ICON_HANDHELD_WIDE, "PSP", "Sony_-_PlayStation_Portable"},
    {"ps2", "disc_blue", ICON_DISC, "PS2", "Sony_-_PlayStation_2"},
    {"ps3", "disc_silver", ICON_DISC, "PS3", "Sony_-_PlayStation_3"},
    {"gc", "disc_mini", ICON_DISC, "GC", "Nintendo_-_GameCube"},
    {"wii", "disc_white", ICON_DISC, "Wii", "Nintendo_-_Wii"},
    {"dc", "disc_silver", ICON_DISC, "DC", "Sega_-_Dreamcast"},
    {"saturn", "disc_silver", ICON_DISC, "Saturn", "Sega_-_Saturn"},
    {"segacd", "disc_silver", ICON_DISC, "Sega CD", "Sega_-_Mega-CD_-_Sega_CD"},
    {"32x", "black", ICON_CART_TALL, "32X", "Sega_-_32X"},
    {"wiiu", "disc_white", ICON_TABLET, "Wii U", "Nintendo_-_Wii_U"},
    {"pcfx", "disc_silver", ICON_DISC, "PC-FX", "NEC_-_PC-FX"},
    {"vb", "small_dark", ICON_HANDHELD_WIDE, "VB", "Nintendo_-_Virtual_Boy"},
    {"ngp", "small_dark", ICON_HANDHELD, "NGP", "SNK_-_Neo_Geo_Pocket_Color"},
    {"ws", "small_dark", ICON_HANDHELD, "WS", "Bandai_-_WonderSwan_Color"},
    {"lynx", "small_dark", ICON_HANDHELD_WIDE, "Lynx", "Atari_-_Lynx"},
    {"atari2600", "black", ICON_CART_TALL, "2600", "Atari_-_2600"},
    {"atari5200", "black", ICON_CART_TALL, "5200", "Atari_-_5200"},
    {"atari7800", "black", ICON_CART_TALL, "7800", "Atari_-_7800"},
    {"jaguar", "black", ICON_CART_TALL, "Jaguar", "Atari_-_Jaguar"},
    {"3do", "disc_silver", ICON_DISC, "3DO", "The_3DO_Company_-_3DO"},
    {"neocd", "disc_silver", ICON_DISC, "Neo CD", "SNK_-_Neo_Geo_CD"},
    {"amiga", "case", ICON_COMPUTER, "Amiga", "Commodore_-_Amiga"},
    {"c64", "case", ICON_COMPUTER, "C64", "Commodore_-_64"},
    {"dos", "case", ICON_COMPUTER, "DOS", "DOS"},
    {"pokemini", "small_dark", ICON_HANDHELD, "PM", "Nintendo_-_Pokemon_Mini"},
    {"scummvm", "case", ICON_COMPUTER, "ScummVM", "ScummVM"},
    {"switch", "minicard", ICON_HYBRID, "Switch", "Nintendo_-_Switch"},
    {"xbox", "disc_green", ICON_DISC, "Xbox", "Microsoft_-_Xbox"},
    {"xbox360", "disc_silver", ICON_DISC, "360", "Microsoft_-_Xbox_360"},
};

static const SysLook *look_of(const System *s) {
    for (size_t i = 0; i < sizeof(LOOKS) / sizeof(*LOOKS); i++)
        if (!strcmp(LOOKS[i].id, s->id)) return &LOOKS[i];
    return NULL;
}

static int tpl_of(const System *s) {
    const SysLook *l = look_of(s);
    const char *name = l ? l->tpl : "case";
    for (int i = 0; i < MEDIA_TPL_COUNT; i++) if (!strcmp(MEDIA_TPL[i].name, name)) return i;
    return 0;
}

static int icon_of(const System *s) {
    const SysLook *l = look_of(s);
    return l ? l->icon : ICON_PAD;
}

static const char *tab_label(const System *s) {
    const SysLook *l = look_of(s);
    return l ? l->tab : s->shortname;
}

/* Downloaded box art carries the system's branding: a banner down the side or
 * across the top (GAME BOY, PlayStation, NINTENDO GAMECUBE...), ratings and
 * seals along the bottom. Labels show the art without it: the part of the box
 * to keep, as fractions of its width and height. Your own art is used whole. */
typedef struct {
    const char *id;
    float x0, y0, x1, y1;
} BoxCrop;

static const BoxCrop BOX_CROPS[] = {
    {"nes", 0, 0, 1, 0.90f},          {"snes", 0, 0, 0.88f, 0.80f},     {"n64", 0, 0, 0.80f, 0.98f},
    {"gb", 0.20f, 0, 1, 0.94f},       {"gbc", 0.19f, 0, 1, 0.90f},      {"gba", 0.15f, 0, 1, 0.87f},
    {"nds", 0.12f, 0, 1, 0.88f},      {"3ds", 0, 0, 0.90f, 0.90f},      {"genesis", 0, 0.05f, 1, 0.92f},
    {"psx", 0.145f, 0, 1, 0.95f},     {"ps2", 0, 0.085f, 1, 0.90f},     {"ps3", 0, 0.08f, 1, 0.90f},
    {"psp", 0, 0.06f, 1, 0.88f},      {"gc", 0, 0.085f, 1, 0.90f},      {"wii", 0, 0.09f, 1, 0.90f},
    {"wiiu", 0, 0.09f, 1, 0.90f},     {"dc", 0, 0, 1, 0.93f},           {"saturn", 0.15f, 0, 1, 1},
    {"switch", 0, 0.075f, 1, 0.90f},  {"xbox", 0, 0.07f, 1, 0.92f},     {"xbox360", 0, 0.08f, 1, 0.92f},
};

static const BoxCrop *box_crop_of(const System *s) {
    for (size_t i = 0; i < sizeof(BOX_CROPS) / sizeof(*BOX_CROPS); i++)
        if (!strcmp(BOX_CROPS[i].id, s->id)) return &BOX_CROPS[i];
    return NULL;
}

/* systems GameTDB has disc scans for, found by the game ID in the disc image */
static int has_gametdb(const System *s) {
    return !strcmp(s->id, "gc") || !strcmp(s->id, "wii");
}

/* each system's platform name in the LaunchBox Games Database (game info, and disc scans) */
static const char *launchbox_platform(const System *s) {
    static const char *const map[][2] = {
        {"nes", "Nintendo Entertainment System"}, {"snes", "Super Nintendo Entertainment System"},
        {"n64", "Nintendo 64"},            {"gb", "Nintendo Game Boy"},       {"gbc", "Nintendo Game Boy Color"},
        {"gba", "Nintendo Game Boy Advance"}, {"nds", "Nintendo DS"},         {"3ds", "Nintendo 3DS"},
        {"genesis", "Sega Genesis"},       {"sms", "Sega Master System"},     {"gg", "Sega Game Gear"},
        {"pce", "NEC TurboGrafx-16"},      {"arcade", "Arcade"},              {"psp", "Sony PSP"},
        {"32x", "Sega 32X"},               {"switch", "Nintendo Switch"},     {"vb", "Nintendo Virtual Boy"},
        {"ngp", "SNK Neo Geo Pocket Color"}, {"ws", "WonderSwan Color"},      {"lynx", "Atari Lynx"},
        {"atari2600", "Atari 2600"},       {"atari5200", "Atari 5200"},       {"atari7800", "Atari 7800"},
        {"jaguar", "Atari Jaguar"},        {"amiga", "Commodore Amiga"},      {"c64", "Commodore 64"},
        {"dos", "MS-DOS"},                 {"pokemini", "Nintendo Pokemon Mini"},
        {"psx", "Sony Playstation"},       {"ps2", "Sony Playstation 2"},     {"ps3", "Sony Playstation 3"},
        {"gc", "Nintendo GameCube"},       {"wii", "Nintendo Wii"},           {"wiiu", "Nintendo Wii U"},
        {"dc", "Sega Dreamcast"},          {"saturn", "Sega Saturn"},         {"segacd", "Sega CD"},
        {"xbox", "Microsoft Xbox"},        {"xbox360", "Microsoft Xbox 360"}, {"pcfx", "NEC PC-FX"},
        {"3do", "3DO Interactive Multiplayer"}, {"neocd", "SNK Neo Geo CD"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++)
        if (!strcmp(map[i][0], s->id)) return map[i][1];
    return NULL;
}

/* a disc scan can be downloaded for this system */
static int has_disc_scans(const System *s) {
    return has_gametdb(s) || (launchbox_platform(s) && MEDIA_TPL[tpl_of(s)].kind == 1);
}

/* The black band some systems print across the bottom of their discs, with
 * the system's name on it (plain lettering, not the console makers' logos).
 * top: where the band starts, as a fraction of the disc's height. */
typedef struct {
    const char *id;
    float top;
    const char *line1, *line2;   /* line2: a second, bold line under it */
} DiscBand;

static const DiscBand DISC_BANDS[] = {
    {"psx", 0.80f, "PlayStation", NULL},
    {"ps2", 0.80f, "PlayStation 2", NULL},
    {"gc", 0.63f, "N I N T E N D O", "GAMECUBE"},
};

static const DiscBand *disc_band_of(const System *s) {
    for (size_t i = 0; i < sizeof(DISC_BANDS) / sizeof(*DISC_BANDS); i++)
        if (!strcmp(DISC_BANDS[i].id, s->id)) return &DISC_BANDS[i];
    return NULL;
}

/* a system colour bright enough to fill a tab with white text on it */
static Uint32 accent_of(Uint32 c) {
    float r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    float mx = SDL_max(r, SDL_max(g, b)), mn = SDL_min(r, SDL_min(g, b));
    if (mx - mn < 40) return 0x6d63e8;           /* greys: the house purple */
    float k = mx < 170 ? 170 / mx : (mx > 225 ? 225 / mx : 1);
    r *= k; g *= k; b *= k;
    return ((Uint32)SDL_min(r, 255) << 16) | ((Uint32)SDL_min(g, 255) << 8) | (Uint32)SDL_min(b, 255);
}

/* ------------------------------------------------------------------------ */
/* Images                                                                    */
/* ------------------------------------------------------------------------ */

static SDL_Texture *T_icons, *T_glow, *T_shadow;
static Uint8 *g_bokeh;          /* 960x540 light map */
static int g_bokeh_w, g_bokeh_h;
static Uint32 g_bg_accent = 0xffffffff;
static SDL_Texture *T_tpl_base[MEDIA_TPL_COUNT], *T_tpl_over[MEDIA_TPL_COUNT];
static Font F_tiny, F_head, F_chip;
static Font F_disc_l, F_disc_m, F_disc_s, F_disc_b;   /* system names on disc bands */

static SDL_Texture *texture_from_png(const unsigned char *png, unsigned len, int *ow, int *oh) {
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(png, (int)len, &w, &h, &n, 4);
    if (!px) return NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(R, s) : NULL;
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    if (s) SDL_FreeSurface(s);
    stbi_image_free(px);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return t;
}

static void ensure_tpl(int i) {
    if (!T_tpl_base[i]) T_tpl_base[i] = texture_from_png(MEDIA_TPL[i].base, MEDIA_TPL[i].base_len, NULL, NULL);
    if (!T_tpl_over[i]) T_tpl_over[i] = texture_from_png(MEDIA_TPL[i].over, MEDIA_TPL[i].over_len, NULL, NULL);
}

static void load_ui_images(void) {
    T_icons = texture_from_png(icon_sheet_png, sizeof(icon_sheet_png), NULL, NULL);
    T_glow = texture_from_png(glow_png, sizeof(glow_png), NULL, NULL);
    if (T_glow) SDL_SetTextureBlendMode(T_glow, SDL_BLENDMODE_ADD);
    T_shadow = texture_from_png(shadow_png, sizeof(shadow_png), NULL, NULL);
    int n;
    g_bokeh = stbi_load_from_memory(bokeh_png, (int)sizeof(bokeh_png), &g_bokeh_w, &g_bokeh_h, &n, 1);
}

static void draw_icon(int icon, int x, int y, int size, Uint32 rgb, Uint8 a) {
    if (!T_icons) return;
    set_color(T_icons, rgb, a);
    SDL_Rect src = {icon * 64, 0, 64, 64}, dst = {x, y, size, size};
    SDL_RenderCopy(R, T_icons, &src, &dst);
}

/* background: deep navy, the bokeh light map tinted with the system colour,
 * a soft glow behind the carousel and a vignette. Built when the system changes. */
/* the stage floor: a wide flat ellipse the games stand on (its far edge just
 * above their feet); rain lands on it, reflections lie on it */
#define FLOOR_CX 960.0f
#define FLOOR_RX 2600.0f        /* wide, so its far edge curves gently */
#define FLOOR_TOP 626.0f        /* FLOOR_Y - 26 */
#define FLOOR_RY 420.0f         /* the near edge is well below the screen */
#define FLOOR_CY (FLOOR_TOP + FLOOR_RY)

/* the floor's far (top) and near (bottom) edge at x; 0 when x is off the floor */
static int floor_span(float x, float *top, float *bot) {
    float u = (x - FLOOR_CX) / FLOOR_RX;
    if (u <= -1 || u >= 1) return 0;
    float k = SDL_sqrtf(1 - u * u) * FLOOR_RY;
    *top = FLOOR_CY - k;
    *bot = FLOOR_CY + k;
    return 1;
}

static void make_background_for(Uint32 accent) {
    if (accent == g_bg_accent && T_bg) return;
    g_bg_accent = accent;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        gs_oom("background");
        return;
    }
    float ar = ((accent >> 16) & 255) / 255.0f, ag = ((accent >> 8) & 255) / 255.0f, ab = (accent & 255) / 255.0f;
    for (int y = 0; y < SCREEN_H; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        float fy = (float)y / SCREEN_H;
        float by = fy * (g_bokeh_h - 1);
        int y0 = (int)by, y1 = SDL_min(y0 + 1, g_bokeh_h - 1);
        float wy = by - y0;
        for (int x = 0; x < SCREEN_W; x++) {
            float fx = (float)x / SCREEN_W;
            float l = 0;
            if (g_bokeh) {
                float bx = fx * (g_bokeh_w - 1);
                int x0 = (int)bx, x1 = SDL_min(x0 + 1, g_bokeh_w - 1);
                float wx = bx - x0;
                const Uint8 *r0 = g_bokeh + y0 * g_bokeh_w, *r1 = g_bokeh + y1 * g_bokeh_w;
                l = ((r0[x0] * (1 - wx) + r0[x1] * wx) * (1 - wy) + (r1[x0] * (1 - wx) + r1[x1] * wx) * wy) / 255.0f;
            }
            /* glow behind the carousel */
            float dx = (fx - 0.5f) * 1.6f, dy = (fy - 0.45f) * 2.4f;
            float glow = SDL_max(0.0f, 1.0f - (dx * dx + dy * dy));
            glow = glow * glow * 0.22f;
            /* vignette */
            float vx = fx - 0.5f, vy = fy - 0.5f;
            float vig = 1.0f - (vx * vx * 0.9f + vy * vy * 1.3f);
            float light = l * 0.55f + glow;
            float r = (11 + 6 * fy) + light * (ar * 150 + 20);
            float g = (13 + 5 * fy) + light * (ag * 150 + 20);
            float b = (25 + 12 * fy) + light * (ab * 150 + 30);
            /* the floor */
            float ex = (x - FLOOR_CX) / FLOOR_RX, ey = (y - FLOOR_CY) / FLOOR_RY;
            float e = ex * ex + ey * ey;
            if (e < 1.02f) {
                float ftop, fbot, d = 0;
                if (floor_span((float)x, &ftop, &fbot) && fbot > ftop) d = (y - ftop) / (fbot - ftop);
                if (d < 0) d = 0;
                if (d > 1) d = 1;
                float far = (1 - d) * (1 - d);
                /* soft pool of light where the games stand */
                float px = (x - FLOOR_CX) / 560.0f, py = (y - FLOOR_TOP - 40) / 170.0f;
                float pool = SDL_max(0.0f, 1.0f - (px * px + py * py));
                /* dark grey for every system; only the background takes the colour */
                float grey = 24 + 14 * far + 26 * pool * pool + 5 * l;
                float fr = grey, fg = grey + 1, fb = grey + 4;
                float a = SDL_min(1.0f, SDL_max(0.0f, (1.0f - e) * 70));   /* soft edge */
                r = r * (1 - a) + fr * a;
                g = g * (1 - a) + fg * a;
                b = b * (1 - a) + fb * a;
                /* bright rim along the far edge */
                if (y < FLOOR_CY) {
                    float rim = SDL_max(0.0f, 1.0f - SDL_fabsf(1.0f - e) * 90) * 0.85f;
                    float side = 1.0f - SDL_fabsf(ex) * 0.6f;
                    rim *= side;
                    r += rim * 120;
                    g += rim * 122;
                    b += rim * 130;
                }
            }
            r *= vig; g *= vig; b *= vig;
            row[x] = 0xff000000u | ((Uint32)SDL_min(r, 255) << 16) | ((Uint32)SDL_min(g, 255) << 8) |
                     (Uint32)SDL_min(b, 255);
        }
    }
    if (T_bg) SDL_DestroyTexture(T_bg);
    T_bg = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
}

/* an image file as a surface, scaled down to fit max_w x max_h */
static SDL_Surface *load_art_surface(const char *path, int max_w, int max_h) {
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return NULL;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory((unsigned char *)data, (int)len, &w, &h, &n, 4);
    free(data);
    if (!px) return NULL;
    float sc = SDL_min((float)max_w / w, (float)max_h / h);
    if (sc > 1) sc = 1;
    int tw = SDL_max(1, (int)(w * sc)), th = SDL_max(1, (int)(h * sc));
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst) {
        stbi_image_free(px);
        return NULL;
    }
    for (int y = 0; y < th; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch);
        int y0 = y * h / th, y1 = SDL_max(y0 + 1, (y + 1) * h / th);
        for (int x = 0; x < tw; x++) {
            int x0 = x * w / tw, x1 = SDL_max(x0 + 1, (x + 1) * w / tw);
            unsigned long acc[4] = {0, 0, 0, 0}, cnt = 0;
            int stepy = SDL_max(1, (y1 - y0) / 4), stepx = SDL_max(1, (x1 - x0) / 4);
            for (int yy = y0; yy < y1; yy += stepy)
                for (int xx = x0; xx < x1; xx += stepx) {
                    unsigned char *p = px + ((size_t)yy * w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    cnt++;
                }
            row[x] = ((Uint32)(acc[3] / cnt) << 24) | ((Uint32)(acc[0] / cnt) << 16) |
                     ((Uint32)(acc[1] / cnt) << 8) | (Uint32)(acc[2] / cnt);
        }
    }
    stbi_image_free(px);
    return dst;
}

/* ------------------------------------------------------------------------ */
/* Covers                                                                    */
/* ------------------------------------------------------------------------ */

#define COVER_DIR "/data/jewelcase/covers"

static int g_art_running;       /* jc-art.elf is downloading */
static int g_art_done, g_art_total;
static char g_art_status[160] = "not started";
static Uint32 g_art_polled;

static int file_exists(const char *path) {
    char rp[1200];
    real_path(path, rp, sizeof(rp));
    FILE *f = fopen(rp, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void cover_base(Game *g, char *out, size_t cap) {
    snprintf(out, cap, COVER_DIR "/%s/%s", g_sys[g->sys].id, g->stem);
}

/* 1 = art file (path in out), 0 = being looked for, -1 = none */
static int cover_of(Game *g, char *out, size_t cap) {
    if (g->art[0]) {
        scopy(out, cap, g->art);
        return 1;
    }
    char base[1100];
    cover_base(g, base, sizeof(base));
    snprintf(out, cap, "%s.jpg", base);
    if (file_exists(out)) return 1;
    snprintf(out, cap, "%s.png", base);   /* as downloaded, before Jewel Case shrinks it */
    if (file_exists(out)) return 1;
    if (!look_of(&g_sys[g->sys]) || !look_of(&g_sys[g->sys])->repo) return -1;
    char none[1200];
    snprintf(none, sizeof(none), "%s.none", base);
    if (file_exists(none)) return -1;
    return 0;   /* not downloaded (yet): looked for again while it's on screen */
}

/* a disc's extras: kind 0 its title logo (libretro Named_Logos), 1 its disc scan (GameTDB) */
static void extra_base(Game *g, int kind, char *out, size_t cap) {
    snprintf(out, cap, COVER_DIR "/%s/%s/%s", g_sys[g->sys].id, kind ? "disc" : "logos", g->stem);
}

/* a game's details-screen extras: kind 0 its info (LaunchBox), 1 a screenshot, 2 its title screen */
static void info_base(Game *g, int kind, char *out, size_t cap) {
    static const char *const dir[] = {"info", "snaps", "titles"};
    snprintf(out, cap, COVER_DIR "/%s/%s/%s", g_sys[g->sys].id, dir[kind], g->stem);
}

/* 1 = have it (path in out), -1 = looked for and not found, 0 = not looked for yet */
static int extra_of(Game *g, int kind, char *out, size_t cap) {
    char base[1100];
    extra_base(g, kind, base, sizeof(base));
    snprintf(out, cap, "%s.png", base);
    if (file_exists(out)) return 1;
    snprintf(out, cap, "%s.jpg", base);
    if (file_exists(out)) return 1;
    char none[1200];
    snprintf(none, sizeof(none), "%s.none", base);
    return file_exists(none) ? -1 : 0;
}

/* your own disc image: <game folder>/discs/<name>.png or .jpg, or <name>-disc.png / .jpg
 * next to the game. Used when no disc scan was downloaded. */
static int user_disc_of(Game *g, char *out, size_t cap) {
    static const char *const fmt[] = {"%s/discs/%s.png", "%s/discs/%s.jpg", "%s/%s-disc.png", "%s/%s-disc.jpg"};
    for (int i = 0; i < 4; i++) {
        snprintf(out, cap, fmt[i], g->dir, g->stem);
        if (file_exists(out)) return 1;
    }
    return 0;
}

static int wants_extras(Game *g) {
    const SysLook *l = look_of(&g_sys[g->sys]);
    return l && l->repo && MEDIA_TPL[tpl_of(&g_sys[g->sys])].kind == 1;
}

/* the extras as one number, to notice when one arrives; *pending = one isn't looked for yet */
static int extras_state(Game *g, int *pending) {
    *pending = 0;
    if (!wants_extras(g)) return 0;
    char p[1200];
    int lo = extra_of(g, 0, p, sizeof(p)), sc = has_disc_scans(&g_sys[g->sys]) ? extra_of(g, 1, p, sizeof(p)) : -1;
    if (lo == 0 || sc == 0) *pending = 1;
    return (lo + 1) + 3 * (sc + 1);
}

static void poll_art_status(void) {
    Uint32 now = SDL_GetTicks();
    if (g_art_polled && (Sint32)(now - g_art_polled) < 1000) return;
    g_art_polled = now;
    char *txt = read_file(COVER_DIR "/status.txt", NULL);
    if (!txt) return;
    char word[16] = "";
    int a = 0, b = 0;
    long t = 0;
    sscanf(txt, "%15s %d %d %ld", word, &a, &b, &t);
    long age = (long)time(NULL) - t;
    if (!strcmp(word, "running") && age >= -5 && age < 120) {
        g_art_running = 1;
        g_art_done = a;
        g_art_total = b;
        if (b == 0) scopy(g_art_status, sizeof(g_art_status), "downloading the LaunchBox disc database");
        else snprintf(g_art_status, sizeof(g_art_status), "downloading, %d of %d checked", a, b);
    } else {
        g_art_running = 0;
        if (!strcmp(word, "done")) snprintf(g_art_status, sizeof(g_art_status), "%d downloaded last run (%d games)", a, b);
        else if (!strcmp(word, "running")) scopy(g_art_status, sizeof(g_art_status), "stopped");
        else snprintf(g_art_status, sizeof(g_art_status), "%.150s", txt);
        char *nl = strchr(g_art_status, '\n');
        if (nl) *nl = 0;
    }
    free(txt);
}


/* Ask jc-art.elf for every game that has no art of its own. */
static void start_cover_helper(void) {
#if defined(GS_NATIVE) || defined(GS_HOST)
    static char *req;
    size_t cap = 4 << 20, len = 0;
    if (!req) req = malloc(cap);
    if (!req) return;
    len = (size_t)snprintf(req, cap, "jewelcase-art 1\n");
    int count = 0;
    /* covers first, then the discs' logos and scans, then game info and screenshots */
    for (int pass = 0; pass < 4; pass++)
        for (int s = 0; s < g_nsys; s++) {
            const SysLook *l = look_of(&g_sys[s]);
            if (!l || !l->repo) continue;
            const char *lbp = launchbox_platform(&g_sys[s]);
            for (int k = 0; k < g_sys[s].ngames; k++) {
                Game *g = &g_sys[s].games[k];
                char base[1100], line[2600];
                int n = 0;
                if (pass == 0 && !g->art[0]) {
                    cover_base(g, base, sizeof(base));
                    n += snprintf(line + n, sizeof(line) - n, "G\t%s\t%s\t%s\n", l->repo, base, g->stem);
                } else if (pass == 1 && wants_extras(g)) {
                    extra_base(g, 0, base, sizeof(base));
                    n += snprintf(line + n, sizeof(line) - n, "L\t%s\t%s\t%s\n", l->repo, base, g->stem);
                    /* the disc: LaunchBox, then GameTDB for GameCube / Wii */
                    extra_base(g, 1, base, sizeof(base));
                    if (lbp)
                        n += snprintf(line + n, sizeof(line) - n, "D\t%s\t%s\t%s\t%s\n", lbp, base, g->stem,
                                      has_gametdb(&g_sys[s]) ? g->path : "");
                    else if (has_gametdb(&g_sys[s]))
                        n += snprintf(line + n, sizeof(line) - n, "T\t-\t%s\t%s\n", base, g->path);
                } else if (pass == 2 && lbp) {
                    info_base(g, 0, base, sizeof(base));
                    n += snprintf(line + n, sizeof(line) - n, "I\t%s\t%s\t%s\n", lbp, base, g->stem);
                } else if (pass == 3) {
                    info_base(g, 1, base, sizeof(base));
                    n += snprintf(line + n, sizeof(line) - n, "S\t%s\t%s\t%s\n", l->repo, base, g->stem);
                    info_base(g, 2, base, sizeof(base));
                    n += snprintf(line + n, sizeof(line) - n, "W\t%s\t%s\t%s\n", l->repo, base, g->stem);
                }
                if (n <= 0) continue;
                if (n >= (int)sizeof(line) || len + (size_t)n + 1 >= cap) break;
                memcpy(req + len, line, (size_t)n + 1);
                len += (size_t)n;
                count++;
            }
        }
    if (!count) return;
    if (write_file("/data/jewelcase-art.txt", req) != 0) {
        scopy(g_art_status, sizeof(g_art_status), "couldn't write /data/jewelcase-art.txt");
        return;
    }
#ifdef GS_NATIVE
    char helper[600];
    snprintf(helper, sizeof(helper), "%s/jc-art.elf", g_appdir);
    int st = send_to_elfldr(helper);
    if (st == -3) scopy(g_art_status, sizeof(g_art_status), "jc-art.elf is missing from the Jewel Case folder");
    else if (st != 200) scopy(g_art_status, sizeof(g_art_status), "ELF loader on port 9021 isn't answering");
    else {
        g_art_running = 1;   /* until the helper's status says otherwise */
        g_art_polled = SDL_GetTicks() + 3000;
        snprintf(g_art_status, sizeof(g_art_status), "starting (%d games)", count);
    }
#endif
#endif
}

/* ------------------------------------------------------------------------ */
/* UI state                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    int kind;           /* 0 recent, 1 system, 2 favourites */
    int sys;
    char label[64];
    int count;
} Entry;

static Entry g_entries[72];
static int g_nentries;
static int g_sel_entry;
static int g_sel_game;
static float g_pos;                 /* carousel position, eases toward g_sel_game */
static float g_tab_scroll, g_tab_target;
static Game *g_recent_games[MAX_RECENT];
static int g_nrecent_games;
static Game **g_fav_games;
static int g_nfav_games;

static char g_modal_title[128];
static char g_modal_text[4096];
static int g_modal;
static int g_launching;
static char g_status_msg[256];
static Uint32 g_status_until;
static char g_config_err[256];
static char g_scan_status[200] = "direct";
static int g_scan_failed;
static int g_builds_left;           /* carousel images still to build */

static void open_modal(const char *title, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_modal_text, sizeof(g_modal_text), fmt, ap);
    va_end(ap);
    scopy(g_modal_title, sizeof(g_modal_title), title);
    g_modal = 1;
}

static void flash(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_msg, sizeof(g_status_msg), fmt, ap);
    va_end(ap);
    g_status_until = SDL_GetTicks() + 2500;
}

static Game *find_game(const char *path) {
    for (int s = 0; s < g_nsys; s++)
        for (int k = 0; k < g_sys[s].ngames; k++)
            if (!strcmp(g_sys[s].games[k].path, path)) return &g_sys[s].games[k];
    return NULL;
}

static void build_lists(void) {
    g_nrecent_games = 0;
    for (int i = 0; i < g_nrecent && g_nrecent_games < MAX_RECENT; i++) {
        Game *hit = find_game(g_recent[i]);
        if (hit) g_recent_games[g_nrecent_games++] = hit;
    }
    if (!g_fav_games) g_fav_games = calloc(MAX_FAVS, sizeof(Game *));
    g_nfav_games = 0;
    for (int i = 0; i < g_nfavs && g_fav_games; i++) {
        Game *hit = find_game(g_favs[i]);
        if (hit) g_fav_games[g_nfav_games++] = hit;
    }
}

static void build_entries(void) {
    g_nentries = 0;
    build_lists();
    if (g_nrecent_games > 0) {
        Entry *e = &g_entries[g_nentries++];
        e->kind = 0;
        e->sys = -1;
        scopy(e->label, sizeof(e->label), "Recently Played");
        e->count = g_nrecent_games;
    }
    if (g_nfav_games > 0) {
        Entry *e = &g_entries[g_nentries++];
        e->kind = 2;
        e->sys = -1;
        scopy(e->label, sizeof(e->label), "Favorites");
        e->count = g_nfav_games;
    }
    for (int s = 0; s < g_nsys && g_nentries < 72; s++) {
        if (g_sys[s].ngames == 0 && g_hide_empty) continue;
        Entry *e = &g_entries[g_nentries++];
        e->kind = 1;
        e->sys = s;
        scopy(e->label, sizeof(e->label), g_sys[s].name);
        e->count = g_sys[s].ngames;
    }
}

static int entry_count(void) {
    if (g_nentries == 0) return 0;
    Entry *e = &g_entries[g_sel_entry];
    if (e->kind == 0) return g_nrecent_games;
    if (e->kind == 2) return g_nfav_games;
    return g_sys[e->sys].ngames;
}

static Game *entry_game(int i) {
    if (g_nentries == 0) return NULL;
    Entry *e = &g_entries[g_sel_entry];
    if (e->kind == 0) return (i >= 0 && i < g_nrecent_games) ? g_recent_games[i] : NULL;
    if (e->kind == 2) return (i >= 0 && i < g_nfav_games) ? g_fav_games[i] : NULL;
    System *s = &g_sys[e->sys];
    return (i >= 0 && i < s->ngames) ? &s->games[i] : NULL;
}

static Uint32 entry_accent(int i) {
    if (i < 0 || i >= g_nentries) return 0x6d63e8;
    Entry *e = &g_entries[i];
    if (e->kind == 0) return 0x6d63e8;
    if (e->kind == 2) return 0xd9a72a;
    return accent_of(g_sys[e->sys].color);
}

static void free_media(Game *g) {
    if (g->media) SDL_DestroyTexture(g->media);
    if (g->refl) SDL_DestroyTexture(g->refl);
    if (g->back) SDL_DestroyTexture(g->back);
    g->media = g->refl = g->back = NULL;
    g->media_pending = g->extra_pending = 0;
}

/* keep the images of the games near the selection only */
#define KEEP_AROUND 9
static void trim_media(void) {
    for (int s = 0; s < g_nsys; s++)
        for (int k = 0; k < g_sys[s].ngames; k++) {
            Game *g = &g_sys[s].games[k];
            if (!g->media) continue;
            int keep = 0;
            int n = entry_count();
            for (int i = SDL_max(0, g_sel_game - KEEP_AROUND); i < SDL_min(n, g_sel_game + KEEP_AROUND + 1); i++)
                if (entry_game(i) == g) keep = 1;
            if (!keep) free_media(g);
        }
}

/* ------------------------------------------------------------------------ */
/* Carousel images                                                           */
/* ------------------------------------------------------------------------ */

/* The art on the label: a dimmed, stretched copy of it fills the label, the art
 * itself sits on top, scaled to fill the label but losing no more than a sliver
 * of its edges (the rest of the gap shows the backdrop). */
static void draw_art_fill(SDL_Surface *art, int x, int y, int w, int h) {
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, art);
    if (!t) return;
    float sa = (float)art->w / art->h, sd = (float)w / h;
    /* backdrop: cropped to fill, dark */
    SDL_Rect src = {0, 0, art->w, art->h};
    if (sa > sd) {
        src.w = (int)(art->h * sd);
        src.x = (art->w - src.w) / 2;
    } else {
        src.h = (int)(art->w / sd);
        src.y = (art->h - src.h) / 2;
    }
    SDL_Rect dst = {x, y, w, h};
    SDL_SetTextureColorMod(t, 70, 70, 76);
    SDL_RenderCopy(R, t, &src, &dst);
    /* the art: between fitting and filling */
    float fit = SDL_min((float)w / art->w, (float)h / art->h), fill = SDL_max((float)w / art->w, (float)h / art->h);
    float sc = SDL_min(fill, fit * 1.18f);
    float vw = art->w * sc, vh = art->h * sc;
    SDL_Rect s2 = {0, 0, art->w, art->h};
    if (vw > w) {
        s2.w = (int)(w / sc);
        s2.x = (art->w - s2.w) / 2;
        vw = (float)w;
    }
    if (vh > h) {
        s2.h = (int)(h / sc);
        s2.y = (int)((art->h - s2.h) * 0.4f);   /* titles tend to sit at the top */
        vh = (float)h;
    }
    SDL_Rect d2 = {x + (int)((w - vw) / 2), y + (int)((h - vh) / 2), (int)vw, (int)vh};
    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_RenderCopy(R, t, &s2, &d2);
    SDL_DestroyTexture(t);
}

static Uint32 art_px(SDL_Surface *art, float u, float v) {
    int ax = (int)(u * art->w), ay = (int)(v * art->h);
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (ax >= art->w) ax = art->w - 1;
    if (ay >= art->h) ay = art->h - 1;
    return ((Uint32 *)((Uint8 *)art->pixels + ay * art->pitch))[ax] & 0xffffff;
}

/* the part of a surface inside fractions x0..x1, y0..y1 (frees the original) */
static SDL_Surface *crop_surface(SDL_Surface *s, const BoxCrop *c) {
    SDL_Rect r = {(int)(s->w * c->x0), (int)(s->h * c->y0), 0, 0};
    r.w = (int)(s->w * c->x1) - r.x;
    r.h = (int)(s->h * c->y1) - r.y;
    if (r.w < 8 || r.h < 8) return s;
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, r.w, r.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!d) return s;
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
    SDL_BlitSurface(s, &r, d, NULL);
    SDL_FreeSurface(s);
    return d;
}

static Uint32 art_px_a(SDL_Surface *art, int x, int y) {
    if (x < 0 || y < 0 || x >= art->w || y >= art->h) return 0;
    return ((Uint32 *)((Uint8 *)art->pixels + y * art->pitch))[x];
}

/* Where a disc's title goes: the band above the hole, as real discs print it.
 * In disc-label coordinates (0..2 r_out). */
static SDL_Rect disc_title_band(int r_in, int r_out) {
    int top = (int)(r_out * 0.11f), bottom = r_out - r_in - (int)(r_out * 0.04f);
    int w = (int)(r_out * 1.42f);
    return (SDL_Rect){r_out - w / 2, top, w, bottom - top};
}

/* The disc label, masked to the ring r_in..r_out.
 *  scan: a disc scan (GameTDB), drawn as it is.
 *  art:  box art printed edge to edge (above the system band, if the system has
 *        one); with a logo, the logo across the band above the hole on a soft
 *        shadow; title = 1 shades that band for title text drawn on it afterwards.
 *  none: a shade of the system colour.
 *  band_top: where the system's black band starts (fraction of the disc), 1 = none. */
static SDL_Texture *disc_label(SDL_Surface *art, SDL_Surface *logo, int scan, int title, Uint32 fill, int r_in,
                               int r_out, float band_top) {
    int d = r_out * 2;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, d, d, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    /* box art: cover the disc down to the band */
    int band_y = band_top < 1 ? (int)(band_top * d) : d + 1;
    float sc = 1, offx = 0, offy = 0;
    if (art && !scan) {
        int ah = SDL_min(d, band_y);
        sc = SDL_max((float)d / art->w, (float)ah / art->h);
        offx = (d - art->w * sc) / 2;
        offy = (ah - art->h * sc) * 0.4f;
    }
    /* the logo's place in the title band */
    SDL_Rect band = disc_title_band(r_in, r_out);
    int lx = 0, ly = 0;
    if (logo) {
        lx = band.x + (band.w - logo->w) / 2;
        ly = band.y + (band.h - logo->h) / 2;
    }
    float bcx = band.x + band.w / 2.0f, bcy = band.y + band.h / 2.0f;
    float brx = (logo ? logo->w : band.w) * 0.62f + 10, bry = (logo ? logo->h : band.h * 0.7f) * 0.75f + 10;
    for (int y = 0; y < d; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < d; x++) {
            float dx = x + 0.5f - r_out, dy = y + 0.5f - r_out;
            float r = SDL_sqrtf(dx * dx + dy * dy);
            float a = SDL_min(1.0f, SDL_max(0.0f, r_out - r)) * SDL_min(1.0f, SDL_max(0.0f, r - r_in));
            if (a <= 0) {
                row[x] = 0;
                continue;
            }
            float c[3];
            if (art && scan) {
                Uint32 b = art_px(art, (x + 0.5f) / d, (y + 0.5f) / d);
                c[0] = (b >> 16) & 255;
                c[1] = (b >> 8) & 255;
                c[2] = b & 255;
            } else if (art) {
                Uint32 b = art_px(art, (x + 0.5f - offx) / (art->w * sc), (y + 0.5f - offy) / (art->h * sc));
                c[0] = (b >> 16) & 255;
                c[1] = (b >> 8) & 255;
                c[2] = b & 255;
                float k = 1;
                /* the title band: a soft shade behind the logo or title */
                if (logo || title) {
                    float ex = (x - bcx) / brx, ey = (y - bcy) / bry;
                    float e = ex * ex + ey * ey;
                    if (e < 1) k *= 1 - 0.42f * (1 - e) * (1 - e);
                }
                for (int i = 0; i < 3; i++) c[i] *= k;
                if (logo) {
                    /* shadow, then the logo */
                    Uint32 sp = art_px_a(logo, x - lx - 3, y - ly - 4);
                    float sa = ((sp >> 24) & 255) / 255.0f * 0.6f;
                    for (int i = 0; i < 3; i++) c[i] *= 1 - sa;
                    Uint32 lp = art_px_a(logo, x - lx, y - ly);
                    float la = ((lp >> 24) & 255) / 255.0f;
                    if (la > 0) {
                        c[0] = c[0] * (1 - la) + ((lp >> 16) & 255) * la;
                        c[1] = c[1] * (1 - la) + ((lp >> 8) & 255) * la;
                        c[2] = c[2] * (1 - la) + (lp & 255) * la;
                    }
                }
            } else {
                float k = 0.55f + 0.45f * (1.0f - r / r_out);
                c[0] = ((fill >> 16) & 255) * k;
                c[1] = ((fill >> 8) & 255) * k;
                c[2] = (fill & 255) * k;
            }
            if (!scan && y >= band_y) {
                /* the system band: black, a hint of sheen at its top edge */
                float e = SDL_max(0.0f, 1.0f - (y - band_y) / 3.0f);
                c[0] = 14 + 40 * e;
                c[1] = 14 + 40 * e;
                c[2] = 17 + 42 * e;
            }
            row[x] = ((Uint32)(a * 255) << 24) | ((Uint32)SDL_clamp(c[0], 0, 255) << 16) |
                     ((Uint32)SDL_clamp(c[1], 0, 255) << 8) | (Uint32)SDL_clamp(c[2], 0, 255);
        }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

static void title_on_label_f(Font *f, const char *title, int x, int y, int w, int h) {
    char lines[3][200];
    /* up to 3 lines, centred */
    char rest[200];
    scopy(rest, sizeof(rest), title);
    int nl = 0;
    while (rest[0] && nl < 3) {
        char l1[200], l2[200];
        int n = wrap2(f, rest, (float)w - 24, l1, l2);
        scopy(lines[nl++], 200, l1);
        if (n < 2) break;
        /* what's left after line 1 */
        size_t used = strlen(l1);
        const char *p = rest + used;
        while (*p == ' ') p++;
        char tmp[200];
        scopy(tmp, sizeof(tmp), p);
        scopy(rest, sizeof(rest), tmp);
        if (nl == 2) {
            ellipsize(f, rest, (float)w - 24, lines[nl++], 200);
            break;
        }
    }
    int total = nl * f->line;
    int ty = y + (h - total) / 2;
    for (int i = 0; i < nl; i++) {
        float tw = text_width(f, lines[i]);
        draw_text(f, x + (w - tw) / 2, ty + i * f->line, lines[i], 0xffffff, 245);
    }
}

static void title_on_label(const char *title, int x, int y, int w, int h) {
    title_on_label_f(&F_tile, title, x, y, w, h);
}

#define REFL_FRACTION 0.36f

static void build_media(Game *g) {
    System *s = &g_sys[g->sys];
    int ti = tpl_of(s);
    const MediaTpl *t = &MEDIA_TPL[ti];
    ensure_tpl(ti);
    if (!T_tpl_base[ti]) return;

    char cover[1200];
    int cst = cover_of(g, cover, sizeof(cover));
    int downloaded = cst == 1 && !g->art[0] && !strncmp(cover, COVER_DIR "/", strlen(COVER_DIR) + 1);
    SDL_Surface *art = NULL;
    /* discs: big enough to cover the whole disc */
    if (cst == 1) art = load_art_surface(cover, t->kind ? t->ly * 4 : t->lw * 2, t->kind ? t->ly * 4 : t->lh * 2);
    size_t cl = strlen(cover);
    if (art && cl > 4 && !strcmp(cover + cl - 4, ".png") && !strncmp(cover, COVER_DIR "/", strlen(COVER_DIR) + 1)) {
        /* a downloaded cover: keep a small JPEG of it instead, so it loads fast next time */
        unsigned char *rgb = malloc((size_t)art->w * art->h * 3);
        if (rgb) {
            for (int y = 0; y < art->h; y++) {
                const Uint32 *row = (const Uint32 *)((Uint8 *)art->pixels + y * art->pitch);
                for (int x = 0; x < art->w; x++) {
                    unsigned char *d = rgb + ((size_t)y * art->w + x) * 3;
                    d[0] = (row[x] >> 16) & 255;
                    d[1] = (row[x] >> 8) & 255;
                    d[2] = row[x] & 255;
                }
            }
            char jpg[1200], rp[1300], rpj[1300];
            snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)(cl - 4), cover);
            real_path(jpg, rpj, sizeof(rpj));
            snprintf(rp, sizeof(rp), "%s.part", rpj);
            if (stbi_write_jpg(rp, art->w, art->h, 3, rgb, 88) && rename(rp, rpj) == 0) {
                char rpp[1300];
                real_path(cover, rpp, sizeof(rpp));
                remove(rpp);
            }
            else remove(rp);
            free(rgb);
        }
    }

    /* the label shows the art without the box's system branding */
    const BoxCrop *bc = downloaded ? box_crop_of(s) : NULL;
    if (art && bc) art = crop_surface(art, bc);

    /* discs: the title logo, or a scan of the real disc */
    SDL_Surface *logo = NULL, *scan = NULL;
    if (t->kind == 1 && wants_extras(g)) {
        char ep[1200];
        if (has_disc_scans(s) && extra_of(g, 1, ep, sizeof(ep)) == 1) scan = load_art_surface(ep, t->ly * 2, t->ly * 2);
        if (!scan && user_disc_of(g, ep, sizeof(ep))) scan = load_art_surface(ep, t->ly * 2, t->ly * 2);
        if (!scan && art && extra_of(g, 0, ep, sizeof(ep)) == 1) {
            SDL_Rect b = disc_title_band(t->lx, t->ly);
            logo = load_art_surface(ep, b.w, (int)(b.h * 0.92f));
            /* the box's own title is at its top: with the logo there instead,
             * the disc shows the art below it */
            static const BoxCrop below_title = {"", 0, 0.28f, 1, 1};
            if (logo && art->h > art->w) art = crop_surface(art, &below_title);
        }
        g->extra_state = extras_state(g, &g->extra_pending);
    } else {
        g->extra_pending = 0;
    }

    SDL_Texture *tgt = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, t->w, t->h);
    if (!tgt) {
        if (art) SDL_FreeSurface(art);
        if (logo) SDL_FreeSurface(logo);
        if (scan) SDL_FreeSurface(scan);
        return;
    }
    SDL_SetTextureBlendMode(tgt, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(R, tgt);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
    SDL_RenderClear(R);
    SDL_SetTextureColorMod(T_tpl_base[ti], 255, 255, 255);
    SDL_SetTextureAlphaMod(T_tpl_base[ti], 255);
    SDL_RenderCopy(R, T_tpl_base[ti], NULL, NULL);

    Uint32 col = accent_of(s->color);
    if (t->kind == 0) {
        if (art && !strcmp(t->name, "wide")) {
            /* SNES label: a black sticker with the art in a window left of centre
             * and the title on the right, the way the real labels are laid out */
            fill_rect(t->lx, t->ly, t->lw, t->lh, 0x111114, 255);
            int ax = t->lx + (int)(t->lw * 0.18f), aw = (int)(t->lw * 0.53f);
            int ay = t->ly + (int)(t->lh * 0.13f), ah = (int)(t->lh * 0.72f);
            draw_art_fill(art, ax, ay, aw, ah);
            fill_rect(t->lx + (int)(t->lw * 0.73f), ay, (int)(t->lw * 0.24f), 3, accent_of(s->color), 255);
            title_on_label_f(&F_tiny, g->title, t->lx + (int)(t->lw * 0.71f) - 12, ay + 8, (int)(t->lw * 0.28f) + 24,
                             (int)(ah * 0.6f));
        } else if (art) {
            draw_art_fill(art, t->lx, t->ly, t->lw, t->lh);
        } else {
            /* blank label: system colour and the title */
            int hh = t->lh / 2;
            fill_rect(t->lx, t->ly, t->lw, hh, col, 255);
            Uint32 dark = ((((col >> 16) & 255) * 3 / 5) << 16) | ((((col >> 8) & 255) * 3 / 5) << 8) | ((col & 255) * 3 / 5);
            fill_rect(t->lx, t->ly + hh, t->lw, t->lh - hh, dark, 255);
            title_on_label(g->title, t->lx, t->ly, t->lw, t->lh);
        }
    } else {
        int title = 0;   /* without a logo the box's own title is on the disc already */
        const DiscBand *band = scan ? NULL : disc_band_of(s);
        float btop = band ? band->top : 1.0f;
        SDL_Texture *lab = scan ? disc_label(scan, NULL, 1, 0, col, t->lx, t->ly, 1.0f)
                                : disc_label(art, logo, 0, title, col, t->lx, t->ly, btop);
        int c = t->w / 2;
        if (lab) {
            SDL_Rect d = {c - t->ly, c - t->ly, t->ly * 2, t->ly * 2};
            SDL_RenderCopy(R, lab, NULL, &d);
            SDL_DestroyTexture(lab);
        }
        if (band) {
            /* the system's name on its band, as wide as the disc allows there */
            float by = c - t->ly + btop * t->ly * 2, bh = c + t->ly - by;
            if (!band->line2) {
                float ty = by + bh * 0.40f;
                float half = SDL_sqrtf(SDL_max(0.0f, (float)t->ly * t->ly - (ty - c) * (ty - c))) * 0.86f;
                Font *f = text_width(&F_disc_l, band->line1) < half * 2 ? &F_disc_l : &F_disc_m;
                draw_text(f, c - text_width(f, band->line1) / 2, ty - f->line / 2.0f, band->line1, 0xf2f2f4, 245);
            } else {
                float y1 = by + bh * 0.22f, y2 = by + bh * 0.48f;
                draw_text(&F_disc_s, c - text_width(&F_disc_s, band->line1) / 2, y1 - F_disc_s.line / 2.0f,
                          band->line1, 0xd8d9de, 240);
                draw_text(&F_disc_b, c - text_width(&F_disc_b, band->line2) / 2, y2 - F_disc_b.line / 2.0f,
                          band->line2, 0xe4e5ea, 245);
            }
        }
        if (!art && !scan) {
            title_on_label(g->title, c - t->ly + 40, c - t->ly + 30, t->ly * 2 - 80, t->ly - t->lx - 20);
        } else if (title) {
            SDL_Rect b = disc_title_band(t->lx, t->ly);
            title_on_label(g->title, c - t->ly + b.x, c - t->ly + b.y, b.w, b.h);
        }
    }
    if (T_tpl_over[ti]) {
        SDL_SetTextureAlphaMod(T_tpl_over[ti], 255);
        SDL_RenderCopy(R, T_tpl_over[ti], NULL, NULL);
    }
    if (art) SDL_FreeSurface(art);
    if (logo) SDL_FreeSurface(logo);
    if (scan) SDL_FreeSurface(scan);

    /* floor reflection: the bottom of the image, flipped and fading out */
    int rh = (int)(t->h * REFL_FRACTION);
    SDL_Surface *full = SDL_CreateRGBSurfaceWithFormat(0, t->w, t->h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *rs = SDL_CreateRGBSurfaceWithFormat(0, t->w, rh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (full && rs && SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, full->pixels, full->pitch) == 0) {
        for (int y = 0; y < rh; y++) {
            const Uint32 *src = (const Uint32 *)((Uint8 *)full->pixels + (t->h - 1 - y) * full->pitch);
            Uint32 *dst = (Uint32 *)((Uint8 *)rs->pixels + y * rs->pitch);
            float f = 1.0f - (float)y / rh;
            float k = 0.38f * f * f;
            for (int x = 0; x < t->w; x++) {
                Uint32 p = src[x];
                Uint32 a = (Uint32)(((p >> 24) & 255) * k);
                dst[x] = (a << 24) | (p & 0xffffff);
            }
        }
        g->refl = SDL_CreateTextureFromSurface(R, rs);
        if (g->refl) SDL_SetTextureBlendMode(g->refl, SDL_BLENDMODE_BLEND);
        g->refl_h = rh;
    }
    if (full) SDL_FreeSurface(full);
    if (rs) SDL_FreeSurface(rs);

    SDL_SetRenderTarget(R, NULL);
    g->media = tgt;
    g->media_w = t->w;
    g->media_h = t->h;
    g->media_pending = (cst == 0);
    g->cover_check = SDL_GetTicks();
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------ */

/* a texture as a quad whose outer edge is shorter (turned away from the centre).
 * hf(u): height factor across the width; side < 0 = left item. */
static void draw_turned(SDL_Texture *tex, float x0, float w, const float *ytop, const float *ybot, int strips,
                        Uint8 bright, Uint8 alpha) {
    SDL_Vertex v[2 * 17];
    int idx[6 * 16];
    SDL_Color c = {bright, bright, bright, alpha};
    for (int j = 0; j <= strips; j++) {
        float u = (float)j / strips;
        v[2 * j] = (SDL_Vertex){{x0 + u * w, ytop[j]}, c, {u, 0}};
        v[2 * j + 1] = (SDL_Vertex){{x0 + u * w, ybot[j]}, c, {u, 1}};
    }
    for (int j = 0; j < strips; j++) {
        int a = 2 * j;
        idx[6 * j + 0] = a; idx[6 * j + 1] = a + 1; idx[6 * j + 2] = a + 2;
        idx[6 * j + 3] = a + 2; idx[6 * j + 4] = a + 1; idx[6 * j + 5] = a + 3;
    }
    SDL_RenderGeometry(R, tex, v, 2 * (strips + 1), idx, 6 * strips);
}

/* ------------------------------------------------------------------------ */
/* Turning the selected game around (right stick)                            */
/* ------------------------------------------------------------------------ */

static float g_spin;            /* degrees, the selected game turned around its upright axis */
static Game *g_spin_game;       /* the game it applies to: another game starts facing front */

/* the back of a game's cartridge or disc: the template seen from behind; a disc
 * shows its shiny data side in its own colour */
static void make_back(Game *g) {
    int ti = tpl_of(&g_sys[g->sys]);
    const MediaTpl *t = &MEDIA_TPL[ti];
    ensure_tpl(ti);
    if (!T_tpl_base[ti]) return;
    SDL_Texture *tgt = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, t->w, t->h);
    if (!tgt) return;
    SDL_Texture *prev = SDL_GetRenderTarget(R);
    SDL_SetTextureBlendMode(tgt, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(R, tgt);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
    SDL_RenderClear(R);
    SDL_SetTextureColorMod(T_tpl_base[ti], 255, 255, 255);
    SDL_SetTextureAlphaMod(T_tpl_base[ti], 255);
    SDL_Texture *body = NULL;
    if (t->kind == 0) {
        /* a cartridge's back is plain: paint the label area over with the shell
         * around it, row by row (keeps the shading), seen from behind */
        int w, h, n;
        unsigned char *px = stbi_load_from_memory(t->base, (int)t->base_len, &w, &h, &n, 4);
        if (px) {
            int x0 = SDL_max(1, t->lx - 3), x1 = SDL_min(w - 2, t->lx + t->lw + 3);
            int y0 = SDL_max(0, t->ly - 3), y1 = SDL_min(h, t->ly + t->lh + 3);
            for (int y = y0; y < y1; y++) {
                unsigned char *row = px + (size_t)y * w * 4;
                unsigned char *a = row + (x0 - 1) * 4, *b = row + (x1 + 1) * 4;
                if (a[3] < 200) a = b;
                if (b[3] < 200) b = a;
                for (int x = x0; x <= x1; x++) {
                    float f = (float)(x - x0) / SDL_max(1, x1 - x0);
                    unsigned char *d = row + x * 4;
                    if (d[3] == 0) continue;
                    for (int c = 0; c < 3; c++) d[c] = (unsigned char)(a[c] * (1 - f) + b[c] * f);
                }
            }
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (sf) {
                for (int y = 0; y < h; y++) {
                    Uint32 *drow = (Uint32 *)((Uint8 *)sf->pixels + y * sf->pitch);
                    const unsigned char *srow = px + (size_t)y * w * 4;
                    for (int x = 0; x < w; x++) {
                        const unsigned char *p = srow + (w - 1 - x) * 4;   /* mirrored */
                        drow[x] = ((Uint32)p[3] << 24) | ((Uint32)p[0] << 16) | ((Uint32)p[1] << 8) | p[2];
                    }
                }
                body = SDL_CreateTextureFromSurface(R, sf);
                SDL_FreeSurface(sf);
            }
            stbi_image_free(px);
        }
    }
    if (body) {
        SDL_SetTextureBlendMode(body, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(R, body, NULL, NULL);
        SDL_DestroyTexture(body);
    } else {
        SDL_RenderCopyEx(R, T_tpl_base[ti], NULL, NULL, 0, NULL, SDL_FLIP_HORIZONTAL);
    }
    if (t->kind == 1) {
        /* the data side: silver, tinted by the disc's colour, with a rainbow sheen */
        float tr = 200, tg = 204, tb = 212, k = 0;
        if (strstr(t->name, "black")) { tr = 26; tg = 26; tb = 32; k = 0.75f; }
        else if (strstr(t->name, "blue")) { tr = 40; tg = 80; tb = 190; k = 0.55f; }
        else if (strstr(t->name, "green")) { tr = 40; tg = 130; tb = 60; k = 0.45f; }
        int r_in = t->lx, r_out = t->ly, d = r_out * 2;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, d, d, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            for (int y = 0; y < d; y++) {
                Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
                for (int x = 0; x < d; x++) {
                    float dx = x + 0.5f - r_out, dy = y + 0.5f - r_out;
                    float r = SDL_sqrtf(dx * dx + dy * dy);
                    float a = SDL_clamp(r_out - r, 0.0f, 1.0f) * SDL_clamp(r - r_in, 0.0f, 1.0f);
                    if (a <= 0) {
                        row[x] = 0;
                        continue;
                    }
                    float th = SDL_atan2f(dy, dx);
                    float v = 0.58f + 0.28f * SDL_cosf(2 * th - 0.8f) + 0.06f * SDL_cosf(6 * th);
                    float c[3] = {v * 235, v * 237, v * 242};
                    const float tint[3] = {tr, tg, tb};
                    for (int i = 0; i < 3; i++) {
                        float rb = 0.07f * SDL_sinf(3 * th + r * 0.05f + i * 2.1f);
                        c[i] = c[i] * (1 - k) + tint[i] * k * (0.6f + 0.8f * v);
                        c[i] = c[i] * (1 + rb);
                    }
                    /* where the data ends, a faint ring */
                    if (SDL_fabsf(r - r_out * 0.93f) < 1.2f) for (int i = 0; i < 3; i++) c[i] *= 0.85f;
                    row[x] = ((Uint32)(a * 255) << 24) | ((Uint32)SDL_clamp(c[0], 0, 255) << 16) |
                             ((Uint32)SDL_clamp(c[1], 0, 255) << 8) | (Uint32)SDL_clamp(c[2], 0, 255);
                }
            }
            SDL_Texture *dt = SDL_CreateTextureFromSurface(R, s);
            SDL_FreeSurface(s);
            if (dt) {
                SDL_SetTextureBlendMode(dt, SDL_BLENDMODE_BLEND);
                SDL_Rect dr = {t->w / 2 - r_out, t->h / 2 - r_out, d, d};
                SDL_RenderCopy(R, dt, NULL, &dr);
                SDL_DestroyTexture(dt);
            }
        }
    }
    SDL_SetRenderTarget(R, prev);
    g->back = tgt;
}

/* one face (or a layer of the body) of a turned game: the texture on the plane
 * at depth zp, turned by angle a around the upright axis through cx */
static void draw_spun_face(SDL_Texture *tex, int flip, float cx, float cy, float w, float h, float ca, float sa,
                           float zp, Uint8 r, Uint8 g, Uint8 b, Uint8 alpha) {
    enum { N = 16 };
    const float F = 2400;    /* how far the eye is: perspective */
    float px[N + 1], top[N + 1], bot[N + 1], tu[N + 1];
    for (int j = 0; j <= N; j++) {
        float u = (float)j / N;
        float X = (u - 0.5f) * w;
        float x3 = X * ca + zp * sa, z3 = -X * sa + zp * ca;
        float k = F / (F + z3);
        px[j] = cx + x3 * k;
        top[j] = cy - h / 2 * k;
        bot[j] = cy + h / 2 * k;
        tu[j] = flip ? 1 - u : u;
    }
    /* each strip its own quad, a hair wider than its share so no seams show */
    SDL_Vertex v[4 * N];
    int idx[6 * N];
    SDL_Color col = {r, g, b, alpha};
    for (int j = 0; j < N; j++) {
        float dir = px[j + 1] >= px[j] ? 1.0f : -1.0f;
        float x1 = px[j + 1] + 0.7f * dir;
        v[4 * j + 0] = (SDL_Vertex){{px[j], top[j]}, col, {tu[j], 0}};
        v[4 * j + 1] = (SDL_Vertex){{px[j], bot[j]}, col, {tu[j], 1}};
        v[4 * j + 2] = (SDL_Vertex){{x1, top[j + 1]}, col, {tu[j + 1], 0}};
        v[4 * j + 3] = (SDL_Vertex){{x1, bot[j + 1]}, col, {tu[j + 1], 1}};
        int a = 4 * j;
        idx[6 * j + 0] = a; idx[6 * j + 1] = a + 1; idx[6 * j + 2] = a + 2;
        idx[6 * j + 3] = a + 2; idx[6 * j + 4] = a + 1; idx[6 * j + 5] = a + 3;
    }
    SDL_RenderGeometry(R, tex, v, 4 * N, idx, 6 * N);
}

/* the game turned by deg degrees: its body as stacked layers for the thickness,
 * the face toward you on top, and its reflection (rt = top of the reflection) */
static void draw_spun(Game *g, float cx, float cy, float w, float h, float deg, float rt, float rh, Uint8 bright,
                      Uint8 alpha) {
    float a = deg * (float)M_PI / 180.0f, ca = SDL_cosf(a), sa = SDL_sinf(a);
    const MediaTpl *t = &MEDIA_TPL[tpl_of(&g_sys[g->sys])];
    int front = ca >= 0;
    if (!front && !g->back) make_back(g);
    SDL_Texture *face = front ? g->media : (g->back ? g->back : g->media);
    /* reflection: the face toward you, fading as it turns edge-on */
    if (g->refl) {
        Uint8 ra = (Uint8)(alpha * (0.25f + 0.75f * SDL_fabsf(ca)));
        draw_spun_face(g->refl, !front, cx, rt + rh / 2, w, rh, ca, sa, 0, bright, bright, bright, ra);
    }
    /* thickness: cartridges and cards are chunky, discs thin */
    float thick = t->kind ? SDL_max(2.0f, w * 0.012f) : w * 0.12f;
    int layers = t->kind ? 2 : (int)SDL_clamp(thick / 2.5f, 3, 12);
    float zn = front ? -thick / 2 : thick / 2, zf = -zn;
    Uint8 dark = (Uint8)(bright * 0.42f);
    for (int i = 0; i < layers; i++) {
        float zp = zf + (zn - zf) * i / layers;
        draw_spun_face(face, !front, cx, cy, w, h, ca, sa, zp, dark, dark, (Uint8)SDL_min(255, dark + 4), alpha);
    }
    draw_spun_face(face, !front, cx, cy, w, h, ca, sa, zn, bright, bright, bright, alpha);
}

/* the right stick turns the selected game; another game starts facing front */
static int update_spin(float dt) {
    Game *cur = entry_game(g_sel_game);
    int changed = 0;
    if (cur != g_spin_game) {
        g_spin_game = cur;
        if (g_spin != 0) changed = 1;
        g_spin = 0;
    }
    int rx = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!gc) continue;
        int v = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        if (abs(v) > abs(rx)) rx = v;
    }
    if (abs(rx) > 7000) {
        float f = (abs(rx) - 7000) / (32767.0f - 7000);
        g_spin += (rx > 0 ? 1 : -1) * f * f * 300.0f * dt;   /* up to 300 degrees a second */
        g_spin = SDL_fmodf(g_spin, 360.0f);
        changed = 1;
    }
    return changed;
}

/* carousel slot look by distance from the centre (interpolated) */
static const float SLOT_SCALE[] = {1.0f, 0.80f, 0.66f, 0.56f, 0.48f, 0.42f, 0.38f};
static const float SLOT_X[] = {0, 400, 660, 870, 1040, 1175, 1280};
static const float SLOT_TURN[] = {0, 1, 1, 1, 1, 1, 1};
static const float SLOT_BRIGHT[] = {255, 205, 170, 140, 115, 95, 80};
static const float SLOT_ALPHA[] = {255, 255, 255, 240, 200, 120, 0};

static float slot_lerp(const float *tab, float d) {
    if (d >= 6) return tab[6];
    int i = (int)d;
    float f = d - i;
    return tab[i] * (1 - f) + tab[i + 1] * f;
}

#define FLOOR_Y 652
#define CAROUSEL_SCALE 1.04f

typedef struct {
    int i;
    float t;
} Slot;

static int slot_cmp(const void *a, const void *b) {
    float da = SDL_fabsf(((const Slot *)a)->t), db = SDL_fabsf(((const Slot *)b)->t);
    return da < db ? 1 : (da > db ? -1 : 0);
}

static void draw_spinner(float cx, float cy, float r) {
    Uint32 now = SDL_GetTicks();
    for (int k = 0; k < 8; k++) {
        float a = (now / 90 + k) % 8 * (float)M_PI / 4;
        float x = cx + SDL_cosf(a) * r, y = cy + SDL_sinf(a) * r;
        Uint8 al = (Uint8)(40 + k * 26);
        round_rect((int)(x - 4), (int)(y - 4), 8, 8, 4, 0xffffff, al);
    }
}

static void draw_carousel(Uint32 accent) {
    int n = entry_count();
    if (n <= 0) return;
    Slot slots[16];
    int ns = 0;
    int c = (int)SDL_floorf(g_pos + 0.5f);
    for (int i = c - 6; i <= c + 6; i++) {
        if (i < 0 || i >= n) continue;
        slots[ns++] = (Slot){i, i - g_pos};
    }
    qsort(slots, (size_t)ns, sizeof(Slot), slot_cmp);   /* far first */

    int budget = 2;   /* image builds per frame */
    g_builds_left = 0;
    for (int k = 0; k < ns; k++) {
        Game *g = entry_game(slots[k].i);
        if (!g) continue;
        float t = slots[k].t, d = SDL_fabsf(t);
        if (d > 5.6f) continue;
        if (!g->media) {
            if (budget > 0 || d < 0.5f) {
                build_media(g);
                budget--;
            } else {
                g_builds_left++;
            }
        } else if ((g->media_pending || g->extra_pending) && SDL_GetTicks() - g->cover_check > 1500) {
            char cover[1200];
            g->cover_check = SDL_GetTicks();
            int pend, rebuild = 0;
            if (g->media_pending && cover_of(g, cover, sizeof(cover)) != 0) rebuild = 1;
            if (g->extra_pending && extras_state(g, &pend) != g->extra_state) rebuild = 1;
            if (rebuild) {
                free_media(g);
                g_builds_left++;
            }
        }
        if (!g->media) continue;

        /* discs and cases are drawn a little smaller so every kind looks about as big */
        const MediaTpl *mt = &MEDIA_TPL[tpl_of(&g_sys[g->sys])];
        float kind_sc = mt->kind ? 0.86f : (mt->h > 360 ? 0.9f : 1.0f);
        float sc = slot_lerp(SLOT_SCALE, d) * CAROUSEL_SCALE * kind_sc;
        /* wide cartridges (N64, SNES) get a little more room between them */
        float spread = SDL_clamp(1.0f + (mt->w * kind_sc - 400.0f) / 400.0f * 1.4f, 1.0f, 1.2f);
        float x = 960 + (t < 0 ? -1 : 1) * slot_lerp(SLOT_X, d) * spread;
        float turn = slot_lerp(SLOT_TURN, d > 1 ? 1 : d);
        Uint8 bright = (Uint8)slot_lerp(SLOT_BRIGHT, d), alpha = (Uint8)slot_lerp(SLOT_ALPHA, d);
        float w = g->media_w * sc * (1.0f - 0.10f * turn), h = g->media_h * sc;
        float lift = (1.0f - SDL_min(1.0f, d)) * 10;
        float cy = FLOOR_Y - h / 2 - lift;
        float x0 = x - w / 2;

        /* shadow and, for the selected one, a glow in the system colour */
        if (T_shadow) {
            SDL_SetTextureAlphaMod(T_shadow, (Uint8)(alpha * 0.7f));
            SDL_Rect sd = {(int)(x - w * 0.6f), (int)(FLOOR_Y - 18), (int)(w * 1.2f), 40};
            SDL_RenderCopy(R, T_shadow, NULL, &sd);
        }
        if (d < 1 && T_glow) {
            set_color(T_glow, accent, (Uint8)(150 * (1 - d)));
            SDL_Rect gd = {(int)(x - w * 0.95f), (int)(cy - h * 0.85f), (int)(w * 1.9f), (int)(h * 1.7f)};
            SDL_RenderCopy(R, T_glow, NULL, &gd);
        }

        int strips = turn > 0.01f ? 12 : 1;
        float yt[17], yb[17], rt[17], rb[17];
        float k = 0.13f * turn;
        float rh = g->refl_h * sc;
        for (int j = 0; j <= strips; j++) {
            float u = (float)j / strips;
            float out = t < 0 ? 1 - u : u;    /* 1 at the outer edge */
            float hf = 1.0f - k * out;
            yt[j] = cy - h * hf / 2;
            yb[j] = cy + h * hf / 2;
            rt[j] = yb[j] + 3 + lift;
            rb[j] = rt[j] + rh * hf;
        }
        if (g == g_spin_game && g_spin != 0 && d < 0.5f) {
            draw_spun(g, x, cy, w, h, g_spin, cy + h / 2 + 3 + lift, rh, bright, alpha);
            continue;
        }
        if (g->refl) draw_turned(g->refl, x0, w, rt, rb, strips, bright, alpha);
        draw_turned(g->media, x0, w, yt, yb, strips, bright, alpha);

        if (g->media_pending && g_art_running && d < 3) {
            const MediaTpl *tp = &MEDIA_TPL[tpl_of(&g_sys[g->sys])];
            float lx = tp->kind ? tp->w / 2.0f : tp->lx + tp->lw / 2.0f;
            float ly = tp->kind ? tp->h * 0.70f : tp->ly + tp->lh * 0.82f;
            draw_spinner(x0 + lx * w / g->media_w, cy - h / 2 + ly * sc, 14 * sc);
        }
    }
}

static void draw_glass(int x, int y, int w, int h, int r, Uint8 a) {
    round_rect(x, y, w, h, r, 0x9aa3c8, (Uint8)(a / 4));
    round_rect(x + 1, y + 1, w - 2, h - 2, r - 1, 0x10131f, (Uint8)(a * 0.7f));
}

/* a pill: optional icon and a label; returns its width. measure only when !draw */
static int chip(int x, int cy, int icon, const char *label, int draw) {
    int h = 46, pad = 20, iw = icon >= 0 ? 30 : 0;
    int w = pad * 2 + iw + (int)text_width(&F_chip, label);
    if (draw) {
        draw_glass(x, cy - h / 2, w, h, h / 2, 230);
        if (icon >= 0) draw_icon(icon, x + pad - 2, cy - 12, 24, 0xdfe3f2, 255);
        draw_text(&F_chip, x + pad + iw, cy - F_chip.line / 2.0f, label, 0xe8ebf5, 255);
    }
    return w;
}

static void format_ago(long t, char *out, size_t cap) {
    if (!t) {
        scopy(out, cap, "Not played yet");
        return;
    }
    if (t == 1) {
        scopy(out, cap, "Played before");
        return;
    }
    long d = (long)time(NULL) - t;
    if (d < 0) d = 0;
    if (d < 90) scopy(out, cap, "Last played just now");
    else if (d < 3600) snprintf(out, cap, "Last played %ld minutes ago", d / 60);
    else if (d < 7200) scopy(out, cap, "Last played an hour ago");
    else if (d < 86400) snprintf(out, cap, "Last played %ld hours ago", d / 3600);
    else if (d < 2 * 86400) scopy(out, cap, "Last played yesterday");
    else if (d < 14 * 86400) snprintf(out, cap, "Last played %ld days ago", d / 86400);
    else if (d < 60 * 86400) snprintf(out, cap, "Last played %ld weeks ago", d / (7 * 86400));
    else snprintf(out, cap, "Last played %ld months ago", d / (30 * 86400));
}

static void draw_info(void) {
    Game *g = entry_game(g_sel_game);
    if (!g) return;
    System *s = &g_sys[g->sys];
    char title[200];
    ellipsize(&F_head, g->title, 1500, title, sizeof(title));
    float tw = text_width(&F_head, title);
    float tx = (SCREEN_W - tw) / 2;
    draw_text(&F_head, tx, 704, title, 0xffffff, 255);
    if (is_fav(g->path)) draw_icon(ICON_STAR, (int)(tx + tw + 18), 722, 36, 0xf2c14e, 255);

    char ago[80], emu[120], cnt[40];
    format_ago(recent_time_of(g->path), ago, sizeof(ago));
    if (s->emu < 0) scopy(emu, sizeof(emu), "No emulator found");
    else if (s->core_name[0]) snprintf(emu, sizeof(emu), "%s \xc2\xb7 %s", g_emus[s->emu].name, s->core_name);
    else scopy(emu, sizeof(emu), g_emus[s->emu].name);
    snprintf(cnt, sizeof(cnt), "%d %s", s->ngames, s->ngames == 1 ? "game" : "games");
    struct { int icon; const char *t; } chips[] = {
        {icon_of(s), s->name}, {ICON_CLOCK, ago}, {ICON_CHIP, emu}, {ICON_STACK, cnt}};
    int total = 0, gap = 14;
    for (int i = 0; i < 4; i++) total += chip(0, 0, chips[i].icon, chips[i].t, 0) + (i ? gap : 0);
    int x = (SCREEN_W - total) / 2, cy = 812;
    for (int i = 0; i < 4; i++) x += chip(x, cy, chips[i].icon, chips[i].t, 1) + gap;

    /* page dots */
    int n = entry_count();
    if (n > 1) {
        int dots = SDL_min(n, 15);
        int active = n <= 15 ? g_sel_game : (int)((float)g_sel_game * (dots - 1) / (n - 1) + 0.5f);
        int dw = 10, aw = 34, sp = 10;
        int wtot = (dots - 1) * (dw + sp) + aw;
        int dx = (SCREEN_W - wtot) / 2;
        for (int i = 0; i < dots; i++) {
            int w = i == active ? aw : dw;
            round_rect(dx, 868, w, 10, 5, i == active ? entry_accent(g_sel_entry) : 0x8a90a8, i == active ? 255 : 110);
            dx += w + sp;
        }
    }
}

static void draw_shoulder(int x, int cy, const char *label, int arrow_left) {
    int w = 76, h = 44;
    draw_glass(x, cy - h / 2, w, h, 14, 255);
    float tw = text_width(&F_chip, label);
    int tx = x + (w - (int)tw) / 2 + (arrow_left ? 7 : -7);
    draw_text(&F_chip, tx, cy - F_chip.line / 2.0f, label, 0xffffff, 255);
    /* small chevron */
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0xdf, 0xe3, 0xf2, 255);
    int ax = arrow_left ? x + 14 : x + w - 14;
    int dir = arrow_left ? 1 : -1;
    for (int t = 0; t < 2; t++) {
        SDL_RenderDrawLine(R, ax + t * dir, cy, ax + 6 * dir + t * dir, cy - 6);
        SDL_RenderDrawLine(R, ax + t * dir, cy, ax + 6 * dir + t * dir, cy + 6);
    }
}

#define TAB_Y 112
#define TAB_SIZE 76
#define TAB_STEP 96

static void draw_tabs(Uint32 accent) {
    draw_shoulder(40, TAB_Y + TAB_SIZE / 2, "L1", 1);
    draw_shoulder(SCREEN_W - 40 - 76, TAB_Y + TAB_SIZE / 2, "R1", 0);
    int left = 140, right = SCREEN_W - 140;
    int span = right - left;
    int total = g_nentries * TAB_STEP;
    /* keep the selected tab in view, centred when the row is wider than the space */
    if (total <= span) g_tab_target = -(float)(span - total) / 2;
    else {
        float want = g_sel_entry * TAB_STEP + TAB_STEP / 2.0f - span / 2.0f;
        g_tab_target = SDL_max(0.0f, SDL_min((float)(total - span), want));
    }
    SDL_Rect clip = {left - 10, TAB_Y - 20, span + 20, TAB_SIZE + 64};
    SDL_RenderSetClipRect(R, &clip);
    for (int i = 0; i < g_nentries; i++) {
        float cx = left + i * TAB_STEP + TAB_STEP / 2.0f - g_tab_scroll;
        if (cx < left - TAB_STEP || cx > right + TAB_STEP) continue;
        Entry *e = &g_entries[i];
        int sel = i == g_sel_entry;
        int icon = e->kind == 0 ? ICON_CLOCK : e->kind == 2 ? ICON_STAR : icon_of(&g_sys[e->sys]);
        const char *lab = e->kind == 0 ? "Recent" : e->kind == 2 ? "Favorites" : tab_label(&g_sys[e->sys]);
        /* fade toward the ends of the row */
        float dist = SDL_fabsf(cx - (left + span / 2.0f)) / (span / 2.0f);
        Uint8 a = sel ? 255 : (Uint8)(235 - SDL_min(1.0f, dist) * 90);
        int sz = TAB_SIZE;
        int x = (int)(cx - sz / 2.0f), y = TAB_Y;
        if (sel) {
            if (T_glow) {
                set_color(T_glow, accent, 170);
                SDL_Rect gd = {x - 40, y - 40, sz + 80, sz + 80};
                SDL_RenderCopy(R, T_glow, NULL, &gd);
            }
            round_rect(x - 4, y - 4, sz + 8, sz + 8, 22, 0xffffff, 60);
            round_rect(x, y, sz, sz, 19, accent, 255);
            draw_icon(icon, x + 15, y + 9, 46, 0xffffff, 255);
            float tw = text_width(&F_tiny, lab);
            if (tw > sz - 6) {
                /* long label: under the tab */
                draw_text(&F_tiny, cx - tw / 2, y + sz + 10, lab, 0xffffff, 255);
            } else {
                draw_text(&F_tiny, cx - tw / 2, y + sz - F_tiny.line - 4, lab, 0xffffff, 255);
            }
        } else {
            draw_glass(x + 6, y + 6, sz - 12, sz - 12, 16, (Uint8)(a * 0.8f));
            draw_icon(icon, x + 20, y + 16, 36, 0xd7dbea, a);
            float tw = text_width(&F_tiny, lab);
            draw_text(&F_tiny, cx - tw / 2, y + sz + 6, lab, 0xaeb4c8, a);
        }
    }
    SDL_RenderSetClipRect(R, NULL);
}

static void clock_text(char *out, size_t cap) {
    time_t now = time(NULL), local = now;
#ifdef GS_NATIVE
    int sceKernelConvertUtcToLocaltime(time_t, time_t *, void *, int *);
    int dst = 0;
    char tz[16];
    if (sceKernelConvertUtcToLocaltime(now, &local, tz, &dst) != 0) local = now;
    long day = (long)(local % 86400);
    if (day < 0) day += 86400;
    int hour = (int)(day / 3600), min = (int)(day / 60 % 60);
#else
    struct tm tmv;
    localtime_r(&local, &tmv);
    int hour = tmv.tm_hour, min = tmv.tm_min;
#endif
    int h = hour % 12;
    snprintf(out, cap, "%d:%02d %s", h ? h : 12, min, hour < 12 ? "AM" : "PM");
}

static void draw_header(void) {
    if (T_logo) {
        SDL_Rect d = {40, 26, 60, 60};
        SDL_RenderCopy(R, T_logo, NULL, &d);
    }
    draw_text(&F_title, 116, 26, "Jewel Case", 0xffffff, 255);

    char clk[32];
    clock_text(clk, sizeof(clk));
    float cw = text_width(&F_chip, clk);
    int x = SCREEN_W - 44 - (int)cw;
    draw_text(&F_chip, x, 56 - F_chip.line / 2.0f, clk, 0xffffff, 255);
    x -= 52;
    draw_icon(ICON_PAD, x, 56 - 18, 36, 0xffffff, 230);
    if (g_art_running && g_art_total > 0) {
        char t[64];
        snprintf(t, sizeof(t), "Covers %d/%d", g_art_done, g_art_total);
        int w = chip(0, 0, ICON_DOWNLOAD, t, 0);
        x -= w + 20;
        chip(x, 56, ICON_DOWNLOAD, t, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* Details screen: one game, large, with what Jewel Case knows about it      */
/* ------------------------------------------------------------------------ */

static int g_details;           /* open */
static float g_det_t;           /* 0..1 opening animation */
static int g_det_game = -1;     /* which game the animation is for */

static void fmt_size(long long n, char *out, size_t cap) {
    if (n >= (1LL << 30)) snprintf(out, cap, "%.2f GB", n / 1073741824.0);
    else if (n >= (1LL << 20)) snprintf(out, cap, "%.1f MB", n / 1048576.0);
    else snprintf(out, cap, "%lld KB", (n + 1023) / 1024);
}

/* "(USA)" / "(Europe) (En,Fr)" in the file name: the first region-like tag */
static void region_of(const char *stem, char *out, size_t cap) {
    out[0] = 0;
    static const char *known[] = {"USA", "Europe", "Japan", "World", "Korea", "Asia", "Australia", "Brazil",
                                  "Canada", "China", "France", "Germany", "Italy", "Spain", "UK", "NTSC", "PAL"};
    const char *p = stem;
    while ((p = strchr(p, '('))) {
        const char *q = strchr(p, ')');
        if (!q) return;
        char tag[96];
        size_t n = (size_t)(q - p - 1);
        if (n < sizeof(tag)) {
            memcpy(tag, p + 1, n);
            tag[n] = 0;
            for (size_t k = 0; k < sizeof(known) / sizeof(*known); k++)
                if (strstr(tag, known[k])) {
                    scopy(out, cap, tag);
                    return;
                }
        }
        p = q + 1;
    }
}

static float ease(float t) { return t < 0 ? 0 : t > 1 ? 1 : 1 - (1 - t) * (1 - t) * (1 - t); }

/* the game's image, big, turned slightly toward the text, standing on the floor */
/* The details screen's extras for the game it shows: its info from LaunchBox,
 * and its screenshot and title screen from libretro, faded in behind it. */
typedef struct {
    Game *g;
    int have_info;
    char name[160], year[8], genre[160], dev[160], pub[160], players[16];
    char overview[1700];
    SDL_Texture *shot[2];
    int nshots;
    int state;          /* which of the three files exist, to notice new ones */
    Uint32 checked;
} DetExtras;

static DetExtras g_dx;

static int det_state(Game *g) {
    char p[1200], b[1100];
    int st = 0;
    for (int k = 0; k < 3; k++) {
        info_base(g, k, b, sizeof(b));
        static const char *const ext[3][2] = {{".txt", ".txt"}, {".png", ".jpg"}, {".png", ".jpg"}};
        for (int e = 0; e < 2; e++) {
            snprintf(p, sizeof(p), "%s%s", b, ext[k][e]);
            if (file_exists(p)) {
                st |= 1 << k;
                break;
            }
        }
    }
    return st;
}

static void det_free(void) {
    for (int i = 0; i < 2; i++)
        if (g_dx.shot[i]) SDL_DestroyTexture(g_dx.shot[i]);
    memset(&g_dx, 0, sizeof(g_dx));
}

static void det_load(Game *g) {
    det_free();
    g_dx.g = g;
    g_dx.state = det_state(g);
    g_dx.checked = SDL_GetTicks();
    char b[1100], p[1200];
    info_base(g, 0, b, sizeof(b));
    snprintf(p, sizeof(p), "%s.txt", b);
    char *txt = read_file(p, NULL);
    if (txt) {
        char *body = strstr(txt, "\n\n");
        if (body) {
            *body = 0;
            body += 2;
            body[strcspn(body, "\r\n")] = 0;
            scopy(g_dx.overview, sizeof(g_dx.overview), body);
        }
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            char *v = strstr(ln, ": ");
            if (!v) continue;
            *v = 0;
            v += 2;
            if (!strcmp(ln, "Name")) scopy(g_dx.name, sizeof(g_dx.name), v);
            else if (!strcmp(ln, "Year")) scopy(g_dx.year, sizeof(g_dx.year), v);
            else if (!strcmp(ln, "Genre")) scopy(g_dx.genre, sizeof(g_dx.genre), v);
            else if (!strcmp(ln, "Developer")) scopy(g_dx.dev, sizeof(g_dx.dev), v);
            else if (!strcmp(ln, "Publisher")) scopy(g_dx.pub, sizeof(g_dx.pub), v);
            else if (!strcmp(ln, "Players")) scopy(g_dx.players, sizeof(g_dx.players), v);
        }
        free(txt);
        g_dx.have_info = 1;
    }
    /* the title screen, then the in-game screenshot */
    for (int k = 2; k >= 1; k--) {
        info_base(g, k, b, sizeof(b));
        static const char *const ext[] = {".png", ".jpg"};
        for (int e = 0; e < 2; e++) {
            snprintf(p, sizeof(p), "%s%s", b, ext[e]);
            if (!file_exists(p)) continue;
            SDL_Surface *sf = load_art_surface(p, 960, 720);
            if (sf) {
                SDL_Texture *tx = SDL_CreateTextureFromSurface(R, sf);
                SDL_FreeSurface(sf);
                if (tx) {
                    SDL_SetTextureBlendMode(tx, SDL_BLENDMODE_BLEND);
                    g_dx.shot[g_dx.nshots++] = tx;
                }
            }
            break;
        }
    }
}

/* the screenshots, full screen behind everything, dimmed; one fades into the next */
static void draw_det_backdrop(Uint8 a) {
    if (!g_dx.nshots) return;
    const Uint32 HOLD = 6000, FADE = 1200;
    Uint32 now = SDL_GetTicks();
    int cur = (int)((now / HOLD) % (Uint32)g_dx.nshots), nxt = (cur + 1) % g_dx.nshots;
    Uint32 in = now % HOLD;
    float f = g_dx.nshots > 1 && in > HOLD - FADE ? (float)(in - (HOLD - FADE)) / FADE : 0;
    for (int k = 0; k < 2; k++) {
        int i = k ? nxt : cur;
        float al = k ? f : 1;
        if (al <= 0 || !g_dx.shot[i]) continue;
        int w, h;
        SDL_QueryTexture(g_dx.shot[i], NULL, NULL, &w, &h);
        float sc = SDL_max((float)SCREEN_W / w, (float)SCREEN_H / h);
        SDL_Rect d = {(int)(SCREEN_W - w * sc) / 2, (int)(SCREEN_H - h * sc) / 2, (int)(w * sc), (int)(h * sc)};
        SDL_SetTextureColorMod(g_dx.shot[i], 120, 120, 132);
        SDL_SetTextureAlphaMod(g_dx.shot[i], (Uint8)(a * 0.42f * al));
        SDL_RenderCopy(R, g_dx.shot[i], NULL, &d);
    }
    /* darker on the left, where the text is */
    for (int x = 0; x < 1100; x += 20)
        fill_rect(x, 0, 20, SCREEN_H, 0x06070c, (Uint8)(a * 0.55f * (1.0f - x / 1100.0f)));
}

static void draw_details_media(Game *g, Uint32 accent, float t) {
    if (!g->media) build_media(g);
    if (!g->media) return;
    const MediaTpl *mt = &MEDIA_TPL[tpl_of(&g_sys[g->sys])];
    float kind_sc = mt->kind ? 0.86f : (mt->h > 360 ? 0.9f : 1.0f);
    float big = SDL_min(1.62f, 560.0f / (g->media_h * kind_sc)) * kind_sc;
    float small = CAROUSEL_SCALE * kind_sc;
    float sc = small + (big - small) * t;
    float cx = 960 + (1390 - 960) * t;
    float w = g->media_w * sc * (1.0f - 0.07f * t), h = g->media_h * sc;
    float floor_y = FLOOR_Y + 40 * t;
    float cy = floor_y - h / 2 - 10;
    float x0 = cx - w / 2;

    if (T_shadow) {
        SDL_SetTextureAlphaMod(T_shadow, 190);
        SDL_Rect sd = {(int)(cx - w * 0.6f), (int)(floor_y - 22), (int)(w * 1.2f), 48};
        SDL_RenderCopy(R, T_shadow, NULL, &sd);
    }
    if (T_glow) {
        set_color(T_glow, accent, 170);
        SDL_Rect gd = {(int)(cx - w * 1.0f), (int)(cy - h * 0.9f), (int)(w * 2.0f), (int)(h * 1.8f)};
        SDL_RenderCopy(R, T_glow, NULL, &gd);
    }
    /* turned toward the left: the left edge is the near one */
    int strips = 12;
    float yt[17], yb[17], rt[17], rb[17];
    float k = 0.08f * t, rh = g->refl_h * sc;
    for (int j = 0; j <= strips; j++) {
        float u = (float)j / strips;
        float hf = 1.0f - k * u;
        yt[j] = cy - h * hf / 2;
        yb[j] = cy + h * hf / 2;
        rt[j] = yb[j] + 13;
        rb[j] = rt[j] + rh * hf;
    }
    if (g == g_spin_game && g_spin != 0) {
        draw_spun(g, cx, cy, w, h, g_spin, cy + h / 2 + 13, rh, 255, 255);
        return;
    }
    if (g->refl) draw_turned(g->refl, x0, w, rt, rb, strips, 255, 255);
    draw_turned(g->media, x0, w, yt, yb, strips, 255, 255);
}

static void draw_details(Uint32 accent) {
    Game *g = entry_game(g_sel_game);
    if (!g) {
        g_details = 0;
        return;
    }
    if (g_det_game != g_sel_game) g_det_game = g_sel_game;
    if (g_dx.g != g) det_load(g);
    else if (SDL_GetTicks() - g_dx.checked > 2000) {
        /* still downloading: show what arrives */
        g_dx.checked = SDL_GetTicks();
        if (det_state(g) != g_dx.state) det_load(g);
    }
    float t = ease(g_det_t);
    Uint8 a = (Uint8)(255 * SDL_min(1.0f, g_det_t * 1.6f));
    draw_det_backdrop(a);
    draw_details_media(g, accent, t);

    if (a < 8) return;
    System *s = &g_sys[g->sys];
    int x = 120 - (int)(40 * (1 - t)), y = 236;

    /* system line */
    draw_icon(icon_of(s), x, y + 2, 30, accent_of(s->color), a);
    char sysline[120];
    snprintf(sysline, sizeof(sysline), "%s", s->name);
    for (char *c = sysline; *c; c++) *c = (char)toupper((unsigned char)*c);
    draw_text(&F_chip, x + 42, y + 2, sysline, 0xc9cee0, a);
    y += 56;

    /* title, up to two lines */
    char l1[200], l2[200];
    int lines = wrap2(&F_head, g->title, 900, l1, l2);
    draw_text(&F_head, x, y, l1, 0xffffff, a);
    if (lines > 1) draw_text(&F_head, x, y + F_head.line - 6, l2, 0xffffff, a);
    if (is_fav(g->path)) {
        float tw = text_width(&F_head, lines > 1 ? l2 : l1);
        draw_icon(ICON_STAR, (int)(x + tw + 16), y + (lines > 1 ? F_head.line - 6 : 0) + 18, 36, 0xf2c14e, a);
    }
    y += lines * (F_head.line - 6) + 22;

    /* chips */
    char ago[80], emu[120], cnt[40];
    format_ago(recent_time_of(g->path), ago, sizeof(ago));
    if (s->emu < 0) scopy(emu, sizeof(emu), "No emulator found");
    else if (s->core_name[0]) snprintf(emu, sizeof(emu), "%s \xc2\xb7 %s", g_emus[s->emu].name, s->core_name);
    else scopy(emu, sizeof(emu), g_emus[s->emu].name);
    snprintf(cnt, sizeof(cnt), "%d of %d", g_sel_game + 1, entry_count());
    int cx = x, cyy = y + 23;
    struct { int icon; const char *t; } chips[] = {{ICON_CLOCK, ago}, {ICON_CHIP, emu}, {ICON_STACK, cnt}};
    for (int i = 0; i < 3; i++) {
        int w = chip(0, 0, chips[i].icon, chips[i].t, 0);
        if (cx + w > x + 900) {
            cx = x;
            cyy += 58;
        }
        chip(cx, cyy, chips[i].icon, chips[i].t, 1);
        cx += w + 12;
    }
    y = cyy + 52;

    if (g_dx.have_info && (g_dx.overview[0] || g_dx.year[0] || g_dx.genre[0])) {
        /* what the game is: year, genre, who made it, players, and its story */
        char meta[400] = "";
        const char *parts[4];
        int np = 0;
        char who[330] = "", pl[40] = "";
        if (g_dx.dev[0] && g_dx.pub[0] && strcmp(g_dx.dev, g_dx.pub))
            snprintf(who, sizeof(who), "%s / %s", g_dx.dev, g_dx.pub);
        else scopy(who, sizeof(who), g_dx.dev[0] ? g_dx.dev : g_dx.pub);
        if (g_dx.players[0] && strcmp(g_dx.players, "0"))
            snprintf(pl, sizeof(pl), "%s player%s", g_dx.players, strcmp(g_dx.players, "1") ? "s" : "");
        if (g_dx.year[0]) parts[np++] = g_dx.year;
        if (g_dx.genre[0]) parts[np++] = g_dx.genre;
        if (who[0]) parts[np++] = who;
        if (pl[0]) parts[np++] = pl;
        for (int i = 0; i < np; i++) {
            if (i) strncat(meta, "  \xc2\xb7  ", sizeof(meta) - strlen(meta) - 1);
            strncat(meta, parts[i], sizeof(meta) - strlen(meta) - 1);
        }
        char mline[300];
        ellipsize(&F_chip, meta, 900, mline, sizeof(mline));
        draw_text(&F_chip, x, y, mline, 0xc9cee0, a);
        y += 46;
        if (g_dx.overview[0]) {
            char ln[6][200];
            int n = wrap_lines(&F_small, g_dx.overview, 860, ln, 6);
            draw_glass(x - 4, y, 900, n * (F_small.line + 4) + 28, 18, (Uint8)(a * 0.7f));
            for (int i = 0; i < n; i++)
                draw_text(&F_small, x + 14, y + 14 + i * (F_small.line + 4), ln[i], 0xe3e6f0, a);
            y += n * (F_small.line + 4) + 28 + 34;
        }
        goto play_button;
    }

    /* facts */
    char region[100], size[40] = "", cover[1200], cover_txt[120];
    region_of(g->stem, region, sizeof(region));
    {
        char rp[1200];
        real_path(g->path, rp, sizeof(rp));
        struct stat st;
        if (stat(rp, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) fmt_size((long long)st.st_size, size, sizeof(size));
    }
    int cst = cover_of(g, cover, sizeof(cover));
    if (g->art[0]) scopy(cover_txt, sizeof(cover_txt), "Your own art");
    else if (cst == 1) scopy(cover_txt, sizeof(cover_txt), "Downloaded (libretro thumbnails)");
    else if (cst == 0) scopy(cover_txt, sizeof(cover_txt), g_art_running ? "Downloading..." : "Not downloaded yet");
    else scopy(cover_txt, sizeof(cover_txt), "None found");
    const char *base = strrchr(g->path, '/');
    base = base ? base + 1 : g->path;
    struct { const char *k; const char *v; } facts[] = {
        {"File", base}, {"Folder", g->dir}, {"Region", region}, {"Size", size}, {"Cover", cover_txt}};
    draw_glass(x - 4, y, 900, 0, 14, 0);
    int fy = y + 8;
    for (int i = 0; i < 5; i++) {
        if (!facts[i].v[0]) continue;
        char v[300];
        ellipsize(&F_chip, facts[i].v, 760, v, sizeof(v));
        draw_text(&F_chip, x, fy, facts[i].k, 0x8d95ad, a);
        draw_text(&F_chip, x + 120, fy, v, 0xe3e6f0, a);
        fy += 40;
    }
    y = fy + 34;

play_button:
    /* Play button */
    int bw = 250, bh = 66;
    if (T_glow) {
        set_color(T_glow, accent, (Uint8)(a * 0.45f));
        SDL_Rect gd = {x - 40, y - 40, bw + 80, bh + 80};
        SDL_RenderCopy(R, T_glow, NULL, &gd);
    }
    round_rect(x, y, bw, bh, bh / 2, accent, a);
    draw_button(x + 44, y + bh / 2, 'x');
    draw_text(&F_body, x + 78, y + (bh - F_body.line) / 2.0f, "Play", 0xffffff, a);
    int fx = x + bw + 18;
    const char *ft = is_fav(g->path) ? "Unfavorite" : "Favorite";
    int fw = 64 + (int)text_width(&F_chip, ft) + 26;
    draw_glass(fx, y, fw, bh, bh / 2, a);
    draw_button(fx + 36, y + bh / 2, 't');
    draw_text(&F_chip, fx + 64, y + (bh - F_chip.line) / 2.0f, ft, 0xffffff, a);
}

static void draw_hints(void) {
    int y = 1012;
    int x = 52;
    struct { char b; const char *t; } hints[] = {
        {'x', "Play"}, {'o', "Close"}, {'t', "Favorite"}, {'s', "Details"}};
    if (g_details) {
        hints[1].t = "Back";
        hints[3].t = "File info";
    }
    for (int i = 0; i < 4; i++) {
        draw_button(x + 17, y, hints[i].b);
        draw_text(&F_small, x + 44, y - F_small.line / 2.0f, hints[i].t, 0xcfd5e6, 255);
        x += 44 + (int)text_width(&F_small, hints[i].t) + 40;
    }
    if (g_details) {
        draw_pill_label(x, y, "<  >");
        x += (int)text_width(&F_small, "<  >") + 28 + 12;
        draw_text(&F_small, x, y - F_small.line / 2.0f, "Browse", 0xcfd5e6, 255);
    } else {
        draw_pill_label(x, y, "L1");
        x += (int)text_width(&F_small, "L1") + 28 + 8;
        draw_text(&F_small, x, y - F_small.line / 2.0f, "/", 0x8d95ad, 255);
        x += 20;
        draw_pill_label(x, y, "R1");
        x += (int)text_width(&F_small, "R1") + 28 + 12;
        draw_text(&F_small, x, y - F_small.line / 2.0f, "Systems", 0xcfd5e6, 255);
    }

    /* settings: the controller's Options button */
    int bx = SCREEN_W - 52 - 56;
    draw_glass(bx, y - 28, 56, 56, 28, 255);
    draw_icon(ICON_GEAR, bx + 14, y - 14, 28, 0xffffff, 255);
    const char *st = "Settings";
    float sw = text_width(&F_small, st);
    draw_text(&F_small, bx - 16 - sw, y - F_small.line / 2.0f, st, 0xcfd5e6, 255);
    /* three-line glyph of the Options button */
    int gx = bx - 16 - (int)sw - 44;
    round_rect(gx, y - 15, 30, 30, 8, 0x2c3145, 255);
    for (int i = 0; i < 3; i++) fill_rect(gx + 8, y - 7 + i * 6, 14, 2, 0xcfd5e6, 255);

    if (g_status_msg[0] && SDL_GetTicks() < g_status_until) {
        float w = text_width(&F_small, g_status_msg);
        draw_text(&F_small, (SCREEN_W - w) / 2, 940, g_status_msg, 0xffc857, 255);
    }
}

/* split text into display lines that fit max_w (breaks on spaces, or anywhere for long paths) */
static int layout_lines(Font *f, const char *text, float max_w, char out[][300], int max_lines) {
    int n = 0;
    const char *p = text;
    while (*p && n < max_lines) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char para[2048];
        if (len >= sizeof(para)) len = sizeof(para) - 1;
        memcpy(para, p, len);
        para[len] = 0;
        int indent = (para[0] == ' ' && para[1] == ' ');
        const char *q = para;
        if (!*q) scopy(out[n++], 300, "");
        while (*q && n < max_lines) {
            size_t best = 0, last_space = 0, i = 0;
            char tmp[300];
            const char *r = q;
            while (*r && i < 290) {
                const char *st = r;
                utf8_next(&r);
                size_t cl = (size_t)(r - st);
                memcpy(tmp + i, st, cl);
                tmp[i + cl] = 0;
                char probe[310];
                snprintf(probe, sizeof(probe), "%s%s", (indent && q != para) ? "  " : "", tmp);
                if (text_width(f, probe) > max_w) break;
                i += cl;
                best = i;
                if (*st == ' ') last_space = i;
            }
            if (*r && *(q + best) && last_space > 0 && last_space > best / 2) best = last_space;
            if (best == 0) best = 1;
            char line[300];
            size_t cut = best < 290 ? best : 290;
            memcpy(line, q, cut);
            line[cut] = 0;
            snprintf(out[n++], 300, "%s%s", (indent && q != para) ? "  " : "", line);
            q += cut;
            if (q != para + len) while (*q == ' ' && !indent) q++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

static void draw_modal(void) {
    fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x05060a, 170);
    static char lines[40][300];
    int w = 1240, pad = 56;
    int nl = layout_lines(&F_body, g_modal_text, w - 2 * pad, lines, 18);
    int h = pad + F_title.line + 24 + nl * (F_body.line + 4) + 40 + 40 + pad;
    if (h > 980) h = 980;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    round_rect(x - 1, y - 1, w + 2, h + 2, 29, 0x9aa3c8, 60);
    round_rect(x, y, w, h, 28, 0x161a28, 250);
    char title[160];
    ellipsize(&F_title, g_modal_title, w - 2 * pad, title, sizeof(title));
    draw_text(&F_title, x + pad, y + pad - 8, title, 0xffffff, 255);
    int ly = y + pad + F_title.line + 16;
    for (int i = 0; i < nl; i++) {
        Uint32 col = (lines[i][0] == ' ' && lines[i][1] == ' ') ? 0x9aa3bb : 0xd5dae8;
        draw_text(&F_body, x + pad, ly, lines[i], col, 255);
        ly += F_body.line + 4;
        if (ly > y + h - 110) break;
    }
    draw_button(x + pad + 17, y + h - pad - 4, 'o');
    draw_text(&F_small, x + pad + 46, y + h - pad - 4 - F_small.line / 2.0f, "Close", 0xcfd5e6, 255);
}

static void draw_launching(const char *title) {
    fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x05060a, 200);
    const char *a = "Starting";
    float w1 = text_width(&F_body, a);
    draw_text(&F_body, (SCREEN_W - w1) / 2, 470, a, 0x8d95ad, 255);
    char cut[200];
    ellipsize(&F_huge, title, 1500, cut, sizeof(cut));
    float w2 = text_width(&F_huge, cut);
    draw_text(&F_huge, (SCREEN_W - w2) / 2, 520, cut, 0xffffff, 255);
}

/* an empty clear jewel case with a message */
static void draw_empty_case(const char *line1, const char *line2) {
    int w = 420, h = 360, x = (SCREEN_W - w) / 2, y = 300;
    if (T_glow) {
        set_color(T_glow, 0x2f8fd8, 90);
        SDL_Rect gd = {x - 200, y - 160, w + 400, h + 320};
        SDL_RenderCopy(R, T_glow, NULL, &gd);
    }
    round_rect(x, y, w, h, 14, 0x9fd4ff, 70);
    round_rect(x + 3, y + 3, w - 6, h - 6, 12, 0x0b1626, 210);
    round_rect(x + 22, y + 22, w - 44, h - 44, 8, 0x9fd4ff, 30);
    round_rect(x + 24, y + 24, w - 48, h - 48, 7, 0x0d1a2c, 220);
    fill_rect(x + 10, y + 8, 6, h - 16, 0x9fd4ff, 60);
    draw_icon(ICON_PAD, SCREEN_W / 2 - 34, y + 92, 68, 0xdff1ff, 255);
    float w1 = text_width(&F_body, line1);
    draw_text(&F_body, (SCREEN_W - w1) / 2, y + 186, line1, 0xffffff, 255);
    if (line2 && line2[0]) {
        char cut[200];
        ellipsize(&F_small, line2, w - 60, cut, sizeof(cut));
        float w2 = text_width(&F_small, cut);
        draw_text(&F_small, (SCREEN_W - w2) / 2, y + 236, cut, 0xa9c6e2, 255);
    }
}

static void draw_no_games(void) {
    {
        char where[120];
        snprintf(where, sizeof(where), "add games to %s/<system>", g_rom_folder);
        draw_empty_case("No games found", where);
    }
    int y = 720;
    const char *t = "Put your games in a folder per system, for example:";
    float w = text_width(&F_body, t);
    draw_text(&F_body, (SCREEN_W - w) / 2, y, t, 0xb9c0d4, 255);
    y += 56;
    const char *ex[] = {"/data/roms/snes/Super Mario World.sfc", "/mnt/usb0/roms/psx/Final Fantasy VII.m3u",
                        "/mnt/usb0/roms/ps2/God of War.iso"};
    for (int i = 0; i < 3; i++, y += 46) {
        float ew = text_width(&F_body, ex[i]);
        draw_text(&F_body, (SCREEN_W - ew) / 2, y, ex[i], 0x8f9bff, 255);
    }
}

static int g_settings;          /* the settings screen is open */
static void draw_settings(void);

/* ---- rain: streaks falling behind the carousel, ripples on the floor ---- */
static int g_rain_on = 1;
static int g_music_on = 1;
static int g_music_volume = 60;
#define RAIN_FAR 130
#define RAIN_NEAR 60
#define RIPPLES 40
typedef struct { float x, y, len, speed, alpha, end_y; int near, land; } Drop;
typedef struct { float x, y, age; int live; } Ripple;
static Drop g_drops[RAIN_FAR + RAIN_NEAR];
static Ripple g_ripples[RIPPLES];
static Uint32 g_rain_last;
static unsigned g_rng = 12345u;
static float frand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (g_rng >> 8) / 16777216.0f;
}
static void drop_reset(Drop *d, int spread) {
    d->near = d >= g_drops + RAIN_FAR;
    d->x = frand() * (SCREEN_W + 300) - 150;
    d->y = spread ? frand() * SCREEN_H : -60 - frand() * 300;
    d->len = d->near ? 34 + frand() * 30 : 14 + frand() * 18;
    d->speed = d->near ? 1350 + frand() * 450 : 750 + frand() * 350;
    d->alpha = d->near ? 70 + frand() * 60 : 28 + frand() * 34;
    /* where it ends: near drops somewhere on the floor (with a ripple), far
     * drops at the floor's far edge (they fall behind it); drops that miss the
     * floor fall out of view */
    d->end_y = SCREEN_H + 60;
    float y0 = d->y, x = d->x;
    float guess = FLOOR_CY;
    for (int it = 0; it < 3; it++) {
        float land_x = x + (guess - y0) * 0.16f;   /* drops drift right as they fall */
        float top, bot;
        if (!floor_span(land_x, &top, &bot)) {
            d->end_y = SCREEN_H + 60;
            break;
        }
        float want = d->near ? top + 6 + (bot - top - 12) * (frand() * 0.92f) : top + 2;
        if (bot > SCREEN_H - 120 && d->near) want = SDL_min(want, (float)SCREEN_H - 120);
        d->end_y = want;
        guess = want;
    }
    d->land = d->end_y < SCREEN_H;
}
static void update_rain(void) {
    Uint32 now = SDL_GetTicks();
    if (!g_rain_last) {
        for (int i = 0; i < RAIN_FAR + RAIN_NEAR; i++) drop_reset(&g_drops[i], 1);
        g_rain_last = now;
        return;
    }
    float dt = (now - g_rain_last) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_rain_last = now;
    for (int i = 0; i < RAIN_FAR + RAIN_NEAR; i++) {
        Drop *d = &g_drops[i];
        d->y += d->speed * dt;
        d->x += d->speed * dt * 0.16f;
        if (d->y >= d->end_y) {
            if (d->near && d->land) {
                for (int k = 0; k < RIPPLES; k++)
                    if (!g_ripples[k].live) {
                        g_ripples[k] = (Ripple){d->x, d->end_y, 0, 1};
                        break;
                    }
            }
            drop_reset(d, 0);
        }
    }
    for (int k = 0; k < RIPPLES; k++) {
        if (!g_ripples[k].live) continue;
        g_ripples[k].age += dt;
        if (g_ripples[k].age > 0.55f) g_ripples[k].live = 0;
    }
}
static void draw_rain(void) {
    update_rain();
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < RAIN_FAR + RAIN_NEAR; i++) {
        Drop *d = &g_drops[i];
        float x1 = d->x - d->len * 0.16f, y1 = d->y - d->len;
        if (y1 >= d->end_y) continue;
        SDL_SetRenderDrawColor(R, 190, 205, 255, (Uint8)d->alpha);
        SDL_RenderDrawLineF(R, x1, y1, d->x, d->y);
        if (d->near) {
            SDL_SetRenderDrawColor(R, 190, 205, 255, (Uint8)(d->alpha * 0.45f));
            SDL_RenderDrawLineF(R, x1 + 1, y1, d->x + 1, d->y);
        }
    }
    /* ripples: flat ellipses that widen and fade */
    for (int k = 0; k < RIPPLES; k++) {
        Ripple *r = &g_ripples[k];
        if (!r->live) continue;
        float t = r->age / 0.55f;
        float top, bot, depth = 0.5f;
        if (floor_span(r->x, &top, &bot) && bot > top) depth = (r->y - top) / (bot - top);
        float rx = (3 + t * 22) * (0.6f + depth * 0.9f), ry = rx * (0.16f + depth * 0.12f);
        SDL_SetRenderDrawColor(R, 200, 214, 255, (Uint8)(110 * (1 - t)));
        SDL_FPoint pts[17];
        for (int j = 0; j <= 16; j++) {
            float a = j * (float)M_PI * 2 / 16;
            pts[j] = (SDL_FPoint){r->x + SDL_cosf(a) * rx, r->y + SDL_sinf(a) * ry};
        }
        SDL_RenderDrawLinesF(R, pts, 17);
    }
}

/* the MP3s in /data/jewelcase/music (or one music.mp3 next to Jewel Case) */
static void start_music(void) {
    if (!g_music_on || music_playing()) return;
    static char tracks[64][1024];
    int n = 0;
    DirEnt *ents = NULL;
    int ne = list_dir("/data/jewelcase/music", &ents);
    for (int i = 0; i < ne && n < 64; i++) {
        if (ents[i].dir || strcasecmp(ext_of(ents[i].name), "mp3")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "/data/jewelcase/music/%s", ents[i].name);
        real_path(p, tracks[n], sizeof(tracks[n]));
        n++;
    }
    free(ents);
    char one[700];
    snprintf(one, sizeof(one), "%s/music.mp3", g_appdir);
    if (!n && file_exists(one)) real_path(one, tracks[n++], sizeof(tracks[0]));
    if (!n && file_exists("/data/jewelcase/music.mp3")) real_path("/data/jewelcase/music.mp3", tracks[n++], sizeof(tracks[0]));
    if (!n) return;
    music_set_volume(g_music_volume);
    music_start(tracks, n);
}


static void render(void) {
    Uint32 accent = entry_accent(g_sel_entry);
    make_background_for(g_nentries ? (g_entries[g_sel_entry].kind == 1 ? g_sys[g_entries[g_sel_entry].sys].color : accent) : 0x2f8fd8);
    if (T_bg) SDL_RenderCopy(R, T_bg, NULL, NULL);
    else fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x12141f, 255);
    if (g_rain_on) draw_rain();
    draw_header();
    if (g_nentries == 0) {
        draw_no_games();
    } else {
        if (g_details && entry_count() > 0) {
            draw_details(accent);
        } else {
        draw_tabs(accent);
        if (entry_count() == 0) {
            char where[200];
            Entry *e = &g_entries[g_sel_entry];
            if (e->kind == 1) snprintf(where, sizeof(where), "add games to %s/%s", g_rom_folder, g_sys[e->sys].folders[0]);
            else where[0] = 0;
            draw_empty_case("No games found", where);
        } else {
            draw_carousel(accent);
            draw_info();
        }
        }
    }
    draw_hints();
    if (g_settings) draw_settings();
    if (g_modal) draw_modal();
}

/* ------------------------------------------------------------------------ */
/* Actions                                                                   */
/* ------------------------------------------------------------------------ */

static void show_emulators(void) {
    char buf[4096];
    buf[0] = 0;
    for (int i = 0; i < g_nemus; i++) {
        Emu *e = &g_emus[i];
        char line[900];
        if (e->found && e->is_title) snprintf(line, sizeof(line), "%s:  %s%s%s\n", e->name, e->title_id,
                                               e->dir[0] ? "  " : "", e->dir);
        else if (e->found) snprintf(line, sizeof(line), "%s:  %s\n", e->name, e->path);
        else continue;
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        if (e->found && e->cores_dir[0]) {
            char cl[1200] = "  cores: ";
            if (!e->ncores) strncat(cl, "none installed", sizeof(cl) - strlen(cl) - 1);
            for (int c = 0; c < e->ncores; c++) {
                if (c) strncat(cl, ", ", sizeof(cl) - strlen(cl) - 1);
                strncat(cl, e->cores[c], sizeof(cl) - strlen(cl) - 1);
            }
            strncat(cl, "\n", sizeof(cl) - strlen(cl) - 1);
            strncat(buf, cl, sizeof(buf) - strlen(buf) - 1);
        }
    }
    {
        char miss[600] = "";
        for (int i = 0; i < g_nemus; i++) {
            if (g_emus[i].found) continue;
            if (miss[0]) strncat(miss, ", ", sizeof(miss) - strlen(miss) - 1);
            strncat(miss, g_emus[i].name, sizeof(miss) - strlen(miss) - 1);
        }
        if (miss[0]) {
            strncat(buf, "Not found: ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, miss, sizeof(buf) - strlen(buf) - 1);
            strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
        }
    }
    {
        char el[400];
#ifdef GS_NATIVE
        snprintf(el, sizeof(el), "\nFolder list: %s\nCovers: %s\nJewel Case " GS_VERSION "\n", g_scan_status, g_art_status);
#else
        snprintf(el, sizeof(el), "\nCovers: %s\nJewel Case " GS_VERSION "\n", g_art_status);
#endif
        strncat(buf, el, sizeof(buf) - strlen(buf) - 1);
    }
    if (g_config_err[0]) {
        strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, g_config_err, sizeof(buf) - strlen(buf) - 1);
    }
    open_modal("Emulators", "%s", buf);
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

static int g_set_sel;
static int g_set_confirm = -1;  /* row waiting for a second press */
static int g_use_scanner;
static SDL_Window *g_win;

static int g_auto_covers = 1;

/* where "Create system folders" makes the system folders. Games are found in a
 * roms folder on every drive either way. */

/* folders for jc-scan.elf to create before it lists ("M<TAB>path" lines) */
static char g_scan_mkdirs[16384];

static const char *const DRIVES[][2] = {
    {"/data", "Internal storage"},
    {"/mnt/ext0", "Extended storage"}, {"/mnt/ext1", "Extended storage 2"},
    {"/mnt/usb0", "USB drive 1"}, {"/mnt/usb1", "USB drive 2"}, {"/mnt/usb2", "USB drive 3"},
    {"/mnt/usb3", "USB drive 4"}, {"/mnt/usb4", "USB drive 5"}, {"/mnt/usb5", "USB drive 6"},
    {"/mnt/usb6", "USB drive 7"}, {"/mnt/usb7", "USB drive 8"},
};
#define NDRIVES ((int)(sizeof(DRIVES) / sizeof(*DRIVES)))

static int drive_of_folder(const char *folder) {
    for (int i = 0; i < NDRIVES; i++) {
        size_t n = strlen(DRIVES[i][0]);
        if (!strncmp(folder, DRIVES[i][0], n) && folder[n] == '/') return i;
    }
    return 0;
}

static int drive_present(int i) {
    return i == 0 || is_dir(DRIVES[i][0]);
}

enum { SET_RESCAN, SET_ROMDIR, SET_FOLDERS, SET_MISSING, SET_REDOWNLOAD, SET_AUTO, SET_MUSIC, SET_VOLUME, SET_RAIN, SET_EMPTY, SET_CLEAR_RECENT, SET_EMULATORS, SET_CREDITS, SET_ABOUT, SET_COUNT };

static void save_ui_settings(void) {
    char path[600], out[400];
    snprintf(path, sizeof(path), "%s", USER_DIR "/ui-settings.json");
    snprintf(out, sizeof(out),
             "{\n  \"showEmptySystems\": %s,\n  \"autoCovers\": %s,\n  \"music\": %s,\n"
             "  \"musicVolume\": %d,\n  \"rain\": %s,\n  \"romFolder\": \"%s\"\n}\n",
             g_hide_empty ? "false" : "true", g_auto_covers ? "true" : "false", g_music_on ? "true" : "false",
             g_music_volume, g_rain_on ? "true" : "false", g_rom_folder);
    write_file(path, out);
}

static void load_ui_settings(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s", USER_DIR "/ui-settings.json");
    char *txt = read_file(path, NULL);
    if (!txt) return;
    cJSON *o = cJSON_Parse(txt);
    free(txt);
    cJSON *v = cJSON_GetObjectItem(o, "showEmptySystems");
    if (cJSON_IsBool(v)) g_hide_empty = !cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(o, "autoCovers");
    if (cJSON_IsBool(v)) g_auto_covers = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(o, "music");
    if (cJSON_IsBool(v)) g_music_on = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(o, "musicVolume");
    if (cJSON_IsNumber(v)) g_music_volume = SDL_max(0, SDL_min(100, v->valueint));
    v = cJSON_GetObjectItem(o, "rain");
    if (cJSON_IsBool(v)) g_rain_on = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(o, "romFolder");
    if (cJSON_IsString(v) && v->valuestring[0] == '/' && strlen(v->valuestring) < sizeof(g_rom_folder))
        scopy(g_rom_folder, sizeof(g_rom_folder), v->valuestring);
    cJSON_Delete(o);
}

static void free_all_media(void) {
    for (int s = 0; s < g_nsys; s++)
        for (int k = 0; k < g_sys[s].ngames; k++) free_media(&g_sys[s].games[k]);
}

/* rebuild the tab row, staying on the same tab (by what it shows) when it's still there */
static void rebuild_tabs(void) {
    int kind = g_nentries ? g_entries[g_sel_entry].kind : 0;
    char sys_id[32] = "";
    if (g_nentries && kind == 1) scopy(sys_id, sizeof(sys_id), g_sys[g_entries[g_sel_entry].sys].id);
    build_entries();
    g_sel_entry = 0;
    for (int i = 0; i < g_nentries; i++)
        if (g_entries[i].kind == kind && (kind != 1 || !strcmp(g_sys[g_entries[i].sys].id, sys_id))) g_sel_entry = i;
    if (g_sel_game >= entry_count()) g_sel_game = SDL_max(0, entry_count() - 1);
    g_pos = (float)g_sel_game;
}

static void present(SDL_Window *win);

static void busy_frame(const char *text) {
    render();
    fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x05060a, 190);
    float w = text_width(&F_title, text);
    draw_text(&F_title, (SCREEN_W - w) / 2, 500, text, 0xffffff, 255);
    present(g_win);
}

#if defined(GS_NATIVE) || defined(GS_HOST)
static int scan_with_helper(SDL_Window *win);
#endif

/* scan everything again: emulators, folders, games */
static void scan_everything(void) {
    free_all_media();
    for (int s = 0; s < g_nsys; s++) {
        free(g_sys[s].games);
        g_sys[s].games = NULL;
        g_sys[s].ngames = 0;
    }
    g_config_err[0] = 0;
    default_emulators();
    default_systems();
    load_config(g_config_err, sizeof(g_config_err));
#if defined(GS_NATIVE) || defined(GS_HOST)
    g_scan_failed = 0;
    if (g_use_scanner) {
        if (scan_with_helper(g_win) != 0) g_scan_failed = 1;
        logf_("folder list: %s", g_scan_status);
    }
#endif
    find_emulators();
    for (int s = 0; s < g_nsys; s++) {
        scan_system(&g_sys[s]);
        pick_emulator(&g_sys[s]);
        if (g_sys[s].ngames) logf_("%s: %d games, emulator %s %s", g_sys[s].id, g_sys[s].ngames,
                                   g_sys[s].emu >= 0 ? g_emus[g_sys[s].emu].id : "none", g_sys[s].core_name);
    }
}

static void remove_file(const char *path) {
    char rp[1200];
    real_path(path, rp, sizeof(rp));
    remove(rp);
}

/* all = also the covers already downloaded; otherwise only the ones not found before */
static int reset_covers(int all) {
    int n = 0;
    char done_repo[48][128];
    int nrepo = 0;
    for (int s = 0; s < g_nsys; s++) {
        const SysLook *l = look_of(&g_sys[s]);
        if (!l || !l->repo) continue;
        for (int k = 0; k < g_sys[s].ngames; k++) {
            Game *g = &g_sys[s].games[k];
            char base[1100], p[1200];
            for (int e = 0; e < 3; e++) {
                char b2[1100];
                info_base(g, e, b2, sizeof(b2));
                static const char *const ext[] = {".none", ".txt", ".png", ".jpg"};
                for (int k = 0; k < (all ? 4 : 1); k++) {
                    snprintf(p, sizeof(p), "%s%s", b2, ext[k]);
                    remove_file(p);
                }
            }
            if (wants_extras(g))
                for (int e = 0; e < 2; e++) {
                    extra_base(g, e, base, sizeof(base));
                    snprintf(p, sizeof(p), "%s.none", base);
                    remove_file(p);
                    if (all) {
                        snprintf(p, sizeof(p), "%s.png", base);
                        remove_file(p);
                        snprintf(p, sizeof(p), "%s.jpg", base);
                        remove_file(p);
                    }
                }
            if (g->art[0]) continue;
            cover_base(g, base, sizeof(base));
            snprintf(p, sizeof(p), "%s.none", base);
            remove_file(p);
            if (all) {
                snprintf(p, sizeof(p), "%s.jpg", base);
                remove_file(p);
                snprintf(p, sizeof(p), "%s.png", base);
                remove_file(p);
            }
            n++;
        }
        if (all && nrepo < 48) {
            /* the collection's file list too, so it's fetched fresh */
            int seen = 0;
            for (int i = 0; i < nrepo; i++) if (!strcmp(done_repo[i], l->repo)) seen = 1;
            if (!seen) {
                char p[400];
                snprintf(p, sizeof(p), COVER_DIR "/%s.idx", l->repo);
                remove_file(p);
                snprintf(p, sizeof(p), COVER_DIR "/%s.Named_Logos.idx", l->repo);
                remove_file(p);
                if (nrepo == 0) {
                    /* the LaunchBox disc index too */
                    remove_file(COVER_DIR "/launchbox.idx");
                    remove_file(COVER_DIR "/launchbox-info.txt");
                    remove_file(COVER_DIR "/launchbox.fail");
                }
                scopy(done_repo[nrepo++], 128, l->repo);
            }
        }
    }
    free_all_media();
    return n;
}

static void settings_row(int i, const char **label, const char **hint, char *value, size_t vcap, int *icon) {
    value[0] = 0;
    switch (i) {
    case SET_RESCAN:
        *label = "Rescan games";
        *hint = "Find games and emulators you've added or removed";
        *icon = ICON_STACK;
        break;
    case SET_ROMDIR: {
        static char hint_buf[160];
        *label = "Games folder";
        snprintf(hint_buf, sizeof(hint_buf), "%s. Press to pick another drive (games on every drive are found)",
                 g_rom_folder);
        *hint = hint_buf;
        *icon = ICON_STACK;
        scopy(value, vcap, DRIVES[drive_of_folder(g_rom_folder)][1]);
        break;
    }
    case SET_FOLDERS: {
        static char hint_buf[160];
        *label = "Create system folders";
        snprintf(hint_buf, sizeof(hint_buf), "Make a folder in %s for every system, plus a guide", g_rom_folder);
        *hint = hint_buf;
        *icon = ICON_STACK;
        break;
    }
    case SET_MISSING:
        *label = "Download missing covers";
        *hint = "Look again for games that have no cover";
        *icon = ICON_DOWNLOAD;
        if (g_art_running) scopy(value, vcap, "Downloading...");
        break;
    case SET_REDOWNLOAD:
        *label = "Redownload all covers";
        *hint = "Replace every downloaded cover (your own art is kept)";
        *icon = ICON_DOWNLOAD;
        break;
    case SET_AUTO:
        *label = "Download covers automatically";
        *hint = "When Jewel Case opens, fetch covers for new games";
        *icon = ICON_DOWNLOAD;
        scopy(value, vcap, g_auto_covers ? "On" : "Off");
        break;
    case SET_MUSIC:
        *label = "Background music";
        *hint = "MP3s in /data/jewelcase/music, shuffled and looped";
        *icon = ICON_DISC;
        scopy(value, vcap, !g_music_on ? "Off" : music_playing() ? "On" : "On (no MP3s found)");
        break;
    case SET_VOLUME:
        *label = "Music volume";
        *hint = "Press to change";
        *icon = ICON_DISC;
        snprintf(value, vcap, "%d%%", g_music_volume);
        break;
    case SET_RAIN:
        *label = "Rain effect";
        *hint = "Rain falling behind the games";
        *icon = ICON_STAR;
        scopy(value, vcap, g_rain_on ? "On" : "Off");
        break;
    case SET_EMPTY:
        *label = "Show empty systems";
        *hint = "Tabs for systems with no games";
        *icon = ICON_PAD;
        scopy(value, vcap, g_hide_empty ? "Off" : "On");
        break;
    case SET_CLEAR_RECENT:
        *label = "Clear recently played";
        *hint = "Empty the Recent tab";
        *icon = ICON_CLOCK;
        break;
    case SET_EMULATORS:
        *label = "Emulators";
        *hint = "Which emulator apps and RetroArch cores were found";
        *icon = ICON_CHIP;
        break;
    case SET_CREDITS:
        *label = "Credits";
        *hint = "Who made the models, art sources and code Jewel Case uses";
        *icon = ICON_STAR;
        break;
    default:
        *label = "About Jewel Case";
        *hint = "Made by SkaRift. Version, folders and cover status";
        *icon = ICON_GEAR;
        scopy(value, vcap, "v" GS_VERSION);
        break;
    }
    if (g_set_confirm == i) scopy(value, vcap, "Press \xe2\x9c\x95 again");
}

static void draw_settings(void) {
    fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x05060a, 175);
    int w = 1060, row_h = 58, gap = 5, pad = 26;
    int h = pad + F_title.line + 26 + SET_COUNT * (row_h + gap) + 70 + pad - 20;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    round_rect(x - 1, y - 1, w + 2, h + 2, 29, 0x9aa3c8, 60);
    round_rect(x, y, w, h, 28, 0x141826, 250);
    draw_icon(ICON_GEAR, x + pad, y + pad + 6, 40, 0xffffff, 255);
    draw_text(&F_title, x + pad + 58, y + pad - 6, "Settings", 0xffffff, 255);
    Uint32 accent = entry_accent(g_sel_entry);
    int ry = y + pad + F_title.line + 26;
    for (int i = 0; i < SET_COUNT; i++, ry += row_h + gap) {
        const char *label, *hint;
        char value[64];
        int icon;
        settings_row(i, &label, &hint, value, sizeof(value), &icon);
        int sel = i == g_set_sel;
        if (sel) {
            round_rect(x + pad - 4, ry - 4, w - 2 * pad + 8, row_h + 8, 22, 0xffffff, 50);
            round_rect(x + pad, ry, w - 2 * pad, row_h, 18, accent, 255);
        } else {
            draw_glass(x + pad, ry, w - 2 * pad, row_h, 18, 200);
        }
        draw_icon(icon, x + pad + 24, ry + (row_h - 34) / 2, 34, 0xffffff, sel ? 255 : 200);
        draw_text(&F_chip, x + pad + 76, ry + 4, label, 0xffffff, 255);
        draw_text(&F_tiny, x + pad + 77, ry + 32, hint, sel ? 0xeef0ff : 0x9aa1b8, 255);
        if (value[0]) {
            float vw = text_width(&F_chip, value);
            draw_text(&F_chip, x + w - pad - 28 - vw, ry + (row_h - F_chip.line) / 2.0f, value,
                      sel ? 0xffffff : 0xc9cee0, 255);
        }
    }
    int hy = y + h - pad - 6;
    draw_button(x + pad + 17, hy, 'x');
    draw_text(&F_small, x + pad + 46, hy - F_small.line / 2.0f, "Select", 0xcfd5e6, 255);
    draw_button(x + pad + 170, hy, 'o');
    draw_text(&F_small, x + pad + 199, hy - F_small.line / 2.0f, "Back", 0xcfd5e6, 255);
}

/* <games folder>/<folder> for every system, and a guide to what goes where.
 * Folders the app can't make itself (on a drive outside its sandbox) are made
 * by jc-scan.elf with the next scan. */
static void create_system_folders(void) {
    char root[600], rp[1300];
    scopy(root, sizeof(root), g_rom_folder);
    /* a drive outside /data: the app's own view of it is sandboxed, so the folder
     * scanner (outside the sandbox) makes the folders there, every time */
    int outside = 0;
#ifdef GS_NATIVE
    outside = strncmp(root, "/data/", 6) != 0;
#endif
    g_scan_mkdirs[0] = 0;
    if (outside) {
        snprintf(g_scan_mkdirs, sizeof(g_scan_mkdirs), "M\t%s\n", root);
    } else {
        real_path(root, rp, sizeof(rp));
        mkdir(rp, 0777);
    }
    int made = 0, had = 0, failed = 0;
    static char guide[16384];
    size_t gl = (size_t)snprintf(guide, sizeof(guide),
        "Jewel Case: put each system's games in its folder.\n"
        "Jewel Case looks in a roms folder on every drive: /data/roms, /mnt/usb0/roms,\n"
        "/mnt/ext0/roms and so on.\n\n");
    for (int i = 0; i < g_nsys; i++) {
        System *sy = &g_sys[i];
        if (!sy->nfolders) continue;
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", root, sy->folders[0]);
        if (outside) {
            if (is_dir(path)) had++;
            else {
                char line[720];
                snprintf(line, sizeof(line), "M\t%s\n", path);
                strncat(g_scan_mkdirs, line, sizeof(g_scan_mkdirs) - strlen(g_scan_mkdirs) - 1);
                failed++;   /* until the scanner has made it */
            }
        } else {
            real_path(path, rp, sizeof(rp));
            struct stat st;
            if (stat(rp, &st) == 0 && S_ISDIR(st.st_mode)) had++;
            else if (mkdir(rp, 0777) == 0) made++;
            else failed++;
        }
        char exts[200] = "";
        for (int k = 0; k < sy->nexts; k++) {
            if (k) strncat(exts, ", ", sizeof(exts) - strlen(exts) - 1);
            strncat(exts, sy->exts[k], sizeof(exts) - strlen(exts) - 1);
        }
        if (gl < sizeof(guide) - 300)
            gl += (size_t)snprintf(guide + gl, sizeof(guide) - gl, "%-10s %s%s(%s)\n", sy->folders[0], sy->name,
                                   sy->folder_games ? ", one folder per game " : " ", exts);
    }
    char gp[700];
    snprintf(gp, sizeof(gp), "%s/_Jewel Case folder guide.txt", root);
    if (outside) {
        /* the guide: written in /data, copied there by the scanner */
        const char *tmp = USER_DIR "/folder-guide.txt";
        write_file(tmp, guide);
        char line[1500];
        snprintf(line, sizeof(line), "C\t%s\t%s\n", tmp, gp);
        strncat(g_scan_mkdirs, line, sizeof(g_scan_mkdirs) - strlen(g_scan_mkdirs) - 1);
        busy_frame("Creating the folders...");
        scan_everything();
        load_recent();
        load_favs();
        rebuild_tabs();
        int now = 0;
        for (int i = 0; i < g_nsys; i++) {
            if (!g_sys[i].nfolders) continue;
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", root, g_sys[i].folders[0]);
            if (is_dir(path)) now++;
        }
        made = SDL_max(0, now - had);
        failed = SDL_max(0, failed - made);
    } else {
        write_file(gp, guide);
    }
    g_scan_mkdirs[0] = 0;
    if (failed && !made && !had)
        open_modal("Couldn't create the folders",
                   "The PS5 didn't let Jewel Case create folders in %s.\n"
                   "Is the drive plugged in? You can also create them over FTP, named like nes, snes, n64, gb, "
                   "gba, nds, psx, ps2, gc, wii, switch.",
                   root);
    else
        flash("%d folders created in %s, %d were already there%s", made, root, had, failed ? " (some failed)" : "");
}

static void settings_activate(void) {
    int i = g_set_sel;
    if ((i == SET_REDOWNLOAD || i == SET_CLEAR_RECENT) && g_set_confirm != i) {
        g_set_confirm = i;   /* destructive: ask for a second press */
        return;
    }
    g_set_confirm = -1;
    switch (i) {
    case SET_RESCAN: {
        busy_frame("Looking for your games...");
        scan_everything();
        load_recent();
        load_favs();
        rebuild_tabs();
        int total = 0;
        for (int s = 0; s < g_nsys; s++) total += g_sys[s].ngames;
        flash("Found %d games", total);
        if (g_auto_covers) start_cover_helper();
        g_settings = 0;
        break;
    }
    case SET_ROMDIR: {
        /* the next drive that's plugged in */
        int cur = drive_of_folder(g_rom_folder), next = cur;
        for (int k = 1; k <= NDRIVES; k++) {
            int c = (cur + k) % NDRIVES;
            if (drive_present(c)) {
                next = c;
                break;
            }
        }
        snprintf(g_rom_folder, sizeof(g_rom_folder), "%s/roms", DRIVES[next][0]);
        save_ui_settings();
        if (next == cur) flash("No other drive found. Plug one in, then Rescan games.");
        else flash("Games folder: %s", g_rom_folder);
        break;
    }
    case SET_FOLDERS:
        create_system_folders();
        break;
    case SET_MISSING:
    case SET_REDOWNLOAD:
        if (g_art_running) {
            flash("Covers are already downloading. Try again when it's finished.");
            break;
        }
        reset_covers(i == SET_REDOWNLOAD);
        start_cover_helper();
        flash(i == SET_REDOWNLOAD ? "Downloading all covers again" : "Looking for missing covers");
        g_settings = 0;
        break;
    case SET_AUTO:
        g_auto_covers = !g_auto_covers;
        save_ui_settings();
        break;
    case SET_MUSIC:
        g_music_on = !g_music_on;
        if (g_music_on) start_music();
        else music_stop();
        save_ui_settings();
        break;
    case SET_VOLUME:
        g_music_volume = g_music_volume >= 100 ? 20 : g_music_volume + 20;
        music_set_volume(g_music_volume);
        save_ui_settings();
        break;
    case SET_RAIN:
        g_rain_on = !g_rain_on;
        save_ui_settings();
        break;
    case SET_EMPTY:
        g_hide_empty = !g_hide_empty;
        save_ui_settings();
        rebuild_tabs();
        break;
    case SET_CLEAR_RECENT: {
        char path[600];
        snprintf(path, sizeof(path), "%s", USER_DIR "/recent.json");
        write_file(path, "[]\n");
        g_nrecent = 0;
        rebuild_tabs();
        flash("Recently played cleared");
        break;
    }
    case SET_EMULATORS:
        show_emulators();
        break;
    case SET_CREDITS:
        open_modal("Credits",
                   "Jewel Case by SkaRift.\n\n"
                   "Cartridge, card and disc renders, from 3D models (changed: labels blanked, logos removed):\n"
                   "  SNES: Steven-Bennis   NES: HippoStance   Game Boy: Karolina D   GBA: Vxcl\n"
                   "  DS / 3DS: Pcwer   Switch: crocoman2019   (all CC BY 4.0, on Sketchfab)\n"
                   "  N64: ejpman (CC BY-NC 4.0, Printables)   Disc: enjoyboy (CD Jewel Case, Blend Swap)\n\n"
                   "Art, downloaded on your console:\n"
                   "  Box art, logos and screenshots: the libretro-thumbnails project\n"
                   "  Game info and disc scans: the LaunchBox Games Database; disc scans: GameTDB\n\n"
                   "Code: ps5-native-app-boilerplate (BlackBearReloaded), the PS5 payload SDK and\n"
                   "  ps5-payload-websrv (John Tornblom), SDL2, cJSON, stb, dr_mp3, miniz, Poppins font.\n"
                   "Thanks to the etaHEN and ShadowMountPlus developers and every emulator's authors.\n\n"
                   "Full list with links and licenses: THIRD_PARTY_NOTICES.md on github.com/SkaRift/JewelCase");
        break;
    default: {
        int total = 0, nsys = 0;
        for (int s = 0; s < g_nsys; s++) {
            total += g_sys[s].ngames;
            if (g_sys[s].ngames) nsys++;
        }
#ifdef GS_NATIVE
        const char *fl = g_scan_status;
#else
        const char *fl = "direct";
#endif
        open_modal("Jewel Case " GS_VERSION,
                   "Made by SkaRift\n\n%d games on %d systems.\n\nFolder list: %s\nCovers: %s\n  saved in " COVER_DIR "\n"
                   "  box art from the libretro thumbnail collection (github.com/libretro-thumbnails),\n"
                   "  disc scans from the LaunchBox Games Database and GameTDB\n\n"
                   "Your own art, named like the game, next to its file or in a covers folder there, "
                   "is used instead of a download.%s%s",
                   total, nsys, fl, g_art_status, g_config_err[0] ? "\n\n" : "", g_config_err);
        break;
    }
    }
}

static void explain_missing(System *s) {
    char needs[1024] = "";
    for (int c = 0; c < s->nemus; c++) {
        Emu *e = emu_find(s->emus[c].emu);
        char line[400];
        if (s->emus[c].ncores) {
            char cores[256] = "";
            for (int k = 0; k < s->emus[c].ncores; k++) {
                if (k) strncat(cores, " / ", sizeof(cores) - strlen(cores) - 1);
                strncat(cores, s->emus[c].cores[k], sizeof(cores) - strlen(cores) - 1);
            }
            snprintf(line, sizeof(line), "  %s with a core: %s\n", e ? e->name : s->emus[c].emu, cores);
        } else {
            snprintf(line, sizeof(line), "  %s\n", e ? e->name : s->emus[c].emu);
        }
        strncat(needs, line, sizeof(needs) - strlen(needs) - 1);
    }
    open_modal("No emulator for this system",
               "%s needs one of these:\n%s\nInstall it in /data/homebrew, or set its path in jewelcase.json.\n"
               "Press the Options button to see which emulators Jewel Case found.",
               s->name, needs);
}

static void show_info(Game *g) {
    System *s = &g_sys[g->sys];
    char cover[1200];
    int cst = cover_of(g, cover, sizeof(cover));
    const char *cv = cst == 1 ? cover : cst == 0 ? (g_art_running ? "downloading" : "not downloaded yet") : "none found";
    LaunchCmd lc;
    if (build_launch(g, &lc) < 0) {
        open_modal(g->title, "System: %s\nFile: %s\nCover: %s\n\nNo emulator found for this system.", s->name, g->path, cv);
        return;
    }
    Emu *e = &g_emus[s->emu];
    open_modal(g->title, "System: %s\nFile: %s\nCover: %s\n\nEmulator: %s%s%s%s\n%s:\n  %s\nWorking folder:\n  %s",
               s->name, g->path, cv,
               e->name, s->core_name[0] ? " (core: " : "", s->core_name, s->core_name[0] ? ")" : "",
               e->is_title ? "Installed app" : "Command", lc.pretty, lc.cwd);
}

static void present(SDL_Window *win);

static void launch_game(Game *g, SDL_Window *win) {
    System *s = &g_sys[g->sys];
    LaunchCmd lc;
    if (build_launch(g, &lc) < 0) {
        explain_missing(s);
        return;
    }
    save_recent(g->path);
    g_launching = 1;
    render();
    draw_launching(g->title);
    present(win);

    music_stop();
    int st = send_launch(&lc);
    g_launching = 0;
    if (st == -2) {
        open_modal("Couldn't start the game",
                   "Jewel Case hands app launches to etaHEN's ELF loader (port 9021),\n"
                   "and it isn't answering. Check that etaHEN is running, then try again.");
    } else if (st == -3) {
        open_modal("Couldn't start the game", "jc-launch.elf is missing from the Jewel Case folder.\nReinstall Jewel Case.");
    } else if (st == -4) {
        open_modal("Couldn't start the game", "Couldn't write RetroArch's game request:\n  %s", lc.request_path);
    } else if (st == -5) {
        open_modal("Couldn't start the game", "Couldn't write /data/jewelcase-launch.txt.");
    } else if (st == -1) {
        open_modal("Couldn't start the game",
                   "Jewel Case starts games through websrv (the Homebrew Launcher's\n"
                   "web server), and it isn't answering on port 8080.\n\n"
                   "Load websrv (or check your etaHEN autoload), then try again.");
    } else if (st != 200 && st != 0) {
        open_modal("Couldn't start the game", "websrv answered with status %d.\n\n%s", st, lc.pretty);
    } else {
        flash("Started %s", g->title);
    }
    (void)win;
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

enum { IN_NONE, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK, IN_FAV, IN_OPTIONS, IN_PREV, IN_NEXT,
       IN_SETTINGS, IN_JUMP_BACK, IN_JUMP_FWD, IN_QUIT };

static int g_quit;

static void select_entry(int i) {
    if (g_nentries == 0) return;
    if (i < 0) i = g_nentries - 1;
    if (i >= g_nentries) i = 0;
    if (i != g_sel_entry) {
        g_sel_entry = i;
        g_sel_game = 0;
        g_pos = 0;
        trim_media();
    }
}

static void select_game(int i) {
    int n = entry_count();
    if (n <= 0) return;
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    if (i != g_sel_game) {
        g_sel_game = i;
        trim_media();
    }
}

static void handle(int in, SDL_Window *win) {
    if (g_modal) {
        if (in == IN_BACK || in == IN_OK) g_modal = 0;
        return;
    }
    if (g_settings) {
        if (in != IN_OK) g_set_confirm = -1;
        if (in == IN_UP) g_set_sel = (g_set_sel + SET_COUNT - 1) % SET_COUNT;
        else if (in == IN_DOWN) g_set_sel = (g_set_sel + 1) % SET_COUNT;
        else if (in == IN_OK) settings_activate();
        else if (in == IN_BACK || in == IN_SETTINGS) g_settings = 0;
        return;
    }
    switch (in) {
    case IN_LEFT:
        select_game(g_sel_game - 1);
        break;
    case IN_RIGHT:
        select_game(g_sel_game + 1);
        break;
    case IN_JUMP_BACK:
        select_game(g_sel_game - 10);
        break;
    case IN_JUMP_FWD:
        select_game(g_sel_game + 10);
        break;
    case IN_PREV:
    case IN_UP:
        if (g_details) break;
        select_entry(g_sel_entry - 1);
        break;
    case IN_NEXT:
    case IN_DOWN:
        if (g_details) break;
        select_entry(g_sel_entry + 1);
        break;
    case IN_OK:
        if (entry_game(g_sel_game)) launch_game(entry_game(g_sel_game), win);
        else if (g_nentries && g_entries[g_sel_entry].kind == 1 && g_sys[g_entries[g_sel_entry].sys].emu < 0)
            explain_missing(&g_sys[g_entries[g_sel_entry].sys]);
        break;
    case IN_BACK:
        if (g_details) {
            g_details = 0;
            break;
        }
        g_quit = 1;
        break;
    case IN_QUIT:
        g_quit = 1;
        break;
    case IN_FAV: {
        Game *g = entry_game(g_sel_game);
        if (!g) break;
        int now = toggle_fav(g->path);
        flash(now ? "Added %s to Favorites" : "Removed %s from Favorites", g->title);
        /* keep the same tab and game selected while the tab row changes */
        int kind = g_entries[g_sel_entry].kind, sys = g_entries[g_sel_entry].sys;
        build_entries();
        int found = 0;
        for (int i = 0; i < g_nentries; i++)
            if (g_entries[i].kind == kind && g_entries[i].sys == sys) {
                g_sel_entry = i;
                found = 1;
            }
        if (!found) g_sel_entry = 0;
        if (g_sel_game >= entry_count()) g_sel_game = SDL_max(0, entry_count() - 1);
        g_pos = (float)g_sel_game;
        break;
    }
    case IN_OPTIONS:
        if (!entry_game(g_sel_game)) break;
        if (g_details) show_info(entry_game(g_sel_game));
        else {
            g_details = 1;
            g_det_t = 0;
        }
        break;
    case IN_SETTINGS:
        g_settings = 1;
        g_set_sel = 0;
        g_set_confirm = -1;
        break;
    }
}

static void present(SDL_Window *win) {
    (void)win;
    SDL_RenderPresent(R);
#ifdef GS_NATIVE
    gs_vout_present(g_screen->pixels, g_screen->pitch);
#endif
}

#ifdef GS_HOST
static void screenshot(const char *path) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ABGR8888, s->pixels, s->pitch);
    stbi_write_png(path, SCREEN_W, SCREEN_H, 4, s->pixels, s->pitch);
    SDL_FreeSurface(s);
    logf_("screenshot %s", path);
}
#endif

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */

#if defined(GS_NATIVE) || defined(GS_HOST)
/* Ask jc-scan.elf (outside the sandbox) to list the folders Jewel Case needs. */
static int scan_with_helper(SDL_Window *win) {
    (void)win;
    static char req[24000];
    snprintf(req, sizeof(req), "jewelcase-scan 1\n");
    strncat(req, g_scan_mkdirs, sizeof(req) - strlen(req) - 1);
    /* the drives themselves, to know which are plugged in */
    {
        const char *dr[] = {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                            "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};
        for (size_t i = 0; i < sizeof(dr) / sizeof(*dr); i++) {
            char line[64];
            snprintf(line, sizeof(line), "0\t%s\n", dr[i]);
            strncat(req, line, sizeof(req) - strlen(req) - 1);
        }
    }
    /* app folders: their sce_sys/param.json names the app (negative depth) */
    const char *hb[] = {"/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
                        "/mnt/usb3/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew"};
    for (size_t i = 0; i < sizeof(hb) / sizeof(*hb); i++) {
        char line[300];
        snprintf(line, sizeof(line), "-5\t%s\n", hb[i]);
        strncat(req, line, sizeof(req) - strlen(req) - 1);
    }
    strncat(req, "-2\t/user/app\n-2\t/system_ex/app\n", sizeof(req) - strlen(req) - 1);
    strncat(req, "1\t/data/jewelcase/music\n", sizeof(req) - strlen(req) - 1);
    for (int r = 0; r < g_nroots; r++) {
        int inside = 0;
        for (size_t i = 0; i < sizeof(hb) / sizeof(*hb); i++)
            if (!strncmp(g_roots[r], hb[i], strlen(hb[i])) && g_roots[r][strlen(hb[i])] == '/') inside = 1;
        if (inside) continue;
        char line[300];
        snprintf(line, sizeof(line), "3\t%s\n", g_roots[r]);
        strncat(req, line, sizeof(req) - strlen(req) - 1);
    }
    char rp[1100];
    real_path("/data/jewelcase-index.txt", rp, sizeof(rp));
    remove(rp);
    if (write_file("/data/jewelcase-scan.txt", req) != 0) {
        scopy(g_scan_status, sizeof(g_scan_status), "couldn't write /data/jewelcase-scan.txt");
        return -1;
    }
    char helper[600];
    snprintf(helper, sizeof(helper), "%s/jc-scan.elf", g_appdir);
#ifdef GS_HOST
    if (getenv("GS_HOST_SCAN")) {
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "%s", getenv("GS_HOST_SCAN"));
        if (system(cmd) != 0) logf_("host scan command failed");
    }
#else
    int st = send_to_elfldr(helper);
    if (st == -3) {
        scopy(g_scan_status, sizeof(g_scan_status), "jc-scan.elf is missing from the Jewel Case folder");
        return -1;
    }
    if (st != 200) {
        scopy(g_scan_status, sizeof(g_scan_status), "ELF loader on port 9021 isn't answering");
        return -1;
    }
#endif
    /* the helper writes the index in a moment */
    for (int tries = 0; tries < 150; tries++) {
        char *txt = read_file("/data/jewelcase-index.txt", NULL);
        if (txt) {
            int n = ix_load(txt);
            if (n >= 0) {
                snprintf(g_scan_status, sizeof(g_scan_status), "%d files and folders (jc-scan.elf)", n);
                return 0;
            }
            free(txt);
        }
        SDL_Delay(200);
    }
    scopy(g_scan_status, sizeof(g_scan_status), "jc-scan.elf didn't answer within 30 seconds");
    return -1;
}
#endif

int main(int argc, char **argv) {
    /* before any other SDL or cJSON call */
    SDL_SetMemoryFunctions(gs_malloc, gs_calloc, gs_realloc, gs_free);
    {
        cJSON_Hooks hooks = {gs_malloc, gs_free};
        cJSON_InitHooks(&hooks);
    }
    const char *script = NULL;
    int appdir_given = 0;
#ifdef GS_NATIVE
    int use_scanner = 1;
#else
    int use_scanner = 0;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fsroot") && i + 1 < argc) scopy(g_fsroot, sizeof(g_fsroot), argv[++i]);
        else if (!strcmp(argv[i], "--appdir") && i + 1 < argc) {
            scopy(g_appdir, sizeof(g_appdir), argv[++i]);
            appdir_given = 1;
        }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--scanner")) use_scanner = 1;
    }
#ifndef GS_NATIVE
    if (!appdir_given) {
        /* websrv / the home-screen icon start us with cwd = our folder */
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) && strstr(cwd, "/homebrew/")) scopy(g_appdir, sizeof(g_appdir), cwd);
    }
#endif
    logf_("Jewel Case %s, app dir %s", GS_VERSION, g_appdir);

#ifdef GS_NATIVE
    {
        char missing[1024], msg[1200];
        if (gs_missing_imports(missing, sizeof(missing)) > 0) {
            snprintf(msg, sizeof(msg), "Jewel Case can't start: the console didn't provide %s", missing);
            logf_("%s", msg);
            gs_notify(msg);
            sleep(4);
            sceSystemServiceLoadExec("exit", NULL);
            for (;;) sleep(1);
        }
    }
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
#endif
#ifdef GS_NATIVE
    /* No SDL video driver here: SDL draws into g_screen and reads the controller;
     * vout.c (the template's tested VideoOut code) shows the frames. */
    {
        const char *verr = gs_vout_init();
        if (verr) gs_fatal("video", verr);
    }
    if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0) gs_fatal("SDL_Init", SDL_GetError());
    g_screen = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!g_screen) gs_fatal("screen image", SDL_GetError());
    R = SDL_CreateSoftwareRenderer(g_screen);
    if (!R) gs_fatal("renderer", SDL_GetError());
    SDL_Window *win = NULL;
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        logf_("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Jewel Case", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCREEN_W, SCREEN_H, 0);
    if (!win) {
        logf_("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!R) {
        logf_("SDL_CreateRenderer: %s", SDL_GetError());
        return 1;
    }
#endif

    make_corner();
    load_ui_images();
    make_background_for(0x6d63e8);
    {
        int w, h, n;
        unsigned char *px = stbi_load_from_memory(logo_png, (int)sizeof(logo_png), &w, &h, &n, 4);
        if (px) {
            SDL_Surface *ls = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
            if (ls) {
                T_logo = SDL_CreateTextureFromSurface(R, ls);
                if (T_logo) SDL_SetTextureBlendMode(T_logo, SDL_BLENDMODE_BLEND);
                SDL_FreeSurface(ls);
            }
            stbi_image_free(px);
        }
    }
    font_init(&F_small, font_regular, 26);
    font_init(&F_body, font_regular, 32);
    font_init(&F_tile, font_regular, 27);
    font_init(&F_title, font_bold, 54);
    font_init(&F_huge, font_bold, 76);
    font_init(&F_head, font_bold, 62);
    font_init(&F_chip, font_regular, 24);
    font_init(&F_tiny, font_regular, 19);
    font_init(&F_disc_l, font_regular, 34);
    font_init(&F_disc_m, font_regular, 28);
    font_init(&F_disc_s, font_regular, 17);
    font_init(&F_disc_b, font_bold, 34);

    /* splash while scanning: the Jewel Case art, with a status line under it */
    {
        int sw, sh, sn;
        unsigned char *px = stbi_load_from_memory(splash_jpg, (int)sizeof(splash_jpg), &sw, &sh, &sn, 4);
        SDL_Texture *st = NULL;
        if (px) {
            SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormatFrom(px, sw, sh, 32, sw * 4, SDL_PIXELFORMAT_ABGR8888);
            if (ss) {
                st = SDL_CreateTextureFromSurface(R, ss);
                SDL_FreeSurface(ss);
            }
            stbi_image_free(px);
        }
        if (st) {
            SDL_RenderCopy(R, st, NULL, NULL);
            SDL_DestroyTexture(st);
        } else {
            fill_rect(0, 0, SCREEN_W, SCREEN_H, 0x0b0d19, 255);
            const char *t = "Jewel Case";
            float w = text_width(&F_huge, t);
            draw_text(&F_huge, (SCREEN_W - w) / 2, 440, t, 0xffffff, 255);
        }
        const char *s = "Looking for your games...";
        float w2 = text_width(&F_small, s);
        draw_text(&F_small, (SCREEN_W - w2) / 2, 1006, s, 0x9aa3c8, 255);
    }
    present(win);

    g_win = win;
    g_use_scanner = use_scanner;
    {
        char rp[1300];
        const char *dirs[] = {"/data/jewelcase", "/data/jewelcase/music", "/data/jewelcase/covers"};
        for (int i = 0; i < 3; i++) {
            real_path(dirs[i], rp, sizeof(rp));
            mkdir(rp, 0777);
        }
        /* earlier builds saved these in the app folder: carry them over once */
        const char *saved[] = {"recent.json", "favorites.json", "ui-settings.json"};
        for (int i = 0; i < 3; i++) {
            char to[700], from[700];
            snprintf(to, sizeof(to), USER_DIR "/%s", saved[i]);
            snprintf(from, sizeof(from), "%s/%s", g_appdir, saved[i]);
            if (file_exists(to)) continue;
            char *old = read_file(from, NULL);
            if (old) {
                write_file(to, old);
                free(old);
            }
        }
        const char *note = "/data/jewelcase/music/_Put your MP3s here.txt";
        if (!file_exists(note))
            write_file(note, "Put MP3 files in this folder and Jewel Case plays them in the background,\n"
                             "shuffled and looped. Restart Jewel Case (or use Settings > Rescan games)\n"
                             "after adding songs. Turn music on or off in Settings.\n");
    }
    load_ui_settings();
    scan_everything();
    load_recent();
    load_favs();
    build_entries();
    g_sel_entry = 0;   /* Recently Played when there is one */
    if (g_config_err[0]) open_modal("Settings file problem", "%s\n\nJewel Case is using its defaults.", g_config_err);
    if (g_scan_failed && g_nentries == 0)
        open_modal("Couldn't list your game folders",
                   "Jewel Case lists folders with a small helper it sends to etaHEN's\n"
                   "ELF loader (port 9021), because apps can't list folders themselves.\n\n"
                   "Check that etaHEN is running, then restart Jewel Case.\n"
                   "  Details: %s", g_scan_status);

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

#ifdef GS_HOST
    if (script) {
        /* scripted run for testing: comma separated actions, "shot:file.png" saves a screenshot */
        char buf[2048];
        scopy(buf, sizeof(buf), script);
        start_cover_helper();
        start_music();
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (!strncmp(tok, "shot:", 5)) {
                for (int k = 0; k < 12; k++) {   /* settle animations + lazy art */
                    g_pos = (float)g_sel_game;
                    g_tab_scroll = g_tab_target;
                    g_det_t = 1;
                    render();
                }
                screenshot(tok + 5);
                continue;
            }
            int in = IN_NONE;
            if (!strcmp(tok, "up")) in = IN_UP;
            else if (!strcmp(tok, "down")) in = IN_DOWN;
            else if (!strcmp(tok, "left")) in = IN_LEFT;
            else if (!strcmp(tok, "right")) in = IN_RIGHT;
            else if (!strcmp(tok, "ok")) in = IN_OK;
            else if (!strcmp(tok, "back")) in = IN_BACK;
            else if (!strcmp(tok, "info")) in = IN_OPTIONS;
            else if (!strcmp(tok, "emus")) in = IN_SETTINGS;
            else if (!strcmp(tok, "fav")) in = IN_FAV;
            else if (!strncmp(tok, "pos:", 4)) { g_pos = (float)atof(tok + 4); continue; }
            else if (!strncmp(tok, "spin:", 5)) {
                g_spin_game = entry_game(g_sel_game);
                g_spin = (float)atof(tok + 5);
                continue;
            }
            else if (!strcmp(tok, "next")) in = IN_NEXT;
            else if (!strcmp(tok, "prev")) in = IN_PREV;
            handle(in, win);
        }
        return 0;
    }
#else
    (void)script;
#endif

    /* input repeat */
    int held = IN_NONE;
    Uint32 held_since = 0, last_repeat = 0;
    int dirty = 1;
    int stick_dir = IN_NONE;

    render();
    present(win);
    if (g_auto_covers) start_cover_helper();
    start_music();
    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            int in = IN_NONE;
            switch (ev.type) {
            case SDL_QUIT:
                g_quit = 1;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: in = IN_UP; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in = IN_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in = IN_LEFT; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in = IN_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_A: in = IN_OK; break;        /* Cross */
                case SDL_CONTROLLER_BUTTON_B: in = IN_BACK; break;      /* Circle */
                case SDL_CONTROLLER_BUTTON_Y: in = IN_FAV; break;       /* Triangle */
                case SDL_CONTROLLER_BUTTON_X: in = IN_OPTIONS; break;   /* Square */
                case SDL_CONTROLLER_BUTTON_START: in = IN_SETTINGS; break;  /* Options */
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: in = IN_PREV; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: in = IN_NEXT; break;
                }
                if ((in >= IN_UP && in <= IN_RIGHT) || in == IN_PREV || in == IN_NEXT) {
                    held = in;
                    held_since = last_repeat = SDL_GetTicks();
                }
                break;
            case SDL_CONTROLLERBUTTONUP:
                held = IN_NONE;
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                    /* L2 / R2: jump 10 games */
                    static int down[2];
                    int k = ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
                    if (!down[k] && ev.caxis.value > 20000) {
                        down[k] = 1;
                        in = k ? IN_JUMP_FWD : IN_JUMP_BACK;
                    } else if (down[k] && ev.caxis.value < 8000) {
                        down[k] = 0;
                    }
                } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    SDL_GameController *gc = SDL_GameControllerFromInstanceID(ev.caxis.which);
                    int ax = gc ? SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) : 0;
                    int ay = gc ? SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) : 0;
                    int d = IN_NONE;
                    if (abs(ax) > 20000 || abs(ay) > 20000) {
                        if (abs(ax) > abs(ay)) d = ax > 0 ? IN_RIGHT : IN_LEFT;
                        else d = ay > 0 ? IN_DOWN : IN_UP;
                    }
                    if (d != stick_dir) {
                        stick_dir = d;
                        if (d != IN_NONE) {
                            in = d;
                            held = d;
                            held_since = last_repeat = SDL_GetTicks();
                        } else {
                            held = IN_NONE;
                        }
                    }
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                switch (ev.key.keysym.sym) {
                case SDLK_UP: in = IN_UP; break;
                case SDLK_DOWN: in = IN_DOWN; break;
                case SDLK_LEFT: in = IN_LEFT; break;
                case SDLK_RIGHT: in = IN_RIGHT; break;
                case SDLK_RETURN: in = IN_OK; break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: in = IN_BACK; break;
                case SDLK_i: in = IN_OPTIONS; break;
                case SDLK_e: in = IN_SETTINGS; break;
                case SDLK_f: in = IN_FAV; break;
                case SDLK_q: in = IN_PREV; break;
                case SDLK_w: in = IN_NEXT; break;
                }
                if (in >= IN_UP && in <= IN_RIGHT) {
                    held = in;
                    held_since = last_repeat = SDL_GetTicks();
                }
                break;
            case SDL_KEYUP:
                held = IN_NONE;
                break;
            }
            if (in != IN_NONE) {
                handle(in, win);
                dirty = 1;
            }
        }

        Uint32 now = SDL_GetTicks();
        if (held != IN_NONE && now - held_since > 380 && now - last_repeat > 85) {
            last_repeat = now;
            handle(held, win);
            dirty = 1;
        }

        /* animate the carousel and the tab row */
        {
            float dp = (float)g_sel_game - g_pos, dt = g_tab_target - g_tab_scroll;
            if (dp * dp > 0.00002f || dt * dt > 0.25f) {
                g_pos += dp * 0.24f;
                g_tab_scroll += dt * 0.3f;
                dirty = 1;
            } else {
                g_pos = (float)g_sel_game;
                g_tab_scroll = g_tab_target;
            }
        }
        {
            static Uint32 spin_last;
            float sdt = spin_last ? SDL_min(0.1f, (now - spin_last) / 1000.0f) : 0;
            spin_last = now;
            if (update_spin(sdt)) dirty = 1;
        }
        if (g_builds_left > 0) dirty = 1;
        if (g_details && g_det_t < 1) {
            g_det_t = SDL_min(1.0f, g_det_t + 0.075f);
            dirty = 1;
        }
        {
            /* rain: about 30 frames a second */
            static Uint32 last_rain;
            if (g_rain_on && !g_launching && SDL_GetTicks() - last_rain >= 33) {
                last_rain = SDL_GetTicks();
                dirty = 1;
            }
        }

        /* covers: follow the helper, redraw the spinners now and then */
        {
            int was = g_art_running;
            poll_art_status();
            static Uint32 last_spin;
            int spinning = 0;
            int n = entry_count();
            for (int i = SDL_max(0, g_sel_game - 3); i < SDL_min(n, g_sel_game + 4); i++) {
                Game *g = entry_game(i);
                if (g && g->media_pending && g_art_running) spinning = 1;
            }
            if ((spinning || was != g_art_running || g_art_running) && SDL_GetTicks() - last_spin > 90) {
                last_spin = SDL_GetTicks();
                dirty = 1;
            }
        }
        /* the clock */
        {
            static time_t shown;
            time_t now = time(NULL);
            if (now / 30 != shown / 30) {
                shown = now;
                dirty = 1;
            }
        }
        if (g_status_until && SDL_GetTicks() > g_status_until) {
            g_status_until = 0;
            g_status_msg[0] = 0;
            dirty = 1;
        }

        if (dirty) {
            render();
            present(win);
            dirty = 0;
        } else {
            SDL_Delay(16);
        }
    }

    music_stop();
    SDL_Quit();
#ifdef GS_NATIVE
    /* a title ends by asking the shell to close it */
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
#endif
    return 0;
}
