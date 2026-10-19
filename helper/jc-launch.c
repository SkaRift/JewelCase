/* Jewel Case launch helper.
 *
 * Jewel Case runs as a normal app, and the PS5 runs one app at a time, so it
 * can't start RetroArch (or another emulator app) itself. Instead it writes
 * /data/jewelcase-launch.txt and sends this payload to the ELF loader on port
 * 9021. The payload closes Jewel Case and starts the requested app.
 *
 * Request file:
 *   jewelcase-launch 1
 *   title=PPSA99169
 *   arg=--ps5-mode=game        (zero or more)
 *   write=<path>\t<line>       (zero or more) a file to write before the app
 *                              starts: PS5SX2 starts the game named in
 *                              /data/PCSX2/logs/disc-launch.txt. Its folder is
 *                              made if missing. Removed again if the app
 *                              doesn't start, so it can't start a game later.
 *
 * How the app is started:
 *   1. sceLncUtilLaunchApp, the call the home screen makes when a tile is
 *      pressed (as PS5SX2's disc payload does). It doesn't go through the
 *      system-service check that answers 0x80940033 for some homebrew and
 *      puts up a "can't start the application" dialog.
 *   2. Only if that is refused and the app hasn't appeared:
 *      sceSystemServiceLaunchApp (ps5-payload-websrv's way).
 * Each step checks whether the app actually started before trying the next,
 * because a second launch request against a starting app pushes it into the
 * background.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST_PATH "/data/jewelcase-launch.txt"
#define MAX_ARGS 32
#define MAX_WRITES 4

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

/* sceSystemServiceLaunchApp's context: not packed, 32-bit check_flag */
typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

/* sceLncUtilLaunchApp's parameter, as PS5SX2's console-tested payload has it */
typedef struct lnc_app_param {
  uint32_t size;
  int32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint64_t check_flag;
} lnc_app_param_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char* title_id, char** argv,
			      app_launch_ctx_t* ctx);
int sceLncUtilInitialize(void);
int sceLncUtilLaunchApp(const char* title_id, const char** argv,
			lnc_app_param_t* param);


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


static int
running_app(void) {
  int id = sceSystemServiceGetAppIdOfRunningBigApp();
  return (id > 0 && id != -1) ? id : 0;
}


/* is `title` the running app? (anything running counts if its title can't
 * be read) */
static int
title_running(const char *title) {
  char running[32];
  int id;

  if(!(id=running_app())) {
    return 0;
  }
  memset(running, 0, sizeof running);
  if(sceSystemServiceGetAppTitleId(id, running)) {
    return 1;
  }
  return !strncmp(running, title, 9);
}


static int
wait_started(const char *title, int ms) {
  for(int t=0; t<ms; t+=250) {
    if(title_running(title)) {
      return 1;
    }
    usleep(250000);
  }
  return title_running(title);
}


/* make `path`'s folder and its parents */
static void
make_parents(const char *path) {
  char d[1024];
  snprintf(d, sizeof d, "%s", path);
  char *slash = strrchr(d, '/');
  if(!slash || slash == d) {
    return;
  }
  *slash = 0;
  for(char *p = d + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      mkdir(d, 0777);
      *p = '/';
    }
  }
  mkdir(d, 0777);
}


/* write `text` and a newline to `path` (through a temporary file, so the app
 * never reads half of it); 0 on success */
static int
write_request(const char *path, const char *text) {
  char tmp[1100];
  FILE *f;

  make_parents(path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if(!(f=fopen(tmp, "w"))) {
    return -1;
  }
  fprintf(f, "%s\n", text);
  if(fclose(f)) {
    unlink(tmp);
    return -1;
  }
  if(rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  chmod(path, 0666);
  return 0;
}


int
main(void) {
  char title[32] = "";
  char *argv[MAX_ARGS + 1];
  int argc = 0;
  char line[2048];
  app_launch_ctx_t ctx;
  lnc_app_param_t prm;
  uint32_t user = 0;
  FILE *f;
  int app_id;
  int err, err_lnc;
  char *wpath[MAX_WRITES], *wtext[MAX_WRITES];
  int nw = 0, written = 0;

  if(!(f=fopen(REQUEST_PATH, "r"))) {
    notify("Jewel Case: no launch request");
    return 1;
  }
  if(!fgets(line, sizeof line, f) || strcmp(line, "jewelcase-launch 1\n")) {
    fclose(f);
    notify("Jewel Case: unreadable launch request");
    return 1;
  }
  while(fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while(n && (line[n-1] == '\n' || line[n-1] == '\r')) {
      line[--n] = 0;
    }
    if(!strncmp(line, "title=", 6)) {
      snprintf(title, sizeof title, "%s", line + 6);
    } else if(!strncmp(line, "arg=", 4) && argc < MAX_ARGS) {
      argv[argc++] = strdup(line + 4);
    } else if(!strncmp(line, "write=", 6) && nw < MAX_WRITES) {
      char *tab = strchr(line + 6, '\t');
      if(tab && line[6] == '/') {
        *tab = 0;
        wpath[nw] = strdup(line + 6);
        wtext[nw] = strdup(tab + 1);
        nw++;
      }
    }
  }
  argv[argc] = 0;
  fclose(f);
  unlink(REQUEST_PATH); /* a request runs once */

  if(!title[0]) {
    notify("Jewel Case: launch request has no title");
    return 1;
  }

  /* the app's own request files (the app only opens without them) */
  for(int i=0; i<nw; i++) {
    if(write_request(wpath[i], wtext[i])) {
      notify("Jewel Case: couldn't write %s; the app opens without the game", wpath[i]);
    } else {
      written |= 1 << i;
    }
  }

  sceUserServiceInitialize(0);
  sceLncUtilInitialize();
  if((err=sceUserServiceGetForegroundUser(&user))) {
    notify("Jewel Case: no signed-in user (0x%x)", err);
    return 1;
  }

  /* close Jewel Case (the running app) and wait until it has fully closed */
  if((app_id=running_app())) {
    if(sceSystemServiceKillApp(app_id, -1, 0, 0)) {
      notify("Jewel Case: couldn't close the running app");
      return 1;
    }
    for(int i=0; i<100 && running_app(); i++) {
      usleep(100000);
    }
  }
  usleep(1000000);

  /* 1. the home screen's own launch call */
  memset(&prm, 0, sizeof prm);
  prm.size = sizeof prm;
  prm.user_id = (int32_t)user;
  err_lnc = sceLncUtilLaunchApp(title, argc ? (const char**)argv : 0, &prm);
  /* accepted: give it time to come up; refused: a short look in case it
   * starts anyway */
  if(wait_started(title, err_lnc >= 0 ? 8000 : 4000)) {
    return 0;
  }

  /* 2. refused, and nothing started: the system service */
  memset(&ctx, 0, sizeof ctx);
  ctx.structsize = sizeof ctx;
  ctx.user_id = user;
  err = sceSystemServiceLaunchApp(title, argc ? argv : 0, &ctx);
  if(err >= 0 || wait_started(title, 3000)) {
    return 0;
  }

  notify("Jewel Case: couldn't start %s (0x%x / 0x%x)", title,
	 (unsigned)err_lnc, (unsigned)err);
  /* it didn't start: don't leave a request that would start the game the
   * next time the app is opened */
  for(int i=0; i<nw; i++) {
    if(written & (1 << i)) {
      unlink(wpath[i]);
    }
  }
  return 1;
}
