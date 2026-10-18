/* Jewel Case cover-art helper.
 *
 * Jewel Case writes /data/jewelcase-art.txt and sends this payload to the ELF
 * loader on port 9021. For each game it finds the box art in the libretro
 * thumbnail collection (github.com/libretro-thumbnails) and saves it where
 * Jewel Case looks for it. Jewel Case scales the images itself.
 *
 * Request file:
 *   jewelcase-art 1
 *   G<TAB>repo<TAB>output path without extension<TAB>ROM file name without extension
 *   L<TAB>repo<TAB>output path without extension<TAB>ROM file name without extension
 *   T<TAB>-<TAB>output path without extension<TAB>disc image path
 *   D<TAB>LaunchBox platform<TAB>output path without extension<TAB>ROM name[<TAB>disc image path]
 * G is the box art (Named_Boxarts), L the game's title logo (Named_Logos), T a
 * disc scan from GameTDB (art.gametdb.com) for GameCube and Wii games, found by
 * the game ID in the disc image's header. D a disc scan from the LaunchBox
 * Games Database (jc-lbdb.c), then from GameTDB when a disc image path is given.
 *
 * For each line: <output>.png when found, <output>.none when not (so it isn't
 * looked for again). Progress: /data/jewelcase/covers/status.txt
 *   running <done> <total> <time>  /  done <found> <total> <time>  /  error <text>
 * Every step is also written to /data/jewelcase/covers/log.txt as it happens,
 * so the log survives if the console goes down.
 *
 * Memory stays small on purpose: downloads go straight to disk in 32 KB
 * pieces, and the collection file lists are read as they arrive, never held
 * whole (a payload process can't afford whole images in memory).
 *
 * Names are matched against each collection's file list (from the GitHub API,
 * kept in /data/jewelcase/covers/<repo>.idx), ignoring region tags, case and
 * punctuation, preferring USA, then World, then Europe releases. Without the
 * list (no API access) a few likely names are tried directly.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef DATA
#define DATA "/data"
#endif
#define REQUEST_PATH DATA "/jewelcase-art.txt"
#define COVER_DIR DATA "/jewelcase/covers"
#define STATUS_PATH COVER_DIR "/status.txt"
#define LOG_PATH COVER_DIR "/log.txt"
#define PID_PATH COVER_DIR "/jc-art.pid"
#define AGENT "JewelCase/1.0 (PS5 homebrew launcher)"
#define MAX_IMAGE (12 << 20)    /* skip anything bigger: not a cover */
#define CHUNK 32768

/* ------------------------------------------------------------------------ */
/* log                                                                       */
/* ------------------------------------------------------------------------ */

static FILE *g_log;

static void logl(const char *fmt, ...) {
  if(!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(g_log, "%ld ", (long)time(NULL));
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
  fsync(fileno(g_log));
}

/* ------------------------------------------------------------------------ */
/* HTTP(S) GET, streamed to a callback                                       */
/* ------------------------------------------------------------------------ */

/* return non-zero to stop the download */
typedef int (*sink_fn)(void *ctx, const char *data, size_t n);

#ifdef JC_HOST
/* host testing: curl does the fetching */
static int http_init(void) { return 0; }
static void http_fini(void) {}
static int http_get(const char *url, sink_fn sink, void *ctx) {
  char tmp[] = "/tmp/jcart-XXXXXX";
  int fd = mkstemp(tmp);
  if(fd < 0) return -1;
  close(fd);
  char cmd[4096];
  snprintf(cmd, sizeof cmd, "curl -sS -L -m 30 -A '%s' -o '%s' -w '%%{http_code}' '%s'", AGENT, tmp, url);
  FILE *p = popen(cmd, "r");
  int st = -1;
  if(p) {
    if(fscanf(p, "%d", &st) != 1) st = -1;
    pclose(p);
  }
  if(st == 0 && !strncmp(url, "file://", 7)) st = access(url + 7, R_OK) == 0 ? 200 : 404;   /* local tests */
  if(st == 200) {
    FILE *f = fopen(tmp, "rb");
    char buf[CHUNK];
    size_t n;
    while(f && (n = fread(buf, 1, sizeof buf, f)) > 0)
      if(sink(ctx, buf, n)) { st = -2; break; }
    if(f) fclose(f);
  }
  unlink(tmp);
  return st;
}
#else
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char*, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void*, size_t);
int sceHttp2GetStatusCode(int, int*);
int sceHttp2ReadData(int, void*, size_t);

static int g_pool = -1, g_ssl = -1, g_http = -1, g_tmpl = -1;

static int http_init(void) {
  sceNetInit();
  if((g_pool = sceNetPoolCreate("jc-art", 32 * 1024, 0)) < 0) return -1;
  if((g_ssl = sceSslInit(256 * 1024)) < 0) return -2;
  if((g_http = sceHttp2Init(g_pool, g_ssl, 256 * 1024, 1)) < 0) return -3;
  if((g_tmpl = sceHttp2CreateTemplate(g_http, AGENT, 3, 1)) < 0) return -4;
  return 0;
}

static void http_fini(void) {
  if(g_tmpl >= 0) sceHttp2DeleteTemplate(g_tmpl);
  if(g_http >= 0) sceHttp2Term(g_http);
  if(g_ssl >= 0) sceSslTerm(g_ssl);
  if(g_pool >= 0) sceNetPoolDestroy(g_pool);
}

static char g_chunk[CHUNK];

static int http_get(const char *url, sink_fn sink, void *ctx) {
  int req = sceHttp2CreateRequestWithURL(g_tmpl, "GET", url, 0);
  if(req < 0) return -1;
  int status = -1;
  if(sceHttp2SendRequest(req, NULL, 0) || sceHttp2GetStatusCode(req, &status)) {
    sceHttp2DeleteRequest(req);
    return -1;
  }
  if(status == 200) {
    int n;
    while((n = sceHttp2ReadData(req, g_chunk, sizeof g_chunk)) > 0) {
      if(sink(ctx, g_chunk, (size_t)n)) {
        status = -2;
        break;
      }
    }
    if(n < 0) status = -3;
  }
  sceHttp2DeleteRequest(req);
  return status;
}
#endif

/* ------------------------------------------------------------------------ */
/* small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void notify(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
#ifdef JC_HOST
  printf("notify: %s\n", msg);
#else
  struct { char pad[45]; char message[3075]; } req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  int sceKernelSendNotificationRequest(int, void*, size_t, int);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
}

static void status(const char *fmt, ...) {
  char tmp[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  FILE *f = fopen(STATUS_PATH ".tmp", "w");
  if(!f) return;
  fprintf(f, "%s\n", tmp);
  fclose(f);
  rename(STATUS_PATH ".tmp", STATUS_PATH);
}

static void mkdirs(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", path);
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  mkdir(tmp, 0777);
}

static void parent_dirs(const char *file) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", file);
  char *s = strrchr(tmp, '/');
  if(s) {
    *s = 0;
    mkdirs(tmp);
  }
}

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static void touch(const char *p) {
  FILE *f = fopen(p, "w");
  if(f) fclose(f);
}

/* libretro file names: the characters in the string below become _ */
static void libretro_name(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for(; *in && o + 1 < cap; in++) out[o++] = strchr("&*/:`<>?\\|\"", *in) ? '_' : *in;
  out[o] = 0;
}

static void url_encode(const char *in, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for(const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
    if(isalnum(*p) || strchr("-_.~", *p)) out[o++] = (char)*p;
    else {
      out[o++] = '%';
      out[o++] = hex[*p >> 4];
      out[o++] = hex[*p & 15];
    }
  }
  out[o] = 0;
}

/* lower-case letters and digits only; bracketed tags, "the", "and" and "a"
 * dropped, common accented letters folded */
static void norm(const char *in, char *out, size_t cap) {
  char words[512];
  size_t w = 0;
  int depth = 0;
  for(const unsigned char *p = (const unsigned char *)in; *p && w + 2 < sizeof words; p++) {
    unsigned c = *p;
    if(c == '(' || c == '[') { depth++; continue; }
    if(c == ')' || c == ']') { if(depth) depth--; continue; }
    if(depth) continue;
    if(c == 0xC3 && p[1]) {   /* UTF-8 Latin-1 range */
      unsigned d = p[1] + 0x40;
      p++;
      if(d >= 0xe0 && d <= 0xe5) c = 'a';
      else if(d >= 0xe8 && d <= 0xeb) c = 'e';
      else if(d >= 0xec && d <= 0xef) c = 'i';
      else if(d >= 0xf2 && d <= 0xf6) c = 'o';
      else if(d >= 0xf9 && d <= 0xfc) c = 'u';
      else if(d == 0xf1) c = 'n';
      else continue;
    }
    if(c >= 0x80) continue;
    words[w++] = isalnum(c) ? (char)tolower(c) : ' ';
  }
  words[w] = 0;
  size_t o = 0;
  char *save = NULL;
  for(char *t = strtok_r(words, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
    if(!strcmp(t, "the") || !strcmp(t, "and") || !strcmp(t, "a")) continue;
    for(; *t && o + 1 < cap; t++) out[o++] = *t;
  }
  out[o] = 0;
}

static int region_score(const char *name, const char *rom) {
  int s = 0;
  if(strstr(name, "(USA)")) s += 60;
  else if(strstr(name, "(USA, ")) s += 55;
  else if(strstr(name, "(World")) s += 50;
  else if(strstr(name, "USA")) s += 45;
  else if(strstr(name, "(Europe")) s += 25;
  else if(strstr(name, "(Japan")) s += 8;
  if(strstr(name, "(Beta") || strstr(name, "(Proto") || strstr(name, "(Demo") ||
     strstr(name, "(Sample") || strstr(name, "(Kiosk") || strstr(name, "Pirate")) s -= 40;
  if(strstr(name, "(Rev ")) s -= 1;
  /* tags the ROM name carries itself */
  const char *p = rom;
  while((p = strchr(p, '('))) {
    const char *q = strchr(p, ')');
    if(!q) break;
    char tag[96];
    size_t n = (size_t)(q - p + 1);
    if(n < sizeof tag) {
      memcpy(tag, p, n);
      tag[n] = 0;
      if(strstr(name, tag)) s += 30;
    }
    p = q + 1;
  }
  return s;
}

/* ------------------------------------------------------------------------ */
/* the collection's file list                                                */
typedef struct {
  char repo[128];
  char folder[32];
  char **names;   /* without .png */
  char **norms;
  int n;
} Index;

static Index g_ix[48];
static int g_nix;

/* small responses (the repository's top folder): kept in memory, max 256 KB */
typedef struct {
  char data[262144];
  size_t len;
} Small;

static int small_sink(void *ctx, const char *d, size_t n) {
  Small *s = ctx;
  if(s->len + n + 1 > sizeof s->data) return 1;
  memcpy(s->data + s->len, d, n);
  s->len += n;
  s->data[s->len] = 0;
  return 0;
}

/* the folder's file list, read as it arrives: every "path":"....png" value is
 * written to the .idx file, one name per line */
typedef struct {
  FILE *out;
  int state;            /* 0 looking for the key, 1 in the value */
  int matched;          /* chars of the key matched so far */
  int esc;
  char name[512];
  size_t nl;
  int count;
} TreeParse;

static const char KEY[] = "\"path\":\"";

static void tree_emit(TreeParse *t) {
  t->name[t->nl] = 0;
  if(t->nl > 4 && !strcasecmp(t->name + t->nl - 4, ".png")) {
    t->name[t->nl - 4] = 0;
    fprintf(t->out, "%s\n", t->name);
    t->count++;
  }
}

static int tree_sink(void *ctx, const char *d, size_t n) {
  TreeParse *t = ctx;
  for(size_t i = 0; i < n; i++) {
    char c = d[i];
    if(t->state == 0) {
      if(c == KEY[t->matched]) {
        if(!KEY[++t->matched]) {
          t->state = 1;
          t->nl = 0;
          t->esc = 0;
          t->matched = 0;
        }
      } else {
        t->matched = (c == KEY[0]) ? 1 : 0;
      }
    } else {
      if(t->esc) {
        /* \uXXXX names are rare in these collections: keep the escape as-is
         * (they won't match and the likely-name fallback is used) */
        if(t->nl + 2 < sizeof t->name) {
          if(c == '"' || c == '\\' || c == '/') t->name[t->nl++] = c;
          else { t->name[t->nl++] = '\\'; t->name[t->nl++] = c; }
        }
        t->esc = 0;
      } else if(c == '\\') {
        t->esc = 1;
      } else if(c == '"') {
        tree_emit(t);
        t->state = 0;
      } else if(t->nl + 1 < sizeof t->name) {
        t->name[t->nl++] = c;
      }
    }
  }
  return 0;
}

static int load_index_file(Index *ix, const char *path) {
  FILE *f = fopen(path, "r");
  if(!f) return -1;
  int cap = 1024;
  ix->names = malloc(sizeof(char *) * cap);
  ix->n = 0;
  char line[600];
  while(ix->names && fgets(line, sizeof line, f)) {
    size_t l = strlen(line);
    while(l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    if(!l) continue;
    if(ix->n == cap) {
      cap *= 2;
      char **nn = realloc(ix->names, sizeof(char *) * cap);
      if(!nn) break;
      ix->names = nn;
    }
    if(!(ix->names[ix->n] = strdup(line))) break;
    ix->n++;
  }
  fclose(f);
  if(!ix->names || ix->n <= 0) return -1;
  ix->norms = malloc(sizeof(char *) * ix->n);
  if(!ix->norms) return -1;
  for(int i = 0; i < ix->n; i++) {
    char nb[512];
    norm(ix->names[i], nb, sizeof nb);
    ix->norms[i] = strdup(nb);
    if(!ix->norms[i]) ix->norms[i] = "";
  }
  return 0;
}

static Small g_small;

static Index *get_index(const char *repo, const char *folder) {
  for(int i = 0; i < g_nix; i++)
    if(!strcmp(g_ix[i].repo, repo) && !strcmp(g_ix[i].folder, folder)) return g_ix[i].n ? &g_ix[i] : NULL;
  if(g_nix >= 48) return NULL;
  Index *ix = &g_ix[g_nix++];
  memset(ix, 0, sizeof *ix);
  snprintf(ix->repo, sizeof ix->repo, "%s", repo);
  snprintf(ix->folder, sizeof ix->folder, "%s", folder);

  char path[600];
  if(!strcmp(folder, "Named_Boxarts")) snprintf(path, sizeof path, COVER_DIR "/%s.idx", repo);
  else snprintf(path, sizeof path, COVER_DIR "/%s.%s.idx", repo, folder);
  if(load_index_file(ix, path) == 0) {
    logl("index %s/%s: %d names (saved)", repo, folder, ix->n);
    return ix;
  }
  ix->n = 0;

  /* 1. the repository's top folder, for the folder's id */
  char url[1024];
  g_small.len = 0;
  g_small.data[0] = 0;
  snprintf(url, sizeof url, "https://api.github.com/repos/libretro-thumbnails/%s/git/trees/master", repo);
  int st = http_get(url, small_sink, &g_small);
  char sha[64] = "";
  if(st == 200) {
    char key[64];
    snprintf(key, sizeof key, "\"path\":\"%s\"", folder);
    const char *p = strstr(g_small.data, key);
    if(p && (p = strstr(p, "\"sha\":\""))) {
      p += 7;
      size_t o = 0;
      while(*p && *p != '"' && o + 1 < sizeof sha) sha[o++] = *p++;
      sha[o] = 0;
    }
  }
  if(!sha[0]) {
    logl("index %s/%s: top folder failed (%d)", repo, folder, st);
    return NULL;
  }

  /* 2. that folder's file list, straight into the .idx file */
  char part[640];
  snprintf(part, sizeof part, "%s.part", path);
  TreeParse tp;
  memset(&tp, 0, sizeof tp);
  if(!(tp.out = fopen(part, "w"))) return NULL;
  snprintf(url, sizeof url, "https://api.github.com/repos/libretro-thumbnails/%s/git/trees/%s", repo, sha);
  st = http_get(url, tree_sink, &tp);
  fclose(tp.out);
  if(st != 200 || tp.count == 0) {
    unlink(part);
    logl("index %s/%s: list failed (%d, %d names)", repo, folder, st, tp.count);
    return NULL;
  }
  rename(part, path);
  if(load_index_file(ix, path) != 0) {
    ix->n = 0;
    return NULL;
  }
  logl("index %s/%s: %d names (downloaded)", repo, folder, ix->n);
  return ix;
}

/* best file in the list for a ROM name, or NULL */
static const char *match(Index *ix, const char *rom) {
  char nr[512];
  norm(rom, nr, sizeof nr);
  size_t lr = strlen(nr);
  if(!lr) return NULL;
  const char *best = NULL;
  int best_score = -100000;
  for(int i = 0; i < ix->n; i++) {
    if(!strcmp(ix->names[i], rom)) return ix->names[i];   /* exact */
    const char *nc = ix->norms[i];
    int score;
    if(!strcmp(nc, nr)) {
      score = 1000;
    } else {
      size_t lc = strlen(nc);
      size_t sh = lc < lr ? lc : lr, lo = lc < lr ? lr : lc;
      /* one name starts with the other ("pokemonemerald" / "pokemonemeraldversion") */
      if(sh < 6 || sh * 10 < lo * 6) continue;
      if(strncmp(nc, nr, sh)) continue;
      score = 500 - (int)(lo - sh) * 4;
    }
    score += region_score(ix->names[i], rom);
    if(score > best_score) {
      best_score = score;
      best = ix->names[i];
    }
  }
  return best;
}

/* ------------------------------------------------------------------------ */
/* download to disk                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
  FILE *f;
  size_t total;
  int checked, kind;    /* kind: 1 PNG, 2 JPEG, 3 text (a symlink's target) */
  char text[400];
  size_t tl;
} FileSink;

static int file_sink(void *ctx, const char *d, size_t n) {
  FileSink *s = ctx;
  if(!s->checked) {
    s->checked = 1;
    if(n >= 4 && !memcmp(d, "\x89PNG", 4)) s->kind = 1;
    else if(n >= 3 && (unsigned char)d[0] == 0xff && (unsigned char)d[1] == 0xd8) s->kind = 2;
    else s->kind = 3;
  }
  if(s->kind == 3) {
    /* the collections use symlinks for shared art: raw.githubusercontent.com
     * answers with the link's target name instead of the image */
    if(s->tl + n + 1 > sizeof s->text) return 1;
    memcpy(s->text + s->tl, d, n);
    s->tl += n;
    s->text[s->tl] = 0;
    return 0;
  }
  s->total += n;
  if(s->total > MAX_IMAGE) return 1;
  return fwrite(d, 1, n, s->f) != n;
}

/* 0 = saved as <out>.png / .jpg */
static int fetch_named(const char *repo, const char *folder, const char *name, const char *out, int hops) {
  char lr[600], enc[1800], url[2400], part[1100];
  libretro_name(name, lr, sizeof lr);
  url_encode(lr, enc, sizeof enc);
  snprintf(part, sizeof part, "%s.part", out);

  FileSink fs;
  memset(&fs, 0, sizeof fs);
  if(!(fs.f = fopen(part, "wb"))) return -1;
  snprintf(url, sizeof url, "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/%s/%s.png",
           repo, folder, enc);
  int st = http_get(url, file_sink, &fs);
  if(st != 200 && st != -2 && !fs.total) {
    /* the libretro thumbnail server, plain HTTP, as RetroArch uses it */
    char sys[200], senc[600];
    size_t o = 0;
    for(const char *p = repo; *p && o + 4 < sizeof sys; p++) {
      if(!strncmp(p, "_-_", 3)) { memcpy(sys + o, " - ", 3); o += 3; p += 2; }
      else sys[o++] = *p == '_' ? ' ' : *p;
    }
    sys[o] = 0;
    url_encode(sys, senc, sizeof senc);
    snprintf(url, sizeof url, "http://thumbnails.libretro.com/%s/%s/%s.png", senc, folder, enc);
    rewind(fs.f);
    fs.total = fs.tl = 0;
    fs.checked = fs.kind = 0;
    st = http_get(url, file_sink, &fs);
  }
  fclose(fs.f);
  if(st == 200 && fs.kind == 3) {
    unlink(part);
    size_t l = fs.tl;
    while(l && (fs.text[l - 1] == '\n' || fs.text[l - 1] == '\r')) fs.text[--l] = 0;
    logl("get %s/%s/%s: link to %s", repo, folder, name, fs.text);
    if(hops < 3 && l > 4 && !strcasecmp(fs.text + l - 4, ".png") && !strchr(fs.text, '/') && !strchr(fs.text, '<')) {
      fs.text[l - 4] = 0;
      return fetch_named(repo, folder, fs.text, out, hops + 1);
    }
    return -1;
  }
  logl("get %s/%s/%s: %d, %lu bytes", repo, folder, name, st, (unsigned long)fs.total);
  if(st != 200 || fs.total < 100 || (fs.kind != 1 && fs.kind != 2)) {
    unlink(part);
    return -1;
  }
  char final[1100];
  snprintf(final, sizeof final, "%s.%s", out, fs.kind == 2 ? "jpg" : "png");
  return rename(part, final);
}

static int find_cover(const char *repo, const char *folder, const char *rom, const char *out) {
  Index *ix = get_index(repo, folder);
  if(ix) {
    const char *name = match(ix, rom);
    if(!name) logl("no match for %s in %s/%s", rom, repo, folder);
    return name ? fetch_named(repo, folder, name, out, 0) : -1;
  }
  /* no file list: try the likely names */
  static const char *suffix[] = {"", " (USA)", " (World)", " (USA, Europe)", " (Europe)"};
  for(size_t i = 0; i < sizeof suffix / sizeof *suffix; i++) {
    if(i && strchr(rom, '(')) break;   /* the ROM already names its region */
    char name[700];
    snprintf(name, sizeof name, "%s%s", rom, suffix[i]);
    if(fetch_named(repo, folder, name, out, 0) == 0) return 0;
  }
  return -1;
}

/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* GameTDB disc scans (GameCube and Wii)                                     */
/* ------------------------------------------------------------------------ */

/* the 6-character game ID from a disc image's header: plain ISO/GCM/NKit,
 * WBFS, GameCube CISO, and Dolphin's RVZ/WIA (which keep a copy of the disc
 * header uncompressed). GCZ is compressed throughout and has no ID to read. */
static int disc_id(const char *path, char id[7]) {
  char rp[1200];
#ifdef JC_HOST
  if(!strncmp(path, "/data/", 6)) snprintf(rp, sizeof rp, "%s%s", DATA, path + 5);
  else snprintf(rp, sizeof rp, "%s", path);
#else
  snprintf(rp, sizeof rp, "%s", path);
#endif
  FILE *f = fopen(rp, "rb");
  if(!f) return -1;
  unsigned char h[8];
  long off = 0;
  if(fread(h, 1, sizeof h, f) == sizeof h) {
    if(!memcmp(h, "WBFS", 4)) off = 0x200;
    else if(!memcmp(h, "CISO", 4)) off = 0x8000;
    else if(!memcmp(h, "WIA\x01", 4) || !memcmp(h, "RVZ\x01", 4)) off = 0x58;
  }
  int ok = 0;
  if(fseek(f, off, SEEK_SET) == 0 && fread(id, 1, 6, f) == 6) {
    ok = 1;
    for(int i = 0; i < 6; i++)
      if(!isupper((unsigned char)id[i]) && !isdigit((unsigned char)id[i])) ok = 0;
  }
  fclose(f);
  id[6] = 0;
  return ok ? 0 : -1;
}

/* GameTDB's language folders to try, from the ID's region letter */
static const char *const *tdb_regions(char r) {
  static const char *const us[] = {"US", "EN", NULL}, *const ja[] = {"JA", NULL}, *const en[] = {"EN", "US", NULL},
                           *const de[] = {"DE", "EN", NULL}, *const fr[] = {"FR", "EN", NULL},
                           *const es[] = {"ES", "EN", NULL}, *const it[] = {"IT", "EN", NULL},
                           *const ko[] = {"KO", "EN", NULL}, *const nl[] = {"NL", "EN", NULL};
  switch(r) {
  case 'E': case 'N': return us;
  case 'J': return ja;
  case 'D': return de;
  case 'F': return fr;
  case 'S': return es;
  case 'I': return it;
  case 'K': case 'Q': case 'T': return ko;
  case 'H': return nl;
  default: return en;
  }
}

/* 0 = saved, -1 = GameTDB has none, -2 = couldn't ask (try again next time) */
static int find_disc_scan(const char *disc, const char *out) {
  char id[7];
  if(disc_id(disc, id) != 0) {
    logl("no game ID in %s", disc);
    return -1;
  }
  char part[1100], final[1100], url[300];
  snprintf(part, sizeof part, "%s.part", out);
  int unreachable = 0;
  for(const char *const *lc = tdb_regions(id[3]); *lc; lc++) {
    FileSink fs;
    memset(&fs, 0, sizeof fs);
    if(!(fs.f = fopen(part, "wb"))) return -1;
    snprintf(url, sizeof url, "https://art.gametdb.com/wii/disc/%s/%s.png", *lc, id);
    int st = http_get(url, file_sink, &fs);
    fclose(fs.f);
    logl("gametdb %s %s: %d, %lu bytes", id, *lc, st, (unsigned long)fs.total);
    if(st == 200 && fs.kind == 1 && fs.total >= 100) {
      snprintf(final, sizeof final, "%s.png", out);
      return rename(part, final);
    }
    if(st != 404) unreachable = 1;
    unlink(part);
  }
  return unreachable ? -2 : -1;
}

#include "jc-lbdb.c"

/* ------------------------------------------------------------------------ */

typedef struct {
  char type;            /* G box art, L logo, T GameTDB disc scan, D LaunchBox disc scan */
  char repo[128];       /* D: the LaunchBox platform */
  char out[900];
  char rom[600];
  char disc[600];       /* D: the disc image, for GameTDB after LaunchBox */
} Job;

static int pending(const char *out) {
  char p[1000];
  static const char *ext[] = {"png", "jpg", "none"};
  for(int i = 0; i < 3; i++) {
    snprintf(p, sizeof p, "%s.%s", out, ext[i]);
    if(exists(p)) return 0;
  }
  return 1;
}

int main(void) {
  mkdirs(COVER_DIR);
  g_log = fopen(LOG_PATH, "w");
  logl("jc-art start (pid %d)", (int)getpid());

  /* one helper at a time */
  FILE *pf = fopen(PID_PATH, "r");
  if(pf) {
    int old = 0;
    if(fscanf(pf, "%d", &old) == 1 && old > 0 && old != getpid() && kill(old, 0) == 0) {
      fclose(pf);
      logl("another jc-art is running (pid %d): leaving", old);
      return 0;
    }
    fclose(pf);
  }
  pf = fopen(PID_PATH, "w");
  if(pf) {
    fprintf(pf, "%d\n", getpid());
    fclose(pf);
  }

  FILE *f = fopen(REQUEST_PATH, "r");
  if(!f) {
    status("error no request");
    logl("no request file");
    unlink(PID_PATH);
    return 1;
  }
  char line[2400];
  if(!fgets(line, sizeof line, f) || strcmp(line, "jewelcase-art 1\n")) {
    fclose(f);
    status("error unreadable request");
    logl("unreadable request");
    unlink(PID_PATH);
    return 1;
  }
  int cap = 256, n = 0;
  Job *jobs = malloc(sizeof(Job) * cap);
  while(jobs && fgets(line, sizeof line, f)) {
    size_t l = strlen(line);
    while(l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    if((line[0] != 'G' && line[0] != 'L' && line[0] != 'T' && line[0] != 'D') || line[1] != '\t') continue;
    char *a = line + 2, *b = strchr(a, '\t'), *c = b ? strchr(b + 1, '\t') : NULL;
    if(!b || !c) continue;
    *b++ = 0;
    *c++ = 0;
    if(n == cap) {
      cap *= 2;
      Job *nj = realloc(jobs, sizeof(Job) * cap);
      if(!nj) break;
      jobs = nj;
    }
    jobs[n].type = line[0];
    snprintf(jobs[n].repo, sizeof jobs[n].repo, "%s", a);
    snprintf(jobs[n].out, sizeof jobs[n].out, "%s", b);
    char *e = strchr(c, '\t');
    if(e) *e++ = 0;
    snprintf(jobs[n].rom, sizeof jobs[n].rom, "%s", c);
    snprintf(jobs[n].disc, sizeof jobs[n].disc, "%s", e ? e : "");
    n++;
  }
  fclose(f);
  unlink(REQUEST_PATH);
  logl("%d games in the request", n);

  int err = http_init();
  if(err) {
    status("error network (%d)", err);
    logl("network start failed (%d)", err);
    http_fini();
    unlink(PID_PATH);
    return 1;
  }

  /* LaunchBox: its index first, if any disc still needs looking for */
  char want[LB_MAXPLAT][48];
  int nwant = 0, need_lb = 0;
  for(int i = 0; i < n; i++) {
    if(jobs[i].type != 'D' || !pending(jobs[i].out)) continue;
    need_lb = 1;
    int seen = 0;
    for(int k = 0; k < nwant; k++) if(plat_eq(want[k], jobs[i].repo)) seen = 1;
    if(!seen && nwant < LB_MAXPLAT) snprintf(want[nwant++], sizeof want[0], "%s", jobs[i].repo);
  }
  if(need_lb && lb_ready(want, nwant) != 0) logl("launchbox: no index, discs fall back to GameTDB / the printed design");

  int found = 0, tried = 0;
  for(int i = 0; i < n; i++) {
    status("running %d %d %ld", i, n, (long)time(NULL));
    char p[1000];
    snprintf(p, sizeof p, "%s.png", jobs[i].out);
    if(exists(p)) continue;
    snprintf(p, sizeof p, "%s.jpg", jobs[i].out);
    if(exists(p)) continue;
    snprintf(p, sizeof p, "%s.none", jobs[i].out);
    if(exists(p)) continue;
    parent_dirs(p);
    tried++;
    int r;
    if(jobs[i].type == 'T') {
      r = find_disc_scan(jobs[i].rom, jobs[i].out);
    } else if(jobs[i].type == 'D') {
      r = find_lb_disc(jobs[i].repo, jobs[i].rom, jobs[i].out);
      if(r != 0 && jobs[i].disc[0]) {
        int r2 = find_disc_scan(jobs[i].disc, jobs[i].out);
        r = r2 == 0 ? 0 : (r == -2 || r2 == -2) ? -2 : -1;
      }
    }
    else r = find_cover(jobs[i].repo, jobs[i].type == 'L' ? "Named_Logos" : "Named_Boxarts", jobs[i].rom, jobs[i].out);
    if(r == 0) {
      if(jobs[i].type == 'G') found++;
    } else if(r != -2) {
      touch(p);
    }
    usleep(50000);   /* gentle on the network and the servers */
  }
  status("done %d %d %ld", found, n, (long)time(NULL));
  logl("done: %d found of %d tried", found, tried);
  if(found) notify("Jewel Case: downloaded %d cover%s", found, found == 1 ? "" : "s");
  http_fini();
  unlink(PID_PATH);
  if(g_log) fclose(g_log);
  return 0;
}
