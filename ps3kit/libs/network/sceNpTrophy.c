/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 */

#include "sceNpTrophy.h"
#include "np_trophy_data.h"   /* host/trophies: the set from the player's game files */
#include "../system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"                   /* ps3_bswap64 -- guest is big-endian */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
/* TROPHY_TRACE=1: one line per trophy call that matters (host/trophies) */
static int trophy_trace(void)
{
    static int t = -1;
    if (t < 0) { const char* e = getenv("TROPHY_TRACE"); t = e && e[0] && e[0] != '0'; }
    return t;
}
static char s_storage_path[512] = "gamedata/trophies";

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
    int                      has_data;     /* host/trophies: the real set is known (count, platinum rule) */
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

static void trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    /* host/trophies: written to <file>.tmp and then put in place, so a crash never leaves a half file */
    char tmppath[1100];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", filepath);
    fp = fopen(tmppath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {   /* every id kept, also ones outside the set */
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    const int bad = ferror(fp);
    if (fclose(fp) != 0 || bad || !np_trophy_replace_file(tmppath, filepath)) {
        remove(tmppath);
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n", filepath);
    }
}

/* host/trophies: the platinum is the system's: given when every trophy that counts for it is unlocked.
 * Returns its id when it was given now, else -1. */
static s32 trophy_platinum_check(TrophyContext* ctx)
{
    const int plat = np_trophy_data_platinum();
    if (!ctx->has_data || plat < 0 || ctx->unlocked[plat]) return -1;
    const int n = np_trophy_data_count();
    for (int i = 0; i < n; i++) {
        const np_trophy_def* d = np_trophy_data_get(i);
        if (i != plat && d && d->pid == plat && !ctx->unlocked[i]) return -1;
    }
    ctx->unlocked[plat] = 1;
    ctx->unlock_time[plat] = (u64)time(NULL);
    printf("[trophy] platinum %d unlocked (every trophy of the set is unlocked)\n", plat);
    np_trophy_event_push(plat, ctx->unlock_time[plat]);
    return plat;
}

#if defined(NP_TROPHY_TEST) && NP_TROPHY_TEST && defined(_WIN32)
/* TEST BUILDS ONLY (-DNP_TROPHY_TEST=1, build\trophy): TROPHY_TEST_UNLOCK=<id>[,<id>...] unlocks those ids through
 * sceNpTrophyUnlockTrophy TROPHY_TEST_DELAY_S seconds (default 20) after the registration, TROPHY_TEST_GAP_S apart. */
#include <windows.h>
static DWORD WINAPI trophy_test_thread(LPVOID arg)
{
    const s32 ctx = (s32)(intptr_t)arg;
    const char* e = getenv("TROPHY_TEST_UNLOCK");
    const char* d = getenv("TROPHY_TEST_DELAY_S");
    const char* gp = getenv("TROPHY_TEST_GAP_S");
    Sleep((DWORD)((d && *d ? atof(d) : 20.0) * 1000.0));
    for (const char* p = e; p && *p; ) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const int id = atoi(p);
        const s32 r = sceNpTrophyUnlockTrophy(ctx, 1, id, NULL);
        printf("[trophy] TEST unlock %d -> 0x%08X\n", id, (unsigned)r);
        while (*p && *p != ',') p++;
        Sleep((DWORD)((gp && *gp ? atof(gp) : 1.0) * 1000.0));
    }
    return 0;
}
#endif

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                ctx->unlock_time[tid] = ts;
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    path = GUEST_PTR(path, const char*);
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    { const char* td = getenv("PS3_TROPHY_DIR");   /* mpnet kit: test runs keep their own trophy file */
      if (td && *td) { snprintf(s_storage_path, sizeof(s_storage_path), "%s", td);
                       printf("[sceNpTrophy] storage %s (PS3_TROPHY_DIR)\n", s_storage_path); } }
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
    }

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId_h;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId_h->data, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!g_ps3_guest_caller) return SCE_NP_TROPHY_ERROR_UNKNOWN;

    /* Load any previously saved trophy data */
    np_trophy_lock();
    trophy_load(&s_contexts[context]);
    s_contexts[context].registered = 1;
    /* host/trophies: the real set from the game files (none: everything as before) */
    {
        TrophyContext* c = &s_contexts[context];
        char full[32];
        snprintf(full, sizeof full, "%.9s_%02u", c->commId.data, (unsigned)c->commId.num);
        const int n = np_trophy_data_load(full, s_storage_path);
        if (n > 0) {
            c->has_data = 1;
            c->total_trophies = (u32)n;
            u32 have = 0;
            for (int i = 0; i < n; i++) have += c->unlocked[i] ? 1u : 0u;
            printf("[trophy] %u of %d unlocked (%s)\n", have, n, full);
            if (trophy_platinum_check(c) >= 0) trophy_save(c);   /* older saves that already had every other trophy */
        }
    }
    np_trophy_unlock();
#if defined(NP_TROPHY_TEST) && NP_TROPHY_TEST && defined(_WIN32)
    { const char* e = getenv("TROPHY_TEST_UNLOCK");
      if (e && *e) { HANDLE th = CreateThread(NULL, 0, trophy_test_thread, (LPVOID)(intptr_t)context, 0, NULL);
                     printf("[trophy] TEST build: TROPHY_TEST_UNLOCK=%s %s\n", e, th ? "armed" : "FAILED");
                     if (th) CloseHandle(th); } }
#endif

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, "
           "arg=0x%08X) -> loaded saved data\n", context, handle, statusCb, arg);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);
    if (!s_trophy_initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;
    const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                               0, 0, arg, 0, 0, 0};
    if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    reqSpace = GUEST_PTR(reqSpace, u64*);

    /* Typical trophy pack size. The guest is big-endian and reads this u64
     * back in BE order, so the value MUST be byte-swapped on write -- otherwise
     * a host-LE 0x100000 is read as 0x0010000000000000 (2^44). LBP feeds this
     * "required disk space" straight into its save-data free-space accounting
     * (sub_379EC0: errB = budget - usedKB - reqSpaceKB); a garbage 2^44 makes
     * errB underflow negative -> cellGame GameData check returns ERROR-B ->
     * boot bails. (Same LE-write class as the cellUserInfo fix.) */
    *reqSpace = ps3_bswap64((u64)(1024 * 1024)); /* 1 MB, big-endian */
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    details = GUEST_PTR(details, SceNpTrophyGameDetails*);
    data = GUEST_PTR(data, SceNpTrophyGameData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        details->numTrophies = 32; /* default trophy set */
        details->numPlatinum = 1;
        details->numGold     = 2;
        details->numSilver   = 8;
        details->numBronze   = 21;
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        data->unlockedTrophies = trophy_count_unlocked(&s_contexts[context]);
        /* Count by grade would require per-trophy grade data;
         * return conservative estimates */
        data->unlockedPlatinum = 0;
        data->unlockedGold     = 0;
        data->unlockedSilver   = 0;
        data->unlockedBronze   = data->unlockedTrophies;
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    details = GUEST_PTR(details, SceNpTrophyDetails*);
    data = GUEST_PTR(data, SceNpTrophyData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        details->trophyId = (u32)trophyId;
        details->trophyGrade = SCE_NP_TROPHY_GRADE_BRONZE;
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        data->trophyId = (u32)trophyId;
        data->unlocked = s_contexts[context].unlocked[trophyId];
        data->timestamp = s_contexts[context].unlock_time[trophyId];
    }

    return CELL_OK;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies) {
        if (trophy_trace()) printf("[trophy] trace UnlockTrophy(%d) -> invalid id\n", trophyId);
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    }

    if (s_contexts[context].has_data && trophyId == np_trophy_data_platinum()) {   /* as on the PS3 */
        if (trophy_trace()) printf("[trophy] trace UnlockTrophy(%d) -> the platinum cannot be unlocked\n", trophyId);
        return SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK;
    }

    np_trophy_lock();
    if (s_contexts[context].unlocked[trophyId]) {
        np_trophy_unlock();
        if (trophy_trace()) printf("[trophy] trace UnlockTrophy(%d) -> already unlocked\n", trophyId);
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;
    }

    /* Unlock the trophy */
    s_contexts[context].unlocked[trophyId] = 1;
    s_contexts[context].unlock_time[trophyId] = (u64)time(NULL);

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, s_contexts[context].commId.data);
    printf("************************************************************\n");

    /* host/trophies: the popup, then the platinum when this was the last one it needs */
    np_trophy_event_push(trophyId, s_contexts[context].unlock_time[trophyId]);
    const s32 plat = trophy_platinum_check(&s_contexts[context]);

    /* Save immediately */
    trophy_save(&s_contexts[context]);
    np_trophy_unlock();

    if (platinumId)
        vm_write32((u32)(uintptr_t)platinumId, (u32)(plat >= 0 ? plat : SCE_NP_TROPHY_INVALID_TROPHY_ID));
    if (trophy_trace()) printf("[trophy] trace UnlockTrophy(%d) -> OK, platinum %d\n", trophyId, plat);

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 flags_ea = (u32)(uintptr_t)flags;
    for (u32 o = 0; o < sizeof(SceNpTrophyFlagArray); o += 4)
        vm_write32(flags_ea + o, 0);

    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            {
            u32 word_ea = flags_ea + (i / 32) * 4;
            vm_write32(word_ea, vm_read32(word_ea) | (1u << (i % 32)));
        }
    }

    vm_write32((u32)(uintptr_t)count, (u32)s_contexts[context].total_trophies);
    if (trophy_trace())
        printf("[trophy] trace GetTrophyUnlockState -> count %u, flags %08X %08X %08X %08X\n",
               s_contexts[context].total_trophies, vm_read32(flags_ea), vm_read32(flags_ea + 4),
               vm_read32(flags_ea + 8), vm_read32(flags_ea + 12));
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, pct);
    return CELL_OK;
}
