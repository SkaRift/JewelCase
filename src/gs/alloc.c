/* Jewel Case memory allocator.
 *
 * In a native title the C library heap stops handing out memory after a few
 * megabytes, while direct memory (the pool the screen buffers come from) has
 * plenty. Large blocks (images, fonts, the screen) are therefore taken from
 * direct memory, one mapping each; small blocks stay on the normal heap.
 *
 * Every block carries a 64-byte header with its size and kind, so free and
 * realloc work for both kinds. SDL, stb, cJSON and Jewel Case all use these
 * functions (SDL_SetMemoryFunctions / macros).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GS_HDR 64
#define GS_MAGIC_SMALL 0x47534d4cu   /* "GSML" */
#define GS_MAGIC_LARGE 0x47534c47u   /* "GSLG" */
#define GS_LARGE_FROM (256u * 1024u)

typedef struct {
    uint32_t magic;
    uint32_t pad;
    size_t size;        /* bytes the caller asked for */
    size_t mapped;      /* large: bytes mapped */
    int64_t phys;       /* large: direct memory address */
} gs_hdr;

#ifdef GS_NATIVE
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
size_t sceKernelGetDirectMemorySize(void);
int munmap(void *addr, size_t len);

#define DM_ALIGN 0x10000          /* 64 KB */
#define DM_TYPE_WB_ONION 0        /* CPU cached */
#define DM_PROT_CPU_RW 0x3

static void *large_alloc(size_t size) {
    size_t mapped = (size + GS_HDR + DM_ALIGN - 1) & ~(size_t)(DM_ALIGN - 1);
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), mapped, DM_ALIGN,
                                      DM_TYPE_WB_ONION, &phys) < 0)
        return NULL;
    void *addr = NULL;
    if (sceKernelMapDirectMemory(&addr, mapped, DM_PROT_CPU_RW, 0, phys, DM_ALIGN) < 0) {
        sceKernelReleaseDirectMemory(phys, mapped);
        return NULL;
    }
    gs_hdr *h = (gs_hdr *)addr;
    h->magic = GS_MAGIC_LARGE;
    h->size = size;
    h->mapped = mapped;
    h->phys = phys;
    return (char *)addr + GS_HDR;
}

static void large_free(gs_hdr *h) {
    size_t mapped = h->mapped;
    int64_t phys = h->phys;
    munmap(h, mapped);
    sceKernelReleaseDirectMemory(phys, mapped);
}
#endif

void *gs_malloc(size_t size) {
#ifdef GS_NATIVE
    if (size >= GS_LARGE_FROM) {
        void *p = large_alloc(size);
        if (p) return p;
        /* fall through: try the heap */
    }
#endif
    gs_hdr *h = (gs_hdr *)malloc(size + GS_HDR);
    if (!h) {
#ifdef GS_NATIVE
        if (size < GS_LARGE_FROM) return large_alloc(size);   /* heap full: use direct memory */
#endif
        return NULL;
    }
    h->magic = GS_MAGIC_SMALL;
    h->size = size;
    return (char *)h + GS_HDR;
}

void gs_free(void *p) {
    if (!p) return;
    gs_hdr *h = (gs_hdr *)((char *)p - GS_HDR);
#ifdef GS_NATIVE
    if (h->magic == GS_MAGIC_LARGE) {
        large_free(h);
        return;
    }
#endif
    h->magic = 0;
    free(h);
}

void *gs_calloc(size_t n, size_t size) {
    if (size && n > (size_t)-1 / size) return NULL;
    void *p = gs_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *gs_realloc(void *p, size_t size) {
    if (!p) return gs_malloc(size);
    if (!size) {
        gs_free(p);
        return NULL;
    }
    gs_hdr *h = (gs_hdr *)((char *)p - GS_HDR);
    if (h->magic == GS_MAGIC_SMALL && size < GS_LARGE_FROM) {
        gs_hdr *n = (gs_hdr *)realloc(h, size + GS_HDR);
        if (n) {
            n->size = size;
            return (char *)n + GS_HDR;
        }
    }
#ifdef GS_NATIVE
    if (h->magic == GS_MAGIC_LARGE && size <= h->mapped - GS_HDR) {
        h->size = size;
        return p;
    }
#endif
    void *q = gs_malloc(size);
    if (!q) return NULL;
    memcpy(q, p, h->size < size ? h->size : size);
    gs_free(p);
    return q;
}
