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


/* ------------------------------------------------------------------------ */
/* menu sounds: a low, soft retro "boop", made here (nothing recorded)       */
/* ------------------------------------------------------------------------ */

#include <math.h>

#define SFX_KINDS 2
static int16_t *g_sfx[SFX_KINDS];
static int g_sfx_len[SFX_KINDS];
static volatile int g_sfx_on = 1;
static volatile int g_sfx_req = -1;      /* the sound to start, -1 none */
static pthread_mutex_t g_sfx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sfx_cv = PTHREAD_COND_INITIALIZER;
static int g_sfx_started;

/* kind 0: moving between games and options; 1: changing system (a little lower) */
static void sfx_make(int kind) {
    float f0 = kind ? 196.0f : 262.0f, f1 = kind ? 147.0f : 208.0f;   /* a falling pitch */
    float dur = kind ? 0.095f : 0.075f;
    int n = (int)(dur * OUT_RATE);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * n);
    if (!pcm) return;
    float phase = 0, lp = 0;
    const float a_lp = expf(-2.0f * 3.14159265f * 1800.0f / OUT_RATE);   /* rounds off the square */
    for (int i = 0; i < n; i++) {
        float t = (float)i / OUT_RATE, u = t / dur;
        float f = f0 + (f1 - f0) * u;
        phase += f / OUT_RATE;
        if (phase >= 1) phase -= 1;
        float sq = phase < 0.5f ? 1.0f : -1.0f;
        lp = (1 - a_lp) * sq + a_lp * lp;
        float env = fminf(1.0f, t / 0.003f) * expf(-t / (dur * 0.35f));
        float v = lp * env * 0.32f;
        int sv = (int)(v * 32767);
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)sv;
    }
    g_sfx[kind] = pcm;
    g_sfx_len[kind] = n;
}

#ifdef GS_NATIVE
static int g_sfx_port = -1;
#endif

static void *sfx_thread(void *arg) {
    (void)arg;
#ifdef GS_NATIVE
    sceAudioOutInit();
    g_sfx_port = sceAudioOutOpen(0xFF, 0, 0, BLOCK, OUT_RATE, 1);
    if (g_sfx_port < 0) return NULL;
#endif
    int16_t out[BLOCK * 2];
    for (;;) {
        pthread_mutex_lock(&g_sfx_mu);
        while (g_sfx_req < 0) pthread_cond_wait(&g_sfx_cv, &g_sfx_mu);
        int kind = g_sfx_req;
        g_sfx_req = -1;
        pthread_mutex_unlock(&g_sfx_mu);
        const int16_t *pcm = g_sfx[kind];
        int len = g_sfx_len[kind], pos = 0;
        while (pcm && pos < len) {
            float gain = g_volume / 100.0f;
            for (int o = 0; o < BLOCK; o++, pos++) {
                int l = pos < len ? (int)(pcm[pos * 2] * gain) : 0;
                out[o * 2] = out[o * 2 + 1] = (int16_t)l;
            }
#ifdef GS_NATIVE
            sceAudioOutOutput(g_sfx_port, out);
#else
            usleep(BLOCK * 1000000 / OUT_RATE);
#endif
            /* a new sound cuts this one short, so quick presses don't pile up */
            if (g_sfx_req >= 0) {
                pthread_mutex_lock(&g_sfx_mu);
                if (g_sfx_req >= 0) {
                    kind = g_sfx_req;
                    g_sfx_req = -1;
                    pcm = g_sfx[kind];
                    len = g_sfx_len[kind];
                    pos = 0;
                }
                pthread_mutex_unlock(&g_sfx_mu);
            }
        }
#ifdef GS_NATIVE
        memset(out, 0, sizeof out);
        sceAudioOutOutput(g_sfx_port, out);   /* end on silence */
#endif
    }
    return NULL;
}

void sfx_set_on(int on) { g_sfx_on = on; }

void sfx_play(int kind) {
    if (!g_sfx_on || kind < 0 || kind >= SFX_KINDS || g_volume <= 0) return;
    if (!g_sfx_started) {
        for (int k = 0; k < SFX_KINDS; k++) sfx_make(k);
        pthread_t th;
        if (pthread_create(&th, NULL, sfx_thread, NULL) != 0) return;
        pthread_detach(th);
        g_sfx_started = 1;
    }
    pthread_mutex_lock(&g_sfx_mu);
    g_sfx_req = kind;
    pthread_cond_signal(&g_sfx_cv);
    pthread_mutex_unlock(&g_sfx_mu);
}
