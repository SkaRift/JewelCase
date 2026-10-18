/* LaunchBox Games Database: game info and disc scans, for jc-art.c (included from it).
 *
 * LaunchBox publishes its whole database as one file, Metadata.zip
 * (gamesdb.launchbox-app.com/Metadata.zip). Its Metadata.xml lists every game
 * (<Game>: Name, DatabaseID, Platform, ReleaseYear, Genres, Developer, Publisher,
 * MaxPlayers, Overview) and every image (<GameImage>: DatabaseID, FileName, Type,
 * Region); an image is at images.launchbox-app.com/<FileName>.
 *
 * The zip is too big to keep, so it is read as it downloads: the zip entries are
 * inflated (miniz) and the XML parsed on the fly, keeping only the games of the
 * platforms asked for. Two files come out of it:
 *   COVER_DIR/launchbox-info.txt  one line per game: id, platform, normalised
 *                                 name, name, year, genres, developer, publisher,
 *                                 players, overview (tab separated)
 *   COVER_DIR/launchbox.idx       #jc-lbdb 2<TAB>platform<TAB>platform...
 *                                 <platform><TAB><normalised name><TAB><offset in
 *                                 the info file><TAB><disc image file or ->
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
#define LB_INFO COVER_DIR "/launchbox-info.txt"
#define LB_FAIL COVER_DIR "/launchbox.fail"
#define LB_MAXPLAT 48

/* the PC test build can point these elsewhere (curl takes file:// URLs) */
static const char *lb_url(const char *env, const char *def) {
#ifdef JC_HOST
  const char *e = getenv(env);
  if(e) return e;
#endif
  (void)env;
  return def;
}

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

/* ------------------------------------------------------------------------ */
/* the index: which platforms it covers, and one platform's games at a time  */
/* ------------------------------------------------------------------------ */

typedef struct {
  char plat[LB_MAXPLAT][48];
  int nplat;
  int loaded;              /* the platform whose entries are in memory, -1 none */
  char **enorm, **efile;   /* efile NULL: no disc image */
  long *eoff;
  int n, cap;
} LbIndex;

static LbIndex g_lb = {.loaded = -1};
static int g_lb_tried;   /* built or loaded this run (0 = not yet, 1 = ok, -1 = failed) */

static int lb_plat_index(LbIndex *ix, const char *p) {
  for(int i = 0; i < ix->nplat; i++)
    if(plat_eq(ix->plat[i], p)) return i;
  return -1;
}

static void lb_free_entries(LbIndex *ix) {
  for(int i = 0; i < ix->n; i++) {
    free(ix->enorm[i]);
    free(ix->efile[i]);
  }
  free(ix->enorm);
  free(ix->efile);
  free(ix->eoff);
  ix->enorm = ix->efile = NULL;
  ix->eoff = NULL;
  ix->n = ix->cap = 0;
  ix->loaded = -1;
}

/* the index's platform list. 0 = there is an index */
static int lb_header(LbIndex *ix) {
  lb_free_entries(ix);
  ix->nplat = 0;
  FILE *f = fopen(LB_IDX, "r");
  if(!f) return -1;
  char line[4096];
  int ok = fgets(line, sizeof line, f) && !strncmp(line, "#jc-lbdb 2", 10);
  fclose(f);
  if(!ok) return -1;
  char *save = NULL;
  strtok_r(line, "\t\r\n", &save);
  for(char *t = strtok_r(NULL, "\t\r\n", &save); t && ix->nplat < LB_MAXPLAT; t = strtok_r(NULL, "\t\r\n", &save))
    snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", t);
  return 0;
}

/* one platform's games into memory */
static int lb_load_plat(LbIndex *ix, int p) {
  if(ix->loaded == p) return 0;
  lb_free_entries(ix);
  FILE *f = fopen(LB_IDX, "r");
  if(!f) return -1;
  char line[700];
  if(!fgets(line, sizeof line, f)) {
    fclose(f);
    return -1;
  }
  while(fgets(line, sizeof line, f)) {
    char *a = line, *b = strchr(a, '\t'), *c = b ? strchr(b + 1, '\t') : NULL, *d = c ? strchr(c + 1, '\t') : NULL;
    if(!d || atoi(a) != p) continue;
    *b++ = 0;
    *c++ = 0;
    *d++ = 0;
    d[strcspn(d, "\r\n")] = 0;
    if(ix->n == ix->cap) {
      int nc = ix->cap ? ix->cap * 2 : 1024;
      char **nn = realloc(ix->enorm, sizeof(char *) * nc);
      if(nn) ix->enorm = nn;
      char **nf = realloc(ix->efile, sizeof(char *) * nc);
      if(nf) ix->efile = nf;
      long *no = realloc(ix->eoff, sizeof(long) * nc);
      if(no) ix->eoff = no;
      if(!nn || !nf || !no) break;
      ix->cap = nc;
    }
    ix->enorm[ix->n] = strdup(b);
    ix->eoff[ix->n] = atol(c);
    ix->efile[ix->n] = strcmp(d, "-") ? strdup(d) : NULL;
    if(!ix->enorm[ix->n]) break;
    ix->n++;
  }
  fclose(f);
  ix->loaded = p;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* building it: zip -> inflate -> XML -> records                             */
/* ------------------------------------------------------------------------ */

typedef struct {
  int id;
  int disc;                /* index into the disc file names, -1 none */
  int score;
} LbGame;

typedef struct {
  /* what to keep */
  LbIndex *ix;
  FILE *info;
  LbGame *games;
  int ngames, cap, sorted;
  char (*discs)[64];
  int ndiscs, dcap;
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
  char text[2400];
  int xlen;
  char r_name[300], r_plat[64], r_id[16], r_file[80], r_type[40], r_region[48];
  char r_year[16], r_date[40], r_genres[200], r_dev[160], r_pub[160], r_players[16], r_overview[1600];
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

/* tabs and line breaks become spaces, so a value fits in one tab-separated field */
static void one_line(char *s) {
  for(; *s; s++)
    if(*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
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
    if(p < 0 || id <= 0 || !b->info) return;
    if(b->ngames == b->cap) {
      int nc = b->cap ? b->cap * 2 : 4096;
      LbGame *ng = realloc(b->games, sizeof(LbGame) * nc);
      if(!ng) return;
      b->games = ng;
      b->cap = nc;
    }
    LbGame *g = &b->games[b->ngames++];
    g->id = id;
    g->disc = -1;
    g->score = -1;
    b->sorted = 0;
    char nb[200];
    norm(b->r_name, nb, sizeof nb);
    const char *year = b->r_year[0] ? b->r_year : b->r_date;
    char y[8];
    snprintf(y, sizeof y, "%.4s", year);
    char *f[] = {b->r_name, b->r_genres, b->r_dev, b->r_pub, b->r_players, b->r_overview};
    for(int i = 0; i < 6; i++) one_line(f[i]);
    fprintf(b->info, "%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", id, p, nb, b->r_name, y, b->r_genres, b->r_dev,
            b->r_pub, b->r_players, b->r_overview);
  } else if(b->rec == 2) {
    if(strcmp(b->r_type, "Disc") || !b->r_file[0] || strlen(b->r_file) >= sizeof b->discs[0]) return;
    if(!b->ngames) return;
    if(!b->sorted) {
      qsort(b->games, b->ngames, sizeof(LbGame), game_cmp);
      b->sorted = 1;
    }
    LbGame key = {.id = atoi(b->r_id)};
    LbGame *g = bsearch(&key, b->games, b->ngames, sizeof(LbGame), game_cmp);
    if(!g) return;
    int s = region_rank(b->r_region);
    if(s <= g->score) return;
    g->score = s;
    if(g->disc < 0) {
      if(b->ndiscs == b->dcap) {
        int nc = b->dcap ? b->dcap * 2 : 1024;
        char (*nd)[64] = realloc(b->discs, sizeof(*nd) * nc);
        if(!nd) return;
        b->discs = nd;
        b->dcap = nc;
      }
      g->disc = b->ndiscs++;
    }
    snprintf(b->discs[g->disc], sizeof b->discs[0], "%s", b->r_file);
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
#define FIELD(tagname, var) else if(!strcmp(name, tagname)) { dst = var; cap = sizeof var; }
      if(0) {}
      FIELD("Name", b->r_name)
      FIELD("Platform", b->r_plat)
      FIELD("DatabaseID", b->r_id)
      FIELD("FileName", b->r_file)
      FIELD("Type", b->r_type)
      FIELD("Region", b->r_region)
      FIELD("ReleaseYear", b->r_year)
      FIELD("ReleaseDate", b->r_date)
      FIELD("Genres", b->r_genres)
      FIELD("Developer", b->r_dev)
      FIELD("Publisher", b->r_pub)
      FIELD("MaxPlayers", b->r_players)
      FIELD("Overview", b->r_overview)
#undef FIELD
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
    if(b->rec) {
      b->r_name[0] = b->r_plat[0] = b->r_id[0] = b->r_file[0] = b->r_type[0] = b->r_region[0] = 0;
      b->r_year[0] = b->r_date[0] = b->r_genres[0] = b->r_dev[0] = b->r_pub[0] = b->r_players[0] = 0;
      b->r_overview[0] = 0;
    }
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

/* download the database and index the games of the platforms in want[] (plus
 * the ones already in the index). 0 = ok */
static int lb_build(const char want[][48], int nwant) {
  LbBuild *b = calloc(1, sizeof *b);
  if(!b) return -1;
  LbIndex *ix = &g_lb;
  char old[LB_MAXPLAT][48];
  int nold = 0;
  if(lb_header(ix) == 0)
    for(int i = 0; i < ix->nplat; i++) snprintf(old[nold++], sizeof old[0], "%s", ix->plat[i]);
  ix->nplat = 0;
  for(int i = 0; i < nold; i++) snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", old[i]);
  for(int i = 0; i < nwant && ix->nplat < LB_MAXPLAT; i++)
    if(lb_plat_index(ix, want[i]) < 0) snprintf(ix->plat[ix->nplat++], sizeof ix->plat[0], "%s", want[i]);
  b->ix = ix;
  b->info = fopen(LB_INFO ".part", "w");
  if(!b->info) {
    free(b);
    return -1;
  }

  logl("launchbox: downloading the database for %d platform(s)", ix->nplat);
  status("running 0 0 %ld", (long)time(NULL));
  int st = http_get(lb_url("JC_LB_URL", LB_URL), zip_sink, b);
  fclose(b->info);
  logl("launchbox: %d, %llu MB downloaded, %llu MB of XML, %d games, %d disc images, error %d", st,
       b->net_bytes >> 20, b->xml_bytes >> 20, b->ngames, b->images, b->error);
  int ok = b->target_done && b->ngames > 0;
  if(ok) {
    if(!b->sorted) qsort(b->games, b->ngames, sizeof(LbGame), game_cmp);
    /* the index: from the info file, with each game's line offset and disc */
    FILE *in = fopen(LB_INFO ".part", "r"), *out = fopen(LB_IDX ".part", "w");
    ok = in && out;
    if(ok) {
      fprintf(out, "#jc-lbdb 2");
      for(int i = 0; i < ix->nplat; i++) fprintf(out, "\t%s", ix->plat[i]);
      fprintf(out, "\n");
      static char line[4096];
      long off = ftell(in);
      int kept = 0, discs = 0;
      while(fgets(line, sizeof line, in)) {
        long here = off;
        off = ftell(in);
        char *a = line, *pb = strchr(a, '\t'), *pc = pb ? strchr(pb + 1, '\t') : NULL;
        char *pd = pc ? strchr(pc + 1, '\t') : NULL;
        if(!pd) continue;
        *pd = 0;
        LbGame key = {.id = atoi(a)};
        LbGame *g = bsearch(&key, b->games, b->ngames, sizeof(LbGame), game_cmp);
        const char *disc = g && g->disc >= 0 ? b->discs[g->disc] : "-";
        if(pc[1]) {
          fprintf(out, "%d\t%s\t%ld\t%s\n", atoi(pb + 1), pc + 1, here, disc);
          kept++;
          if(*disc != '-') discs++;
        }
      }
      logl("launchbox: index written, %d games, %d with a disc", kept, discs);
    }
    if(in) fclose(in);
    if(out) fclose(out);
    if(ok) {
      rename(LB_INFO ".part", LB_INFO);
      rename(LB_IDX ".part", LB_IDX);
    }
  }
  unlink(LB_INFO ".part");
  free(b->games);
  free(b->discs);
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
  return lb_header(ix);
}

/* the index, loaded or built, covering every platform in want[]. 0 = ready */
static int lb_ready(const char want[][48], int nwant) {
  if(g_lb_tried) return g_lb_tried > 0 ? 0 : -1;
  g_lb_tried = -1;
  if(lb_header(&g_lb) == 0) {
    int all = 1;
    for(int i = 0; i < nwant; i++)
      if(lb_plat_index(&g_lb, want[i]) < 0) all = 0;
    if(all) {
      logl("launchbox: index ready, %d platforms", g_lb.nplat);
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

/* the index entry for a ROM on a platform: -1 none, -2 no index */
static int lb_match(const char *platform, const char *rom) {
  if(g_lb_tried <= 0) return -2;
  int p = lb_plat_index(&g_lb, platform);
  if(p < 0 || lb_load_plat(&g_lb, p) != 0) return -2;
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
    if(g_lb.efile[i]) score += 2;   /* of two equal names, the one with a disc */
    if(score > best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

/* 0 = saved, -1 = LaunchBox has no disc for it, -2 = couldn't ask */
static int find_lb_disc(const char *platform, const char *rom, const char *out) {
  int e = lb_match(platform, rom);
  if(e == -2) return -2;
  if(e < 0 || !g_lb.efile[e]) {
    logl("launchbox: no disc for %s (%s)", rom, platform);
    return -1;
  }
  char url[400], part[1100], enc[300];
  url_encode(g_lb.efile[e], enc, sizeof enc);
  snprintf(url, sizeof url, "%s%s", lb_url("JC_LB_IMG", LB_IMG), enc);
  snprintf(part, sizeof part, "%s.part", out);
  FileSink fs;
  memset(&fs, 0, sizeof fs);
  if(!(fs.f = fopen(part, "wb"))) return -2;
  int st = http_get(url, file_sink, &fs);
  fclose(fs.f);
  logl("launchbox: %s -> %s: %d, %lu bytes", rom, g_lb.efile[e], st, (unsigned long)fs.total);
  if(st == 200 && (fs.kind == 1 || fs.kind == 2) && fs.total >= 100) {
    char final[1100];
    snprintf(final, sizeof final, "%s.%s", out, fs.kind == 2 ? "jpg" : "png");
    return rename(part, final);
  }
  unlink(part);
  return st == 404 ? -1 : -2;
}

/* the game's info into <out>.txt: "Key: value" lines, a blank line, the overview.
 * 0 = saved, -1 = not in the database, -2 = no index */
static int find_lb_info(const char *platform, const char *rom, const char *out) {
  int e = lb_match(platform, rom);
  if(e == -2) return -2;
  if(e < 0) {
    logl("launchbox: no info for %s (%s)", rom, platform);
    return -1;
  }
  FILE *in = fopen(LB_INFO, "r");
  if(!in) return -2;
  static char line[4096];
  int ok = fseek(in, g_lb.eoff[e], SEEK_SET) == 0 && fgets(line, sizeof line, in);
  fclose(in);
  if(!ok) return -2;
  line[strcspn(line, "\r\n")] = 0;
  char *f[10] = {0};
  int nf = 0;
  for(char *p = line; nf < 10; nf++) {
    f[nf] = p;
    char *t = strchr(p, '\t');
    if(!t) {
      nf++;
      break;
    }
    *t = 0;
    p = t + 1;
  }
  if(nf < 10) return -1;
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.txt.part", out);
  FILE *o = fopen(tmp, "w");
  if(!o) return -2;
  static const char *keys[] = {"Name", "Year", "Genre", "Developer", "Publisher", "Players"};
  for(int i = 0; i < 6; i++)
    if(f[3 + i][0]) fprintf(o, "%s: %s\n", keys[i], f[3 + i]);
  fprintf(o, "\n%s\n", f[9]);
  fclose(o);
  char final[1100];
  snprintf(final, sizeof final, "%s.txt", out);
  return rename(tmp, final);
}
