/*
 * ps3recomp - trophy set data from the player's own game files (host/trophies, 2026-11-22). See np_trophy_data.h.
 *
 * TROPHY.TRP (plain container): big-endian header u32 magic 0xDCA24D00, u32 version, u64 file size, u32 entry count,
 * u32 entry size (64); entries from 0x40: char name[32], u64 offset, u64 size, u32 flags, padding. Files inside:
 * TROPCONF.SFM (ids, ttype, hidden, pid), TROP.SFM (English texts), TROP_<nn>.SFM (other languages, trophy
 * language numbers: 00 ja, 02 fr, 03 es, 04 de, 05 it, 06 nl, 07 pt, 08 ru, 09 ko, 10/11 zh, 12 fi, 13 sv, 14 da,
 * 15 no, 16 pl, 17 pt-BR, 19 tr), ICON0.PNG, TROP<nnn>.PNG. SFM = UTF-8 XML after a signature comment.
 */
#include "np_trophy_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define nt_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <dirent.h>
#define nt_mkdir(p) mkdir(p, 0755)
#include <strings.h>
#define _stricmp strcasecmp
#endif

extern const char* ppu_vfs_root;

void (*g_np_trophy_sound_hook)(int id, char grade) = 0;

static struct {
    int tried, n, plat;
    char ver[16];
    char dir[600];                 /* the cache folder <root>/<NPCOMMID> */
    np_trophy_def t[NP_TROPHY_DATA_MAX];
} s_td = { 0, 0, -1, "", "", {{0}} };

/* ---- small helpers -------------------------------------------------------------------------------------------- */
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const unsigned char* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static unsigned char* nt_read_file(const char* path, size_t* n_out, size_t limit)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > limit) { fclose(f); return NULL; }
    unsigned char* b = (unsigned char*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    *n_out = (size_t)n;
    return b;
}

static void nt_mkdirs(const char* path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++)
        if (*p == '/' || *p == '\\') { const char c = *p; *p = 0; nt_mkdir(tmp); *p = c; }
    nt_mkdir(tmp);
}

/* writes via <path>.tmp + replace; skipped when the file is already there with this size */
static int nt_write_if_changed(const char* path, const unsigned char* d, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); const long have = ftell(f); fclose(f); if (have == (long)n) return 0; }
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    const int ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
    if (!np_trophy_replace_file(tmp, path)) { remove(tmp); return -1; }
    return 1;
}
int np_trophy_replace_file(const char* tmp, const char* dst)
{
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 1 : 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

/* ---- the TRP container ---------------------------------------------------------------------------------------- */
typedef struct { unsigned char* d; size_t n; int count; } nt_trp;

static int nt_trp_open(nt_trp* t, const char* path)
{
    memset(t, 0, sizeof *t);
    t->d = nt_read_file(path, &t->n, (size_t)64 << 20);
    if (!t->d) return 0;
    if (t->n < 0x40 || be32(t->d) != 0xDCA24D00u || be64(t->d + 8) != (uint64_t)t->n) goto bad;
    {
        const uint32_t cnt = be32(t->d + 16), esz = be32(t->d + 20);
        if (esz != 64u || cnt == 0 || cnt > 1024u || 0x40u + (uint64_t)cnt * 64u > t->n) goto bad;
        for (uint32_t i = 0; i < cnt; i++) {
            const unsigned char* e = t->d + 0x40 + i * 64u;
            const uint64_t off = be64(e + 32), sz = be64(e + 40);
            if (memchr(e, 0, 32) == NULL || off > t->n || sz > t->n - off) goto bad;
        }
        t->count = (int)cnt;
    }
    return 1;
bad:
    free(t->d); memset(t, 0, sizeof *t);
    return 0;
}
static const unsigned char* nt_trp_find(const nt_trp* t, const char* name, size_t* n)
{
    for (int i = 0; i < t->count; i++) {
        const unsigned char* e = t->d + 0x40 + (size_t)i * 64u;
        if (_stricmp((const char*)e, name) == 0) { *n = (size_t)be64(e + 40); return t->d + be64(e + 32); }
    }
    return NULL;
}

/* ---- SFM (XML) ------------------------------------------------------------------------------------------------ */
/* the value of attr="..." inside the tag [p, end) */
static int nt_attr(const char* p, const char* end, const char* attr, char* out, size_t cap)
{
    const size_t al = strlen(attr);
    for (const char* q = p; q + al + 2 < end; q++) {
        if ((q == p || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r') &&
            strncmp(q, attr, al) == 0 && q[al] == '=' && q[al + 1] == '"') {
            const char* v = q + al + 2; const char* ve = v;
            while (ve < end && *ve != '"') ve++;
            size_t n = (size_t)(ve - v); if (n >= cap) n = cap - 1;
            memcpy(out, v, n); out[n] = 0;
            return 1;
        }
    }
    return 0;
}
static void nt_put_utf8(char** o, char* oe, unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (*o + n >= oe) return;
    memcpy(*o, b, (size_t)n); *o += n;
}
/* the text of <tag>...</tag> inside [p, end), entities decoded, white space runs folded to one space */
static int nt_elem(const char* p, const char* end, const char* tag, char* out, size_t cap)
{
    char open[40], close[40];
    snprintf(open, sizeof open, "<%s>", tag); snprintf(close, sizeof close, "</%s>", tag);
    const size_t ol = strlen(open), cl = strlen(close);
    const char* a = NULL;
    for (const char* q = p; q + ol <= end; q++) if (strncmp(q, open, ol) == 0) { a = q + ol; break; }
    if (!a) return 0;
    const char* b = NULL;
    for (const char* q = a; q + cl <= end; q++) if (strncmp(q, close, cl) == 0) { b = q; break; }
    if (!b) return 0;
    char* o = out; char* oe = out + cap - 1; int sp = 0;
    for (const char* q = a; q < b && o < oe; ) {
        unsigned char c = (unsigned char)*q;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { sp = 1; q++; continue; }
        if (sp && o > out) *o++ = ' ';
        sp = 0;
        if (c == '&') {
            const char* sc = q; while (sc < b && *sc != ';' && sc - q < 12) sc++;
            if (sc < b && *sc == ';') {
                const size_t n = (size_t)(sc - q - 1); const char* e = q + 1; unsigned cp = 0; int ok = 1;
                if (n == 3 && !strncmp(e, "amp", 3)) cp = '&';
                else if (n == 2 && !strncmp(e, "lt", 2)) cp = '<';
                else if (n == 2 && !strncmp(e, "gt", 2)) cp = '>';
                else if (n == 4 && !strncmp(e, "quot", 4)) cp = '"';
                else if (n == 4 && !strncmp(e, "apos", 4)) cp = '\'';
                else if (n >= 2 && e[0] == '#' && (e[1] == 'x' || e[1] == 'X')) cp = (unsigned)strtoul(e + 2, NULL, 16);
                else if (n >= 2 && e[0] == '#') cp = (unsigned)strtoul(e + 1, NULL, 10);
                else ok = 0;
                if (ok && cp && cp < 0x110000) { nt_put_utf8(&o, oe, cp); q = sc + 1; continue; }
            }
        }
        *o++ = (char)c; q++;
    }
    *o = 0;
    return 1;
}

/* TROPCONF.SFM -> ids, grade, hidden, pid. Returns the count (0: unusable). */
static int nt_parse_conf(const char* x, size_t n, np_trophy_def* t, char* ver, size_t vcap)
{
    const char* end = x + n;
    int count = 0, plat = 0;
    ver[0] = 0;
    nt_elem(x, end, "trophyset-version", ver, vcap);
    for (const char* p = x; (p = strstr(p, "<trophy ")) != NULL && p < end; ) {
        const char* te = strchr(p, '>');
        if (!te || te >= end) break;
        char a[16];
        if (!nt_attr(p, te, "id", a, sizeof a)) return 0;
        const int id = atoi(a);
        if (id != count || id >= NP_TROPHY_DATA_MAX) return 0;   /* ids 0..n-1 in order */
        np_trophy_def* d = &t[id];
        memset(d, 0, sizeof *d);
        d->id = id;
        if (!nt_attr(p, te, "ttype", a, sizeof a) || !strchr("PGSB", a[0]) || !a[0]) return 0;
        d->grade = a[0];
        if (d->grade == 'P') plat++;
        d->hidden = nt_attr(p, te, "hidden", a, sizeof a) && !strcmp(a, "yes");
        d->pid = nt_attr(p, te, "pid", a, sizeof a) ? atoi(a) : -1;
        count++;
        p = te + 1;
    }
    if (count == 0 || plat > 1) return 0;
    return count;
}
/* TROP*.SFM -> names and details for the ids parsed */
static int nt_parse_texts(const char* x, size_t n, np_trophy_def* t, int count)
{
    const char* end = x + n;
    int got = 0;
    for (const char* p = x; (p = strstr(p, "<trophy ")) != NULL && p < end; ) {
        const char* te = strchr(p, '>');
        if (!te) break;
        const char* ce = strstr(te, "</trophy>");
        if (!ce || ce > end) break;
        char a[16];
        if (nt_attr(p, te, "id", a, sizeof a)) {
            const int id = atoi(a);
            if (id >= 0 && id < count) {
                if (nt_elem(te, ce, "name", t[id].name, sizeof t[id].name)) got++;
                nt_elem(te, ce, "detail", t[id].detail, sizeof t[id].detail);
            }
        }
        p = ce;
    }
    return got;
}

/* the console language (PS3_SYS_LANG, CELL_SYSUTIL_LANG_* numbers) -> the TROP_<nn>.SFM name ("" = TROP.SFM) */
static void nt_lang_file(char* out, size_t cap)
{
    const char* e = getenv("PS3_SYS_LANG");
    int v = (e && *e >= '0' && *e <= '9') ? atoi(e) : 1;
    out[0] = 0;
    /* the TROP_<nn> numbers ARE the CELL_SYSUTIL_LANG numbers (2 French -> TROP_02, 3 Spanish -> TROP_03). The US
     * disc's Latin-American Spanish (the launcher sends 21; 20 too, to be safe) has no own file in this set: TROP_03. */
    if (v == 20 || v == 21) v = 3;
    if (v == 1 || v == 18 || v < 0 || v > 19) return;   /* English: TROP.SFM */
    snprintf(out, cap, "TROP_%02d.SFM", v);
}

/* ---- finding the TRP ------------------------------------------------------------------------------------------ */
static int nt_conf_version(const char* path, char* ver, size_t cap)
{
    nt_trp t;
    if (!nt_trp_open(&t, path)) return 0;
    size_t n = 0;
    const unsigned char* c = nt_trp_find(&t, "TROPCONF.SFM", &n);
    np_trophy_def* tmp = (np_trophy_def*)calloc(NP_TROPHY_DATA_MAX, sizeof *tmp);
    int ok = 0;
    if (c && tmp) {
        char* x = (char*)malloc(n + 1);
        if (x) { memcpy(x, c, n); x[n] = 0; ok = nt_parse_conf(x, n, tmp, ver, cap) > 0; free(x); }
    }
    free(tmp); free(t.d);
    return ok;
}
static void nt_consider(const char* path, char* best, size_t bcap, char* best_ver)
{
    char ver[16];
    if (!nt_conf_version(path, ver, sizeof ver)) return;
    if (!best[0] || strcmp(ver, best_ver) > 0) { snprintf(best, bcap, "%s", path); snprintf(best_ver, 16, "%s", ver); }
}

int np_trophy_data_count(void) { return s_td.n; }
int np_trophy_data_platinum(void) { return s_td.plat; }
const char* np_trophy_data_set_version(void) { return s_td.ver; }
const np_trophy_def* np_trophy_data_get(int id) { return id >= 0 && id < s_td.n ? &s_td.t[id] : NULL; }
void np_trophy_data_icon_path(int id, char* out, size_t cap)
{
    out[0] = 0;
    if (id < 0 || id >= s_td.n || !s_td.dir[0]) return;
    snprintf(out, cap, "%s/TROP%03d.PNG", s_td.dir, id);
    FILE* f = fopen(out, "rb");
    if (f) fclose(f); else out[0] = 0;
}

int np_trophy_data_load(const char* id, const char* cache_root)
{
    if (s_td.tried) return s_td.n;
    s_td.tried = 1;
    if (!id || !*id || strlen(id) > 20 || strpbrk(id, "/\\.:")) return 0;
    const char* off = getenv("PS3_TROPHY_DATA");
    if (off && off[0] == '0') { printf("[trophy] PS3_TROPHY_DATA=0: no trophy list (the game runs as without it)\n"); return 0; }
    snprintf(s_td.dir, sizeof s_td.dir, "%s/%s", cache_root && *cache_root ? cache_root : "gamedata/trophies", id);

    /* candidates: PS3_TROPHY_TRP (tests), the disc, an installed update's copy; the newest trophy-set version wins */
    char best[700] = "", best_ver[16] = "", p[700];
    const char* forced = getenv("PS3_TROPHY_TRP");
    if (forced && *forced) nt_consider(forced, best, sizeof best, best_ver);
    else {
        const char* root = ppu_vfs_root && *ppu_vfs_root ? ppu_vfs_root : ".";
        snprintf(p, sizeof p, "%s/PS3_GAME/TROPDIR/%s/TROPHY.TRP", root, id); nt_consider(p, best, sizeof best, best_ver);
        snprintf(p, sizeof p, "%s/TROPDIR/%s/TROPHY.TRP", root, id);          nt_consider(p, best, sizeof best, best_ver);
        const char* hdd0 = getenv("PS3_HDD0_ROOT");
#ifdef _WIN32
        if (hdd0 && *hdd0) {
            char pat[700]; snprintf(pat, sizeof pat, "%s/game/*", hdd0);
            WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                    snprintf(p, sizeof p, "%s/game/%s/TROPDIR/%s/TROPHY.TRP", hdd0, fd.cFileName, id);
                    nt_consider(p, best, sizeof best, best_ver);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
#else
        (void)hdd0;
#endif
    }

    char lang[24]; nt_lang_file(lang, sizeof lang);
    int n = 0;
    if (best[0]) {
        nt_trp t;
        if (nt_trp_open(&t, best)) {
            size_t cn = 0;
            const unsigned char* c = nt_trp_find(&t, "TROPCONF.SFM", &cn);
            char* x = c ? (char*)malloc(cn + 1) : NULL;
            if (x) { memcpy(x, c, cn); x[cn] = 0; n = nt_parse_conf(x, cn, s_td.t, s_td.ver, sizeof s_td.ver); free(x); }
            if (n > 0) {
                size_t tn = 0;
                const unsigned char* tx = lang[0] ? nt_trp_find(&t, lang, &tn) : NULL;
                if (!tx) tx = nt_trp_find(&t, "TROP.SFM", &tn);
                x = tx ? (char*)malloc(tn + 1) : NULL;
                if (x) { memcpy(x, tx, tn); x[tn] = 0; nt_parse_texts(x, tn, s_td.t, n); free(x); }
                /* the cache for the launcher and the popup: every file of the set (the player's own data) */
                nt_mkdirs(s_td.dir);
                int wrote = 0, failed = 0;
                for (int i = 0; i < t.count; i++) {
                    const unsigned char* e = t.d + 0x40 + (size_t)i * 64u;
                    if (strpbrk((const char*)e, "/\\:") || ((const char*)e)[0] == '.') continue;
                    snprintf(p, sizeof p, "%s/%s", s_td.dir, (const char*)e);
                    const int r = nt_write_if_changed(p, t.d + be64(e + 32), (size_t)be64(e + 40));
                    if (r > 0) wrote++; else if (r < 0) failed++;
                }
                printf("[trophy] set %s %s from %s: %d trophies, cache %s (%d written, %d failed)\n",
                       id, s_td.ver, best, n, s_td.dir, wrote, failed);
            }
            free(t.d);
        }
    }
    if (n <= 0) {   /* no TRP (disc image not mounted...): the cache from an earlier start */
        size_t cn = 0;
        snprintf(p, sizeof p, "%s/TROPCONF.SFM", s_td.dir);
        unsigned char* x = nt_read_file(p, &cn, (size_t)4 << 20);
        if (x) { n = nt_parse_conf((const char*)x, cn, s_td.t, s_td.ver, sizeof s_td.ver); free(x); }
        if (n > 0) {
            snprintf(p, sizeof p, "%s/%s", s_td.dir, lang[0] ? lang : "TROP.SFM");
            x = nt_read_file(p, &cn, (size_t)4 << 20);
            if (!x) { snprintf(p, sizeof p, "%s/TROP.SFM", s_td.dir); x = nt_read_file(p, &cn, (size_t)4 << 20); }
            if (x) { nt_parse_texts((const char*)x, cn, s_td.t, n); free(x); }
            printf("[trophy] set %s %s from the cache %s: %d trophies\n", id, s_td.ver, s_td.dir, n);
        }
    }
    if (n <= 0) {
        printf("[trophy] no trophy list for %s (no TROPDIR in the game files): trophies are kept as before, no popup\n", id);
        memset(s_td.t, 0, sizeof s_td.t); s_td.ver[0] = 0;
        return 0;
    }
    s_td.n = n;
    for (int i = 0; i < n; i++) if (s_td.t[i].grade == 'P') s_td.plat = i;
    return n;
}

/* ---- unlock events -------------------------------------------------------------------------------------------- */
#define NT_EV 16
static np_trophy_event s_ev[NT_EV];
static unsigned s_ev_head, s_ev_tail;   /* head: next write, tail: next read */
#ifdef _WIN32
static CRITICAL_SECTION s_ev_cs;
static INIT_ONCE s_ev_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK nt_ev_init(PINIT_ONCE o, PVOID a, PVOID* c) { (void)o; (void)a; (void)c; InitializeCriticalSection(&s_ev_cs); return TRUE; }
#define EV_LOCK()   do { InitOnceExecuteOnce(&s_ev_once, nt_ev_init, NULL, NULL); EnterCriticalSection(&s_ev_cs); } while (0)
#define EV_UNLOCK() LeaveCriticalSection(&s_ev_cs)
#else
#define EV_LOCK()   do {} while (0)
#define EV_UNLOCK() do {} while (0)
#endif
#ifdef _WIN32
static CRITICAL_SECTION s_st_cs;
static INIT_ONCE s_st_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK nt_st_init(PINIT_ONCE o, PVOID a, PVOID* c) { (void)o; (void)a; (void)c; InitializeCriticalSection(&s_st_cs); return TRUE; }
void np_trophy_lock(void) { InitOnceExecuteOnce(&s_st_once, nt_st_init, NULL, NULL); EnterCriticalSection(&s_st_cs); }
void np_trophy_unlock(void) { LeaveCriticalSection(&s_st_cs); }
#else
void np_trophy_lock(void) {}
void np_trophy_unlock(void) {}
#endif
void np_trophy_event_push(int id, unsigned long long unix_time)
{
    const np_trophy_def* d = np_trophy_data_get(id);
    if (!d) return;
    EV_LOCK();
    s_ev[s_ev_head % NT_EV].id = id; s_ev[s_ev_head % NT_EV].grade = d->grade; s_ev[s_ev_head % NT_EV].unix_time = unix_time;
    s_ev_head++;
    if (s_ev_head - s_ev_tail > NT_EV) s_ev_tail = s_ev_head - NT_EV;
    EV_UNLOCK();
    if (g_np_trophy_sound_hook) g_np_trophy_sound_hook(id, d->grade);
}
int np_trophy_event_take(np_trophy_event* out)
{
    if (s_ev_head == s_ev_tail) return 0;   /* the common case, no lock (a stale read only delays a frame) */
    int got = 0;
    EV_LOCK();
    if (s_ev_head != s_ev_tail) { *out = s_ev[s_ev_tail % NT_EV]; s_ev_tail++; got = 1; }
    EV_UNLOCK();
    return got;
}
