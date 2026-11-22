/* zeroemu pool (host\zeroemu\pool\DESIGN.md, 2026-11-20): ZP_POOL=1 replaces the six spurs_pm_worker threads AND the
 * SPURS kernel thread by one pool of plain host threads, one per lane (virtual SPU), with no scheduler thread.
 * Default OFF: with ZP_POOL unset nothing here runs and the old threads start exactly as before.
 *
 * - A lane = one virtual SPU (slot): its priority row, its Wws progress rows, its per-(wid, lane) policy contexts.
 *   Phase A: pool thread k hosts lane k (same placement as SPU worker k).
 * - The run itself is spurs_pm_worker's body, copied unchanged (zp_run_body): inline natives through the hook, else
 *   the lifted policy module through its local-store context; the same bracket (GCM park, sound boost, stats).
 * - Admission: the kernel's pass (spurs_admit over every free lane, the first pick rotating) is run by
 *     (a) the thread that just retired a run (END pass), and
 *     (b) the "leader": one idle pool thread that waits for kicks (ReadyCount*, WakeUp, API calls, GPU reports that
 *         clear a back-off, SPU commits to the instance's first two lines) or for the earliest idle back-off due,
 *         at most ZP_SAFETY_US (1000) -- a kick by a store nobody hooks is seen that late, as the sub-8-core kernel.
 *   A filled lane's thread is woken on its own word (WakeByAddressSingle) only if it is idle.
 * - Idle: spin ZP_SPIN_US (30) on the own word, then wait (leader: event + high-resolution timer; others:
 *   WaitOnAddress, 100 ms safety).
 * - [zp] line every 5 s: passes and admissions per kind, the timer-only ("safety") admissions of KICKED work
 *   (= kicks no hook reported), wakes, lost wakes (must be 0). */
#pragma comment(lib, "Synchronization.lib")
typedef struct {
    volatile LONG word;      /* bumped: my lane filled, promoted to leader, a kick for the leader, stop */
    volatile LONG in_wait;   /* the leader inside its event wait (kick -> SetEvent) */
    int idle;                /* under si->lock */
    struct SpursInst* si; u32 idx;
} ZpThr;
typedef struct {
    int on;
    volatile LONG leader;    /* the leader thread (index), -1 none; written under si->lock */
    HANDLE ev, timer;
    /* zp-2: mux = fewer threads than lanes (ZP_THREADS, or logical processors - 2 below 8): a thread runs any lane
     * that is filled and not claimed; claimed[lane] = the thread running it, -1 none (under si->lock). */
    int mux; u32 nthr, rr;
    int claimed[CELL_SPURS_MAX_SPU];
    ZpThr t[CELL_SPURS_MAX_SPU];
} ZpInst;
static ZpInst s_zp[MAX_SPURS_INST];
static SPU_THREAD_LOCAL int t_zp_me = -1;
static SPU_THREAD_LOCAL int t_zp_take_one;   /* mux: the pass's first fill is taken by the passing thread */
enum { ZPW_END, ZPW_KICK, ZPW_DUE, ZPW_SAFE, ZPW_LEAD, ZPW_N };
static volatile long long s_zs_pass[ZPW_N], s_zs_adm[ZPW_N], s_zs_admk[ZPW_N];
static volatile long long s_zs_kicks, s_zs_kick_ev, s_zs_fill_wake, s_zs_sleep_l, s_zs_sleep_n, s_zs_spin_hit,
                          s_zs_ntimeout, s_zs_lost, s_zs_promote, s_zs_runs;
static unsigned long long s_zs_next;
static unsigned s_zp_spin_us = 30, s_zp_safety_us = 1000;
/* zp-3 tuning: ZP_PRIO, and ZP_AB (side B in odd 15 s windows: ZP_SPIN_US_B, ZP_PRIO_B) */
static int s_zp_prio = 99, s_zp_prio_b = 99, s_zp_ab;
static unsigned s_zp_spin_b = 30;
static volatile int g_zp_side;            /* 1 = side B now */
static volatile LONG g_zp_side_gen;       /* bumped at every side change: threads re-apply their priority */
static unsigned long long zp_ms_since_creation(void)
{
    FILETIME cr, ex, ke, us, now;
    GetProcessTimes(GetCurrentProcess(), &cr, &ex, &ke, &us); GetSystemTimeAsFileTime(&now);
    const unsigned long long c = ((unsigned long long)cr.dwHighDateTime << 32) | cr.dwLowDateTime;
    const unsigned long long n = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return n > c ? (n - c) / 10000ull : 0;
}
static DWORD WINAPI zp_ab_thread(LPVOID arg)
{
    (void)arg;
    long last = -1;
    for (;;) {
        const unsigned long long t = zp_ms_since_creation();
        const long win = (long)(t / 15000ull);
        if (win != last) {
            last = win;
            __atomic_store_n(&g_zp_side, (int)(win & 1), __ATOMIC_SEQ_CST);
            _InterlockedIncrement(&g_zp_side_gen);
            fprintf(stderr, "[zp-ab] window %ld %s (t %.1f s)\n", win, (win & 1) ? "ON" : "OFF", (double)t / 1000.0);
        }
        const unsigned long long next = (unsigned long long)(win + 1) * 15000ull, now = zp_ms_since_creation();
        Sleep(next > now ? (DWORD)(next - now) + 1 : 1);
    }
    return 0;
}
static inline unsigned zp_spin_now(void) { return s_zp_ab && __atomic_load_n(&g_zp_side, __ATOMIC_RELAXED) ? s_zp_spin_b : s_zp_spin_us; }
static inline int zp_prio_now(void) { return s_zp_ab && __atomic_load_n(&g_zp_side, __ATOMIC_RELAXED) ? s_zp_prio_b : s_zp_prio; }

static int zp_enabled(void)
{
    static int s_e = -1;
    if (s_e < 0) {
        const char* e = getenv("ZP_POOL"); s_e = e ? (*e == '1') : 1;
        const char* s = getenv("ZP_SPIN_US"); if (s) s_zp_spin_us = (unsigned)atoi(s);
        s = getenv("ZP_SAFETY_US"); if (s && atoi(s) > 0) s_zp_safety_us = (unsigned)atoi(s);
        s = getenv("ZP_PRIO"); if (s && *s) s_zp_prio = atoi(s);
        s_zp_spin_b = s_zp_spin_us; s_zp_prio_b = s_zp_prio;
        s = getenv("ZP_AB"); s_zp_ab = s && *s == '1';
        s = getenv("ZP_SPIN_US_B"); if (s) s_zp_spin_b = (unsigned)atoi(s);
        s = getenv("ZP_PRIO_B"); if (s && *s) s_zp_prio_b = atoi(s);
    }
    return s_e;
}
static inline ZpInst* zp_of(struct SpursInst* si) { return &s_zp[si - s_inst]; }

/* A wake source (after the state change it announces): the leader looks again. A fence and one load when no leader
 * waits; a SetEvent only while it is inside its event wait. */
static void zp_kick(struct SpursInst* si)
{
    ZpInst* z = zp_of(si);
    if (!z->on) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const LONG l = __atomic_load_n(&z->leader, __ATOMIC_SEQ_CST);
    if (l < 0) return;   /* no idle thread: the next END pass sees the change */
    ZpThr* T = &z->t[l];
    _InterlockedIncrement(&T->word);
    __atomic_add_fetch(&s_zs_kicks, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&T->in_wait, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&s_zs_kick_ev, 1, __ATOMIC_RELAXED);
        SetEvent(z->ev);
    }
}

/* spurs_slot_fill's wake (si->lock held): the lane's own thread, if it waits. Returns 1 when the pool is on. */
static int zp_fill_wake(struct SpursInst* si, struct SpursPmSlot* slot)
{
    ZpInst* z = zp_of(si);
    if (!z->on) return 0;
    if (z->mux) {
        /* any idle thread may run it; the passing pool thread takes the first fill of its own pass */
        if (t_zp_take_one) {
            /* only if it is the one lane the passing thread will pick: no other filled lane waits */
            int other = 0;
            for (u32 sn = 0; sn < si->nspus; sn++)
                if (sn != slot->spu_num && si->slots[sn].busy && z->claimed[sn] < 0) other = 1;
            if (!other) { t_zp_take_one = 0; return 1; }
        }
        for (u32 k = 0; k < z->nthr; k++) {
            const u32 j = (z->rr + k) % z->nthr;
            ZpThr* T = &z->t[j];
            if (!T->idle || z->leader == (LONG)j) continue;
            T->idle = 0;   /* taken: not chosen twice */
            _InterlockedIncrement(&T->word);
            __atomic_add_fetch(&s_zs_fill_wake, 1, __ATOMIC_RELAXED);
            WakeByAddressSingle((PVOID)&T->word);
            z->rr = j + 1;
            return 1;
        }
        const LONG l = z->leader;
        if (l >= 0 && z->t[l].idle) {
            _InterlockedIncrement(&z->t[l].word);
            __atomic_add_fetch(&s_zs_fill_wake, 1, __ATOMIC_RELAXED);
            SetEvent(z->ev);
        }
        return 1;   /* no idle thread: the next thread that finishes a run takes it */
    }
    const u32 lane = slot->spu_num;
    if ((int)lane == t_zp_me) return 1;   /* the filler runs it next itself */
    ZpThr* T = &z->t[lane];
    _InterlockedIncrement(&T->word);
    if (T->idle) {
        __atomic_add_fetch(&s_zs_fill_wake, 1, __ATOMIC_RELAXED);
        if (z->leader == (LONG)lane) SetEvent(z->ev);
        else WakeByAddressSingle((PVOID)&T->word);
    }
    return 1;
}

static void zp_wake_all(struct SpursInst* si)
{
    ZpInst* z = zp_of(si);
    if (!z->on) return;
    for (u32 sn = 0; sn < z->nthr; sn++) { _InterlockedIncrement(&z->t[sn].word); WakeByAddressAll((PVOID)&z->t[sn].word); }
    if (z->ev) SetEvent(z->ev);
}

static void zp_stats_maybe_print(unsigned long long now)
{
    if (now < s_zs_next) return;
    const int first = s_zs_next == 0;
    s_zs_next = now + 5000000ull;
    if (first) return;
    long long p[ZPW_N], a[ZPW_N], k[ZPW_N];
    for (int i = 0; i < ZPW_N; i++) {
        p[i] = __atomic_exchange_n(&s_zs_pass[i], 0, __ATOMIC_RELAXED);
        a[i] = __atomic_exchange_n(&s_zs_adm[i], 0, __ATOMIC_RELAXED);
        k[i] = __atomic_exchange_n(&s_zs_admk[i], 0, __ATOMIC_RELAXED);
    }
    printf("[zp] per 5s: runs %lld; passes/admitted(kicked) end %lld/%lld(%lld) kick %lld/%lld(%lld) "
           "lead %lld/%lld(%lld) due %lld/%lld(%lld) safety %lld/%lld(%lld = timer-only kicked); kicks %lld (%lld events); "
           "fill wakes %lld; sleeps leader %lld other %lld; spin hits %lld; promotes %lld; 100ms timeouts %lld; LOST %lld\n",
           __atomic_exchange_n(&s_zs_runs, 0, __ATOMIC_RELAXED),
           p[ZPW_END], a[ZPW_END], k[ZPW_END], p[ZPW_KICK], a[ZPW_KICK], k[ZPW_KICK], p[ZPW_LEAD], a[ZPW_LEAD], k[ZPW_LEAD],
           p[ZPW_DUE], a[ZPW_DUE], k[ZPW_DUE], p[ZPW_SAFE], a[ZPW_SAFE], k[ZPW_SAFE],
           __atomic_exchange_n(&s_zs_kicks, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_zs_kick_ev, 0, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_zs_fill_wake, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_zs_sleep_l, 0, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_zs_sleep_n, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_zs_spin_hit, 0, __ATOMIC_RELAXED),
           __atomic_exchange_n(&s_zs_promote, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_zs_ntimeout, 0, __ATOMIC_RELAXED),
           s_zs_lost);
    fflush(stdout);
}

/* The kernel's pass (spurs_kernel_thread's admission loop), si->lock held. Returns the lanes filled. */
static int zp_pass(struct SpursInst* si, int why)
{
    if (si->stopping) return 0;
    const u32 ea = si->ea;
    AcquireSRWLockExclusive(&s_workload_registry_lock);
    si->pass_no++;
    const unsigned idle_base = spurs_idle_base_us();
    const unsigned long long pass_us = idle_base ? ps3_qpc_us() : 0;
    const u32 enabled = vm_read32(ea + SPURS_WKL_ENABLED);
    const u32 sn0 = spurs_ready_level() ? si->pass_no % si->nspus : 0;
    int n = 0, nk = 0;
    for (u32 k = 0; k < si->nspus; k++) {
        const u32 sn = (sn0 + k) % si->nspus;
        if (si->slots[sn].busy) continue;
        if (spurs_admit(si, sn, ea, enabled, idle_base, pass_us, 0)) { n++; if (si->admit_kicked) nk++; }
    }
    ReleaseSRWLockExclusive(&s_workload_registry_lock);
    __atomic_add_fetch(&s_zs_pass[why], 1, __ATOMIC_RELAXED);
    if (n) { __atomic_add_fetch(&s_zs_adm[why], n, __ATOMIC_RELAXED); __atomic_add_fetch(&s_zs_admk[why], nk, __ATOMIC_RELAXED); }
    if (si == &s_inst[0]) zp_stats_maybe_print(pass_us ? pass_us : ps3_qpc_us());
    return n;
}

/* The leader's wait: until the earliest idle back-off still ahead is due, at most ZP_SAFETY_US. 1 = a due. */
static unsigned zp_leader_wait_us(struct SpursInst* si, int* is_due)
{
    const unsigned long long now = ps3_qpc_us();
    unsigned long long due = now + s_zp_safety_us;
    *is_due = 0;
    const u32 ea = si->ea, nwid = si->is2 ? SPURS_MAX_WID : 16u;
    const u32 enabled = vm_read32(ea + SPURS_WKL_ENABLED);
    for (u32 w = 0; w < nwid; w++) {
        if (!(enabled & (0x80000000u >> w)) || vm_read8(ea + wkl_state_off(w)) != 2 ||
            !s_workloads[w].in_use || s_workloads[w].spurs_ea != ea)
            continue;
        if (g_spurs_wid_block & (1u << w)) continue;
        if (si->idle[w] && si->idle_due[w] > now && si->idle_due[w] < due) { due = si->idle_due[w]; *is_due = 1; }
    }
    const unsigned us = (unsigned)(due - now);
    return us ? us : 1;
}

/* The leader is not idle any more: hand the role to an idle thread (which then makes its own pass). */
static void zp_resign(struct SpursInst* si, ZpInst* z, u32 me)
{
    if (z->leader != (LONG)me) return;
    __atomic_store_n(&z->leader, -1, __ATOMIC_SEQ_CST);
    for (u32 k = 1; k < z->nthr; k++) {
        const u32 j = (me + k) % z->nthr;
        if (!z->t[j].idle || (!z->mux && si->slots[j].busy)) continue;
        __atomic_store_n(&z->leader, (LONG)j, __ATOMIC_SEQ_CST);
        if (z->mux) z->t[j].idle = 0;   /* as a fill wake: not chosen twice */
        _InterlockedIncrement(&z->t[j].word);
        WakeByAddressSingle((PVOID)&z->t[j].word);
        __atomic_add_fetch(&s_zs_promote, 1, __ATOMIC_RELAXED);
        return;
    }
}

/* The lane this thread runs next (si->lock held), -1 none. Phase A: its own lane when filled. mux: a filled lane
 * nobody runs, the sound lane 5 first, then from a rotating start. */
static int zp_pick(struct SpursInst* si, ZpInst* z, u32 me)
{
    if (!z->mux) return si->slots[me].busy ? (int)me : -1;
    const u32 n = si->nspus;
    if (n > 5 && si->slots[5].busy && z->claimed[5] < 0) return 5;
    const u32 r = si->pass_no;
    for (u32 k = 0; k < n; k++) {
        const u32 sn = (r + k) % n;
        if (si->slots[sn].busy && z->claimed[sn] < 0) return (int)sn;
    }
    return -1;
}

/* spurs_pm_worker's run, unchanged (2026-11-20 main 95F11D02, lines 2721-2814): returns its poll count. */
static unsigned zp_run_body(struct SpursPmSlot* slot)
{
#include "zp_runbody.inc"
    return polls;
}

static inline int zp_spin(ZpThr* T, LONG seen)
{
    const unsigned sp = zp_spin_now();
    if (!sp) return 0;
    const unsigned long long t0 = ps3_qpc_us();
    do {
        for (int i = 0; i < 32; i++) { if (__atomic_load_n(&T->word, __ATOMIC_ACQUIRE) != seen) return 1; YieldProcessor(); }
    } while (ps3_qpc_us() - t0 < sp);
    return 0;
}

static DWORD WINAPI zp_worker(LPVOID p)
{
    ZpThr* T = (ZpThr*)p;
    struct SpursInst* si = T->si;
    ZpInst* z = zp_of(si);
    const u32 me = T->idx;
#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#endif
    if (si == &s_inst[0]) host_pin_thread(HOST_ROLE_SPU, (int)me);
    if (!z->mux) t_zp_me = (int)me;
    LONG prio_gen = -1; int prio_base = GetThreadPriority(GetCurrentThread());
    AcquireSRWLockExclusive(&si->lock);
    for (;;) {
        {   const LONG g = __atomic_load_n(&g_zp_side_gen, __ATOMIC_RELAXED);
            if (g != prio_gen) {
                prio_gen = g;
                const int pr = zp_prio_now();
                SetThreadPriority(GetCurrentThread(), pr >= -2 && pr <= 2 ? pr : prio_base);
            } }
        const int lane = zp_pick(si, z, me);
        if (lane >= 0) {
            struct SpursPmSlot* slot = &si->slots[lane];
            z->claimed[lane] = (int)me;
            zp_resign(si, z, me);
            ReleaseSRWLockExclusive(&si->lock);
            const unsigned polls = zp_run_body(slot);
            __atomic_add_fetch(&s_zs_runs, 1, __ATOMIC_RELAXED);
            AcquireSRWLockExclusive(&si->lock);
            z->claimed[lane] = -1;
            spurs_slot_retire(si, slot, polls);
            t_zp_take_one = z->mux;
            zp_pass(si, ZPW_END);
            t_zp_take_one = 0;
            continue;
        }
        if (si->stopping) break;
        if (z->leader < 0) __atomic_store_n(&z->leader, (LONG)me, __ATOMIC_SEQ_CST);
        if (z->leader == (LONG)me) {
            /* Dekker with zp_kick: leader published (seq_cst) before `seen` and the pass; a kick after the pass's
             * reads bumps the word. */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            const LONG seen = __atomic_load_n(&T->word, __ATOMIC_SEQ_CST);
            t_zp_take_one = z->mux;
            zp_pass(si, ZPW_LEAD);
            t_zp_take_one = 0;
            if (zp_pick(si, z, me) >= 0) continue;
            int is_due = 0;
            const unsigned us = zp_leader_wait_us(si, &is_due);
            T->idle = 1;
            ReleaseSRWLockExclusive(&si->lock);
            int why = ZPW_KICK;
            if (zp_spin(T, seen)) __atomic_add_fetch(&s_zs_spin_hit, 1, __ATOMIC_RELAXED);
            else {
                __atomic_store_n(&T->in_wait, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&T->word, __ATOMIC_SEQ_CST) == seen && !__atomic_load_n(&si->stopping, __ATOMIC_RELAXED)) {
                    __atomic_add_fetch(&s_zs_sleep_l, 1, __ATOMIC_RELAXED);
                    DWORD r;
                    LARGE_INTEGER rel; rel.QuadPart = -(LONGLONG)us * 10;
                    if (z->timer && SetWaitableTimer(z->timer, &rel, 0, NULL, NULL, FALSE)) {
                        HANDLE hs[2] = { z->ev, z->timer };
                        r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                        if (r == WAIT_OBJECT_0 + 1) why = is_due ? ZPW_DUE : ZPW_SAFE;
                        else CancelWaitableTimer(z->timer);
                    } else {
                        r = WaitForSingleObject(z->ev, (us + 999) / 1000);
                        if (r == WAIT_TIMEOUT) why = is_due ? ZPW_DUE : ZPW_SAFE;
                    }
                }
                __atomic_store_n(&T->in_wait, 0, __ATOMIC_SEQ_CST);
            }
            AcquireSRWLockExclusive(&si->lock);
            T->idle = 0;
            if (z->leader == (LONG)me && zp_pick(si, z, me) < 0) {
                t_zp_take_one = z->mux;
                zp_pass(si, why);
                t_zp_take_one = 0;
            }
            continue;
        }
        /* not the leader: wait for a lane to run, a promotion or stop */
        const LONG seen = T->word;
        T->idle = 1;
        ReleaseSRWLockExclusive(&si->lock);
        if (zp_spin(T, seen)) __atomic_add_fetch(&s_zs_spin_hit, 1, __ATOMIC_RELAXED);
        else if (__atomic_load_n(&T->word, __ATOMIC_ACQUIRE) == seen) {
            __atomic_add_fetch(&s_zs_sleep_n, 1, __ATOMIC_RELAXED);
            LONG cmp = seen;
            if (!WaitOnAddress((volatile VOID*)&T->word, &cmp, sizeof(LONG), 100) && GetLastError() == ERROR_TIMEOUT) {
                __atomic_add_fetch(&s_zs_ntimeout, 1, __ATOMIC_RELAXED);
                AcquireSRWLockExclusive(&si->lock);
                /* lost: a lane waits for this thread and nothing else is awake to take it */
                if (zp_pick(si, z, me) >= 0 && T->idle) {
                    int other = 0;
                    if (z->mux) for (u32 k = 0; k < z->nthr; k++) if (k != me && !z->t[k].idle) other = 1;
                    if (!other) __atomic_add_fetch(&s_zs_lost, 1, __ATOMIC_RELAXED);
                }
                ReleaseSRWLockExclusive(&si->lock);
            }
        }
        AcquireSRWLockExclusive(&si->lock);
        T->idle = 0;
    }
    zp_resign(si, z, me);
    ReleaseSRWLockExclusive(&si->lock);
    return 0;
}

/* spurs_scheduler_start with ZP_POOL=1 (after its memsets): the pool instead of workers + kernel. */
static s32 zp_start(struct SpursInst* si)
{
    ZpInst* z = zp_of(si);
    memset(z, 0, sizeof(*z));
    z->leader = -1;
    for (u32 k = 0; k < CELL_SPURS_MAX_SPU; k++) z->claimed[k] = -1;
    /* threads: ZP_THREADS, else the zp-4 rule from the tuning runs (pool NOTES 6.C): <= 4 logical processors 3,
       5-7 4, 8-11 5, >= 12 6 (one lane per thread); capped at the lanes below */
    int lps = 0;
    { DWORD_PTR pm = 0, sm = 0; if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) for (; pm; pm &= pm - 1) lps++; }
    int n = lps <= 4 ? 3 : lps <= 7 ? 4 : lps <= 11 ? 5 : 6;
    { const char* e = getenv("ZP_THREADS"); if (e && atoi(e) > 0) n = atoi(e); }
    if (n > (int)si->nspus) n = (int)si->nspus;
    z->nthr = (u32)n;
    z->mux = z->nthr < si->nspus;
    z->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    z->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!z->ev) return CELL_SPURS_CORE_ERROR_NOMEM;
    z->on = 1;
    __atomic_store_n(&s_zp_any, 1, __ATOMIC_SEQ_CST);
    { extern void (*g_spu_line_commit_hook)(uint32_t); g_spu_line_commit_hook = spurs_line_committed; }
    for (u32 sn = 0; sn < si->nspus; sn++) {
        struct SpursPmSlot* slot = &si->slots[sn];
        memset(slot, 0, sizeof(*slot));
        slot->instance = si; slot->spu_num = sn;
    }
    /* the thread handles live in slots[0..nthr-1].thread (spurs_scheduler_stop joins every non-NULL one) */
    for (u32 k = 0; k < z->nthr; k++) {
        z->t[k].si = si; z->t[k].idx = k;
        HANDLE h = CreateThread(NULL, 1u << 20, zp_worker, &z->t[k], 0, NULL);
        if (!h) {
            AcquireSRWLockExclusive(&si->lock);
            si->stopping = 1; zp_wake_all(si);
            ReleaseSRWLockExclusive(&si->lock);
            for (u32 j = 0; j < k; j++) { WaitForSingleObject(si->slots[j].thread, INFINITE); CloseHandle(si->slots[j].thread); si->slots[j].thread = NULL; }
            AcquireSRWLockExclusive(&si->lock); si->kernel_live = 0; si->stopping = 0; ReleaseSRWLockExclusive(&si->lock);
            z->on = 0;
            return CELL_SPURS_CORE_ERROR_NOMEM;
        }
        si->slots[k].thread = h;
        SetThreadDescription(h, L"job pool");
    }
    si->coordinator = NULL;
    if (s_zp_ab) { HANDLE h = CreateThread(NULL, 64u << 10, zp_ab_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    if (s_zp_prio != 99 || s_zp_ab)
        fprintf(stderr, "[zp] tuning: prio %d%s; AB %s (side B: spin %u us, prio %d)\n", s_zp_prio, s_zp_prio == 99 ? " (as placed)" : "",
                s_zp_ab ? "on, 15 s windows, odd = B" : "off", s_zp_spin_b, s_zp_prio_b);
    fprintf(stderr, "[zp] pool on: %u threads for %u lanes (%s; %d logical processors), no SPURS kernel thread; "
            "spin %u us, leader safety %u us%s\n", z->nthr, si->nspus, z->mux ? "lanes shared" : "one per lane", lps,
            s_zp_spin_us, s_zp_safety_us, z->timer ? "" : " (no high-resolution timer: 1 ms waits)");
    return CELL_OK;
}
