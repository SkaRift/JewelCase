/* The version a Switch .nsp shows ("1.4.0"), read from its control data.
 *
 * An update's title version (v327680) only counts releases; the text the console
 * shows is the display version in the title's control.nacp, which is inside its
 * encrypted Control NCA. This reads it with the user's own keys (ProsperoEden's
 * <game files folder>/keys/prod.keys, and title.keys when a ticket doesn't
 * carry the title key). Jewel Case ships no keys.
 *
 *   PFS0 (the .nsp) -> each .nca's header (AES-XTS, header_key): the Control one
 *   -> its key: the title key from the .tik (or title.keys), decrypted with
 *      titlekek_NN; or the key area, decrypted with key_area_key_*_NN
 *   -> section 0 (RomFS, AES-CTR) -> control.nacp -> display version at 0x3060
 *
 * Shared by jc-scan.c (the console) and main.c (the PC build). Plain C, no
 * libraries: a small AES-128 is included.
 *
 * Copyright (C) 2026 SkaRift
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JC_NX_VERSION_H
#define JC_NX_VERSION_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* ------------------------------------------------------------------------ */
/* AES-128                                                                   */
/* ------------------------------------------------------------------------ */

static const uint8_t nx_sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

typedef struct { uint8_t rk[176]; uint8_t inv[256]; } NxAes;

static uint8_t nx_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t nx_mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while(b) {
    if(b & 1) r ^= a;
    a = nx_xt(a);
    b >>= 1;
  }
  return r;
}

static void nx_aes_init(NxAes *c, const uint8_t key[16]) {
  static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
  memcpy(c->rk, key, 16);
  for(int i = 4; i < 44; i++) {
    uint8_t t[4];
    memcpy(t, c->rk + (i - 1) * 4, 4);
    if(i % 4 == 0) {
      uint8_t u = t[0];
      t[0] = (uint8_t)(nx_sbox[t[1]] ^ rcon[i / 4 - 1]);
      t[1] = nx_sbox[t[2]];
      t[2] = nx_sbox[t[3]];
      t[3] = nx_sbox[u];
    }
    for(int j = 0; j < 4; j++) c->rk[i * 4 + j] = (uint8_t)(c->rk[(i - 4) * 4 + j] ^ t[j]);
  }
  for(int i = 0; i < 256; i++) c->inv[nx_sbox[i]] = (uint8_t)i;
}

static void nx_aes_enc(const NxAes *c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  for(int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ c->rk[i]);
  for(int r = 1; r <= 10; r++) {
    uint8_t t[16];
    for(int i = 0; i < 16; i++) t[i] = nx_sbox[s[i]];
    /* shift rows (column-major state) */
    for(int col = 0; col < 4; col++)
      for(int row = 0; row < 4; row++) s[col * 4 + row] = t[((col + row) % 4) * 4 + row];
    if(r != 10)
      for(int col = 0; col < 4; col++) {
        uint8_t *p = s + col * 4, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(nx_xt(a0) ^ nx_xt(a1) ^ a1 ^ a2 ^ a3);
        p[1] = (uint8_t)(a0 ^ nx_xt(a1) ^ nx_xt(a2) ^ a2 ^ a3);
        p[2] = (uint8_t)(a0 ^ a1 ^ nx_xt(a2) ^ nx_xt(a3) ^ a3);
        p[3] = (uint8_t)(nx_xt(a0) ^ a0 ^ a1 ^ a2 ^ nx_xt(a3));
      }
    for(int i = 0; i < 16; i++) s[i] ^= c->rk[r * 16 + i];
  }
  memcpy(out, s, 16);
}

static void nx_aes_dec(const NxAes *c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  for(int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ c->rk[160 + i]);
  for(int r = 9; r >= 0; r--) {
    uint8_t t[16];
    /* inverse shift rows, inverse sub bytes */
    for(int col = 0; col < 4; col++)
      for(int row = 0; row < 4; row++) t[((col + row) % 4) * 4 + row] = s[col * 4 + row];
    for(int i = 0; i < 16; i++) s[i] = (uint8_t)(c->inv[t[i]] ^ c->rk[r * 16 + i]);
    if(r != 0)
      for(int col = 0; col < 4; col++) {
        uint8_t *p = s + col * 4, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(nx_mul(a0, 14) ^ nx_mul(a1, 11) ^ nx_mul(a2, 13) ^ nx_mul(a3, 9));
        p[1] = (uint8_t)(nx_mul(a0, 9) ^ nx_mul(a1, 14) ^ nx_mul(a2, 11) ^ nx_mul(a3, 13));
        p[2] = (uint8_t)(nx_mul(a0, 13) ^ nx_mul(a1, 9) ^ nx_mul(a2, 14) ^ nx_mul(a3, 11));
        p[3] = (uint8_t)(nx_mul(a0, 11) ^ nx_mul(a1, 13) ^ nx_mul(a2, 9) ^ nx_mul(a3, 14));
      }
  }
  memcpy(out, s, 16);
}

static void nx_ecb_dec(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  NxAes c;
  nx_aes_init(&c, key);
  nx_aes_dec(&c, in, out);
}

/* Nintendo's AES-XTS: 0x200-byte sectors, the sector number as a big-endian tweak */
static void nx_xts_dec(const uint8_t key[32], uint8_t *buf, size_t len, uint64_t sector) {
  NxAes k1, k2;
  nx_aes_init(&k1, key);
  nx_aes_init(&k2, key + 16);
  for(size_t off = 0; off + 0x200 <= len; off += 0x200, sector++) {
    uint8_t tw[16] = {0}, t[16];
    for(int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sector >> (8 * i));
    nx_aes_enc(&k2, tw, t);
    for(size_t b = 0; b < 0x200; b += 16) {
      uint8_t x[16];
      for(int i = 0; i < 16; i++) x[i] = (uint8_t)(buf[off + b + i] ^ t[i]);
      nx_aes_dec(&k1, x, x);
      for(int i = 0; i < 16; i++) buf[off + b + i] = (uint8_t)(x[i] ^ t[i]);
      /* t *= alpha */
      uint8_t carry = (uint8_t)(t[15] >> 7);
      for(int i = 15; i > 0; i--) t[i] = (uint8_t)((t[i] << 1) | (t[i - 1] >> 7));
      t[0] = (uint8_t)((t[0] << 1) ^ (carry ? 0x87 : 0));
    }
  }
}

/* AES-CTR over `len` bytes that sit at absolute offset `abs` in the NCA */
static void nx_ctr(const uint8_t key[16], const uint8_t ctr_hi[8], uint64_t abs, uint8_t *buf, size_t len) {
  NxAes c;
  nx_aes_init(&c, key);
  uint64_t blk = abs >> 4;
  size_t skip = (size_t)(abs & 15), done = 0;
  while(done < len) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, ctr_hi, 8);
    for(int i = 0; i < 8; i++) ctr[15 - i] = (uint8_t)(blk >> (8 * i));
    nx_aes_enc(&c, ctr, ks);
    for(size_t i = skip; i < 16 && done < len; i++) buf[done++] ^= ks[i];
    skip = 0;
    blk++;
  }
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

#define NX_GENS 32

typedef struct { uint8_t rid[16], key[16]; } NxTitleKey;

typedef struct {
  int have_header;
  uint8_t header_key[32];
  uint32_t have_tkek, have_kak[3];
  uint8_t titlekek[NX_GENS][16];
  uint8_t kak[3][NX_GENS][16];   /* key_area_key_application / ocean / system */
  NxTitleKey *tk;
  int ntk;
} NxKeys;

static int nx_hex(const char *s, uint8_t *out, size_t n) {
  for(size_t i = 0; i < n; i++) {
    int v = 0;
    for(int k = 0; k < 2; k++) {
      char c = s[i * 2 + k];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if(d < 0) return -1;
      v = v * 16 + d;
    }
    out[i] = (uint8_t)v;
  }
  return 0;
}

/* "name = hexvalue" lines: calls fn for each */
static void nx_key_lines(const char *path, void (*fn)(NxKeys *, const char *, const char *), NxKeys *k) {
  FILE *f = fopen(path, "r");
  if(!f) return;
  char line[512];
  while(fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '=');
    if(!eq) continue;
    char *a = line, *b = eq + 1;
    *eq = 0;
    while(*a == ' ' || *a == '\t') a++;
    char *e = a + strlen(a);
    while(e > a && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    while(*b == ' ' || *b == '\t') b++;
    e = b + strlen(b);
    while(e > b && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if(*a && *b) fn(k, a, b);
  }
  fclose(f);
}

static void nx_prod_key(NxKeys *k, const char *name, const char *val) {
  size_t vl = strlen(val);
  if(!strcasecmp(name, "header_key") && vl == 64) {
    if(!nx_hex(val, k->header_key, 32)) k->have_header = 1;
    return;
  }
  static const char *const kak[3] = {"key_area_key_application_", "key_area_key_ocean_", "key_area_key_system_"};
  const char *num = NULL;
  int which = -1;
  if(!strncasecmp(name, "titlekek_", 9)) num = name + 9;
  for(int i = 0; i < 3 && !num; i++)
    if(!strncasecmp(name, kak[i], strlen(kak[i]))) {
      num = name + strlen(kak[i]);
      which = i;
    }
  if(!num || strlen(num) != 2 || vl != 32) return;
  uint8_t g;
  if(nx_hex(num, &g, 1) || g >= NX_GENS) return;
  if(which < 0) {
    if(!nx_hex(val, k->titlekek[g], 16)) k->have_tkek |= 1u << g;
  } else if(!nx_hex(val, k->kak[which][g], 16)) {
    k->have_kak[which] |= 1u << g;
  }
}

static void nx_title_key(NxKeys *k, const char *name, const char *val) {
  if(strlen(name) != 32 || strlen(val) != 32) return;
  NxTitleKey t;
  if(nx_hex(name, t.rid, 16) || nx_hex(val, t.key, 16)) return;
  NxTitleKey *n = (NxTitleKey *)realloc(k->tk, (size_t)(k->ntk + 1) * sizeof(NxTitleKey));
  if(!n) return;
  k->tk = n;
  k->tk[k->ntk++] = t;
}

/* load <dir>/prod.keys and <dir>/title.keys; returns 1 when the header key is there */
static int nx_keys_load(NxKeys *k, const char *prod_path, const char *title_path) {
  free(k->tk);
  memset(k, 0, sizeof *k);
  nx_key_lines(prod_path, nx_prod_key, k);
  if(title_path) nx_key_lines(title_path, nx_title_key, k);
  return k->have_header;
}

/* ------------------------------------------------------------------------ */
/* The .nsp                                                                  */
/* ------------------------------------------------------------------------ */

static int nx_read_at(FILE *f, uint64_t off, void *buf, size_t len) {
  if(fseeko(f, (off_t)off, SEEK_SET)) return -1;
  return fread(buf, 1, len, f) == len ? 0 : -1;
}
static uint32_t nx_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t nx_u64(const uint8_t *p) { return nx_u32(p) | ((uint64_t)nx_u32(p + 4) << 32); }

/* the encrypted title key a ticket in the .nsp holds for `rid` (0 found) */
static int nx_ticket_key(FILE *f, const uint8_t *pfs, size_t table, size_t data, unsigned n, const uint8_t rid[16],
                         uint8_t out[16]) {
  char want[37];
  for(int i = 0; i < 16; i++) snprintf(want + i * 2, 3, "%02x", rid[i]);
  memcpy(want + 32, ".tik", 5);
  for(unsigned i = 0; i < n; i++) {
    const uint8_t *e = pfs + 16 + (size_t)i * 24;
    const char *name = (const char *)pfs + table + nx_u32(e + 16);
    if(strcasecmp(name, want)) continue;
    uint8_t t[0x400];
    uint64_t sz = nx_u64(e + 8);
    if(sz < 0x2C0 || nx_read_at(f, data + nx_u64(e), t, sizeof t < sz ? sizeof t : (size_t)sz)) return -1;
    size_t sig;
    switch(nx_u32(t)) {
    case 0x10000: case 0x10003: sig = 0x200 + 0x3C; break;   /* RSA-4096 */
    case 0x10001: case 0x10004: sig = 0x100 + 0x3C; break;   /* RSA-2048 */
    case 0x10002: case 0x10005: sig = 0x3C + 0x40; break;    /* ECDSA */
    default: return -1;
    }
    const uint8_t *d = t + 4 + sig;
    if(4 + sig + 0x180 > sizeof t || d[0x141] != 0) return -1;   /* 0: common; personalized needs title.keys */
    memcpy(out, d + 0x40, 16);
    return 0;
  }
  return -1;
}

/* the display version of the title in this .nsp, from its Control NCA (0 found) */
static int nx_nsp_version(FILE *f, const NxKeys *k, char *out, size_t cap) {
  if(!k || !k->have_header || cap < 2) return -1;
  static uint8_t pfs[256 * 1024];
  if(fseeko(f, 0, SEEK_SET)) return -1;
  size_t got = fread(pfs, 1, sizeof pfs, f);
  if(got < 16 || memcmp(pfs, "PFS0", 4)) return -1;
  unsigned n = nx_u32(pfs + 4), strsz = nx_u32(pfs + 8);
  size_t table = 16 + (size_t)n * 24, data = table + strsz;
  if(!n || n > 4000 || data > got) return -1;
  for(unsigned i = 0; i < n; i++) {
    const uint8_t *e = pfs + 16 + (size_t)i * 24;
    unsigned no = nx_u32(e + 16);
    if(no >= strsz) continue;
    const char *name = (const char *)pfs + table + no;
    size_t l = strnlen(name, strsz - no);
    if(l < 4 || strcasecmp(name + l - 4, ".nca") || (l > 9 && !strcasecmp(name + l - 9, ".cnmt.nca"))) continue;
    uint64_t nca = data + nx_u64(e), ncasz = nx_u64(e + 8);
    if(ncasz < 0xC00) continue;
    uint8_t h[0xC00];
    if(nx_read_at(f, nca, h, sizeof h)) continue;
    nx_xts_dec(k->header_key, h, sizeof h, 0);
    if(memcmp(h + 0x200, "NCA3", 4) || h[0x205] != 2) continue;   /* NCA3, Control */
    /* its key */
    int gen = h[0x206] > h[0x220] ? h[0x206] : h[0x220];
    int rev = gen > 0 ? gen - 1 : 0;
    if(rev >= NX_GENS) continue;
    uint8_t key[16];
    static const uint8_t zero[16];
    if(memcmp(h + 0x230, zero, 16)) {
      uint8_t enc[16];
      int found = !nx_ticket_key(f, pfs, table, data, n, h + 0x230, enc);
      for(int t = 0; t < k->ntk && !found; t++)
        if(!memcmp(k->tk[t].rid, h + 0x230, 16)) {
          memcpy(enc, k->tk[t].key, 16);
          found = 1;
        }
      if(!found || !(k->have_tkek & (1u << rev))) continue;
      nx_ecb_dec(k->titlekek[rev], enc, key);
    } else {
      int which = h[0x207];
      if(which > 2 || !(k->have_kak[which] & (1u << rev))) continue;
      nx_ecb_dec(k->kak[which][rev], h + 0x300 + 2 * 16, key);   /* the AES-CTR key */
    }
    /* section 0: RomFS (IVFC); its last level is the RomFS itself */
    const uint8_t *fs = h + 0x400;
    uint64_t sec = (uint64_t)nx_u32(h + 0x240) * 0x200, sec_end = (uint64_t)nx_u32(h + 0x244) * 0x200;
    if(fs[2] != 0 || fs[3] != 3 || sec_end <= sec || memcmp(fs + 8, "IVFC", 4)) continue;
    int crypt = fs[4];
    if(crypt != 1 && crypt != 3) continue;   /* none, or AES-CTR */
    uint64_t romfs = sec + nx_u64(fs + 8 + 0x10 + 5 * 0x18);
    uint8_t ctr_hi[8];
    for(int b = 0; b < 8; b++) ctr_hi[b] = fs[0x140 + 7 - b];
#define NX_READ(off, buf, len)                                                    \
    (nx_read_at(f, nca + (off), (buf), (len)) ? -1                                \
     : (crypt == 3 ? nx_ctr(key, ctr_hi, (off), (uint8_t *)(buf), (len)) : (void)0, 0))
    uint8_t rh[0x50];
    if(NX_READ(romfs, rh, sizeof rh) || nx_u64(rh) != 0x50) continue;
    uint64_t fmeta = nx_u64(rh + 0x38), fmeta_sz = nx_u64(rh + 0x40), fdata = nx_u64(rh + 0x48);
    if(!fmeta_sz || fmeta_sz > 1024 * 1024) continue;
    uint8_t *m = (uint8_t *)malloc((size_t)fmeta_sz);
    if(!m) continue;
    int ok = -1;
    if(!NX_READ(romfs + fmeta, m, (size_t)fmeta_sz)) {
      for(uint64_t p = 0; p + 0x20 <= fmeta_sz && ok < 0;) {
        uint64_t doff = nx_u64(m + p + 8), dsz = nx_u64(m + p + 16);
        uint32_t nl = nx_u32(m + p + 28);
        if(p + 0x20 + nl > fmeta_sz) break;
        if(nl == 12 && !memcmp(m + p + 0x20, "control.nacp", 12) && dsz >= 0x3070) {
          char v[17];
          if(!NX_READ(romfs + fdata + doff + 0x3060, v, 16)) {
            v[16] = 0;
            size_t o = 0;
            for(size_t c = 0; v[c] && o + 1 < cap; c++)
              if(v[c] > ' ' && v[c] < 0x7f) out[o++] = v[c];
            out[o] = 0;
            ok = o ? 0 : -1;
          }
        }
        p += 0x20 + ((nl + 3) & ~3u);
      }
    }
#undef NX_READ
    free(m);
    if(!ok) return 0;
  }
  return -1;
}

#endif
