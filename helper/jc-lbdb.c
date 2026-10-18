/* LaunchBox Games Database disc scans, for jc-art.c (included from it).
 *
 * LaunchBox publishes its whole database as one file, Metadata.zip
 * (gamesdb.launchbox-app.com/Metadata.zip). Its Metadata.xml lists every game
 * (<Game>: Name, DatabaseID, Platform) and every image (<GameImage>: DatabaseID,
 * FileName, Type, Region); an image is at images.launchbox-app.com/<FileName>.
 *
 * The zip is too big to keep, so it is read as it downloads: the zip entries
 * are inflated (miniz) and the XML parsed on the fly, keeping only the disc
 * images of the platforms asked for. The result is a small index,
 * COVER_DIR/launchbox.idx:
 *   #jc-lbdb 1<TAB>platform<TAB>platform...
 *   <platform index><TAB><normalised name><TAB><image file name>
 * built again when a platform is missing from it (or when Jewel Case deletes it).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../third_party/miniz/miniz_tinfl.c"   /* the vendored miniz, inflate only */

#define LB_URL "https://gamesdb.launchbox-app.com/Metadata.zip"
#define LB_IMG "https://images.launchbox-app.com/"
#define LB_IDX COVER_DIR "/launchbox.idx"
#define LB_FAIL COVER_DIR "/launchbox.fail"
#define LB_MAXPLAT 16

/* the PC test build can point these elsewhere (curl takes file:// URLs) */
static const char *lb_url(const char *env, const char *def) {
#ifdef JC_HOST
  const char *e = getenv(env);
  if(e) return e;
#endif
  (void)env;
  return def;
}

/* ------------------------------------------------------------------------ */
/* the index                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
  char plat[LB_MAXPLAT][48];
  int nplat;
  /* entries: platform, normalised name, file */
  unsigned char *eplat;
  char **enorm, **efile;
  int n;
} LbIndex;

static LbIndex g_lb;
static int g_lb_tried;   /* built or loaded this run (0 = not yet, 1 = ok, -1 = failed) */

/* platform names compared on letters and digits only, ignoring case */
static int plat_eq(const char *a, const char *b) {
  for(;;) {
    while(*a && !isalnum((unsigned char)*a)) a++;
    while(*b && !isalnum((unsigned char)*b)) b++;
    if(!*a || !*b) return !*a && !*b;
    if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    a++;
    b++;
  }
}

static int lb_plat_index(LbIndex *ix, const char *p) {
  for(int i = 0; i < ix->nplat; i++)
    if(plat_eq(ix->plat[i], p)) return i;
  return -1;
}

static void lb_free(LbIndex *ix) {
  for(int i = 0; i < ix->n; i++) {
    free(ix->enorm[i]);
    free(ix->efile[i]);
  }
  free(ix->eplat);
  free(ix->enorm);
  free(ix->efile);
  memset(ix, 0, sizeof *ix);
}

static int lb_load(LbIndex *ix) {
  lb_free(ix);
  FILE *f = fopen(LB_IDX, "r");
  if(!f) return -1;
  char line[700];
  if(!fgets(line, sizeof line, f) || strncmp(line, "#jc-lbdb 1", 10)) {
    fclose(f);
    return -1;
  }
  char *save = NULL;
  strtok_r(line, "\t\r\n", &save);
  for(char *t = strtok_r(NULL, "\t\r\n", &save); t && ix->nplat < LB_MAXPLAT; t = strtok_r(NULL, "\t\r\n", &save))
    snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", t);
  int cap = 0;
  while(fgets(line, sizeof line, f)) {
    char *a = line, *b = strchr(a, '\t'), *c = b ? strchr(b + 1, '\t') : NULL;
    if(!b || !c) continue;
    *b++ = 0;
    *c++ = 0;
    c[strcspn(c, "\r\n")] = 0;
    if(ix->n == cap) {
      cap = cap ? cap * 2 : 1024;
      unsigned char *np = realloc(ix->eplat, cap);
      if(np) ix->eplat = np;
      char **nn = realloc(ix->enorm, sizeof(char *) * cap);
      if(nn) ix->enorm = nn;
      char **nf = realloc(ix->efile, sizeof(char *) * cap);
      if(nf) ix->efile = nf;
      if(!np || !nn || !nf) break;
    }
    ix->eplat[ix->n] = (unsigned char)atoi(a);
    ix->enorm[ix->n] = strdup(b);
    ix->efile[ix->n] = strdup(c);
    if(!ix->enorm[ix->n] || !ix->efile[ix->n]) break;
    ix->n++;
  }
  fclose(f);
  return 0;
}

/* ------------------------------------------------------------------------ */
/* building it: zip -> inflate -> XML -> records                             */
/* ------------------------------------------------------------------------ */

typedef struct {
  int id;
  unsigned char plat;
  int score;
  char norm[60];
  char file[64];
} LbGame;

typedef struct {
  /* what to keep */
  LbIndex *ix;
  LbGame *games;
  int ngames, cap, sorted;
  int images;
  /* zip */
  int zstate;              /* 0 header, 1 name, 2 extra, 3 inflate, 4 stored skip, 5 descriptor, 6 end */
  unsigned char hdr[30];
  int hlen;
  int method, flags, namelen, extralen;
  unsigned long csize;
  unsigned long skip;
  char name[256];
  int nlen;
  int target, target_done;
  tinfl_decompressor inf;
  unsigned char dict[TINFL_LZ_DICT_SIZE];
  size_t dict_ofs;
  unsigned char desc[4];
  int dlen;
  unsigned long long xml_bytes, net_bytes;
  int error;
  /* XML */
  int xstate;              /* 0 text, 1 tag */
  char tag[300];
  int tlen;
  int depth;
  int rec;                 /* 0 none, 1 Game, 2 GameImage */
  char field[40];
  char text[520];
  int xlen;
  char r_name[300], r_plat[64], r_id[16], r_file[80], r_type[40], r_region[48];
} LbBuild;

static void xml_unescape(char *s) {
  char *o = s;
  for(char *p = s; *p;) {
    if(*p == '&') {
      if(!strncmp(p, "&amp;", 5)) { *o++ = '&'; p += 5; continue; }
      if(!strncmp(p, "&lt;", 4)) { *o++ = '<'; p += 4; continue; }
      if(!strncmp(p, "&gt;", 4)) { *o++ = '>'; p += 4; continue; }
      if(!strncmp(p, "&quot;", 6)) { *o++ = '"'; p += 6; continue; }
      if(!strncmp(p, "&apos;", 6)) { *o++ = '\''; p += 6; continue; }
      if(p[1] == '#') {
        char *e = strchr(p, ';');
        if(e && e - p < 10) {
          long v = p[2] == 'x' ? strtol(p + 3, NULL, 16) : strtol(p + 2, NULL, 10);
          if(v > 0 && v < 0x80) *o++ = (char)v;
          else if(v < 0x800) { *o++ = (char)(0xC0 | (v >> 6)); *o++ = (char)(0x80 | (v & 63)); }
          p = e + 1;
          continue;
        }
      }
    }
    *o++ = *p++;
  }
  *o = 0;
}

static int game_cmp(const void *a, const void *b) {
  int x = ((const LbGame *)a)->id, y = ((const LbGame *)b)->id;
  return x < y ? -1 : x > y;
}

static int region_rank(const char *r) {
  if(!*r) return 30;
  if(strstr(r, "North America") || strstr(r, "United States")) return 50;
  if(strstr(r, "World")) return 40;
  if(strstr(r, "Canada")) return 35;
  if(strstr(r, "Europe") || strstr(r, "United Kingdom") || strstr(r, "Australia")) return 20;
  if(strstr(r, "Japan")) return 10;
  return 15;
}

static void lb_record(LbBuild *b) {
  if(b->rec == 1) {
    int p = lb_plat_index(b->ix, b->r_plat);
    int id = atoi(b->r_id);
    if(p < 0 || id <= 0) return;
    if(b->ngames == b->cap) {
      int nc = b->cap ? b->cap * 2 : 2048;
      LbGame *ng = realloc(b->games, sizeof(LbGame) * nc);
      if(!ng) return;
      b->games = ng;
      b->cap = nc;
    }
    LbGame *g = &b->games[b->ngames++];
    memset(g, 0, sizeof *g);
    g->id = id;
    g->plat = (unsigned char)p;
    g->score = -1;
    norm(b->r_name, g->norm, sizeof g->norm);
    b->sorted = 0;
  } else if(b->rec == 2) {
    if(strcmp(b->r_type, "Disc") || !b->r_file[0] || strlen(b->r_file) >= sizeof b->games[0].file) return;
    if(!b->ngames) return;
    if(!b->sorted) {
      qsort(b->games, b->ngames, sizeof(LbGame), game_cmp);
      b->sorted = 1;
    }
    LbGame key = {.id = atoi(b->r_id)};
    LbGame *g = bsearch(&key, b->games, b->ngames, sizeof(LbGame), game_cmp);
    if(!g) return;
    int s = region_rank(b->r_region);
    if(s > g->score) {
      g->score = s;
      snprintf(g->file, sizeof g->file, "%s", b->r_file);
    }
    b->images++;
  }
}

static void xml_tag(LbBuild *b) {
  b->tag[b->tlen] = 0;
  char *t = b->tag;
  if(t[0] == '?' || t[0] == '!') return;
  int closing = t[0] == '/';
  int self = b->tlen > 0 && t[b->tlen - 1] == '/';
  if(closing) t++;
  char name[40];
  int n = 0;
  while(*t && !isspace((unsigned char)*t) && *t != '/' && n + 1 < (int)sizeof name) name[n++] = *t++;
  name[n] = 0;
  if(closing) {
    if(b->depth == 3 && b->rec && !strcmp(name, b->field)) {
      b->text[b->xlen] = 0;
      xml_unescape(b->text);
      char *dst = NULL;
      size_t cap = 0;
      if(!strcmp(name, "Name")) { dst = b->r_name; cap = sizeof b->r_name; }
      else if(!strcmp(name, "Platform")) { dst = b->r_plat; cap = sizeof b->r_plat; }
      else if(!strcmp(name, "DatabaseID")) { dst = b->r_id; cap = sizeof b->r_id; }
      else if(!strcmp(name, "FileName")) { dst = b->r_file; cap = sizeof b->r_file; }
      else if(!strcmp(name, "Type")) { dst = b->r_type; cap = sizeof b->r_type; }
      else if(!strcmp(name, "Region")) { dst = b->r_region; cap = sizeof b->r_region; }
      if(dst) snprintf(dst, cap, "%s", b->text);
      b->field[0] = 0;
    } else if(b->depth == 2 && b->rec) {
      lb_record(b);
      b->rec = 0;
    }
    if(b->depth > 0) b->depth--;
    return;
  }
  if(self) return;
  b->depth++;
  if(b->depth == 2) {
    b->rec = !strcmp(name, "Game") ? 1 : !strcmp(name, "GameImage") ? 2 : 0;
    if(b->rec) b->r_name[0] = b->r_plat[0] = b->r_id[0] = b->r_file[0] = b->r_type[0] = b->r_region[0] = 0;
  } else if(b->depth == 3 && b->rec) {
    snprintf(b->field, sizeof b->field, "%s", name);
    b->xlen = 0;
  }
}

static void xml_feed(LbBuild *b, const unsigned char *d, size_t n) {
  for(size_t i = 0; i < n; i++) {
    char c = (char)d[i];
    if(b->xstate == 0) {
      if(c == '<') {
        b->xstate = 1;
        b->tlen = 0;
      } else if(b->depth == 3 && b->rec && b->field[0] && b->xlen + 1 < (int)sizeof b->text) {
        b->text[b->xlen++] = c;
      }
    } else {
      if(c == '>') {
        /* inside a comment, keep going until "-->" */
        if(b->tlen >= 3 && !strncmp(b->tag, "!--", 3) &&
           !(b->tlen >= 5 && b->tag[b->tlen - 1] == '-' && b->tag[b->tlen - 2] == '-')) {
          if(b->tlen + 1 < (int)sizeof b->tag) b->tag[b->tlen++] = c;
          continue;
        }
        xml_tag(b);
        b->xstate = 0;
      } else if(b->tlen + 1 < (int)sizeof b->tag) {
        b->tag[b->tlen++] = c;
      }
    }
  }
  b->xml_bytes += n;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int zip_sink(void *ctx, const char *data, size_t n) {
  LbBuild *b = ctx;
  const unsigned char *d = (const unsigned char *)data;
  b->net_bytes += n;
  while(n > 0) {
    switch(b->zstate) {
    case 0: {   /* local file header */
      size_t k = sizeof b->hdr - b->hlen;
      if(k > n) k = n;
      memcpy(b->hdr + b->hlen, d, k);
      b->hlen += k;
      d += k;
      n -= k;
      if(b->hlen < (int)sizeof b->hdr) break;
      b->hlen = 0;
      unsigned long sig = rd32(b->hdr);
      if(sig != 0x04034b50) {
        b->zstate = 6;   /* central directory: no more entries */
        return 1;
      }
      b->flags = rd16(b->hdr + 6);
      b->method = rd16(b->hdr + 8);
      b->csize = rd32(b->hdr + 18);
      b->namelen = rd16(b->hdr + 26);
      b->extralen = rd16(b->hdr + 28);
      b->nlen = 0;
      b->zstate = 1;
      break;
    }
    case 1:     /* file name */
      while(n > 0 && b->nlen < b->namelen) {
        if(b->nlen + 1 < (int)sizeof b->name) b->name[b->nlen] = (char)*d;
        b->nlen++;
        d++;
        n--;
      }
      if(b->nlen < b->namelen) break;
      b->name[b->nlen < (int)sizeof b->name ? b->nlen : (int)sizeof b->name - 1] = 0;
      b->skip = b->extralen;
      b->zstate = 2;
      break;
    case 2:     /* extra field */
      if(b->skip) {
        size_t k = b->skip < n ? b->skip : n;
        b->skip -= k;
        d += k;
        n -= k;
        if(b->skip) break;
      }
      {
        const char *base = strrchr(b->name, '/');
        base = base ? base + 1 : b->name;
        b->target = !strcasecmp(base, "Metadata.xml");
      }
      logl("launchbox: zip entry %s (method %d, %lu bytes)", b->name, b->method, b->csize);
      if(b->method == 8) {
        tinfl_init(&b->inf);
        b->dict_ofs = 0;
        b->zstate = 3;
      } else if(b->method == 0 && !(b->flags & 8)) {
        b->skip = b->csize;
        b->zstate = 4;
        if(b->target) {
          b->error = 1;   /* not expected: Metadata.xml is always compressed */
          return 1;
        }
      } else {
        b->error = 2;
        return 1;
      }
      break;
    case 3: {   /* inflate */
      size_t in = n, out = TINFL_LZ_DICT_SIZE - b->dict_ofs;
      tinfl_status st = tinfl_decompress(&b->inf, d, &in, b->dict, b->dict + b->dict_ofs, &out,
                                         TINFL_FLAG_HAS_MORE_INPUT);
      d += in;
      n -= in;
      if(out) {
        if(b->target) {
          unsigned long long before = b->xml_bytes;
          xml_feed(b, b->dict + b->dict_ofs, out);
          if((before >> 24) != (b->xml_bytes >> 24))
            status("running 0 0 %ld", (long)time(NULL));   /* still busy, every 16 MB */
        }
        b->dict_ofs = (b->dict_ofs + out) & (TINFL_LZ_DICT_SIZE - 1);
      }
      if(st == TINFL_STATUS_DONE) {
        if(b->target) {
          b->target_done = 1;
          return 1;      /* everything needed: stop the download */
        }
        b->zstate = (b->flags & 8) ? 5 : 0;
        b->dlen = 0;
        b->skip = 0;
      } else if(st < 0) {
        b->error = 3;
        return 1;
      } else if(st == TINFL_STATUS_NEEDS_MORE_INPUT && n == 0) {
        return 0;
      }
      break;
    }
    case 4: {   /* stored entry we don't need */
      size_t k = b->skip < n ? b->skip : n;
      b->skip -= k;
      d += k;
      n -= k;
      if(!b->skip) b->zstate = 0;
      break;
    }
    case 5:     /* data descriptor (12, 16 or, zip64, 24 bytes): find the next header after it */
      while(n > 0) {
        if(b->dlen == 4) {
          memmove(b->desc, b->desc + 1, 3);
          b->dlen = 3;
        }
        b->desc[b->dlen++] = *d++;
        n--;
        b->skip++;
        if(b->dlen == 4 && b->skip >= 12 && (rd32(b->desc) == 0x04034b50 || rd32(b->desc) == 0x02014b50)) {
          memcpy(b->hdr, b->desc, 4);
          b->hlen = 4;
          b->zstate = 0;
          break;
        }
        if(b->skip > 64) {   /* not a descriptor we know */
          b->error = 4;
          return 1;
        }
      }
      break;
    default:
      return 1;
    }
  }
  return 0;
}

/* download and index the disc images of the platforms in want[] (plus the
 * ones already in the index). 0 = ok */
static int lb_build(const char want[][48], int nwant) {
  LbBuild *b = calloc(1, sizeof *b);
  if(!b) return -1;
  LbIndex *ix = &g_lb;
  char old[LB_MAXPLAT][48];
  int nold = 0;
  if(lb_load(ix) == 0) {
    for(int i = 0; i < ix->nplat; i++) snprintf(old[nold++], sizeof old[0], "%s", ix->plat[i]);
  }
  lb_free(ix);
  for(int i = 0; i < nold; i++) snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", old[i]);
  for(int i = 0; i < nwant && ix->nplat < LB_MAXPLAT; i++)
    if(lb_plat_index(ix, want[i]) < 0) snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", want[i]);
  b->ix = ix;

  logl("launchbox: downloading the database for %d platform(s)", ix->nplat);
  status("running 0 0 %ld", (long)time(NULL));
  int st = http_get(lb_url("JC_LB_URL", LB_URL), zip_sink, b);
  logl("launchbox: %d, %llu MB downloaded, %llu MB of XML, %d games, %d disc images, error %d", st,
       b->net_bytes >> 20, b->xml_bytes >> 20, b->ngames, b->images, b->error);
  int ok = b->target_done && b->ngames > 0;
  if(ok) {
    char part[300];
    snprintf(part, sizeof part, "%s.part", LB_IDX);
    FILE *f = fopen(part, "w");
    if(f) {
      fprintf(f, "#jc-lbdb 1");
      for(int i = 0; i < ix->nplat; i++) fprintf(f, "\t%s", ix->plat[i]);
      fprintf(f, "\n");
      int kept = 0;
      for(int i = 0; i < b->ngames; i++)
        if(b->games[i].file[0] && b->games[i].norm[0]) {
          fprintf(f, "%d\t%s\t%s\n", b->games[i].plat, b->games[i].norm, b->games[i].file);
          kept++;
        }
      fclose(f);
      rename(part, LB_IDX);
      logl("launchbox: index written, %d discs", kept);
    } else {
      ok = 0;
    }
  }
  free(b->games);
  free(b);
  if(!ok) {
    FILE *f = fopen(LB_FAIL, "w");
    if(f) {
      fprintf(f, "%ld\n", (long)time(NULL));
      fclose(f);
    }
    return -1;
  }
  unlink(LB_FAIL);
  return lb_load(ix);
}

/* the index, loaded or built, covering every platform in want[]. 0 = ready */
static int lb_ready(const char want[][48], int nwant) {
  if(g_lb_tried) return g_lb_tried > 0 ? 0 : -1;
  g_lb_tried = -1;
  if(lb_load(&g_lb) == 0) {
    int all = 1;
    for(int i = 0; i < nwant; i++)
      if(lb_plat_index(&g_lb, want[i]) < 0) all = 0;
    if(all) {
      logl("launchbox: index loaded, %d discs", g_lb.n);
      g_lb_tried = 1;
      return 0;
    }
  }
  /* after a failed attempt, wait a few hours before downloading it all again */
  FILE *f = fopen(LB_FAIL, "r");
  if(f) {
    long t = 0;
    if(fscanf(f, "%ld", &t) != 1) t = 0;
    fclose(f);
    if(time(NULL) - t < 6 * 3600) {
      logl("launchbox: last download failed, not trying again yet");
      return -1;
    }
  }
  if(lb_build(want, nwant) != 0) return -1;
  g_lb_tried = 1;
  return 0;
}

/* 0 = saved, -1 = LaunchBox has no disc for it, -2 = couldn't ask */
static int find_lb_disc(const char *platform, const char *rom, const char *out) {
  if(g_lb_tried <= 0) return -2;
  int p = lb_plat_index(&g_lb, platform);
  if(p < 0) return -2;
  char nr[512];
  norm(rom, nr, sizeof nr);
  size_t lr = strlen(nr);
  if(!lr) return -1;
  int best = -1, best_score = -1;
  for(int i = 0; i < g_lb.n; i++) {
    if(g_lb.eplat[i] != p) continue;
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
    if(score > best_score) {
      best_score = score;
      best = i;
    }
  }
  if(best < 0) {
    logl("launchbox: no disc for %s (%s)", rom, platform);
    return -1;
  }
  char url[400], part[1100], enc[300];
  url_encode(g_lb.efile[best], enc, sizeof enc);
  snprintf(url, sizeof url, "%s%s", lb_url("JC_LB_IMG", LB_IMG), enc);
  snprintf(part, sizeof part, "%s.part", out);
  FileSink fs;
  memset(&fs, 0, sizeof fs);
  if(!(fs.f = fopen(part, "wb"))) return -2;
  int st = http_get(url, file_sink, &fs);
  fclose(fs.f);
  logl("launchbox: %s -> %s: %d, %lu bytes", rom, g_lb.efile[best], st, (unsigned long)fs.total);
  if(st == 200 && (fs.kind == 1 || fs.kind == 2) && fs.total >= 100) {
    char final[1100];
    snprintf(final, sizeof final, "%s.%s", out, fs.kind == 2 ? "jpg" : "png");
    return rename(part, final);
  }
  unlink(part);
  return st == 404 ? -1 : -2;
}
