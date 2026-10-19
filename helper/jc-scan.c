/* Jewel Case folder scanner.
 *
 * Jewel Case is a sandboxed app: under ShadowMountPlus it can open files in
 * /data but can't list folders. This payload runs outside the sandbox (sent to
 * the ELF loader on port 9021), lists the folders Jewel Case asks for and writes
 * the result to /data/jewelcase-index.txt, which Jewel Case reads instead.
 *
 * Request (/data/jewelcase-scan.txt), one folder per line:
 *   jewelcase-scan 1
 *   <depth>\t<path>
 *   M\t<path>                       create this folder (and its parents) first
 *   C\t<from>\t<to>                  copy a file (a guide Jewel Case wrote in /data)
 * Index (/data/jewelcase-index.txt):
 *   jewelcase-index 1
 *   D\t<path>                       a folder
 *   F\t<path>                       a file
 *   T\t<titleId>\t<folder>\t<name>  an installed app (from sce_sys/param.json)
 *   S\t<path>\t<title ID>\t<kind>\t<version>   a Switch .nsp / .nsz: what it holds, read
 *                                   from its (unencrypted) table of contents: kind G game,
 *                                   U update, D DLC; version "-" when it doesn't say
 *   P\t<folder>                     ProsperoEden's game files folder (its settings)
 *   end
 * More request lines:
 *   V\t<from>\t<to>                  move a file (into ProsperoEden's updates folder):
 *                                   renamed, or copied then removed across drives
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST "/data/jewelcase-scan.txt"
#define INDEX "/data/jewelcase-index.txt"
#define INDEX_TMP "/data/jewelcase-index.txt.tmp"
#define MAX_ENTRIES 400000

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

static void
notify(const char *fmt, ...) {
  notify_request_t req;
  va_list args;
  memset(&req, 0, sizeof req);
  va_start(args, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, args);
  va_end(args);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* big folders inside emulator apps that never hold games, cores or settings */
static const char *SKIP[] = {
  "shaders", "shaders_slang", "overlays", "radv-shader-cache", "library",
  "savestates", "savefiles", "screenshots", "filters", "autoconfig",
  "assets", "thumbnails", "logs", "cheats", "database", "downloads",
  "playlists", "sce_module", "info", 0
};

static long g_entries;

/* ------------------------------------------------------------------------ */
/* Switch files: what an .nsp holds                                          */
/* ------------------------------------------------------------------------ */

/* An NSP (or NSZ) is a PFS0 archive: a plain list of the files in it. Its ticket is named
 * after the rights ID, whose first 16 hex digits are the title ID (a game's ends in 000, its
 * update's in 800, DLC otherwise). Some also carry their .cnmt.xml, which says the type and
 * version outright. */
static unsigned long long rd64(const unsigned char *p) {
  unsigned long long v = 0;
  for(int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static const char *xml_tag(const char *x, const char *tag, char *out, size_t cap) {
  char open[40];
  snprintf(open, sizeof open, "<%s>", tag);
  const char *p = strstr(x, open);
  if(!p) return NULL;
  p += strlen(open);
  size_t n = 0;
  while(*p && *p != '<' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return out;
}

static void
switch_info(FILE *out, const char *path) {
  FILE *f = fopen(path, "rb");
  if(!f) return;
  static unsigned char hdr[256 * 1024];
  size_t got = fread(hdr, 1, sizeof hdr, f);
  if(got < 16 || memcmp(hdr, "PFS0", 4)) {
    fclose(f);
    return;
  }
  unsigned n = rd32(hdr + 4), strsz = rd32(hdr + 8);
  size_t table = 16 + (size_t)n * 24, data = table + strsz;
  if(n == 0 || n > 4000 || data > got) {
    fclose(f);
    return;
  }
  const char *strs = (const char *)hdr + table;
  unsigned long long tid = 0;
  char kind = 0, ver[24] = "-";
  for(unsigned i = 0; i < n; i++) {
    const unsigned char *e = hdr + 16 + (size_t)i * 24;
    unsigned no = rd32(e + 16);
    if(no >= strsz) continue;
    const char *name = strs + no;
    size_t l = strnlen(name, strsz - no);
    if(l == 36 && !strcmp(name + 32, ".tik") && !tid) {
      char h[17];
      memcpy(h, name, 16);
      h[16] = 0;
      tid = strtoull(h, NULL, 16);
    } else if(l > 9 && !strcmp(name + l - 9, ".cnmt.xml")) {
      unsigned long long off = data + rd64(e), sz = rd64(e + 8);
      if(sz > 0 && sz < 64 * 1024) {
        static char xml[64 * 1024 + 1];
        if(fseeko(f, (off_t)off, SEEK_SET) == 0) {
          size_t k = fread(xml, 1, (size_t)sz, f);
          xml[k] = 0;
          char v[64];
          if(xml_tag(xml, "Type", v, sizeof v))
            kind = !strcmp(v, "Patch") ? 'U' : !strcmp(v, "AddOnContent") ? 'D' : !strcmp(v, "Application") ? 'G' : kind;
          if(xml_tag(xml, "Id", v, sizeof v)) tid = strtoull(v, NULL, 16);
          if(xml_tag(xml, "Version", v, sizeof v)) snprintf(ver, sizeof ver, "%s", v);
        }
      }
    }
  }
  fclose(f);
  if(!tid) return;
  if(!kind) kind = (tid & 0xfff) == 0x800 ? 'U' : (tid & 0xfff) ? 'D' : 'G';
  fprintf(out, "S\t%s\t%016llX\t%c\t%s\n", path, tid, kind, ver);
}

static int ends_with(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcasecmp(s + a - b, suf);
}

static void walk(FILE *out, const char *dir, int depth, int app_levels);

/* ProsperoEden's game files folder, from its settings ("game_files"), default /data/prosperoeden */
static void
prospero_folder(FILE *out) {
  FILE *f = fopen("/data/prosperoeden/config/prosperoeden.json", "r");
  char gf[600] = "";
  if(f) {
    static char buf[256 * 1024];
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    /* the top-level key (each game's settings live under "games", which has none) */
    const char *p = strstr(buf, "\"game_files\"");
    if(p) {
      p = strchr(p + 12, ':');
      if(p) p = strchr(p, '"');
      if(p) {
        p++;
        size_t n = 0;
        while(*p && *p != '"' && n + 1 < sizeof gf) gf[n++] = *p++;
        gf[n] = 0;
      }
    }
  }
  const char *base = gf[0] == '/' ? gf : "/data/prosperoeden";
  fprintf(out, "P\t%s\n", base);
  /* its games (one folder each, the update and DLC beside them) and its updates folder */
  static const char *const sub[] = {"roms", "updates"};
  for(int i = 0; i < 2; i++) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/%s", base, sub[i]);
    struct stat st;
    if(stat(dir, &st) || !S_ISDIR(st.st_mode)) continue;
    fprintf(out, "D\t%s\n", dir);
    walk(out, dir, 2, 0);
  }
}

/* move a file: a rename on the same drive, else copied and then removed (0 ok) */
static int
move_file(const char *from, const char *to) {
  if(!rename(from, to)) return 0;
  FILE *a = fopen(from, "rb");
  if(!a) return -1;
  char part[1300];
  snprintf(part, sizeof part, "%s.part", to);
  FILE *b = fopen(part, "wb");
  if(!b) {
    fclose(a);
    return -1;
  }
  static char buf[1 << 20];
  size_t k;
  int ok = 1;
  while((k = fread(buf, 1, sizeof buf, a)) > 0)
    if(fwrite(buf, 1, k, b) != k) {
      ok = 0;
      break;
    }
  if(ferror(a)) ok = 0;
  fclose(a);
  if(fclose(b)) ok = 0;
  if(!ok || rename(part, to)) {
    unlink(part);
    return -1;
  }
  chmod(to, 0666);
  unlink(from);
  return 0;
}

static int
skip_name(const char *name) {
  for(int i=0; SKIP[i]; i++) {
    if(!strcasecmp(name, SKIP[i])) return 1;
  }
  return 0;
}

/* titleId / titleName from a param.json, without a JSON library */
static int
json_string(const char *txt, const char *key, char *out, size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(txt, pat);
  if(!p) return -1;
  p = strchr(p + strlen(pat), ':');
  if(!p) return -1;
  p = strchr(p, '"');
  if(!p) return -1;
  p++;
  size_t n = 0;
  while(*p && *p != '"' && n + 1 < cap) {
    if(*p == '\\' && p[1]) p++;
    out[n++] = *p++;
  }
  out[n] = 0;
  return 0;
}

static void
app_info(FILE *out, const char *dir) {
  char path[1200], tid[64] = "", name[256] = "";
  snprintf(path, sizeof path, "%s/sce_sys/param.json", dir);
  FILE *f = fopen(path, "r");
  if(!f) return;
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  json_string(buf, "titleId", tid, sizeof tid);
  json_string(buf, "titleName", name, sizeof name);
  for(char *c = name; *c; c++) if(*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
  if(tid[0]) fprintf(out, "T\t%s\t%s\t%s\n", tid, dir, name);
}

/* app_levels: how many folder levels below here may be apps; 2 also finds
 * apps kept in a versioned folder (ProsperoEden-v1.000.100/PPSA99008). */
static void
walk(FILE *out, const char *dir, int depth, int app_levels) {
  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d)) && g_entries < MAX_ENTRIES) {
    if(e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    int isdir = e->d_type == DT_DIR;
    if(e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
      struct stat st;
      isdir = !stat(path, &st) && S_ISDIR(st.st_mode);
    }
    fprintf(out, "%c\t%s\n", isdir ? 'D' : 'F', path);
    g_entries++;
    if(!isdir && (ends_with(e->d_name, ".nsp") || ends_with(e->d_name, ".nsz"))) switch_info(out, path);
    if(isdir) {
      if(app_levels > 0) app_info(out, path);
      if(depth > 1 && !skip_name(e->d_name)) walk(out, path, depth - 1, app_levels - 1);
      else if(app_levels > 1) walk(out, path, 1, app_levels - 1);  /* look for apps one level down */
    }
  }
  closedir(d);
}

int
main(void) {
  /* Jewel Case's own folders, so people can drop files in without making them */
  mkdir("/data/jewelcase", 0777);
  mkdir("/data/jewelcase/music", 0777);
  mkdir("/data/jewelcase/covers", 0777);
  FILE *req = fopen(REQUEST, "r");
  char line[1200];
  if(!req) {
    notify("Jewel Case: no scan request");
    return 1;
  }
  if(!fgets(line, sizeof line, req) || strcmp(line, "jewelcase-scan 1\n")) {
    fclose(req);
    notify("Jewel Case: unreadable scan request");
    return 1;
  }
  FILE *out = fopen(INDEX_TMP, "w");
  if(!out) {
    fclose(req);
    notify("Jewel Case: can't write " INDEX_TMP);
    return 1;
  }
  fprintf(out, "jewelcase-index 1\n");
  while(fgets(line, sizeof line, req)) {
    size_t n = strlen(line);
    while(n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    char *tab = strchr(line, '\t');
    if(!tab) continue;
    *tab = 0;
    if(line[0] == 'C') {
      char *from = tab + 1, *to = strchr(from, '\t');
      if(!to) continue;
      *to++ = 0;
      FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
      if(a && b) {
        char buf[8192];
        size_t k;
        while((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b);
      }
      if(b) fclose(b);
      if(a) fclose(a);
      chmod(to, 0666);
      continue;
    }
    if(line[0] == 'V') {
      char *from = tab + 1, *to = strchr(from, '\t');
      if(!to) continue;
      *to++ = 0;
      /* its folder first */
      char dir[1200];
      snprintf(dir, sizeof dir, "%s", to);
      char *sl = strrchr(dir, '/');
      if(sl) {
        *sl = 0;
        for(char *c = dir + 1; *c; c++)
          if(*c == '/') {
            *c = 0;
            mkdir(dir, 0777);
            *c = '/';
          }
        mkdir(dir, 0777);
      }
      int r = move_file(from, to);
      fprintf(out, "R\t%s\t%s\t%d\n", from, to, r);
      if(r) notify("Jewel Case: couldn't move %s", from);
      continue;
    }
    if(line[0] == 'M') {
      /* make a folder Jewel Case can't make from inside its sandbox */
      char *p = tab + 1;
      for(char *c = p + 1; *c; c++)
        if(*c == '/') {
          *c = 0;
          mkdir(p, 0777);
          *c = '/';
        }
      mkdir(p, 0777);
      chmod(p, 0777);
      continue;
    }
    int depth = atoi(line);
    const char *path = tab + 1;
    int app_levels = depth < 0 ? 2 : 0;
    if(depth < 0) depth = -depth;
    struct stat st;
    if(stat(path, &st) || !S_ISDIR(st.st_mode)) continue;
    fprintf(out, "D\t%s\n", path);
    walk(out, path, depth, app_levels);
  }
  fclose(req);
  prospero_folder(out);   /* after any moves, so they're listed where they are now */
  fprintf(out, "end\n");
  fclose(out);
  chmod(INDEX_TMP, 0666);
  if(rename(INDEX_TMP, INDEX)) {
    notify("Jewel Case: can't write " INDEX);
    return 1;
  }
  return 0;
}
