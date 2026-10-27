/*
 * athdemo - WPA2-PSK + ping demo for PIEG.
 *
 *   athdemo selftest                     crypto known-answer tests
 *   athdemo probe                        find Atheros cards, read AR_SREV
 *   athdemo sim [pass [target]]          full run against a built-in fake AP
 *   athdemo connect ssid pass [ip gw [target]]   real card (see below)
 *
 * What is real here: SHA-1, HMAC, PBKDF2 (PMK), AES, AES key wrap, CCM/CCMP,
 * 802.11 data frames, the WPA2 4-way handshake (supplicant side), ARP, IPv4,
 * ICMP echo, and a fake AP so all of it runs end to end without hardware.
 *
 * What is NOT here: the ath9k radio itself. Bringing up an AR9565 needs the
 * chip's register init tables, EEPROM/calibration, TX/RX DMA rings and 802.11
 * management (scan/auth/assoc). Those are not in this file, so `connect`
 * identifies the card and then stops. The nic_t struct is where that driver
 * plugs in. Limits: WPA2-PSK/CCMP only, static IP (no DHCP), no group-key
 * rekey, weak RNG (TSC based).
 *
 * Build for host tests:  gcc -DATHDEMO_HOST -O2 -Wall -Wextra athdemo.c
 */

#ifdef ATHDEMO_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void p_putc(char c) { putchar(c); }
static void p_puts(const char *s) { fputs(s, stdout); }
static void p_putln(const char *s) { puts(s); }
static void p_u64(uint64_t n) { printf("%llu", (unsigned long long)n); }
static void p_hex(uint64_t n) { printf("0x%llx", (unsigned long long)n); }
static size_t p_strlen(const char *s) { return strlen(s); }
static int p_streq(const char *a, const char *b) { return !strcmp(a, b); }
#else
#include "libpieg.h"
#endif

#define MAXF 1600

/* ================================================================
 * Small helpers
 * ================================================================ */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static void p_byte(uint8_t b)
{
    const char *h = "0123456789abcdef";
    p_putc(h[b >> 4]);
    p_putc(h[b & 15]);
}
static void p_mac(const uint8_t *m)
{
    for (int i = 0; i < 6; i++) { if (i) p_putc(':'); p_byte(m[i]); }
}
static void p_ip(const uint8_t *a)
{
    for (int i = 0; i < 4; i++) { if (i) p_putc('.'); p_u64(a[i]); }
}

/* ================================================================
 * Time and randomness
 * ================================================================ */

static uint64_t tsc_per_ms = 2000000;   /* fallback guess: 2 GHz */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t now_ms(void) { return rdtsc() / tsc_per_ms; }

#ifndef ATHDEMO_HOST
static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Time ~10 ms of PIT channel 2 against the TSC. */
static void tsc_calibrate(void)
{
    uint8_t save = inb(0x61);
    outb(0x61, (uint8_t)((save & 0xFC) | 0x00));   /* gate off, speaker off */
    outb(0x43, 0xB0);                              /* ch2, lo/hi, mode 0 */
    outb(0x42, 0x9C);                              /* 11932 = 0x2E9C */
    outb(0x42, 0x2E);
    outb(0x61, (uint8_t)((save & 0xFC) | 0x01));   /* gate on: count starts */
    uint64_t t0 = rdtsc();
    uint32_t guard = 200000000u;
    while (!(inb(0x61) & 0x20) && --guard)
        ;
    uint64_t t1 = rdtsc();
    outb(0x61, save);
    if (guard && t1 > t0)
        tsc_per_ms = (t1 - t0) / 10;
}
#else
static void tsc_calibrate(void) { }
#endif

static uint8_t rng_state[20];
static uint32_t rng_ctr;

/* sha1 is defined below; forward declare the pieces the RNG needs. */
typedef struct { uint32_t h[5]; uint8_t buf[64]; uint32_t n; uint64_t total; } sha1_t;
static void sha1_init(sha1_t *c);
static void sha1_update(sha1_t *c, const void *d, size_t n);
static void sha1_final(sha1_t *c, uint8_t out[20]);

static void rand_bytes(uint8_t *out, size_t n)
{
    while (n) {
        sha1_t c;
        uint64_t t = rdtsc();
        sha1_init(&c);
        sha1_update(&c, rng_state, 20);
        sha1_update(&c, &t, 8);
        rng_ctr++;
        sha1_update(&c, &rng_ctr, 4);
        sha1_final(&c, rng_state);
        size_t k = n < 20 ? n : 20;
        memcpy(out, rng_state, k);
        out += k;
        n -= k;
    }
}

/* ================================================================
 * SHA-1, HMAC-SHA1, PBKDF2
 * ================================================================ */

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_init(sha1_t *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->n = 0;
    c->total = 0;
}

static void sha1_update(sha1_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->total += n;
    while (n) {
        size_t k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += (uint32_t)k;
        p += k;
        n -= k;
        if (c->n == 64) { sha1_block(c->h, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_t *c, uint8_t out[20])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha1_update(c, &pad, 1);
    uint8_t len[8];
    put64(len, bits);
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4*i]   = (uint8_t)(c->h[i] >> 24);
        out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8);
        out[4*i+3] = (uint8_t)c->h[i];
    }
}

static void hmac_sha1(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t out[20])
{
    uint8_t k[64], pad[64], inner[20];
    sha1_t c;

    memset(k, 0, sizeof k);
    if (kl > 64) {
        sha1_init(&c); sha1_update(&c, key, kl); sha1_final(&c, k);
    } else {
        memcpy(k, key, kl);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c); sha1_update(&c, pad, 64); sha1_update(&c, msg, ml); sha1_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha1_init(&c); sha1_update(&c, pad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* salt up to 32 bytes (an SSID). */
static void pbkdf2_sha1(const char *pass, const uint8_t *salt, size_t sl,
                        uint32_t iter, uint8_t *out, size_t outlen)
{
    uint8_t s[36], u[20], t[20];
    size_t pl = p_strlen(pass);

    for (uint32_t blk = 1; outlen; blk++) {
        memcpy(s, salt, sl);
        s[sl] = (uint8_t)(blk >> 24); s[sl+1] = (uint8_t)(blk >> 16);
        s[sl+2] = (uint8_t)(blk >> 8); s[sl+3] = (uint8_t)blk;
        hmac_sha1((const uint8_t *)pass, pl, s, sl + 4, u);
        memcpy(t, u, 20);
        for (uint32_t i = 1; i < iter; i++) {
            hmac_sha1((const uint8_t *)pass, pl, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t k = outlen < 20 ? outlen : 20;
        memcpy(out, t, k);
        out += k;
        outlen -= k;
    }
}

/* ================================================================
 * AES-128, key wrap (RFC 3394), CCM (M=8, L=2)
 * ================================================================ */

static uint8_t sbox[256], isbox[256];
static int aes_ready;

typedef struct { uint8_t rk[176]; } aes_t;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gm(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}
static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

static void aes_tables(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) isbox[sbox[i]] = (uint8_t)i;
    aes_ready = 1;
}

static void aes_set(aes_t *a, const uint8_t key[16])
{
    if (!aes_ready) aes_tables();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[tmp];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = a->rk[i - 16 + j] ^ t[j];
    }
}

static void aes_enc(const aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++)
                t[4*c + w] = sbox[s[4 * ((c + w) & 3) + w]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4*c;
                uint8_t a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                s[4*c]   = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                s[4*c+1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                s[4*c+2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                s[4*c+3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16*r + i];
    }
    memcpy(out, s, 16);
}

static void aes_dec(const aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++)
                t[4*c + w] = isbox[s[4 * ((c + 4 - w) & 3) + w]];
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ a->rk[16*r + i];
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4*c], a1 = s[4*c+1], a2 = s[4*c+2], a3 = s[4*c+3];
                s[4*c]   = gm(a0,14) ^ gm(a1,11) ^ gm(a2,13) ^ gm(a3, 9);
                s[4*c+1] = gm(a0, 9) ^ gm(a1,14) ^ gm(a2,11) ^ gm(a3,13);
                s[4*c+2] = gm(a0,13) ^ gm(a1, 9) ^ gm(a2,14) ^ gm(a3,11);
                s[4*c+3] = gm(a0,11) ^ gm(a1,13) ^ gm(a2, 9) ^ gm(a3,14);
            }
        }
    }
    memcpy(out, s, 16);
}

/* n = wrapped length (multiple of 8, >= 24). Returns 0 on success. */
static int aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out)
{
    aes_t a;
    uint8_t A[8], B[16];
    size_t nn = n / 8 - 1;

    aes_set(&a, kek);
    memcpy(A, in, 8);
    memcpy(out, in + 8, nn * 8);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = nn; i >= 1; i--) {
            uint64_t t = (uint64_t)nn * (uint64_t)j + i;
            memcpy(B, A, 8);
            B[7] ^= (uint8_t)t; B[6] ^= (uint8_t)(t >> 8);
            memcpy(B + 8, out + 8 * (i - 1), 8);
            aes_dec(&a, B, B);
            memcpy(A, B, 8);
            memcpy(out + 8 * (i - 1), B + 8, 8);
        }
    }
    for (int i = 0; i < 8; i++)
        if (A[i] != 0xA6) return -1;
    return 0;
}

/* out is n + 8 bytes. */
static void aes_wrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out)
{
    aes_t a;
    uint8_t A[8], B[16];
    size_t nn = n / 8;

    aes_set(&a, kek);
    memset(A, 0xA6, 8);
    memcpy(out + 8, in, n);
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i <= nn; i++) {
            uint64_t t = (uint64_t)nn * (uint64_t)j + i;
            memcpy(B, A, 8);
            memcpy(B + 8, out + 8 * i, 8);
            aes_enc(&a, B, B);
            memcpy(A, B, 8);
            A[7] ^= (uint8_t)t; A[6] ^= (uint8_t)(t >> 8);
            memcpy(out + 8 * i, B + 8, 8);
        }
    }
    memcpy(out, A, 8);
}

/* CCM with M = 8, L = 2, 13-byte nonce, aad up to 30 bytes. */
static void ccm_mac(const aes_t *a, const uint8_t nonce[13], const uint8_t *aad, size_t alen,
                    const uint8_t *msg, size_t mlen, uint8_t tag[8])
{
    uint8_t x[16], b[16], ab[48];

    b[0] = 0x59;
    memcpy(b + 1, nonce, 13);
    put16(b + 14, (uint16_t)mlen);
    aes_enc(a, b, x);

    memset(ab, 0, sizeof ab);
    put16(ab, (uint16_t)alen);
    memcpy(ab + 2, aad, alen);
    for (size_t off = 0; off < 2 + alen; off += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= ab[off + i];
        aes_enc(a, x, x);
    }
    for (size_t off = 0; off < mlen; off += 16) {
        size_t k = mlen - off < 16 ? mlen - off : 16;
        for (size_t i = 0; i < k; i++) x[i] ^= msg[off + i];
        aes_enc(a, x, x);
    }
    memcpy(tag, x, 8);
}

/* XORs the CTR keystream (counter 1..) into data, returns S0 for the tag. */
static void ccm_ctr(const aes_t *a, const uint8_t nonce[13], uint8_t *data, size_t len, uint8_t s0[16])
{
    uint8_t ai[16], s[16];

    ai[0] = 0x01;
    memcpy(ai + 1, nonce, 13);
    put16(ai + 14, 0);
    aes_enc(a, ai, s0);
    for (size_t off = 0, ctr = 1; off < len; off += 16, ctr++) {
        put16(ai + 14, (uint16_t)ctr);
        aes_enc(a, ai, s);
        size_t k = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < k; i++) data[off + i] ^= s[i];
    }
}

static void ccm_encrypt(const aes_t *a, const uint8_t nonce[13], const uint8_t *aad, size_t alen,
                        uint8_t *data, size_t len, uint8_t mic[8])
{
    uint8_t tag[8], s0[16];
    ccm_mac(a, nonce, aad, alen, data, len, tag);
    ccm_ctr(a, nonce, data, len, s0);
    for (int i = 0; i < 8; i++) mic[i] = tag[i] ^ s0[i];
}

static int ccm_decrypt(const aes_t *a, const uint8_t nonce[13], const uint8_t *aad, size_t alen,
                       uint8_t *data, size_t len, const uint8_t mic[8])
{
    uint8_t tag[8], s0[16];
    ccm_ctr(a, nonce, data, len, s0);
    ccm_mac(a, nonce, aad, alen, data, len, tag);
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)(tag[i] ^ s0[i] ^ mic[i]);
    return diff ? -1 : 0;
}

/* ================================================================
 * 802.11 data frames with CCMP
 * ================================================================ */

static const uint8_t llc_snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };

typedef struct { aes_t aes; uint64_t pn; int on; } ckey_t;

#define DIR_TODS   0x0100
#define DIR_FROMDS 0x0200

static void ccmp_prep(const uint8_t *h, int qos, uint64_t pn, uint8_t aad[24], size_t *alen, uint8_t nonce[13])
{
    uint16_t fc = (uint16_t)(h[0] | (h[1] << 8));
    fc &= (uint16_t)~(0x0070 | 0x0800 | 0x1000 | 0x2000);
    aad[0] = (uint8_t)fc; aad[1] = (uint8_t)(fc >> 8);
    memcpy(aad + 2, h + 4, 18);          /* A1 A2 A3 */
    aad[20] = h[22] & 0x0F;              /* fragment number only */
    aad[21] = 0;
    *alen = 22;
    nonce[0] = 0;
    if (qos) {
        aad[22] = h[24] & 0x0F; aad[23] = 0;
        *alen = 24;
        nonce[0] = h[24] & 0x0F;
    }
    memcpy(nonce + 1, h + 10, 6);        /* A2 */
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
}

static size_t d11_build(uint8_t *o, uint16_t dir, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3,
                        uint16_t et, const uint8_t *pl, size_t n, ckey_t *k, uint16_t seq)
{
    uint16_t fc = (uint16_t)(0x0008 | dir | (k ? 0x4000 : 0));
    size_t p = 24;

    if (n + 64 > MAXF) return 0;
    o[0] = (uint8_t)fc; o[1] = (uint8_t)(fc >> 8);
    o[2] = o[3] = 0;
    memcpy(o + 4, a1, 6); memcpy(o + 10, a2, 6); memcpy(o + 16, a3, 6);
    o[22] = (uint8_t)(seq << 4); o[23] = (uint8_t)((uint16_t)(seq << 4) >> 8);

    uint8_t *body = o + p + (k ? 8 : 0);
    memcpy(body, llc_snap, 6);
    put16(body + 6, et);
    memcpy(body + 8, pl, n);
    size_t bl = 8 + n;

    if (!k) return p + bl;

    uint64_t pn = ++k->pn;
    o[p]   = (uint8_t)pn;         o[p+1] = (uint8_t)(pn >> 8);
    o[p+2] = 0;                   o[p+3] = 0x20;      /* ExtIV, key id 0 */
    o[p+4] = (uint8_t)(pn >> 16); o[p+5] = (uint8_t)(pn >> 24);
    o[p+6] = (uint8_t)(pn >> 32); o[p+7] = (uint8_t)(pn >> 40);

    uint8_t aad[24], nonce[13];
    size_t alen;
    ccmp_prep(o, 0, pn, aad, &alen, nonce);
    ccm_encrypt(&k->aes, nonce, aad, alen, body, bl, body + bl);
    return p + 8 + bl + 8;
}

/* body must hold MAXF bytes; outputs point into f or body. */
static int d11_parse(const uint8_t *f, size_t n, uint16_t want_dir, ckey_t *k, uint8_t *body,
                     const uint8_t **a1, const uint8_t **a2, const uint8_t **a3,
                     uint16_t *et, const uint8_t **pl, size_t *pln)
{
    if (n < 24 || n > MAXF) return -1;
    uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
    if ((fc & 0x000C) != 0x0008) return -1;          /* data frames only */
    if ((fc & 0x0300) != want_dir) return -1;
    if (fc & 0x0070) return -1;                      /* plain or QoS data */
    int qos = (fc & 0x0080) != 0;
    size_t hl = 24 + (qos ? 2 : 0);
    if (n < hl) return -1;

    *a1 = f + 4; *a2 = f + 10; *a3 = f + 16;
    const uint8_t *b = f + hl;
    size_t blen = n - hl;
    int prot = (fc & 0x4000) != 0;

    if (prot) {
        if (!k || !k->on || blen < 8 + 8 + 8) return -1;
        uint64_t pn = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[4] << 16) |
                      ((uint64_t)b[5] << 24) | ((uint64_t)b[6] << 32) | ((uint64_t)b[7] << 40);
        if (pn <= k->pn) return -1;                  /* replay */
        size_t dl = blen - 16;
        memcpy(body, b + 8, dl);
        uint8_t aad[24], nonce[13];
        size_t alen;
        ccmp_prep(f, qos, pn, aad, &alen, nonce);
        if (ccm_decrypt(&k->aes, nonce, aad, alen, body, dl, b + 8 + dl) != 0) return -1;
        k->pn = pn;
        blen = dl;
    } else {
        memcpy(body, b, blen);
    }
    if (blen < 8 || memcmp(body, llc_snap, 6) != 0) return -1;
    *et = be16(body + 6);
    if (k && k->on && !prot && *et != 0x888E) return -1;   /* no plaintext once keyed */
    *pl = body + 8;
    *pln = blen - 8;
    return 0;
}

/* ================================================================
 * NIC interface
 * ================================================================ */

typedef struct {
    int (*connect)(const char *ssid, uint8_t bssid[6], uint8_t mac[6]);
    int (*tx)(const uint8_t *frame, size_t len);
    int (*rx)(uint8_t *buf, size_t cap);            /* length, 0 if none */
} nic_t;

static nic_t nic;
static uint8_t g_bssid[6], g_mac[6];
static ckey_t k_tx, k_rx;

static int link_send(uint16_t et, const uint8_t *dst, const uint8_t *pl, size_t n)
{
    static uint8_t f[MAXF];
    static uint16_t seq;
    size_t len = d11_build(f, DIR_TODS, g_bssid, g_mac, dst, et, pl, n, k_tx.on ? &k_tx : 0, seq++);
    return len ? nic.tx(f, len) : -1;
}

/* Returns 1 and a payload pointer valid until the next call. */
static int link_recv(uint16_t *et, uint8_t src[6], const uint8_t **pl, size_t *n)
{
    static uint8_t f[MAXF], body[MAXF];
    const uint8_t *a1, *a2, *a3;
    int len = nic.rx(f, sizeof f);

    if (len <= 0) return 0;
    if (d11_parse(f, (size_t)len, DIR_FROMDS, &k_rx, body, &a1, &a2, &a3, et, pl, n) < 0)
        return 0;
    if (memcmp(a2, g_bssid, 6) != 0) return 0;
    if (memcmp(a1, g_mac, 6) != 0 && a1[0] != 0xFF) return 0;
    memcpy(src, a3, 6);
    return 1;
}

/* ================================================================
 * WPA2 4-way handshake (supplicant)
 * ================================================================ */

typedef struct {
    uint8_t pmk[32], ptk[48], anonce[32], snonce[32], gtk[16];
    uint8_t aa[6], spa[6];
    uint64_t replay;
    int st;                  /* 0 idle, 1 got msg1, 2 done */
    int have_gtk, mic_fail;
} supp_t;

static supp_t sup;

static void ptk_derive(const uint8_t *pmk, const uint8_t *aa, const uint8_t *spa,
                       const uint8_t *an, const uint8_t *sn, uint8_t *ptk)
{
    uint8_t b[100], t[20];

    memcpy(b, "Pairwise key expansion", 22);
    b[22] = 0;
    int a_first = memcmp(aa, spa, 6) < 0;
    memcpy(b + 23, a_first ? aa : spa, 6);
    memcpy(b + 29, a_first ? spa : aa, 6);
    int an_first = memcmp(an, sn, 32) < 0;
    memcpy(b + 35, an_first ? an : sn, 32);
    memcpy(b + 67, an_first ? sn : an, 32);
    for (int i = 0; i < 3; i++) {
        b[99] = (uint8_t)i;
        hmac_sha1(pmk, 32, b, 100, t);
        memcpy(ptk + 20 * i, t, i < 2 ? 20 : 8);
    }
}

static void eapol_mic(const uint8_t *kck, uint8_t *f, size_t n)
{
    uint8_t t[20];
    memset(f + 81, 0, 16);
    hmac_sha1(kck, 16, f, n, t);
    memcpy(f + 81, t, 16);
}

static int eapol_mic_ok(const uint8_t *kck, uint8_t *f, size_t n)
{
    uint8_t save[16];
    memcpy(save, f + 81, 16);
    eapol_mic(kck, f, n);
    int ok = memcmp(save, f + 81, 16) == 0;
    memcpy(f + 81, save, 16);
    return ok;
}

/* 99-byte EAPOL-Key header plus key data. */
static size_t eapol_key(uint8_t *o, uint16_t ki, uint16_t klen, uint64_t rc, const uint8_t *nonce,
                        const uint8_t *kd, size_t kdl, const uint8_t *kck)
{
    memset(o, 0, 99 + kdl);
    o[0] = 2; o[1] = 3;
    put16(o + 2, (uint16_t)(95 + kdl));
    o[4] = 2;
    put16(o + 5, ki);
    put16(o + 7, klen);
    put64(o + 9, rc);
    if (nonce) memcpy(o + 17, nonce, 32);
    put16(o + 97, (uint16_t)kdl);
    if (kdl) memcpy(o + 99, kd, kdl);
    if (kck) eapol_mic(kck, o, 99 + kdl);
    return 99 + kdl;
}

static const uint8_t rsn_ie[22] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,             /* group cipher CCMP */
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, /* pairwise CCMP */
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, /* AKM: PSK */
    0x00, 0x00
};

/* Feed one EAPOL frame; returns the length of a reply to send (0 = none). */
static size_t supp_rx(supp_t *s, const uint8_t *in, size_t n, uint8_t *out)
{
    static uint8_t f[512], kd[512];

    if (n < 99 || n > sizeof f) return 0;
    memcpy(f, in, n);
    if (f[1] != 3 || f[4] != 2) return 0;
    size_t blen = (size_t)be16(f + 2) + 4;
    if (blen < 99 || blen > n) return 0;
    n = blen;
    uint16_t ki = be16(f + 5);
    size_t kdl = be16(f + 97);
    if (99 + kdl > n) return 0;
    if ((ki & 7) != 2 || !(ki & 0x0008)) return 0;   /* CCMP pairwise only */
    uint64_t rc = be64(f + 9);

    if (!(ki & 0x0100)) {                             /* message 1 */
        if (!(ki & 0x0080)) return 0;
        memcpy(s->anonce, f + 17, 32);
        rand_bytes(s->snonce, 32);
        ptk_derive(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
        s->replay = rc;
        s->st = 1;
        return eapol_key(out, 0x010A, 0, rc, s->snonce, rsn_ie, sizeof rsn_ie, s->ptk);
    }

    if (s->st < 1) return 0;                          /* message 3 */
    if (!eapol_mic_ok(s->ptk, f, n)) { s->mic_fail = 1; return 0; }
    if ((ki & 0x00C0) != 0x00C0) return 0;           /* install + ack */
    if (rc <= s->replay) return 0;
    if (memcmp(f + 17, s->anonce, 32) != 0) return 0;
    s->replay = rc;

    if (ki & 0x1000) {
        if (kdl < 24 || (kdl & 7) || kdl > 200) return 0;
        if (aes_unwrap(s->ptk + 16, f + 99, kdl, kd) != 0) return 0;
        kdl -= 8;
    } else {
        memcpy(kd, f + 99, kdl);
    }
    for (size_t p = 0; p + 2 <= kdl;) {
        size_t l = kd[p + 1];
        if (p + 2 + l > kdl) break;
        if (kd[p] == 0xDD && l >= 22 && kd[p+2] == 0x00 && kd[p+3] == 0x0F &&
            kd[p+4] == 0xAC && kd[p+5] == 0x01) {
            memcpy(s->gtk, kd + p + 8, 16);
            s->have_gtk = 1;
        }
        p += 2 + l;
    }
    s->st = 2;
    return eapol_key(out, 0x030A, 0, rc, 0, 0, 0, s->ptk);
}

/* ================================================================
 * ARP, IPv4, ICMP echo
 * ================================================================ */

static uint8_t my_ip[4] = { 192, 168, 1, 50 };
static uint8_t gw_ip[4] = { 192, 168, 1, 1 };
static uint8_t netmask[4] = { 255, 255, 255, 0 };
static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
static const uint8_t zero6[6];

static uint16_t csum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static void arp_pkt(uint8_t *p, uint16_t op, const uint8_t *sha, const uint8_t *spa,
                    const uint8_t *tha, const uint8_t *tpa)
{
    put16(p, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4; put16(p + 6, op);
    memcpy(p + 8, sha, 6); memcpy(p + 14, spa, 4);
    memcpy(p + 18, tha, 6); memcpy(p + 24, tpa, 4);
}

static void arp_send(uint16_t op, const uint8_t *tha, const uint8_t *tpa, const uint8_t *dst_mac)
{
    uint8_t p[28];
    arp_pkt(p, op, g_mac, my_ip, tha, tpa);
    link_send(0x0806, dst_mac, p, 28);
}

static uint16_t icmp_id = 0x5049, icmp_seq;
static int icmp_got, arp_ok;
static uint8_t icmp_from[4], arp_ip[4], arp_mac[6];

static void net_rx(void)
{
    uint16_t et;
    uint8_t src[6];
    const uint8_t *pl;
    size_t n;

    while (link_recv(&et, src, &pl, &n)) {
        if (et == 0x0806 && n >= 28) {
            uint16_t op = be16(pl + 6);
            if (op == 1 && !memcmp(pl + 24, my_ip, 4)) {
                uint8_t tha[6], tpa[4];
                memcpy(tha, pl + 8, 6); memcpy(tpa, pl + 14, 4);
                arp_send(2, tha, tpa, tha);
            } else if (op == 2) {
                memcpy(arp_ip, pl + 14, 4); memcpy(arp_mac, pl + 8, 6);
                arp_ok = 1;
            }
        } else if (et == 0x0800 && n >= 28 && pl[9] == 1) {
            size_t ihl = (size_t)(pl[0] & 15) * 4;
            if (n >= ihl + 8 && pl[ihl] == 0 && be16(pl + ihl + 4) == icmp_id &&
                be16(pl + ihl + 6) == icmp_seq && !memcmp(pl + 16, my_ip, 4)) {
                icmp_got = 1;
                memcpy(icmp_from, pl + 12, 4);
            }
        }
    }
}

static int arp_resolve(const uint8_t *ip, uint8_t *mac)
{
    for (int tries = 0; tries < 3; tries++) {
        arp_ok = 0;
        arp_send(1, zero6, ip, bcast);
        uint64_t end = now_ms() + 1000;
        while (now_ms() < end) {
            net_rx();
            if (arp_ok && !memcmp(arp_ip, ip, 4)) { memcpy(mac, arp_mac, 6); return 0; }
            arp_ok = 0;
        }
    }
    return -1;
}

static void icmp_echo_send(const uint8_t *dst, const uint8_t *mac, uint16_t seq)
{
    uint8_t p[60];

    memset(p, 0, sizeof p);
    p[0] = 0x45;
    put16(p + 2, sizeof p);
    put16(p + 4, seq);
    p[8] = 64; p[9] = 1;
    memcpy(p + 12, my_ip, 4); memcpy(p + 16, dst, 4);
    put16(p + 10, csum(p, 20));
    p[20] = 8;
    put16(p + 24, icmp_id); put16(p + 26, seq);
    for (int i = 0; i < 32; i++) p[28 + i] = (uint8_t)(0x61 + (i % 23));
    put16(p + 22, csum(p + 20, 40));
    link_send(0x0800, mac, p, sizeof p);
}

static int do_ping(const uint8_t *target, int count)
{
    uint8_t mac[6];
    int onlink = 1, got = 0;

    for (int i = 0; i < 4; i++)
        if ((target[i] & netmask[i]) != (my_ip[i] & netmask[i])) onlink = 0;
    const uint8_t *hop = onlink ? target : gw_ip;

    p_puts("resolving "); p_ip(hop); p_putln(onlink ? "" : " (gateway)");
    if (arp_resolve(hop, mac) != 0) {
        p_putln("no ARP reply from next hop");
        return 1;
    }
    p_puts("  is at "); p_mac(mac); p_putln("");
    p_puts("PING "); p_ip(target); p_putln("");

    for (int i = 1; i <= count; i++) {
        icmp_seq = (uint16_t)i;
        icmp_got = 0;
        uint64_t t0 = rdtsc();
        icmp_echo_send(target, mac, icmp_seq);
        uint64_t end = now_ms() + 1000;
        while (now_ms() < end && !icmp_got)
            net_rx();
        if (icmp_got) {
            got++;
            p_puts("reply from "); p_ip(icmp_from);
            p_puts(": seq="); p_u64((uint64_t)i);
            p_puts(" time="); p_u64((rdtsc() - t0) * 1000 / tsc_per_ms); p_putln(" us");
        } else {
            p_puts("seq="); p_u64((uint64_t)i); p_putln(" timeout");
        }
    }
    p_u64((uint64_t)count); p_puts(" sent, "); p_u64((uint64_t)got); p_putln(" received");
    return got ? 0 : 1;
}

/* ================================================================
 * Fake AP (WPA2-PSK, CCMP, answers ARP for the gateway and any ping)
 * ================================================================ */

#define SIM_SSID "PIEG-SIM"
#define SIM_PASS "pieg-sim-pass"
#define SIMQ 8

typedef struct {
    uint8_t pmk[32], ptk[48], anonce[32], gtk[16], bssid[6], sta[6];
    uint64_t replay;
    int st;                 /* 0 idle, 1 sent msg1, 2 sent msg3, 3 keyed */
    ckey_t tx, rx;
    uint8_t q[SIMQ][320];
    size_t ql[SIMQ];
    int qh, qt;
    uint16_t seq;
    const char *pass;
} sim_t;

static sim_t sim;

static void sim_push(const uint8_t *f, size_t n)
{
    if (!n || n > sizeof sim.q[0] || ((sim.qt + 1) % SIMQ) == sim.qh) return;
    memcpy(sim.q[sim.qt], f, n);
    sim.ql[sim.qt] = n;
    sim.qt = (sim.qt + 1) % SIMQ;
}

static void sim_send(uint16_t et, const uint8_t *pl, size_t n, int protect)
{
    uint8_t f[320];
    size_t len = d11_build(f, DIR_FROMDS, sim.sta, sim.bssid, sim.bssid, et, pl, n,
                           protect ? &sim.tx : 0, sim.seq++);
    sim_push(f, len);
}

static int sim_connect(const char *ssid, uint8_t bssid[6], uint8_t mac[6])
{
    static const uint8_t ap[6] = { 0x02, 0x50, 0x49, 0x45, 0x47, 0x01 };
    static const uint8_t sta[6] = { 0x02, 0x50, 0x49, 0x45, 0x47, 0x99 };
    uint8_t m1[99];

    memset(&sim, 0, sizeof sim);
    sim.pass = SIM_PASS;
    memcpy(sim.bssid, ap, 6); memcpy(sim.sta, sta, 6);
    memcpy(bssid, ap, 6); memcpy(mac, sta, 6);
    pbkdf2_sha1(sim.pass, (const uint8_t *)ssid, p_strlen(ssid), 4096, sim.pmk, 32);
    rand_bytes(sim.anonce, 32);
    rand_bytes(sim.gtk, 16);
    sim.replay = 1;
    sim.st = 1;
    size_t n = eapol_key(m1, 0x008A, 16, sim.replay, sim.anonce, 0, 0, 0);
    sim_send(0x888E, m1, n, 0);
    p_putln("[sim] fake AP '" SIM_SSID "' up, sent handshake message 1");
    return 0;
}

static void sim_eapol(const uint8_t *in, size_t n)
{
    static uint8_t f[512], out[256], kd[64], wr[72];

    if (n < 99 || n > sizeof f) return;
    memcpy(f, in, n);
    uint16_t ki = be16(f + 5);
    size_t blen = (size_t)be16(f + 2) + 4;
    if (blen < 99 || blen > n) return;
    n = blen;

    if (sim.st == 1 && (ki & 0x0100) && !(ki & 0x0200)) {            /* msg 2 */
        ptk_derive(sim.pmk, sim.bssid, sim.sta, sim.anonce, f + 17, sim.ptk);
        if (!eapol_mic_ok(sim.ptk, f, n)) {
            p_putln("[sim] message 2 MIC check failed: client passphrase differs");
            return;
        }
        memcpy(kd, rsn_ie, 22);
        kd[22] = 0xDD; kd[23] = 22;
        kd[24] = 0x00; kd[25] = 0x0F; kd[26] = 0xAC; kd[27] = 0x01;
        kd[28] = 0x01; kd[29] = 0x00;
        memcpy(kd + 30, sim.gtk, 16);
        kd[46] = 0xDD; kd[47] = 0x00;                                /* pad to 48 */
        aes_wrap(sim.ptk + 16, kd, 48, wr);
        sim.replay++;
        size_t m = eapol_key(out, 0x13CA, 16, sim.replay, sim.anonce, wr, 56, sim.ptk);
        sim.st = 2;
        sim_send(0x888E, out, m, 0);
    } else if (sim.st == 2 && (ki & 0x0200)) {                       /* msg 4 */
        if (!eapol_mic_ok(sim.ptk, f, n)) return;
        aes_set(&sim.tx.aes, sim.ptk + 32);
        sim.rx.aes = sim.tx.aes;
        sim.tx.pn = sim.rx.pn = 0;
        sim.tx.on = sim.rx.on = 1;
        sim.st = 3;
        p_putln("[sim] handshake complete, CCMP on");
    }
}

static void sim_data(uint16_t et, const uint8_t *dst, const uint8_t *pl, size_t n)
{
    static const uint8_t gw[4] = { 192, 168, 1, 1 };
    uint8_t out[160];

    if (et == 0x0806 && n >= 28 && be16(pl + 6) == 1 && !memcmp(pl + 24, gw, 4)) {
        arp_pkt(out, 2, sim.bssid, gw, pl + 8, pl + 14);
        sim_send(0x0806, out, 28, 1);
    } else if (et == 0x0800 && n >= 28 && n <= sizeof out && !memcmp(dst, sim.bssid, 6) && pl[9] == 1) {
        size_t ihl = (size_t)(pl[0] & 15) * 4;
        if (ihl < 20 || n < ihl + 8 || pl[ihl] != 8) return;
        memcpy(out, pl, n);
        memcpy(out + 12, pl + 16, 4);                                /* swap src/dst */
        memcpy(out + 16, pl + 12, 4);
        out[8] = 60;
        out[10] = out[11] = 0;
        put16(out + 10, csum(out, ihl));
        out[ihl] = 0;
        out[ihl + 2] = out[ihl + 3] = 0;
        put16(out + ihl + 2, csum(out + ihl, n - ihl));
        sim_send(0x0800, out, n, 1);
    }
}

static int sim_tx(const uint8_t *frame, size_t len)
{
    static uint8_t body[MAXF];
    const uint8_t *a1, *a2, *a3, *pl;
    uint16_t et;
    size_t n;

    if (d11_parse(frame, len, DIR_TODS, &sim.rx, body, &a1, &a2, &a3, &et, &pl, &n) < 0)
        return 0;
    if (et == 0x888E) sim_eapol(pl, n);
    else if (sim.st == 3) sim_data(et, a3, pl, n);
    return 0;
}

static int sim_rx(uint8_t *buf, size_t cap)
{
    if (sim.qh == sim.qt) return 0;
    size_t n = sim.ql[sim.qh];
    if (n > cap) n = cap;
    memcpy(buf, sim.q[sim.qh], n);
    sim.qh = (sim.qh + 1) % SIMQ;
    return (int)n;
}

/* ================================================================
 * ath9k backend: identification only
 * ================================================================ */

#ifndef ATHDEMO_HOST

static const char *ath_chip(uint16_t id)
{
    switch (id) {
    case 0x002A: return "AR9280";
    case 0x002B: return "AR9285";
    case 0x002D: case 0x002E: return "AR9287";
    case 0x0030: return "AR9300";
    case 0x0032: case 0x0037: return "AR9485";
    case 0x0033: return "AR9580";
    case 0x0034: return "AR9462";
    case 0x0036: return "AR9565";
    default:     return 0;
    }
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint32_t cfg_addr(const pieg_pciinfo_t *d, uint32_t off)
{
    return 0x80000000u | ((uint32_t)d->bus << 16) | ((uint32_t)d->device << 11) |
           ((uint32_t)d->function << 8) | (off & 0xFC);
}
static uint32_t cfg_read(const pieg_pciinfo_t *d, uint32_t off)
{
    outl(0xCF8, cfg_addr(d, off));
    return inl(0xCFC);
}
static void cfg_write(const pieg_pciinfo_t *d, uint32_t off, uint32_t v)
{
    outl(0xCF8, cfg_addr(d, off));
    outl(0xCFC, v);
}

/* Returns 0 if a supported card was found. */
static int ath9k_probe(void)
{
    unsigned int count = pieg->pci_count();
    int found = 0;

    for (unsigned int i = 0; i < count; i++) {
        pieg_pciinfo_t d;
        if (pieg->pci_info(i, &d) != 0) continue;
        if (d.vendor_id != 0x168C || d.class_code != 0x02) continue;

        const char *name = ath_chip(d.device_id);
        p_puts("Atheros NIC "); p_hex(d.vendor_id); p_putc(':'); p_hex(d.device_id);
        p_puts(" at "); p_hex(d.bus); p_putc(':'); p_hex(d.device); p_putc('.'); p_hex(d.function);
        p_puts(name ? "  " : "  (unlisted ID)");
        if (name) p_puts(name);
        p_putln("");

        uint32_t bar0 = d.bar[0];
        if ((bar0 & 1) || (((bar0 >> 1) & 3) == 2 && d.bar[1] != 0) || !(bar0 & ~0xFu)) {
            p_putln("  BAR0 unusable (I/O, above 4 GiB, or unassigned)");
            continue;
        }
        uint32_t cmd = cfg_read(&d, 0x04);
        cfg_write(&d, 0x04, (cmd & 0xFFFFu) | 0x6u);                 /* mem + bus master */
        volatile uint32_t *srev = (volatile uint32_t *)(uintptr_t)((bar0 & ~0xFu) + 0x4020u);
        p_puts("  BAR0 "); p_hex(bar0 & ~0xFu);
        p_puts("  AR_SREV "); p_hex(*srev); p_putln("");
        if (name) found = 1;
    }
    if (!count || !found) p_putln("no supported Atheros card found (run lspci)");
    return found ? 0 : 1;
}

#else
static int ath9k_probe(void) { p_putln("probe: not available in the host build"); return 1; }
#endif

static int ath9k_connect(const char *ssid, uint8_t bssid[6], uint8_t mac[6])
{
    (void)ssid; (void)bssid; (void)mac;
    if (ath9k_probe() != 0) return -1;
    p_putln("ath9k: radio bring-up is not implemented in athdemo.");
    p_putln("  still needed: chip init tables, EEPROM + calibration, TX/RX DMA rings,");
    p_putln("  and scan/auth/assoc. Try 'athdemo sim' to run the WPA2 + ping stack.");
    return -1;
}
static int ath9k_tx(const uint8_t *f, size_t n) { (void)f; (void)n; return -1; }
static int ath9k_rx(uint8_t *b, size_t cap) { (void)b; (void)cap; return 0; }

/* ================================================================
 * Self test
 * ================================================================ */

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}
static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) out[n++] = (uint8_t)((hexval(s[0]) << 4) | hexval(s[1]));
    return n;
}

static int fails;
static void check(const char *name, const uint8_t *got, size_t n, const char *exp_hex)
{
    uint8_t exp[64];
    size_t en = unhex(exp_hex, exp);
    int ok = en == n && memcmp(exp, got, n) == 0;
    p_puts(ok ? "ok    " : "FAIL  "); p_putln(name);
    if (!ok) fails++;
}

static int selftest(void)
{
    uint8_t out[64], key[16], pt[32], nonce[13], aad[8], mic[8];
    sha1_t c;
    aes_t a;

    sha1_init(&c); sha1_update(&c, "abc", 3); sha1_final(&c, out);
    check("SHA-1 abc", out, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");

    hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    check("HMAC-SHA1 RFC 2202 #2", out, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    pbkdf2_sha1("password", (const uint8_t *)"IEEE", 4, 4096, out, 32);
    check("PBKDF2 (PMK) password/IEEE", out, 32,
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");

    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    unhex("00112233445566778899aabbccddeeff", pt);
    aes_set(&a, key);
    aes_enc(&a, pt, out);
    check("AES-128 FIPS-197 encrypt", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes_dec(&a, out, out);
    check("AES-128 FIPS-197 decrypt", out, 16, "00112233445566778899aabbccddeeff");

    aes_wrap(key, pt, 16, out);
    check("AES key wrap RFC 3394", out, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    uint8_t w[24];
    memcpy(w, out, 24);
    int r = aes_unwrap(key, w, 24, out);
    check("AES key unwrap RFC 3394", out, r ? 0 : 16, "00112233445566778899aabbccddeeff");

    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0xC0 + i);
    unhex("00000003020100a0a1a2a3a4a5", nonce);
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)i;
    for (int i = 0; i < 23; i++) pt[i] = (uint8_t)(8 + i);
    aes_set(&a, key);
    ccm_encrypt(&a, nonce, aad, 8, pt, 23, mic);
    memcpy(out, pt, 23); memcpy(out + 23, mic, 8);
    check("CCM RFC 3610 #1", out, 31,
          "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0");
    r = ccm_decrypt(&a, nonce, aad, 8, pt, 23, mic);
    p_puts(r == 0 ? "ok    " : "FAIL  "); p_putln("CCM decrypt + MIC verify");
    if (r) fails++;

    p_puts(fails ? "SELFTEST FAILED: " : "all passed, failures: "); p_u64((uint64_t)fails); p_putln("");
    return fails != 0;
}

/* ================================================================
 * Connect + ping
 * ================================================================ */

static int parse_ip(const char *s, uint8_t *out)
{
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s++ - '0'); digits++; }
        if (!digits || v > 255 || (i < 3 && *s++ != '.')) return -1;
        out[i] = (uint8_t)v;
    }
    return *s ? -1 : 0;
}

static int run(const char *ssid, const char *pass, const uint8_t *target)
{
    static uint8_t reply[256];
    size_t pl = p_strlen(pass), sl = p_strlen(ssid);

    if (pl < 8 || pl > 63) { p_putln("passphrase must be 8..63 characters"); return 1; }
    if (sl < 1 || sl > 32) { p_putln("SSID must be 1..32 characters"); return 1; }

    p_puts("connecting to "); p_putln(ssid);
    if (nic.connect(ssid, g_bssid, g_mac) != 0) return 1;
    p_puts("BSSID "); p_mac(g_bssid); p_puts("  our MAC "); p_mac(g_mac); p_putln("");

    p_putln("deriving PMK (4096 rounds)...");
    memset(&sup, 0, sizeof sup);
    pbkdf2_sha1(pass, (const uint8_t *)ssid, sl, 4096, sup.pmk, 32);
    memcpy(sup.aa, g_bssid, 6);
    memcpy(sup.spa, g_mac, 6);

    p_putln("waiting for the 4-way handshake...");
    uint64_t end = now_ms() + 2500;
    while (now_ms() < end && sup.st != 2) {
        uint16_t et;
        uint8_t src[6];
        const uint8_t *pkt;
        size_t n;
        if (!link_recv(&et, src, &pkt, &n) || et != 0x888E) continue;
        size_t rl = supp_rx(&sup, pkt, n, reply);
        if (rl) link_send(0x888E, g_bssid, reply, rl);
    }
    if (sup.st != 2) {
        p_putln(sup.mic_fail ? "handshake failed: message 3 MIC invalid"
                             : "handshake timed out (wrong passphrase, or no reply from the AP)");
        return 1;
    }

    aes_set(&k_tx.aes, sup.ptk + 32);
    k_rx.aes = k_tx.aes;
    k_tx.pn = k_rx.pn = 0;
    k_tx.on = k_rx.on = 1;
    p_putln("WPA2 handshake complete, CCMP keys installed");

    return do_ping(target, 4);
}

int main(int argc, char **argv)
{
    uint8_t target[4];
    const char *mode = argc > 1 ? argv[1] : "";

    tsc_calibrate();

    if (p_streq(mode, "selftest"))
        return selftest();
    if (p_streq(mode, "probe"))
        return ath9k_probe();

    if (p_streq(mode, "sim")) {
        const char *pass = argc > 2 ? argv[2] : SIM_PASS;
        memcpy(target, gw_ip, 4);
        if (argc > 3 && parse_ip(argv[3], target) != 0) { p_putln("bad target IP"); return 1; }
        nic.connect = sim_connect; nic.tx = sim_tx; nic.rx = sim_rx;
        return run(SIM_SSID, pass, target);
    }

    if (p_streq(mode, "connect") && argc >= 4) {
        if (argc > 5 && (parse_ip(argv[4], my_ip) || parse_ip(argv[5], gw_ip))) {
            p_putln("bad ip/gateway"); return 1;
        }
        memcpy(target, gw_ip, 4);
        if (argc > 6 && parse_ip(argv[6], target) != 0) { p_putln("bad target IP"); return 1; }
        nic.connect = ath9k_connect; nic.tx = ath9k_tx; nic.rx = ath9k_rx;
        return run(argv[2], argv[3], target);
    }

    p_putln("usage: athdemo selftest");
    p_putln("       athdemo probe");
    p_putln("       athdemo sim [passphrase [target-ip]]");
    p_putln("       athdemo connect <ssid> <passphrase> [ip gateway [target-ip]]");
    return 1;
}
