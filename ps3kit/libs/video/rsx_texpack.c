/*
 * ps3recomp - texture packs (dump + replace), 2026-11-15. See rsx_texpack.h.
 *
 * Everything here runs only when LD_TEX_DUMP or LD_TEX_PACK is set; with
 * neither, rsx_live_draw.c calls nothing but rsx_texpack_enabled() (a cached
 * flag) on a texture decode.
 *
 * LD_TEX_PACK_MB=<n> (default 1024): replacement textures kept on the GPU (one
 * reference each), so a texture the cache evicts and decodes again is neither
 * read from its PNG nor uploaded again. LD_TEX_PACK_PRELOAD=0: no decoding ahead.
 */
#if defined(_WIN32)

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_texpack.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../codec/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../codec/stb_image_write.h"

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* ------------------------------------------------------------------ setup */

static int  s_dump_on, s_pack_on, s_any;
static char s_dump_dir[MAX_PATH], s_pack_dir[MAX_PATH];
static u64* s_index; static u32 s_index_n;   /* sorted hashes of <pack>\<hash>.png */
static u64  s_cache_budget = 1024ull << 20;
static CRITICAL_SECTION s_lock;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static int hex16(const char* s, u64* out)
{
    u64 v = 0;
    for (int i = 0; i < 16; i++) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (u64)d;
    }
    *out = v;
    return 1;
}
static int cmp_u64(const void* a, const void* b)
{
    const u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}

static void preload_start(void);
static BOOL CALLBACK tp_init(PINIT_ONCE o, PVOID p, PVOID* c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    const char* e = getenv("LD_TEX_DUMP");
    if (e && *e) {
        snprintf(s_dump_dir, sizeof s_dump_dir, "%s", e);
        CreateDirectoryA(s_dump_dir, NULL);
        s_dump_on = 1;
        fprintf(stderr, "[tex-dump] writing new textures to %s%c", s_dump_dir, 10);
    }
    e = getenv("LD_TEX_PACK_MB");
    if (e && *e) s_cache_budget = (u64)atoll(e) << 20;
    e = getenv("LD_TEX_PACK");
    if (e && *e) {
        snprintf(s_pack_dir, sizeof s_pack_dir, "%s", e);
        char pat[MAX_PATH]; WIN32_FIND_DATAA fd; u32 cap = 0;
        snprintf(pat, sizeof pat, "%s\\*.png", s_pack_dir);
        HANDLE fh = FindFirstFileA(pat, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {   /* <16 hex>.png */
                u64 h;
                if (strlen(fd.cFileName) == 20 && _stricmp(fd.cFileName + 16, ".png") == 0 && hex16(fd.cFileName, &h)) {
                    if (s_index_n == cap) {
                        cap = cap ? cap * 2 : 256;
                        u64* n = (u64*)realloc(s_index, cap * sizeof(u64));
                        if (!n) break;
                        s_index = n;
                    }
                    s_index[s_index_n++] = h;
                }
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
        if (s_index_n) qsort(s_index, s_index_n, sizeof(u64), cmp_u64);
        s_pack_on = s_index_n > 0;
        if (s_pack_on) preload_start();
        fprintf(stderr, "[tex-pack] %u files indexed in %s (memory for decoded files %llu MB)%c", s_index_n,
                s_pack_dir, (unsigned long long)(s_cache_budget >> 20), 10);
    }
    s_any = s_dump_on || s_pack_on;
    return TRUE;
}
static void tp_setup(void) { InitOnceExecuteOnce(&s_once, tp_init, NULL, NULL); }

int rsx_texpack_enabled(void)
{
    static int v = -1;
    if (v < 0) { tp_setup(); v = s_any; }
    return v;
}
int rsx_texpack_dump_on(void) { tp_setup(); return s_dump_on; }
int rsx_texpack_pack_on(void) { tp_setup(); return s_pack_on; }

void rsx_texpack_skip_note(const char* why, u32 base_fmt)
{
    static u32 seen[64]; static u32 n;
    const u32 key = (base_fmt & 0xFFu) | ((u32)(why[0] & 0x7F) << 8) | ((u32)(why[1] & 0x7F) << 16);
    EnterCriticalSection(&s_lock);
    u32 i = 0;
    while (i < n && seen[i] != key) i++;
    if (i == n && n < 64) {
        seen[n++] = key;
        fprintf(stderr, "[tex-pack] not replaced/dumped: %s (fmt 0x%02X) - logged once%c", why, base_fmt & 0xFFu, 10);
    }
    LeaveCriticalSection(&s_lock);
}

/* ------------------------------------------------------------------- hash */

#define P1 11400714785074694791ull
#define P2 14029467366897019727ull
#define P3  1609587929392839161ull
#define P4  9650029242287828579ull
#define P5  2870177450012600261ull
static inline u64 rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static inline u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 xround(u64 acc, u64 in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; }
static inline u64 xmerge(u64 acc, u64 v) { acc ^= xround(0, v); return acc * P1 + P4; }

/* XXH64 of a byte stream fed in pieces (the rows). */
typedef struct { u64 v[4]; u8 buf[32]; u32 nbuf; u64 total; } xs_t;
static void xs_init(xs_t* s)
{
    s->v[0] = 0 + P1 + P2; s->v[1] = 0 + P2; s->v[2] = 0; s->v[3] = 0 - P1;
    s->nbuf = 0; s->total = 0;
}
static void xs_feed(xs_t* s, const u8* p, size_t n)
{
    s->total += n;
    if (s->nbuf) {
        while (n && s->nbuf < 32) { s->buf[s->nbuf++] = *p++; n--; }
        if (s->nbuf < 32) return;
        for (int i = 0; i < 4; i++) s->v[i] = xround(s->v[i], rd64(s->buf + i * 8));
        s->nbuf = 0;
    }
    while (n >= 32) {
        s->v[0] = xround(s->v[0], rd64(p)); s->v[1] = xround(s->v[1], rd64(p + 8));
        s->v[2] = xround(s->v[2], rd64(p + 16)); s->v[3] = xround(s->v[3], rd64(p + 24));
        p += 32; n -= 32;
    }
    while (n) { s->buf[s->nbuf++] = *p++; n--; }
}
static u64 xs_final(const xs_t* s)
{
    u64 h;
    if (s->total >= 32) {
        h = rotl(s->v[0], 1) + rotl(s->v[1], 7) + rotl(s->v[2], 12) + rotl(s->v[3], 18);
        for (int i = 0; i < 4; i++) h = xmerge(h, s->v[i]);
    } else h = s->v[2] + P5;
    h += s->total;
    const u8* p = s->buf; u32 n = s->nbuf;
    while (n >= 8) { h ^= xround(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; n -= 8; }
    if (n >= 4) { h ^= (u64)rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; n -= 4; }
    while (n) { h ^= (*p) * P5; h = rotl(h, 11) * P1; p++; n--; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

u64 rsx_texpack_hash(const u8* src, u32 row_bytes, u32 rows, u32 stride, u32 base_fmt, u32 w, u32 h, u32 remap)
{
    xs_t s; xs_init(&s);
    if (stride == row_bytes) xs_feed(&s, src, (size_t)row_bytes * rows);
    else for (u32 y = 0; y < rows; y++) xs_feed(&s, src + (size_t)y * stride, row_bytes);
    const u32 tail[4] = { base_fmt & 0xFFu, w, h, remap & 0xFFFFu };
    xs_feed(&s, (const u8*)tail, sizeof tail);
    return xs_final(&s);
}

/* ------------------------------------------------------------- BC decode */

static void rgb565(u32 c, u8* o) {
    const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (u8)((r << 3) | (r >> 2)); o[1] = (u8)((g << 2) | (g >> 4)); o[2] = (u8)((b << 3) | (b >> 2));
}
/* One 4x4 block's colours into px[16][4]; alpha written only for BC1. */
static void bc_color(const u8* b, u8 px[16][4], int bc1)
{
    const u32 c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    u8 pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !bc1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    const u32 idx = rd32(b + 4);
    for (int i = 0; i < 16; i++) {
        const u8* c = pal[(idx >> (i * 2)) & 3];
        px[i][0] = c[0]; px[i][1] = c[1]; px[i][2] = c[2];
        if (bc1) px[i][3] = c[3];
    }
}
static void bc3_alpha(const u8* b, u8 px[16][4])
{
    u32 a[8]; a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
    else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = (u8)a[(bits >> (3 * i)) & 7];
}
static u8* bc_to_rgba(int bc, const u8* blocks, u32 w, u32 h)
{
    u8* out = (u8*)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = bc == 1 ? 8 : 16;
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8* b = blocks + ((size_t)by * bw + bx) * bsz;
            u8 px[16][4];
            if (bc == 1) bc_color(b, px, 1);
            else {
                bc_color(b + 8, px, 0);
                if (bc == 2) for (int i = 0; i < 16; i++) { const u32 a = (b[i / 2] >> ((i & 1) * 4)) & 15; px[i][3] = (u8)(a * 17); }
                else bc3_alpha(b, px);
            }
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++) {
                    const u32 X = bx * 4 + x, Y = by * 4 + y;
                    if (X < w && Y < h) memcpy(out + ((size_t)Y * w + X) * 4, px[y * 4 + x], 4);
                }
        }
    return out;
}

/* ------------------------------------------------------------------ dump */

typedef struct dump_job { struct dump_job* next; u64 hash; int bc; u32 w, h; size_t n; u8* data; } dump_job;
static dump_job *s_q_head, *s_q_tail;
static size_t s_q_bytes;
static CONDITION_VARIABLE s_q_cv = CONDITION_VARIABLE_INIT;
static int s_q_thread;
static volatile LONG s_dumped, s_dump_fail;

typedef struct { u64* t; u32 cap, n; } hset_t;          /* open addressing, 0 = empty (hash|1) */
static hset_t s_dumped_set, s_logged_set;
#define s_seen s->t
#define s_seen_cap s->cap
#define s_seen_n s->n
static int hset_add(hset_t* s, u64 h)                   /* 1: new */
{
    h |= 1;
    if ((s_seen_n + 1) * 2 > s_seen_cap) {
        const u32 nc = s_seen_cap ? s_seen_cap * 2 : 1u << 14;
        u64* n = (u64*)calloc(nc, sizeof(u64));
        if (!n) return 0;
        for (u32 i = 0; i < s_seen_cap; i++)
            if (s_seen[i]) { u32 k = (u32)(s_seen[i] * P1 >> 40) & (nc - 1); while (n[k]) k = (k + 1) & (nc - 1); n[k] = s_seen[i]; }
        free(s_seen); s_seen = n; s_seen_cap = nc;
    }
    u32 k = (u32)(h * P1 >> 40) & (s_seen_cap - 1);
    while (s_seen[k]) { if (s_seen[k] == h) return 0; k = (k + 1) & (s_seen_cap - 1); }
    s_seen[k] = h; s_seen_n++;
    return 1;
}
#undef s_seen
#undef s_seen_cap
#undef s_seen_n

static void write_job(dump_job* j)
{
    u8* rgba = j->bc ? bc_to_rgba(j->bc, j->data, j->w, j->h) : j->data;
    char fn[MAX_PATH];
    snprintf(fn, sizeof fn, "%s\\%016llx.png", s_dump_dir, (unsigned long long)j->hash);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", fn);
    int ok = rgba && stbi_write_png(tmp, (int)j->w, (int)j->h, 4, rgba, (int)(j->w * 4)) &&
             MoveFileExA(tmp, fn, MOVEFILE_REPLACE_EXISTING);
    if (!ok) { DeleteFileA(tmp); InterlockedIncrement(&s_dump_fail); }
    else {
        const LONG n = InterlockedIncrement(&s_dumped);
        if (n <= 4 || (n & 255) == 0)
            fprintf(stderr, "[tex-dump] %ld textures written (last %016llx.png %ux%u%s), %ld failed%c", n,
                    (unsigned long long)j->hash, j->w, j->h, j->bc ? " from DXT" : "", (long)s_dump_fail, 10);
    }
    if (rgba && rgba != j->data) free(rgba);
    free(j->data);
    free(j);
}
static DWORD WINAPI dump_thread(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        EnterCriticalSection(&s_lock);
        while (!s_q_head) SleepConditionVariableCS(&s_q_cv, &s_lock, INFINITE);
        dump_job* j = s_q_head;
        s_q_head = j->next; if (!s_q_head) s_q_tail = NULL;
        LeaveCriticalSection(&s_lock);
        const size_t n = j->n;
        write_job(j);
        EnterCriticalSection(&s_lock);
        s_q_bytes -= n;
        LeaveCriticalSection(&s_lock);
    }
}

/* Addresses whose content changed again within 30 frames of a dump: a texture
 * the game writes as it runs (movies, CPU effects) -- no more dumps from them. */
#define ADDR_BITS 12u
static struct { u64 addr; u32 frame; u32 dynamic; } s_tp_addr[1u << ADDR_BITS];
static long s_dyn_n;

void rsx_texpack_dump(u64 hash, int bc, const u8* data, u32 w, u32 h, u32 loc, u32 off, u32 frame)
{
    if (!rsx_texpack_dump_on() || !w || !h) return;
    EnterCriticalSection(&s_lock);
    const u64 addr = ((u64)loc << 32) | off;
    const u32 ai = (u32)((addr * P1) >> (64 - ADDR_BITS));
    if (s_tp_addr[ai].addr == addr && s_tp_addr[ai].dynamic) { LeaveCriticalSection(&s_lock); return; }
    if (!hset_add(&s_dumped_set, hash)) { LeaveCriticalSection(&s_lock); return; }
    if (s_tp_addr[ai].addr == addr && frame - s_tp_addr[ai].frame < 30u) {
        s_tp_addr[ai].dynamic = 1;
        if (s_dyn_n++ < 8)
            fprintf(stderr, "[tex-dump] %u:0x%08X changes every few frames: not dumped any more%c", loc, off, 10);
        LeaveCriticalSection(&s_lock);
        return;
    }
    s_tp_addr[ai].addr = addr; s_tp_addr[ai].frame = frame; s_tp_addr[ai].dynamic = 0;
    LeaveCriticalSection(&s_lock);

    char fn[MAX_PATH];
    snprintf(fn, sizeof fn, "%s\\%016llx.png", s_dump_dir, (unsigned long long)hash);
    if (GetFileAttributesA(fn) != INVALID_FILE_ATTRIBUTES) return;   /* from an earlier run */
    const size_t n = bc ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (bc == 1 ? 8 : 16) : (size_t)w * h * 4;
    dump_job* j = (dump_job*)calloc(1, sizeof *j);
    if (!j || !(j->data = (u8*)malloc(n))) { free(j); return; }
    memcpy(j->data, data, n);
    j->hash = hash; j->bc = bc; j->w = w; j->h = h; j->n = n;
    EnterCriticalSection(&s_lock);
    if (!s_q_thread) {
        HANDLE th = CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
        if (th) { CloseHandle(th); s_q_thread = 1; }
    }
    /* No thread, or 256 MB already waiting: write it here (the game waits). */
    if (!s_q_thread || s_q_bytes > (256u << 20)) { LeaveCriticalSection(&s_lock); write_job(j); return; }
    s_q_bytes += n;
    if (s_q_tail) s_q_tail->next = j; else s_q_head = j;
    s_q_tail = j;
    WakeConditionVariable(&s_q_cv);
    LeaveCriticalSection(&s_lock);
}

/* ---------------------------------------------------------------- replace */

/* A full box-filtered mip chain of a w x h RGBA8 image, every level in one block. */
typedef struct { u8* mem; u32 w, h, n; u64 bytes; } chain_t;

static void chain_level(u32 w, u32 h, u32 i, u32* lw, u32* lh, u64* off)
{
    u64 o = 0;
    for (u32 k = 0; k < i; k++) {
        o += (u64)w * h * 4;
        w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1;
    }
    *lw = w; *lh = h; *off = o;
}
static int chain_build(chain_t* ch, const u8* top, u32 w, u32 h)
{
    u32 n = 1;
    for (u32 mw = w, mh = h; (mw > 1 || mh > 1) && n < 16; n++) { mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1; }
    u32 lw, lh; u64 total;
    chain_level(w, h, n, &lw, &lh, &total);
    u8* mem = (u8*)malloc((size_t)total);
    if (!mem) return 0;
    memcpy(mem, top, (size_t)w * h * 4);
    for (u32 i = 1; i < n; i++) {
        u32 sw, sh, dw, dh; u64 so, dof;
        chain_level(w, h, i - 1, &sw, &sh, &so);
        chain_level(w, h, i, &dw, &dh, &dof);
        const u8* s = mem + so; u8* d = mem + dof;
        for (u32 y = 0; y < dh; y++)
            for (u32 x = 0; x < dw; x++) {
                const u32 x0 = x * 2 < sw ? x * 2 : sw - 1, x1 = x * 2 + 1 < sw ? x * 2 + 1 : x0;
                const u32 y0 = y * 2 < sh ? y * 2 : sh - 1, y1 = y * 2 + 1 < sh ? y * 2 + 1 : y0;
                const u8 *a = s + ((size_t)y0 * sw + x0) * 4, *b = s + ((size_t)y0 * sw + x1) * 4,
                         *e = s + ((size_t)y1 * sw + x0) * 4, *f = s + ((size_t)y1 * sw + x1) * 4;
                u8* o = d + ((size_t)y * dw + x) * 4;
                for (int k = 0; k < 4; k++) o[k] = (u8)((a[k] + b[k] + e[k] + f[k] + 2) / 4);
            }
    }
    ch->mem = mem; ch->w = w; ch->h = h; ch->n = n; ch->bytes = total;
    return 1;
}
static const char* chain_load(u64 hash, chain_t* ch)
{
    char fn[MAX_PATH];
    snprintf(fn, sizeof fn, "%s\\%016llx.png", s_pack_dir, (unsigned long long)hash);
    int w = 0, h = 0, comp = 0;
    u8* px = stbi_load(fn, &w, &h, &comp, 4);
    if (!px) return "cannot read the PNG";
    const char* why = NULL;
    if (w > 16384 || h > 16384) why = "bigger than 16384";
    else if (!chain_build(ch, px, (u32)w, (u32)h)) why = "out of memory";
    stbi_image_free(px);
    return why;
}

/* LD_TEX_PACK_PRELOAD (default on; =0 off): a low-priority thread decodes the
 * pack's PNGs (and their mips) ahead of use, up to LD_TEX_PACK_MB of memory,
 * so a texture's first use does not wait for its PNG on the drawing thread. */
static u8* s_pre_state;     /* per index entry: 0 waiting, 1 loading, 2 ready, 3 failed, 4 taken by a use */
static chain_t* s_pre;
static u64 s_pre_bytes;
static volatile LONG s_pre_ready;

static int index_pos(u64 h)
{
    u32 lo = 0, hi = s_index_n;
    while (lo < hi) { const u32 m = (lo + hi) / 2; if (s_index[m] < h) lo = m + 1; else hi = m; }
    return lo < s_index_n && s_index[lo] == h ? (int)lo : -1;
}
static DWORD WINAPI preload_thread(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const DWORD t0 = GetTickCount();
    u64 bytes = 0;
    for (u32 i = 0; i < s_index_n; i++) {
        for (;;) {   /* over the budget: wait until uses take some (they move to the GPU) */
            EnterCriticalSection(&s_lock);
            const u64 b = s_pre_bytes;
            LeaveCriticalSection(&s_lock);
            if (b < s_cache_budget) break;
            { static int said; if (!said) { said = 1;
                fprintf(stderr, "[tex-pack] pack bigger than LD_TEX_PACK_MB (%llu MB): decoding ahead paused at %u of %u files; the rest decode as they are used%c",
                        (unsigned long long)(s_cache_budget >> 20), i, s_index_n, 10); } }
            Sleep(200);
        }
        EnterCriticalSection(&s_lock);
        const int mine = s_pre_state[i] == 0;
        if (mine) s_pre_state[i] = 1;
        LeaveCriticalSection(&s_lock);
        if (!mine) continue;
        chain_t ch = {0};
        const char* why = chain_load(s_index[i], &ch);
        EnterCriticalSection(&s_lock);
        if (s_pre_state[i] == 4 || why) { free(ch.mem); if (s_pre_state[i] != 4) s_pre_state[i] = 3; }
        else { s_pre[i] = ch; s_pre_state[i] = 2; s_pre_bytes += ch.bytes; bytes += ch.bytes; InterlockedIncrement(&s_pre_ready); }
        LeaveCriticalSection(&s_lock);
    }
    fprintf(stderr, "[tex-pack] preload done: %ld of %u files decoded ahead (%.0f MB) in %.1f s%c", (long)s_pre_ready,
            s_index_n, (double)bytes / 1048576.0, (double)(GetTickCount() - t0) / 1000.0, 10);
    return 0;
}
static void preload_start(void)
{
    const char* e = getenv("LD_TEX_PACK_PRELOAD");
    if ((e && e[0] == '0') || !s_index_n) return;
    s_pre_state = (u8*)calloc(s_index_n, 1);
    s_pre = (chain_t*)calloc(s_index_n, sizeof(chain_t));
    if (!s_pre_state || !s_pre) { free(s_pre_state); free(s_pre); s_pre_state = NULL; s_pre = NULL; return; }
    HANDLE th = CreateThread(NULL, 0, preload_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else { free(s_pre_state); free(s_pre); s_pre_state = NULL; s_pre = NULL; }
}

typedef struct {
    u64 hash;            /* 0: empty */
    u64 bytes;
    u64 use;
    int failed;          /* could not be used: never tried again */
    u32 orig_w, orig_h, orig_mips;
    rsx_texpack_image img;
    u8* mem;             /* the chain (freed once the GPU copy is kept) */
    void* gpu;           /* the texture made from it, one reference held here */
} cache_t;
static rsx_texpack_ref_fn s_ref;
/* Textures this module holds (rsx_texpack_owns): open addressing, 1 = deleted. */
static uintptr_t* s_own; static u32 s_own_cap, s_own_n;
static void own_add(const void* p)
{
    if ((s_own_n + 1) * 2 > s_own_cap) {
        const u32 nc = s_own_cap ? s_own_cap * 2 : 1024;
        uintptr_t* n = (uintptr_t*)calloc(nc, sizeof(uintptr_t));
        if (!n) return;
        u32 live = 0;
        for (u32 i = 0; i < s_own_cap; i++)
            if (s_own[i] > 1) { u32 k = (u32)((s_own[i] >> 4) * 2654435761u) & (nc - 1); while (n[k]) k = (k + 1) & (nc - 1); n[k] = s_own[i]; live++; }
        free(s_own); s_own = n; s_own_cap = nc; s_own_n = live;
    }
    u32 k = (u32)(((uintptr_t)p >> 4) * 2654435761u) & (s_own_cap - 1);
    while (s_own[k] > 1) { if (s_own[k] == (uintptr_t)p) return; k = (k + 1) & (s_own_cap - 1); }
    s_own[k] = (uintptr_t)p; s_own_n++;
}
static uintptr_t* own_find(const void* p)
{
    if (!s_own_cap) return NULL;
    u32 k = (u32)(((uintptr_t)p >> 4) * 2654435761u) & (s_own_cap - 1);
    for (u32 n = 0; n < s_own_cap && s_own[k]; n++, k = (k + 1) & (s_own_cap - 1))
        if (s_own[k] == (uintptr_t)p) return &s_own[k];
    return NULL;
}
int rsx_texpack_owns(const void* gpu) { return gpu && own_find(gpu) != NULL; }
/* [tex-pack] every 5 s while a pack is on: what the drawing thread asked of it. */
static struct { long long finds, kept_hits, uploads, png_sync, pre_used; } s_st;
static u64 s_gpu_bytes;
static void stats_tick(void)
{
    static DWORD last;
    const DWORD now = GetTickCount();
    if (!last) last = now;
    if (now - last < 5000) return;
    last = now;
    fprintf(stderr, "[tex-pack] per 5s: %lld lookups (%lld already on the GPU, %lld uploads: %lld preloaded, %lld PNG decoded on the drawing thread); %.0f MB of replacements on the GPU%c",
            s_st.finds, s_st.kept_hits, s_st.uploads, s_st.pre_used, s_st.png_sync, (double)s_gpu_bytes / 1048576.0, 10);
    memset(&s_st, 0, sizeof s_st);
}
#define CACHE_N 4096u
static cache_t s_cache[CACHE_N];
static u64 s_cache_bytes, s_use;
static long s_used_n;

static void cache_drop(cache_t* c)
{
    s_cache_bytes -= c->bytes;
    free(c->mem);
    if (c->gpu) {
        uintptr_t* o = own_find(c->gpu);
        if (o) *o = 1;
        s_gpu_bytes -= c->bytes;
        if (s_ref) s_ref(c->gpu, 0);   /* textures still using it keep their own reference */
    }
    memset(c, 0, sizeof *c);
}

int rsx_texpack_find(u64 hash, u32 orig_w, u32 orig_h, u32 orig_mips, int same_size_only, u64 max_bytes,
                     const rsx_texpack_image** out, void** gpu, rsx_texpack_ref_fn ref)
{
    *out = NULL; *gpu = NULL;
    if (!rsx_texpack_pack_on()) return 0;
    const int ip = index_pos(hash);
    if (ip < 0) return 0;
    s_ref = ref;
    s_st.finds++;
    stats_tick();
    const u64 key = hash ? hash : 1;
    u32 slot = (u32)((key * P1) >> 52) & (CACHE_N - 1), free_slot = CACHE_N;
    cache_t* c = NULL;
    for (u32 p = 0; p < 64; p++) {   /* linear probe, bounded */
        cache_t* e = &s_cache[(slot + p) & (CACHE_N - 1)];
        if (e->hash == key && e->orig_w == orig_w && e->orig_h == orig_h && e->orig_mips == orig_mips) { c = e; break; }
        if (!e->hash && free_slot == CACHE_N) free_slot = (slot + p) & (CACHE_N - 1);
    }
    if (c) {
        if (c->failed) return 0;
        c->use = ++s_use;
        if (c->gpu) { s_ref(c->gpu, 1); *gpu = c->gpu; s_st.kept_hits++; return 1; }
        if (!c->mem) return 0;
        *out = &c->img;
        return 1;
    }
    /* The chain: from the preload thread when it is ready, else decoded here. */
    chain_t ch = {0};
    int from_pre = 0;
    if (s_pre_state) {
        EnterCriticalSection(&s_lock);
        if (s_pre_state[ip] == 2) { ch = s_pre[ip]; s_pre[ip].mem = NULL; s_pre_bytes -= ch.bytes; from_pre = 1; }
        s_pre_state[ip] = 4;
        LeaveCriticalSection(&s_lock);
    }
    const char* why = from_pre ? NULL : chain_load(hash, &ch);
    if (from_pre) s_st.pre_used++; else s_st.png_sync++;
    if (free_slot == CACHE_N) {   /* full probe window: the least recently used one in it goes */
        u32 best = slot;
        for (u32 p = 1; p < 64; p++) if (s_cache[(slot + p) & (CACHE_N - 1)].use < s_cache[best].use) best = (slot + p) & (CACHE_N - 1);
        cache_drop(&s_cache[best]);
        free_slot = best;
    }
    c = &s_cache[free_slot];
    c->hash = key; c->orig_w = orig_w; c->orig_h = orig_h; c->orig_mips = orig_mips; c->use = ++s_use;
    if (!why && same_size_only && (ch.w != orig_w || ch.h != orig_h))
        why = "texture uses unnormalized coordinates: only the same size works";
    if (why) {
        free(ch.mem);
        c->failed = 1;
        fprintf(stderr, "[tex-pack] %016llx.png NOT used: %s%c", (unsigned long long)hash, why, 10);
        return 0;
    }
    /* Levels: the original's count plus one per halving down to its size. */
    u32 k = 0;
    while ((ch.w >> k) > orig_w || (ch.h >> k) > orig_h) k++;
    u32 n = (orig_mips ? orig_mips : 1) + k;
    if (n > ch.n) n = ch.n;
    /* The upload limit (as create_texture_mipped lays levels out: 256-byte rows,
     * 512-byte starts): top levels are left out until the chain fits. */
    u32 first = 0, lw, lh; u64 off, t = 0;
    for (u32 i = 0; i < n; i++) { chain_level(ch.w, ch.h, i, &lw, &lh, &off); t += (((u64)lw * 4 + 255) & ~255ull) * lh + 512; }
    while (first + 1 < n && t > max_bytes) {
        chain_level(ch.w, ch.h, first, &lw, &lh, &off); t -= (((u64)lw * 4 + 255) & ~255ull) * lh + 512; first++;
    }
    if (first) {
        static int logged;
        chain_level(ch.w, ch.h, first, &lw, &lh, &off);
        if (logged++ < 8)
            fprintf(stderr, "[tex-pack] %016llx.png %ux%u too big to upload at once: drawn at %ux%u%c",
                    (unsigned long long)hash, ch.w, ch.h, lw, lh, 10);
    }
    c->mem = ch.mem; c->bytes = ch.bytes;
    c->img.n = n - first;
    for (u32 i = first; i < n; i++) {
        chain_level(ch.w, ch.h, i, &lw, &lh, &off);
        c->img.w[i - first] = lw; c->img.h[i - first] = lh; c->img.data[i - first] = ch.mem + off;
    }
    s_cache_bytes += c->bytes;
    if (hset_add(&s_logged_set, hash)) {   /* the first use of each file is logged */
        s_used_n++;
        fprintf(stderr, "[tex-pack] %016llx.png used%s: %ux%u (%u mips) for a %ux%u texture (%ld replaced so far)%c",
                (unsigned long long)hash, from_pre ? "" : " (decoded on first use)", ch.w, ch.h, c->img.n, orig_w,
                orig_h, s_used_n, 10);
    }
    /* Over the memory budget: drop the least recently used decoded chains not yet on the
     * GPU (not this one). Replacements on the GPU are kept: dropping them made every use
     * decode its PNG again on the drawing thread (a 2.6 GB pack under the 1 GB default ran
     * at 8 fps). Their GPU memory is reported instead (once, below). */
    while (s_cache_bytes - s_gpu_bytes > s_cache_budget) {
        cache_t* lru = NULL;
        for (u32 i = 0; i < CACHE_N; i++)
            if (s_cache[i].mem && !s_cache[i].gpu && &s_cache[i] != c && (!lru || s_cache[i].use < lru->use)) lru = &s_cache[i];
        if (!lru) break;
        cache_drop(lru);
    }
    *out = &c->img;
    return 1;
}

void rsx_texpack_keep(u64 hash, u32 orig_w, u32 orig_h, u32 orig_mips, void* gpu)
{
    const u64 key = hash ? hash : 1;
    const u32 slot = (u32)((key * P1) >> 52) & (CACHE_N - 1);
    for (u32 p = 0; p < 64; p++) {
        cache_t* e = &s_cache[(slot + p) & (CACHE_N - 1)];
        if (e->hash == key && e->orig_w == orig_w && e->orig_h == orig_h && e->orig_mips == orig_mips) {
            if (e->gpu || !gpu || !s_ref) return;
            s_ref(gpu, 1);
            e->gpu = gpu;
            free(e->mem); e->mem = NULL;   /* the bytes now count as GPU memory */
            own_add(gpu);
            s_gpu_bytes += e->bytes;
            s_st.uploads++;
            if (s_gpu_bytes > s_cache_budget) {
                static int said;
                if (!said) { said = 1;
                    fprintf(stderr, "[tex-pack] replacements now use more than LD_TEX_PACK_MB (%llu MB) of GPU memory: kept (not decoded again)%c",
                            (unsigned long long)(s_cache_budget >> 20), 10); }
            }
            return;
        }
    }
}

#endif /* _WIN32 */
