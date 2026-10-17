/* Small stand-ins for libc functions the app container's runtime does not export.
 * Only libiconv (pulled in by SDL2) and assert() reach them. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef GS_NATIVE
typedef int nl_item;

char *nl_langinfo(nl_item item) {
    (void)item;
    return (char *)"UTF-8";
}

int ___mb_cur_max(void) {
    return 4;   /* UTF-8 */
}

void __assert(const char *func, const char *file, int line, const char *expr) {
    printf("assertion failed: %s (%s:%d %s)\n", expr, file, line, func ? func : "");
    abort();
}
#endif

#ifdef GS_NATIVE
#include <ctype.h>
#include <string.h>
/* SDL2 uses strcasestr, which only the WebKit POSIX module exports. */
char *strcasestr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0) return (char *)haystack;
    }
    return NULL;
}
#endif

#ifdef GS_NATIVE
/* --- Keyboard and IME dialog -------------------------------------------------
 * SDL's PS5 video driver starts the USB keyboard library during SDL_Init, but
 * a native title can't use that module: its import stays empty and the call
 * jumps to address 0. Jewel Case has no use for a keyboard, so these replace
 * the imports and simply report "not available". */
int sceKeyboardInit(void) { return -1; }
int sceKeyboardOpen(int user, int type, int index, void *param) {
    (void)user; (void)type; (void)index; (void)param;
    return -1;
}
int sceKeyboardClose(int handle) { (void)handle; return -1; }
int sceKeyboardReadState(int handle, void *state) { (void)handle; (void)state; return -1; }
int sceImeDialogInit(const void *param, void *extended) { (void)param; (void)extended; return -1; }
int sceImeDialogGetStatus(void) { return 0; }
int sceImeDialogGetResult(void *result) { (void)result; return -1; }
int sceImeDialogTerm(void) { return 0; }

/* --- Import check ------------------------------------------------------------
 * Weak references read the import table at run time: a function the console
 * didn't provide shows up as NULL here instead of crashing later. */
#define GS_IMPORTS(X) \
    X(sceUserServiceInitialize) X(sceUserServiceGetForegroundUser) X(sceUserServiceGetLoginUserIdList) \
    X(scePadInit) X(scePadOpen) X(scePadReadState) X(scePadGetHandle) \
    X(sceVideoOutOpen) X(sceVideoOutSetFlipRate) X(sceVideoOutSubmitFlip) X(sceVideoOutWaitVblank) \
    X(sceVideoOutRegisterBuffers2) X(sceVideoOutSetBufferAttribute2) \
    X(sceKernelGetDirectMemorySize) X(sceKernelAllocateDirectMemory) X(sceKernelMapDirectMemory) \
    X(sceSystemServiceHideSplashScreen) X(sceSystemServiceLoadExec)

#define GS_DECLARE(name) extern int name() __attribute__((weak));
GS_IMPORTS(GS_DECLARE)

/* Fills buf with the names of missing imports; returns how many. */
int gs_missing_imports(char *buf, size_t cap) {
    int n = 0;
    buf[0] = 0;
#define GS_CHECK(name) \
    if (!name) { \
        if (n) strncat(buf, ", ", cap - strlen(buf) - 1); \
        strncat(buf, #name, cap - strlen(buf) - 1); \
        n++; \
    }
    GS_IMPORTS(GS_CHECK)
    return n;
}

typedef struct {
    char useless1[45];
    char message[3075];
} gs_notify_t;
int sceKernelSendNotificationRequest(int, gs_notify_t *, size_t, int);

void gs_notify(const char *text) {
    gs_notify_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#endif
