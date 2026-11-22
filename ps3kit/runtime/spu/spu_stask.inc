/* spu_stask.inc -- stask (2026-11-21): a cheaper SPURS task path. Included by spu_workload.c after spu_async_thread.
 * Every part is OFF unless its switch is set; with none set the code paths are the old ones.
 *
 *  ST_FPC=1       task ELF fingerprint cache. Key = image host address + size; a hit needs the image bytes to equal
 *                 the copy taken when the fingerprint was computed (memcmp, exact: equal bytes = equal FNV-1a), so a
 *                 changed image is always hashed again. ST_FPC_CHECK=1 also computes the full hash beside every hit,
 *                 counts mismatches and dispatches with the full one. Negative controls (they force CHECK):
 *                 ST_FPC_NEG=1 stores fp^1 (the check must count every hit as a mismatch), ST_FPC_NEG=2 flips one
 *                 byte of the stored copy (every lookup must count as "changed" and be hashed again).
 *  ST_POOL=1      task host threads are parked and reused instead of one CreateThread per task. A reused thread
 *                 gets what a new one has: the same 256 MB stack reservation and stack guarantee (it is the same
 *                 thread), the SPU-side priority asked again per task (normal when none), the end-of-task
 *                 lock-line / MFC forget of its whole stack (as at thread exit), then this module's static TLS put
 *                 back to the image's template and the TLS callbacks run with DLL_THREAD_ATTACH (= a new thread's
 *                 TLS; ST_POOL_TLS=0 skips that, diagnostic only). Parked threads exit after ST_POOL_IDLE_MS
 *                 (default 10000) without work; at most ST_POOL_MAX (default 64) park, more exit as before.
 *  ST_API_SPIN=1  spurs_api_lock (cellSpursReadyCountStore & co.) takes the instance and registry locks with
 *                 TryAcquire + PAUSE for up to ST_SPIN_US (default 20) before the blocking acquire: the same locks,
 *                 the same critical section, only no sleep while the kernel thread is between passes.
 *  ST_AB=1/2      in-run A/B: the switches above act only in odd (1) / even (2) 15 s windows since process start;
 *                 "[st-ab] window N ON|OFF" lines. ST_STATS=1 (implied by any switch): "[st] per 5s:" lines. */
#ifdef _WIN32
#include <intrin.h>

static int s_st_init = 0, s_st_fpc, s_st_fpc_check, s_st_fpc_neg, s_st_pool, s_st_pool_tls = 1, s_st_spin, s_st_ab, s_st_stats;
static unsigned s_st_spin_us = 20, s_st_pool_idle_ms = 10000, s_st_pool_max = 64;
static volatile int g_st_side_on = 1;    /* 1 = the switches act now */
static SRWLOCK s_st_init_lock = SRWLOCK_INIT;
static volatile long long s_st_fpc_hit, s_st_fpc_miss, s_st_fpc_chg, s_st_fpc_mism, s_st_fpc_chk,
                          s_st_disp_n, s_st_disp_us, s_st_pool_reuse, s_st_pool_new, s_st_pool_exit, s_st_pool_plain,
                          s_st_api_n, s_st_api_spin_won, s_st_api_fell, s_st_api_wait_us, s_st_api_free,
                          s_st_fp_us, s_st_hand_us;
static int s_st_noboost;
static volatile long s_st_parked;
extern unsigned long long ps3_qpc_us(void);

static unsigned long long st_ms_since_creation(void)
{
    FILETIME cr, ex, ke, us, now;
    GetProcessTimes(GetCurrentProcess(), &cr, &ex, &ke, &us); GetSystemTimeAsFileTime(&now);
    const unsigned long long c = ((unsigned long long)cr.dwHighDateTime << 32) | cr.dwLowDateTime;
    const unsigned long long n = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return n > c ? (n - c) / 10000ull : 0;
}
static DWORD WINAPI st_ab_thread(LPVOID arg)
{
    (void)arg;
    long last = -1; unsigned long long next_stats = 5000;
    long long p[18] = {0};
    for (;;) {
        const unsigned long long t = st_ms_since_creation();
        if (s_st_ab) {
            const long win = (long)(t / 15000ull);
            if (win != last) {
                last = win;
                const int on = s_st_ab == 1 ? (int)(win & 1) : !(win & 1);
                __atomic_store_n(&g_st_side_on, on, __ATOMIC_SEQ_CST);
                fprintf(stderr, "[st-ab] window %ld %s (t %.1f s)\n", win, on ? "ON" : "OFF", (double)t / 1000.0);
                fflush(stderr);
            }
        }
        if (s_st_stats && t >= next_stats) {
            next_stats = (t / 5000ull + 1) * 5000ull;
            long long c[16] = { s_st_fpc_hit, s_st_fpc_miss, s_st_fpc_chg, s_st_fpc_mism, s_st_fpc_chk, s_st_disp_n,
                                s_st_disp_us, s_st_pool_reuse, s_st_pool_new, s_st_pool_exit, s_st_pool_plain, s_st_api_n,
                                s_st_api_spin_won, s_st_api_fell, s_st_api_wait_us, s_st_api_free, s_st_fp_us, s_st_hand_us };
            long long d[18];
            for (int k = 0; k < 18; k++) { d[k] = c[k] - p[k]; p[k] = c[k]; }
            fprintf(stderr, "[st] per 5s: dispatch %lld (%lld us on the calling threads) | fpc hit %lld miss %lld "
                            "changed %lld checked %lld MISMATCH %lld (total %lld) | pool reuse %lld new %lld plain %lld "
                            "idle-exit %lld parked %ld | api locks %lld free %lld spin-won %lld blocked %lld wait %lld us "
                            "| fp %lld us handoff %lld us\n",
                    d[5], d[6], d[0], d[1], d[2], d[4], d[3], c[3], d[7], d[8], d[10], d[9],
                    (long)__atomic_load_n(&s_st_parked, __ATOMIC_RELAXED), d[11], d[15], d[12], d[13], d[14], d[16], d[17]);
            fflush(stderr);
        }
        unsigned long long nt = next_stats;
        if (s_st_ab) { const unsigned long long nw = (t / 15000ull + 1) * 15000ull; if (nw < nt || !s_st_stats) nt = nw; }
        const unsigned long long now = st_ms_since_creation();
        Sleep(nt > now ? (DWORD)(nt - now) + 1 : 1);
    }
    return 0;
}
static void st_init_slow(void)
{
    AcquireSRWLockExclusive(&s_st_init_lock);
    if (!__atomic_load_n(&s_st_init, __ATOMIC_ACQUIRE)) {
        const char* e;
        e = getenv("ST_FPC");       s_st_fpc = !(e && *e == '0');
        e = getenv("ST_FPC_CHECK"); s_st_fpc_check = e && *e == '1';
        e = getenv("ST_FPC_NEG");   s_st_fpc_neg = e ? atoi(e) : 0; if (s_st_fpc_neg) s_st_fpc_check = 1;
        e = getenv("ST_POOL");      s_st_pool = !(e && *e == '0');
        e = getenv("ST_POOL_TLS");  if (e && *e == '0') s_st_pool_tls = 0;
        e = getenv("ST_POOL_IDLE_MS"); if (e && atoi(e) > 0) s_st_pool_idle_ms = (unsigned)atoi(e);
        e = getenv("ST_POOL_MAX");  if (e && atoi(e) >= 0) s_st_pool_max = (unsigned)atoi(e);
        e = getenv("ST_POOL_NOBOOST"); s_st_noboost = e && *e == '1';
        e = getenv("ST_API_SPIN");  s_st_spin = e && *e == '1';
        e = getenv("ST_SPIN_US");   if (e && atoi(e) >= 0) s_st_spin_us = (unsigned)atoi(e);
        e = getenv("ST_AB");        s_st_ab = e ? atoi(e) : 0; if (s_st_ab != 1 && s_st_ab != 2) s_st_ab = 0;
        e = getenv("ST_STATS");     s_st_stats = (e && *e == '1');
        if (s_st_ab) g_st_side_on = 0;
        if (s_st_fpc || s_st_pool || s_st_spin || s_st_stats)
            fprintf(stderr, "[st] stask: fpc %d (check %d neg %d) pool %d (tls reset %d, idle %u ms, max %u) api spin %d "
                            "(%u us) ab %d\n", s_st_fpc, s_st_fpc_check, s_st_fpc_neg, s_st_pool, s_st_pool_tls,
                    s_st_pool_idle_ms, s_st_pool_max, s_st_spin, s_st_spin_us, s_st_ab);
        if (s_st_ab || s_st_stats) { HANDLE h = CreateThread(NULL, 64u << 10, st_ab_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
        __atomic_store_n(&s_st_init, 1, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&s_st_init_lock);
}
static inline void st_init(void) { if (!__atomic_load_n(&s_st_init, __ATOMIC_ACQUIRE)) st_init_slow(); }
static inline int st_side(void) { return __atomic_load_n(&g_st_side_on, __ATOMIC_RELAXED); }

/* ---- ST_FPC: fingerprint cache ---------------------------------------------------------------------------------- */
#define STF_N 64
#define STF_MAX_BYTES (48u << 20)
typedef struct { const uint8_t* img; uint32_t size; uint64_t fp; uint8_t* copy; } stf_ent;
static stf_ent s_stf[STF_N];
static long s_stf_n;
static size_t s_stf_bytes;
static SRWLOCK s_stf_lock = SRWLOCK_INIT;

static uint64_t st_fingerprint_body(const uint8_t* image, uint32_t size)
{
    st_init();
    if (!s_st_fpc || !st_side()) return spu_workload_fingerprint(image, size);
    uint64_t fp = 0; int hit = 0, found = 0;
    AcquireSRWLockShared(&s_stf_lock);
    for (long i = 0; i < s_stf_n; i++) {
        const stf_ent* e = &s_stf[i];
        if (e->img == image && e->size == size) {
            found = 1;
            if (memcmp(e->copy, image, size) == 0) { fp = e->fp; hit = 1; }
            break;
        }
    }
    ReleaseSRWLockShared(&s_stf_lock);
    if (hit) {
        __atomic_add_fetch(&s_st_fpc_hit, 1, __ATOMIC_RELAXED);
        if (s_st_fpc_check) {
            const uint64_t full = spu_workload_fingerprint(image, size);
            __atomic_add_fetch(&s_st_fpc_chk, 1, __ATOMIC_RELAXED);
            if (full != fp) {
                const long long n = __atomic_add_fetch(&s_st_fpc_mism, 1, __ATOMIC_RELAXED);
                if (n <= 8) fprintf(stderr, "[st] fpc MISMATCH image %p size %u cached %016llX full %016llX\n",
                                    (const void*)image, size, (unsigned long long)fp, (unsigned long long)full);
                fp = full;
            }
        }
        return fp;
    }
    if (found) __atomic_add_fetch(&s_st_fpc_chg, 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&s_st_fpc_miss, 1, __ATOMIC_RELAXED);
    /* The copy is taken BEFORE the hash: a store landing in between makes the next lookup differ and hash again. */
    uint8_t* copy = (uint8_t*)malloc(size);
    if (copy) memcpy(copy, image, size);
    const uint64_t full = spu_workload_fingerprint(copy ? copy : image, size);
    if (!copy) return full;
    uint64_t store = full;
    if (s_st_fpc_neg == 1) store ^= 1;
    if (s_st_fpc_neg == 2 && size) copy[size / 2] ^= 0x5A;
    AcquireSRWLockExclusive(&s_stf_lock);
    long i = 0;
    for (; i < s_stf_n; i++) if (s_stf[i].img == image && s_stf[i].size == size) break;
    if (i < s_stf_n) {                          /* changed image: replace its entry */
        free(s_stf[i].copy); s_stf[i].copy = copy; s_stf[i].fp = store; copy = NULL;
    } else if (s_stf_n < STF_N && s_stf_bytes + size <= STF_MAX_BYTES) {
        s_stf[i].img = image; s_stf[i].size = size; s_stf[i].fp = store; s_stf[i].copy = copy; copy = NULL;
        s_stf_bytes += size; s_stf_n++;
    }
    ReleaseSRWLockExclusive(&s_stf_lock);
    free(copy);
    return full;
}

static uint64_t st_fingerprint(const uint8_t* image, uint32_t size)
{
    st_init();
    if (!s_st_stats) return st_fingerprint_body(image, size);
    const unsigned long long t0 = ps3_qpc_us();
    const uint64_t fp = st_fingerprint_body(image, size);
    __atomic_add_fetch(&s_st_fp_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
    return fp;
}

/* ---- ST_POOL: parked task threads ------------------------------------------------------------------------------- */
typedef struct st_worker { HANDLE ev; spu_async_job* job; struct st_worker* next; int idle; } st_worker;
static st_worker* s_stp_idle;
static SRWLOCK s_stp_lock = SRWLOCK_INIT;

extern unsigned long _tls_index;
static void st_tls_reset(void)
{
    static int s_ok = -1;
    static const IMAGE_TLS_DIRECTORY64* s_td;
    static HMODULE s_mod;
    if (s_ok < 0) {
        s_mod = GetModuleHandleW(NULL);
        const IMAGE_DOS_HEADER* dh = (const IMAGE_DOS_HEADER*)s_mod;
        const IMAGE_NT_HEADERS64* nh = (const IMAGE_NT_HEADERS64*)((const uint8_t*)s_mod + dh->e_lfanew);
        const IMAGE_DATA_DIRECTORY* dd = &nh->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        s_td = dd->VirtualAddress ? (const IMAGE_TLS_DIRECTORY64*)((const uint8_t*)s_mod + dd->VirtualAddress) : NULL;
        s_ok = s_td != NULL;
    }
    if (!s_ok) return;
    void** tlsp = (void**)__readgsqword(0x58);          /* TEB.ThreadLocalStoragePointer */
    uint8_t* blk = (uint8_t*)tlsp[_tls_index];
    const size_t raw = (size_t)(s_td->EndAddressOfRawData - s_td->StartAddressOfRawData);
    memcpy(blk, (const void*)(uintptr_t)s_td->StartAddressOfRawData, raw);
    if (s_td->SizeOfZeroFill) memset(blk + raw, 0, s_td->SizeOfZeroFill);
    PIMAGE_TLS_CALLBACK* cb = (PIMAGE_TLS_CALLBACK*)(uintptr_t)s_td->AddressOfCallBacks;
    for (; cb && *cb; cb++) (*cb)(s_mod, DLL_THREAD_ATTACH, NULL);
}

/* The body of spu_async_thread for one job, minus the thread start (done once by the worker). */
static void st_run_one(spu_async_job* j)
{
    { extern int host_spu_side_prio(void); const int sp = host_spu_side_prio();
      const int want = sp ? sp : THREAD_PRIORITY_NORMAL;
      if (GetThreadPriority(GetCurrentThread()) != want) SetThreadPriority(GetCurrentThread(), want); }
    spu_async_run(j);
    { ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi);
      extern void spu_coh_forget_range(uintptr_t, uintptr_t);
      spu_coh_forget_range((uintptr_t)lo, (uintptr_t)hi);
      spu_mfc_forget_range((uintptr_t)lo, (uintptr_t)hi); }
}

static DWORD WINAPI spu_async_thread_pooled(LPVOID p)
{
    st_worker* w = (st_worker*)p;
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    if (s_st_noboost) SetThreadPriorityBoost(GetCurrentThread(), TRUE);   /* ST_POOL_NOBOOST (test): no wake boost */
    spu_async_job* j = w->job;
    for (;;) {
        st_run_one(j);
        if (s_st_pool_tls) st_tls_reset();
        /* park, unless enough threads park already (then exit, as every task thread did before) */
        AcquireSRWLockExclusive(&s_stp_lock);
        if ((unsigned)s_st_parked >= s_st_pool_max) { ReleaseSRWLockExclusive(&s_stp_lock); break; }
        w->job = NULL; w->idle = 1; w->next = s_stp_idle; s_stp_idle = w;
        __atomic_add_fetch(&s_st_parked, 1, __ATOMIC_RELAXED);
        ReleaseSRWLockExclusive(&s_stp_lock);
        int leave = 0;
        for (;;) {
            if (WaitForSingleObject(w->ev, s_st_pool_idle_ms) == WAIT_OBJECT_0) break;
            AcquireSRWLockExclusive(&s_stp_lock);
            if (w->idle) {                       /* still parked: take it off the list and exit */
                st_worker** pp = &s_stp_idle;
                while (*pp && *pp != w) pp = &(*pp)->next;
                if (*pp) *pp = w->next;
                __atomic_sub_fetch(&s_st_parked, 1, __ATOMIC_RELAXED);
                leave = 1;
            }
            ReleaseSRWLockExclusive(&s_stp_lock);
            if (leave) break;
            /* handed a job meanwhile: its event is set, the next wait returns at once */
        }
        if (leave) { __atomic_add_fetch(&s_st_pool_exit, 1, __ATOMIC_RELAXED); break; }
        j = w->job;
    }
    CloseHandle(w->ev); free(w);
    return 0;
}

/* Returns 1 when the job went to a pool thread (parked or new), 0 = caller takes the old CreateThread path. */
static int st_pool_dispatch(spu_async_job* j)
{
    st_init();
    if (!s_st_pool || !st_side()) { if (s_st_pool) __atomic_add_fetch(&s_st_pool_plain, 1, __ATOMIC_RELAXED); return 0; }
    AcquireSRWLockExclusive(&s_stp_lock);
    st_worker* w = s_stp_idle;
    if (w) { s_stp_idle = w->next; w->next = NULL; w->idle = 0; w->job = j; __atomic_sub_fetch(&s_st_parked, 1, __ATOMIC_RELAXED); }
    ReleaseSRWLockExclusive(&s_stp_lock);
    if (w) {
        const unsigned long long t0 = s_st_stats ? ps3_qpc_us() : 0;
        SetEvent(w->ev);
        if (s_st_stats) __atomic_add_fetch(&s_st_hand_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_st_pool_reuse, 1, __ATOMIC_RELAXED);
        return 1;
    }
    w = (st_worker*)calloc(1, sizeof *w);
    if (!w) return 0;
    w->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!w->ev) { free(w); return 0; }
    w->job = j;
    HANDLE th = CreateThread(NULL, 256u << 20, spu_async_thread_pooled, w, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { CloseHandle(w->ev); free(w); return 0; }
    CloseHandle(th);
    __atomic_add_fetch(&s_st_pool_new, 1, __ATOMIC_RELAXED);
    return 1;
}

static inline void st_disp_note(unsigned long long t0)
{
    if (!s_st_stats) return;
    __atomic_add_fetch(&s_st_disp_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_st_disp_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
}
static inline unsigned long long st_disp_t0(void) { st_init(); return s_st_stats ? ps3_qpc_us() : 0; }

/* ---- ST_API_SPIN: spurs_api_lock's acquires (called from cellSpurs.c) ------------------------------------------- */
void st_srw_acquire(SRWLOCK* l)
{
    st_init();
    if (!s_st_spin || !st_side()) {
        if (!s_st_stats) { AcquireSRWLockExclusive(l); return; }
        __atomic_add_fetch(&s_st_api_n, 1, __ATOMIC_RELAXED);
        if (TryAcquireSRWLockExclusive(l)) { __atomic_add_fetch(&s_st_api_free, 1, __ATOMIC_RELAXED); return; }
        const unsigned long long t0 = ps3_qpc_us();
        AcquireSRWLockExclusive(l);
        __atomic_add_fetch(&s_st_api_fell, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_st_api_wait_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&s_st_api_n, 1, __ATOMIC_RELAXED);
    if (TryAcquireSRWLockExclusive(l)) { __atomic_add_fetch(&s_st_api_free, 1, __ATOMIC_RELAXED); return; }
    const unsigned long long t0 = ps3_qpc_us();
    for (unsigned k = 1;; k++) {
        _mm_pause();
        if (!(k & 15) && TryAcquireSRWLockExclusive(l)) {
            __atomic_add_fetch(&s_st_api_spin_won, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&s_st_api_wait_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
            return;
        }
        if (!(k & 63) && ps3_qpc_us() - t0 >= s_st_spin_us) break;
    }
    AcquireSRWLockExclusive(l);
    __atomic_add_fetch(&s_st_api_fell, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_st_api_wait_us, (long long)(ps3_qpc_us() - t0), __ATOMIC_RELAXED);
}
#else
static uint64_t st_fingerprint(const uint8_t* image, uint32_t size) { return spu_workload_fingerprint(image, size); }
static int st_pool_dispatch(spu_async_job* j) { (void)j; return 0; }
static inline void st_disp_note(unsigned long long t0) { (void)t0; }
static inline unsigned long long st_disp_t0(void) { return 0; }
void st_srw_acquire(SRWLOCK* l) { AcquireSRWLockExclusive(l); }
#endif
