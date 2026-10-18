/* Jewel Case: putting frames on screen in a native title.
 *
 * Follows the template's hardware-tested VideoOut sequence
 * (ps5-native-app-boilerplate src/demo_renderer.cpp, GPL-3.0-or-later):
 * two 1920x1080 RGBA8 buffers in direct memory, tiled layout, CPU writes
 * flushed before each flip. Jewel Case draws with SDL into a normal linear
 * image; gs_vout_present converts it into the tiled buffer that isn't shown,
 * then flips to it.
 *
 * The converting, flushing and waiting for the flip happen on a thread of their
 * own: gs_vout_present only copies the frame and returns, so the next frame is
 * drawn while this one goes on screen (done one after the other, the two didn't
 * fit in one 60 Hz refresh, and the app ran at 30 frames a second).
 */
#ifdef GS_NATIVE

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_W 1920
#define FRAME_H 1080
#define FRAME_BYTES 0x1000000
#define MEMORY_BYTES (FRAME_BYTES * 2)
#define MEMORY_ALIGN 0x200000
#define MEMORY_TYPE_WC_GARLIC 3
#define MAP_PROT 0x33
#define PIXEL_FORMAT_RGBA8_SRGB 0x8000000022000000ULL

typedef struct {
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} VideoBuffer;

typedef struct {
    uint8_t reserved[80];
} VideoAttribute;

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceHideSplashScreen(void);
int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode, int64_t flip_argument);
int sceVideoOutWaitVblank(int32_t handle);
void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute, uint64_t pixel_format, uint32_t tiling_mode,
                                    uint32_t width, uint32_t height, uint64_t option, uint32_t dcc_control,
                                    uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start,
                                VideoBuffer *buffers, int32_t buffer_count, VideoAttribute *attribute,
                                int32_t category, void *option);

static int g_video = -1;
static uint8_t *g_frames[2];
static int64_t g_frame_no = 1;
/* tiled address = block(x,y) + (ypart ^ xpart); precomputed per row / column */
static uint32_t g_xpart[FRAME_W], g_xblock[FRAME_W];
static uint32_t g_ypart[FRAME_H], g_yblock[FRAME_H];

/* the frame handed to the flip thread */
static uint32_t *g_stage;
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_pending;           /* a frame waiting for (or being put on) the screen */
static int g_threaded;

static void flush_range(void *address, size_t length) {
    uint8_t *at = (uint8_t *)address;
    const uint8_t *end = at + length;
    for (; at < end; at += 64) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void *flip_thread(void *arg);

/* Returns 0, or a short reason. */
const char *gs_vout_init(void) {
    const uint32_t blocks_per_row = (FRAME_W + 127u) >> 7;
    for (uint32_t x = 0; x < FRAME_W; x++) {
        g_xpart[x] = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^
                     ((x << 9) & 0xa000u);
        g_xblock[x] = (x >> 7) << 16;
    }
    for (uint32_t y = 0; y < FRAME_H; y++) {
        g_ypart[y] = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u);
        g_yblock[y] = ((y >> 7) * blocks_per_row) << 16;
    }

    sceSystemServiceHideSplashScreen();
    g_video = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (g_video < 0) return "sceVideoOutOpen failed";

    size_t pool = sceKernelGetDirectMemorySize();
    if (pool < MEMORY_BYTES) return "not enough direct memory";
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)pool, MEMORY_BYTES, MEMORY_ALIGN, MEMORY_TYPE_WC_GARLIC, &phys) < 0)
        return "direct memory allocation failed";
    void *mapped = NULL;
    if (sceKernelMapDirectMemory(&mapped, MEMORY_BYTES, MAP_PROT, 0, phys, MEMORY_ALIGN) < 0)
        return "direct memory mapping failed";

    g_frames[0] = (uint8_t *)mapped;
    g_frames[1] = (uint8_t *)mapped + FRAME_BYTES;
    memset(mapped, 0, MEMORY_BYTES);
    flush_range(mapped, MEMORY_BYTES);

    VideoBuffer buffers[2] = {{g_frames[0], NULL, NULL, NULL}, {g_frames[1], NULL, NULL, NULL}};
    VideoAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetFlipRate(g_video, 0);
    sceVideoOutSetBufferAttribute2(&attr, PIXEL_FORMAT_RGBA8_SRGB, 0, FRAME_W, FRAME_H, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers, 2, &attr, 0, NULL) < 0)
        return "video buffer registration failed";
    if (sceVideoOutSubmitFlip(g_video, 0, 1, 1) < 0) return "first flip failed";
    sceVideoOutWaitVblank(g_video);
    /* the flip thread; without it frames are put on screen in line, as before */
    g_stage = malloc((size_t)FRAME_W * 4 * FRAME_H);
    if (g_stage && pthread_create(&g_thread, NULL, flip_thread, NULL) == 0) g_threaded = 1;
    return 0;
}

/* pixels: 1920x1080, 32-bit words laid out 0xAABBGGRR (SDL's ABGR8888). */
static void flip_frame(const void *pixels, int pitch) {
    if (g_video < 0) return;
    g_frame_no++;
    int back = (int)(g_frame_no & 1);
    uint8_t *dst = g_frames[back];
    const uint8_t *src_row = (const uint8_t *)pixels;
    for (uint32_t y = 0; y < FRAME_H; y++, src_row += pitch) {
        const uint32_t *src = (const uint32_t *)src_row;
        uint8_t *rowbase = dst + g_yblock[y];
        uint32_t yp = g_ypart[y];
        for (uint32_t x = 0; x < FRAME_W; x++)
            *(uint32_t *)(rowbase + g_xblock[x] + (yp ^ g_xpart[x])) = src[x] | 0xff000000u;
    }
    flush_range(dst, (size_t)((FRAME_H + 127) / 128) * ((FRAME_W + 127) / 128) * 0x10000);
    if (sceVideoOutSubmitFlip(g_video, back, 1, g_frame_no) < 0) {
        sceKernelUsleep(4000);   /* previous flip still queued: try once more */
        sceVideoOutSubmitFlip(g_video, back, 1, g_frame_no);
    }
    sceVideoOutWaitVblank(g_video);
}

static void *flip_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_pending) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        flip_frame(g_stage, FRAME_W * 4);
        pthread_mutex_lock(&g_mu);
        g_pending = 0;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

void gs_vout_present(const void *pixels, int pitch) {
    if (g_video < 0) return;
    if (!g_threaded) {
        flip_frame(pixels, pitch);
        return;
    }
    /* wait for the last frame to be on screen, hand this one over, and go on */
    pthread_mutex_lock(&g_mu);
    while (g_pending) pthread_cond_wait(&g_cv, &g_mu);
    const uint8_t *src = (const uint8_t *)pixels;
    if (pitch == FRAME_W * 4) memcpy(g_stage, src, (size_t)FRAME_W * 4 * FRAME_H);
    else
        for (int y = 0; y < FRAME_H; y++) memcpy(g_stage + (size_t)y * FRAME_W, src + (size_t)y * pitch, FRAME_W * 4);
    g_pending = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

#endif
