/* LaunchBox Games Database: game info, disc scans and box art, for jc-art.c
 * (included from it).
 *
 * LaunchBox publishes its whole database as one big file (Metadata.zip). Jewel
 * Case doesn't download that: tools/launchbox_pack.py keeps only the systems and
 * fields it uses, one small file per system, shipped in the app. Jewel Case
 * copies the ones for the systems you have games for to COVER_DIR/lb/<slug>.lbz
 * (raw deflate); the first time one is needed here it is unpacked next to it as
 * <slug>.txt:
 *   #jc-lb 1<TAB>platform<TAB>database date
 *   name, year, genres, developer, publisher, players, disc image file,
 *   box front image file, overview     (tab separated, "-" for no image)
 * An image is at images.launchbox-app.com/<file>; only those for your games
 * are downloaded.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../third_party/miniz/miniz_tinfl.c"   /* the vendored miniz, inflate only */

#define LB_IMG "https://images.launchbox-app.com/"
#define LB_DIR COVER_DIR "/lb"

/* the PC test build can point these elsewhere (curl takes file:// URLs) */
static const char *lb_url(const char *env, const char *def) {
#ifdef JC_HOST
  const char *e = getenv(env);
  if(e) return e;
#endif
  (void)env;
  return def;
}

/* a platform's file name: its letters and digits, lower case (as in main.c) */
static void lb_slug(const char *p, char *out, size_t cap) {
  size_t o = 0;
  for(; *p && o + 1 < cap; p++)
    if((unsigned char)*p < 0x80 && isalnum((unsigned char)*p)) out[o++] = (char)tolower((unsigned char)*p);
  out[o] = 0;
}

/* ------------------------------------------------------------------------ */
/* one platform's games, in memory                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
  char slug[64];           /* the platform loaded, "" none */
  int missing;             /* 1: no data for it */
  char **enorm, **edisc, **ebox;   /* image NULL: none */
  long *eoff;
  int n, cap;
} LbPlat;

static LbPlat g_lb;

static void lb_free(void) {
  for(int i = 0; i < g_lb.n; i++) {
    free(g_lb.enorm[i]);
    free(g_lb.edisc[i]);
    free(g_lb.ebox[i]);
  }
  free(g_lb.enorm);
  free(g_lb.edisc);
  free(g_lb.ebox);
  free(g_lb.eoff);
  memset(&g_lb, 0, sizeof g_lb);
}

/* <slug>.lbz -> <slug>.txt. 0 = ok */
static int lb_unpack(const char *lbz, const char *txt) {
  FILE *in = fopen(lbz, "rb");
  if(!in) return -1;
  char part[600];
  snprintf(part, sizeof part, "%s.part", txt);
  FILE *out = fopen(part, "wb");
  if(!out) {
    fclose(in);
    return -1;
  }
  static tinfl_decompressor inf;
  static unsigned char dict[TINFL_LZ_DICT_SIZE], buf[CHUNK];
  tinfl_init(&inf);
  size_t dict_ofs = 0, have = 0, pos = 0;
  unsigned long long total = 0;
  int eof = 0, ok = 0;
  for(;;) {
    if(pos == have && !eof) {
      have = fread(buf, 1, sizeof buf, in);
      pos = 0;
      if(have < sizeof buf) eof = 1;
    }
    size_t nin = have - pos, nout = TINFL_LZ_DICT_SIZE - dict_ofs;
    tinfl_status st = tinfl_decompress(&inf, buf + pos, &nin, dict, dict + dict_ofs, &nout,
                                       eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    pos += nin;
    if(nout && fwrite(dict + dict_ofs, 1, nout, out) != nout) break;
    total += nout;
    dict_ofs = (dict_ofs + nout) & (TINFL_LZ_DICT_SIZE - 1);
    if(st == TINFL_STATUS_DONE) {
      ok = 1;
      break;
    }
    if(st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && eof && pos == have)) break;
  }
  fclose(in);
  if(fclose(out) != 0) ok = 0;
  logl("launchbox: unpacked %s, %llu KB%s", lbz, total >> 10, ok ? "" : " (damaged)");
  if(!ok || rename(part, txt) != 0) {
    unlink(part);
    return -1;
  }
  return 0;
}

static char *lb_dup_img(const char *s) { return strcmp(s, "-") && *s ? strdup(s) : NULL; }

/* the games of one platform. 0 = loaded, -1 = no data for it */
static int lb_load(const char *platform) {
  char slug[64];
  lb_slug(platform, slug, sizeof slug);
  if(!strcmp(g_lb.slug, slug)) return g_lb.missing ? -1 : 0;
  lb_free();
  snprintf(g_lb.slug, sizeof g_lb.slug, "%s", slug);
  char txt[400], lbz[400];
  snprintf(txt, sizeof txt, LB_DIR "/%s.txt", slug);
  snprintf(lbz, sizeof lbz, LB_DIR "/%s.lbz", slug);
  FILE *f = NULL;
  struct stat sl, st;
  /* a newer copy from the app (Jewel Case removes the old .txt then too) */
  if(stat(lbz, &sl) == 0 && (stat(txt, &st) != 0 || sl.st_mtime > st.st_mtime)) lb_unpack(lbz, txt);
  f = fopen(txt, "r");
  static char line[4096];
  if(!f || !fgets(line, sizeof line, f) || strncmp(line, "#jc-lb 1\t", 9)) {
    if(f) fclose(f);
    logl("launchbox: no data for %s (%s)", platform, txt);
    g_lb.missing = 1;
    return -1;
  }
  long off = ftell(f);
  while(fgets(line, sizeof line, f)) {
    long here = off;
    off = ftell(f);
    char *fld[9];
    int nf = 0;
    for(char *p = line; nf < 9;) {
      fld[nf++] = p;
      char *t = strchr(p, '\t');
      if(!t) break;
      *t = 0;
      p = t + 1;
    }
    if(nf < 9 || !fld[0][0]) continue;
    if(g_lb.n == g_lb.cap) {
      int nc = g_lb.cap ? g_lb.cap * 2 : 1024;
      char **a = realloc(g_lb.enorm, sizeof(char *) * nc);
      if(a) g_lb.enorm = a;
      char **b = realloc(g_lb.edisc, sizeof(char *) * nc);
      if(b) g_lb.edisc = b;
      char **c = realloc(g_lb.ebox, sizeof(char *) * nc);
      if(c) g_lb.ebox = c;
      long *o = realloc(g_lb.eoff, sizeof(long) * nc);
      if(o) g_lb.eoff = o;
      if(!a || !b || !c || !o) break;
      g_lb.cap = nc;
    }
    char nb[512];
    norm(fld[0], nb, sizeof nb);
    g_lb.enorm[g_lb.n] = strdup(nb);
    g_lb.edisc[g_lb.n] = lb_dup_img(fld[6]);
    g_lb.ebox[g_lb.n] = lb_dup_img(fld[7]);
    g_lb.eoff[g_lb.n] = here;
    if(!g_lb.enorm[g_lb.n]) break;
    g_lb.n++;
  }
  fclose(f);
  logl("launchbox: %s, %d games", platform, g_lb.n);
  return 0;
}

/* the entry for a ROM on a platform: -1 none, -2 no data. want: 'D' disc, 'B' box,
 * 0 either (of two equal names, the one with that image wins) */
static int lb_match(const char *platform, const char *rom, int want) {
  if(lb_load(platform) != 0) return -2;
  char nr[512];
  norm(rom, nr, sizeof nr);
  size_t lr = strlen(nr);
  if(!lr) return -1;
  int best = -1, best_score = -1;
  for(int i = 0; i < g_lb.n; i++) {
    const char *nc = g_lb.enorm[i];
    int score;
    if(!strcmp(nc, nr)) {
      score = 1000;
    } else {
      size_t lc = strlen(nc);
      size_t sh = lc < lr ? lc : lr, lo = lc < lr ? lr : lc;
      if(sh < 6 || sh * 10 < lo * 7 || strncmp(nc, nr, sh)) continue;
      score = 500 - (int)(lo - sh) * 4;
    }
    if((want == 'D' && g_lb.edisc[i]) || (want == 'B' && g_lb.ebox[i])) score += 2;
    if(score > best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

/* a disc scan ('D') or the box front ('B') into <out>.png/.jpg.
 * 0 = saved, -1 = LaunchBox has none, -2 = couldn't ask */
static int find_lb_image(const char *platform, const char *rom, const char *out, int want) {
  int e = lb_match(platform, rom, want);
  if(e == -2) return -2;
  const char *file = e < 0 ? NULL : want == 'D' ? g_lb.edisc[e] : g_lb.ebox[e];
  if(!file) {
    logl("launchbox: no %s for %s (%s)", want == 'D' ? "disc" : "box", rom, platform);
    return -1;
  }
  char url[400], part[1100], enc[300];
  url_encode(file, enc, sizeof enc);
  snprintf(url, sizeof url, "%s%s", lb_url("JC_LB_IMG", LB_IMG), enc);
  snprintf(part, sizeof part, "%s.part", out);
  FileSink fs;
  memset(&fs, 0, sizeof fs);
  if(!(fs.f = fopen(part, "wb"))) return -2;
  int st = http_get(url, file_sink, &fs);
  fclose(fs.f);
  logl("launchbox: %s -> %s: %d, %lu bytes", rom, file, st, (unsigned long)fs.total);
  if(st == 200 && (fs.kind == 1 || fs.kind == 2) && fs.total >= 100) {
    char final[1100];
    snprintf(final, sizeof final, "%s.%s", out, fs.kind == 2 ? "jpg" : "png");
    return rename(part, final);
  }
  unlink(part);
  return st == 404 ? -1 : -2;
}

/* the game's info into <out>.txt: "Key: value" lines, a blank line, the overview.
 * 0 = saved, -1 = not in the database, -2 = no data */
static int find_lb_info(const char *platform, const char *rom, const char *out) {
  int e = lb_match(platform, rom, 0);
  if(e == -2) return -2;
  if(e < 0) {
    logl("launchbox: no info for %s (%s)", rom, platform);
    return -1;
  }
  char txt[400];
  snprintf(txt, sizeof txt, LB_DIR "/%s.txt", g_lb.slug);
  FILE *in = fopen(txt, "r");
  if(!in) return -2;
  static char line[4096];
  int ok = fseek(in, g_lb.eoff[e], SEEK_SET) == 0 && fgets(line, sizeof line, in);
  fclose(in);
  if(!ok) return -2;
  line[strcspn(line, "\r\n")] = 0;
  char *f[9] = {0};
  int nf = 0;
  for(char *p = line; nf < 9;) {
    f[nf++] = p;
    char *t = strchr(p, '\t');
    if(!t) break;
    *t = 0;
    p = t + 1;
  }
  if(nf < 9) return -1;
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.txt.part", out);
  FILE *o = fopen(tmp, "w");
  if(!o) return -2;
  static const char *keys[] = {"Name", "Year", "Genre", "Developer", "Publisher", "Players"};
  for(int i = 0; i < 6; i++)
    if(f[i][0]) fprintf(o, "%s: %s\n", keys[i], f[i]);
  fprintf(o, "\n%s\n", f[8]);
  fclose(o);
  char final[1100];
  snprintf(final, sizeof final, "%s.txt", out);
  return rename(tmp, final);
}
