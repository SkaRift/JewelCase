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
 *   end
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
  fprintf(out, "end\n");
  fclose(out);
  chmod(INDEX_TMP, 0666);
  if(rename(INDEX_TMP, INDEX)) {
    notify("Jewel Case: can't write " INDEX);
    return 1;
  }
  return 0;
}
