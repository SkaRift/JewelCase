/* Jewel Case background music.
 *
 * Plays the MP3s it's given, one after another in a shuffled order, looping
 * the list, on its own thread. Decoding: dr_mp3 (public domain / MIT-0).
 * Output on the PS5: sceAudioOut, 48 kHz 16-bit stereo, 256 frames a block
 * (each sceAudioOutOutput call waits for the previous block, which paces it).
 * Other sample rates are resampled to 48 kHz (linear).
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *gs_malloc(size_t size);
void *gs_realloc(void *p, size_t size);
void gs_free(void *p);

#define DRMP3_MALLOC(sz) gs_malloc(sz)
#define DRMP3_REALLOC(p, sz) gs_realloc(p, sz)
#define DRMP3_FREE(p) gs_free(p)
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define OUT_RATE 48000
#define BLOCK 256
#define MAX_TRACKS 64

#ifdef GS_NATIVE
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *ptr);
int sceAudioOutClose(int32_t handle);
#endif

static char g_tracks[MAX_TRACKS][1024];
static int g_ntracks;
static volatile int g_run;          /* 1 while the thread should play */
static volatile int g_volume = 60;  /* percent */
static volatile float g_fade = 0;   /* 0..1, eases toward g_fade_to */
static volatile float g_fade_to = 1;
static pthread_t g_thread;
static int g_started;
static char g_now_playing[256];

static unsigned char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64 << 20)) {
        fclose(f);
        return NULL;
    }
    unsigned char *d = gs_malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        gs_free(d);
        d = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return d;
}

#ifdef GS_NATIVE
static int g_port = -1;
static int out_open(void) {
    sceAudioOutInit();
    /* user 0xFF = system, port type 0 = main, param 1 = 16-bit stereo */
    g_port = sceAudioOutOpen(0xFF, 0, 0, BLOCK, OUT_RATE, 1);
    return g_port >= 0 ? 0 : -1;
}
static void out_block(const int16_t *pcm) { sceAudioOutOutput(g_port, pcm); }
static void out_close(void) {
    if (g_port >= 0) {
        sceAudioOutOutput(g_port, NULL);   /* let the last block finish */
        sceAudioOutClose(g_port);
    }
    g_port = -1;
}
#else
/* host: no sound device; keep real-time pacing so the thread behaves the same */
static int out_open(void) { return 0; }
static void out_block(const int16_t *pcm) {
    (void)pcm;
    usleep(BLOCK * 1000000 / OUT_RATE);
}
static void out_close(void) {}
#endif

static void *music_thread(void *arg) {
    (void)arg;
    if (out_open() != 0) {
        g_run = 0;
        return NULL;
    }
    int16_t out[BLOCK * 2];
    float in[BLOCK * 2 * 4 + 8];
    int order[MAX_TRACKS];
    for (int i = 0; i < g_ntracks; i++) order[i] = i;
    srand((unsigned)time(NULL));
    int failures = 0;

    while (g_run && failures < g_ntracks) {
        /* shuffle each pass through the list */
        for (int i = g_ntracks - 1; i > 0; i--) {
            int j = rand() % (i + 1), t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        failures = 0;
        for (int ti = 0; ti < g_ntracks && g_run; ti++) {
            size_t len = 0;
            unsigned char *data = read_all(g_tracks[order[ti]], &len);
            drmp3 mp3;
            if (!data || !drmp3_init_memory(&mp3, data, len, NULL)) {
                if (data) gs_free(data);
                failures++;
                continue;
            }
            const char *base = strrchr(g_tracks[order[ti]], '/');
            snprintf(g_now_playing, sizeof g_now_playing, "%s", base ? base + 1 : g_tracks[order[ti]]);
            int ch = (int)mp3.channels;
            double step = (double)mp3.sampleRate / OUT_RATE;   /* input frames per output frame */
            double pos = 0;                                     /* position inside the input block */
            int have = 0;                                       /* frames in `in` */
            int done = 0;
            /* `in` holds stereo float frames; keep one frame of history for interpolation */
            while (g_run && !done) {
                for (int o = 0; o < BLOCK; o++) {
                    while ((int)pos + 1 >= have) {
                        /* refill: keep the last frame, read more */
                        if (have > 0) {
                            in[0] = in[(have - 1) * 2];
                            in[1] = in[(have - 1) * 2 + 1];
                            pos -= have - 1;
                            have = 1;
                        }
                        float tmp[BLOCK * 2 * 2];
                        drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, BLOCK, tmp);
                        if (got == 0) {
                            done = 1;
                            break;
                        }
                        for (drmp3_uint64 k = 0; k < got; k++) {
                            float l = tmp[k * ch], r = ch > 1 ? tmp[k * ch + 1] : l;
                            in[have * 2] = l;
                            in[have * 2 + 1] = r;
                            have++;
                        }
                    }
                    if (done) {
                        for (; o < BLOCK; o++) out[o * 2] = out[o * 2 + 1] = 0;
                        break;
                    }
                    int i0 = (int)pos;
                    float f = (float)(pos - i0);
                    float l = in[i0 * 2] * (1 - f) + in[(i0 + 1) * 2] * f;
                    float r = in[i0 * 2 + 1] * (1 - f) + in[(i0 + 1) * 2 + 1] * f;
                    /* fade + volume */
                    g_fade += (g_fade_to - g_fade) * 0.00003f;
                    float gain = g_fade * g_volume / 100.0f;
                    int sl = (int)(l * gain * 32767), sr = (int)(r * gain * 32767);
                    if (sl > 32767) sl = 32767;
                    if (sl < -32768) sl = -32768;
                    if (sr > 32767) sr = 32767;
                    if (sr < -32768) sr = -32768;
                    out[o * 2] = (int16_t)sl;
                    out[o * 2 + 1] = (int16_t)sr;
                    pos += step;
                }
                out_block(out);
            }
            drmp3_uninit(&mp3);
            gs_free(data);
        }
    }
    out_close();
    g_run = 0;
    return NULL;
}

/* paths: real file paths. Returns 0 when playing. */
int music_start(char (*paths)[1024], int n) {
    if (g_started) return 0;
    g_ntracks = 0;
    for (int i = 0; i < n && g_ntracks < MAX_TRACKS; i++) snprintf(g_tracks[g_ntracks++], 1024, "%s", paths[i]);
    if (!g_ntracks) return -1;
    g_run = 1;
    g_fade = 0;
    g_fade_to = 1;
    if (pthread_create(&g_thread, NULL, music_thread, NULL) != 0) {
        g_run = 0;
        return -1;
    }
    g_started = 1;
    return 0;
}

void music_set_volume(int pct) { g_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct; }

/* fade out and stop (waits up to about a second) */
void music_stop(void) {
    if (!g_started) return;
    g_fade_to = 0;
    for (int i = 0; i < 40 && g_fade > 0.02f && g_run; i++) usleep(25000);
    g_run = 0;
    pthread_join(g_thread, NULL);
    g_started = 0;
}

int music_playing(void) { return g_started && g_run; }

const char *music_now(void) { return g_now_playing; }
