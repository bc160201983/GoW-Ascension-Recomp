/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "rsx_live_draw.h"
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "../../include/ps3emu/guest_call.h" /* g_ps3_guest_caller for direct handler dispatch */

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_FLIP_STATUS_DONE;

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static volatile u32 s_flip_request_count = 0;

u32 cellGcm_flip_request_count(void)
{
    return s_flip_request_count;
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;
/* Set by the flip (guest thread, at a get==put frame boundary); consumed by
 * the ticker, which presents the accumulated batch BEFORE draining further --
 * presenting on a raw flip-count change raced the drain and showed empty or
 * mixed batches (wave: black flashes, layout flicker). */
static volatile int s_flip_pending = 0;
/* A flip _cellGcmSetFlipCommand queued in the FIFO: | bufferId, bit 8 = ours. */
#define GCM_FLIP_MARKER 0xFEAD0100u
/* ponytail: fixed headroom, far above one frame's commands (Simpsons ~10 KB);
 * derive it from the observed per-frame volume if a title outgrows it. */
#define GCM_FLIP_WRAP_BYTES 0x40000u

/* Set by the host (boot_main) to present a frame. With it, a flip queued in
 * the command stream presents right where the drain reaches it -- the frame
 * is complete there, and the next one has not started -- instead of raising
 * s_flip_pending for the ticker. The ticker checks the flag at its own pace,
 * so two flips between two checks showed one frame (God of War: Ascension:
 * ~25% of its frames never reached the screen), and presenting again on the
 * count change showed others twice. PS3_PRESENT_AT_FLIP=0: the ticker way. */
void (*g_gcm_flip_present_hook)(void) = 0;
static int gcm_present_at_flip(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3_PRESENT_AT_FLIP"); on = !(e && *e == '0'); }
    return on && g_gcm_flip_present_hook;
}

int cellGcm_take_flip_pending(void)
{
    int v = s_flip_pending;
    s_flip_pending = 0;
    return v;
}

/* The flip now names this buffer. The shared draw engine presents on the
 * host's call and only learns a buffer from 0xE944 by itself, so it would
 * keep presenting display buffer 0 for a title that flips through 0xFEAD. */
static void gcm_set_current_display_buffer(u32 bufferId)
{
    s_current_display_buffer_id = bufferId;
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_note_flip(bufferId);
}


/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage
 *
 * set_offset_table/clear_offset_table keep these two host arrays in step with
 * the IO mappings, and that part was always fine. What was not: the tables
 * cellGcmGetOffsetTable hands out are indexed BY THE GUEST --
 * ioAddress[ea >> 20] and eaAddress[io >> 20] -- and it handed over 64-bit host
 * pointers through a struct whose guest form is two 4-byte fields, so the title
 * got two truncated addresses pointing at nothing it could read.
 *
 * The guest-visible copy lives in the reserved GCM window alongside the label
 * and control blocks, published on each call. 4096 pages covers the 4 GiB
 * address space at 1 MiB granularity, 8 KiB a side, and this is not a hot path.
 * 0xFFFF means "not mapped", as it does in the host tables. */
/* Default home for the HLE-visible GCM window; a port overrides it before
 * running guest code when its title claims this address range. */
uint32_t ppu_hle_inject_base = 0x20000000u;

static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];
static u16 s_io_last_ea[65536];   /* per IO page: the ea page it last mapped + 1 (0 = never) */
volatile u32 g_gcm_io_remap_gen;  /* bumped when an IO page comes back for other main memory */
/* Mapping and unmapping (the IO page tables, the mapping list, gcm_io_alloc):
 * one thread at a time, as libgcm's own gcmio lock does. */
static volatile long s_io_lock;
static void gcm_io_lock(void)   { while (__atomic_exchange_n(&s_io_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void gcm_io_unlock(void) { __atomic_store_n(&s_io_lock, 0, __ATOMIC_RELEASE); }

#define GCM_OFFSET_TABLE_PAGES   4096u                 /* 4 GiB / 1 MiB       */
#define GCM_OFFSET_TABLE_IO_ADDR (VM_HLE_INJECT_BASE + 0x3000u)           /* ea >> 20  -> io>>20 */
#define GCM_OFFSET_TABLE_EA_ADDR (VM_HLE_INJECT_BASE + 0x5000u)           /* io >> 20  -> ea>>20 */

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */

/* Handler mask exported for the RSX event delivery path. Bit layout matches
 * the RPCS3 SYS_RSX_EVENT enum (vblank=0x02, flip=0x04, user_cmd=0x80).
 * The runner's vblank tick reads this to decide which events to send to
 * the libgcm interrupt thread. Updated by Set*Handler below.
 *
 * If g_gcm_driver_info_ea is non-zero, the mask is also written to
 * driver_info + 0x12C0 so the game's own libgcm interrupt path can read it
 * directly from guest memory. The runner sets g_gcm_driver_info_ea during
 * RSX context allocation. */
uint32_t g_gcm_handler_mask = 0;
uint32_t g_gcm_driver_info_ea = 0;

static u32 s_flip_handler_opd     = 0;

/* GCM_FLIPCB_ONTICK=1: fire the guest flip handler only when the flip
 * completes (cellGcmTickFlip), not also when it is requested. */
static int gcm_flipcb_on_tick_only(void)
{
    static int v = -1;
    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;
    return v;
}

static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static u32 s_flip_handler_code    = 0;
static u32 s_vblank_handler_code  = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    if (!getenv("YDKJ_HANDLERFIX") || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(_n < 6 && ++_n) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}
static void gcm_update_handler_mask(void)
{
    uint32_t m = 0;
    if (s_vblank_handler_opd) m |= 0x02;
    if (s_flip_handler_opd)   m |= 0x04;
    if (s_user_handler_opd)   m |= 0x80;
    g_gcm_handler_mask = m;
    if (g_gcm_driver_info_ea)
        vm_write32(g_gcm_driver_info_ea + 0x12C0, m);
}

/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Flip timing */
static u64 s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static u32 s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
#ifdef _WIN32
static volatile LONG s_user_command = 0;
#define GCM_USER_STORE(cmd) InterlockedExchange(&s_user_command, (LONG)(cmd))
/* Pair the pending marker with its cause so a later producer cannot
 * overwrite a cause already claimed by the callback pump. */
static volatile LONG64 s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) InterlockedExchange64(&s_user_pending, (LONG64)(value))
#define GCM_USER_PENDING_TAKE() ((u64)InterlockedExchange64(&s_user_pending, 0))
#else
#include <stdatomic.h>
static atomic_uint s_user_command = 0;
#define GCM_USER_STORE(cmd) atomic_store(&s_user_command, (cmd))
static _Atomic(u64) s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) atomic_store(&s_user_pending, (value))
#define GCM_USER_PENDING_TAKE() atomic_exchange(&s_user_pending, 0)
#endif

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* NV4097_SET_CONTEXT_DMA_REPORT (0x1A8): where NV4097_GET_REPORT writes.
 * Local (0xFEED0000, the default): the s_report_data slots above, read back
 * through cellGcmGetReport* / cellGcmGetReportDataAddress. Main memory
 * (0xFEED0001, or CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN 0xBAD68000): the
 * report's 24-bit offset is an IO offset, and the 16-byte report goes to
 * guest memory at gcm_io2ea(offset), where the title reads it itself.
 * Killzone 3 sets 0xFEED0001 and keeps its reports in its own frame data. */
static u32 s_report_dma = 0xFEED0000u;
static u64 s_report_last_ts;

/* GET_REPORT timestamp in nanoseconds, strictly rising: two reports in one
 * walker pass must not share a time stamp or go backwards. */
static u64 gcm_report_timestamp(void)
{
    u64 t = get_timestamp_ns();
    if (t <= s_report_last_ts) t = s_report_last_ts + 1u;
    s_report_last_ts = t;
    return t;
}

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  (VM_HLE_INJECT_BASE + 0x0000u)
#define GCM_LABEL_STRIDE      0x10u
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR (VM_HLE_INJECT_BASE + 0x2000u)

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* The guest variable cellGcmInit's ctx_out pointed at -- the title's own
 * gCellGcmCurrentContext. A title that switches to a command buffer of its own
 * does it by repointing this, and until something reads it back the runtime has
 * no way to notice. GCM_CTXDBG=1 reports it. */
static u32 s_gcm_ctx_out_ea = 0;

/* Set by cellGcm_syscall_bringup: this run reaches RSX through the lv2 sys_rsx_*
 * syscalls (libs/video/sys_rsx.c) rather than through the HLE cellGcm* imports. */
static int s_gcm_syscall_mode = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Set once the title has asked for the offset tables (cellGcmGetOffsetTable):
 * from then on every mapping change is written through to the guest copy it
 * holds a pointer to. The copy used to be refreshed only by that call, so a
 * title that took the pointer at boot and mapped afterwards read 0xFFFF for
 * its new pages: God of War: Ascension's renderer computed io offset -1 for
 * the main memory it had just mapped ("iooffset == -1"), and its later unmap
 * failed. */
static int s_offset_tables_published = 0;
static void offset_table_write_through(u32 ea_page, u32 io_page)
{
    if (!s_offset_tables_published) return;
    if (ea_page < GCM_OFFSET_TABLE_PAGES)
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + ea_page * 2, s_io_address_table[ea_page]);
    if (io_page < GCM_OFFSET_TABLE_PAGES)
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + io_page * 2, s_ea_address_table[io_page]);
}

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    /* Round UP: a mapping is described by whole 1MB table entries, so a size
     * that is not a multiple of 1MB still needs its final partial page mapped.
     * Truncating dropped that page -- and dropped the mapping ENTIRELY for any
     * size below 1MB (pages == 0) -- after which cellGcmResolveOffset finds no
     * IO entry and falls back to VRAM, where the guest never wrote anything. */
    u32 pages = (size + 0xFFFFFu) >> 20;  /* number of 1MB pages, rounded up */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    /* IO pages mapped again: anything keyed by (main memory, offset) -- the
     * renderer's texture cache -- looks at its content once more
     * (g_gcm_io_remap_gen), whether a page now maps other memory or the same.
     * God of War: Ascension unmaps an area's memory and maps the same
     * addresses again for the next one (gcm_io_alloc keeps its io = ea -
     * ioAddress pairing, so they come back at the same offsets). Before 0.1.14
     * every map got new offsets and every texture there was decoded fresh;
     * now a cached one relies on the write watch and LD_TEX_AUDIT, which have
     * not been seen to miss a change (tester log, 2026-11-03), so this is a
     * cheap safety net, one content check per texture per map call. */
    u32 again = 0, other = 0;
    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
        offset_table_write_through(ea_page + i, io_page + i);
        const u16 was = s_io_last_ea[io_page + i];   /* ea page + 1; 0 = never mapped */
        if (was) { again++; if (was != (u16)(ea_page + i + 1)) other++; }
        s_io_last_ea[io_page + i] = (u16)(ea_page + i + 1);
    }
    if (again) {
        g_gcm_io_remap_gen++;
        static int said;
        if (said++ < 32)
            printf("[cellGcmSys] io 0x%08X..0x%08X mapped again (%u pages, %u to other memory): "
                   "main-memory textures re-checked\n", io, io + (pages << 20) - 1u, again, other);
    }
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
        offset_table_write_through(ea_page + i, io_page + i);
    }
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Valid tiled pitch sizes (power-of-two aligned, common RSX values) */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = 0xFEED0000u;
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_user_handler = NULL;
    s_second_v_handler = NULL;
    s_last_flip_time = get_timestamp_ns();
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    /* Set up the initial IO mapping for the command buffer region */
    if (ioAddress != 0 && ioSize > 0) {
        populate_offset_table(ioAddress, 0, ioSize);

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }

    /* Zero the guest-visible control register (put/get/ref). */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, 0);

    /* Watch SPU DMA for pushbuffers. A title that builds its command stream on
     * an SPU never tells libgcm where that stream is, so the write is the only
     * evidence of it -- see gcm_note_spu_put. Costs one NULL check per SPU DMA
     * for titles that do not. */
    { extern void (*g_spu_put_hook)(u32 ea, u32 size);
      void gcm_note_spu_put(u32 ea, u32 size);       /* defined with the walker */
      void gcm_forget_spu_blocks(void);
      gcm_forget_spu_blocks();
      g_spu_put_hook = gcm_note_spu_put; }
    /* And which of those PUTs land in RSX-visible memory, to be written with
     * the word the walker waits on last (GCM_PUT_ORDER, spu_dma.h). */
    { extern const u16* volatile g_gcm_ea_io_tab;
      g_gcm_ea_io_tab = s_io_address_table; }

    s_gcm_initialized = 1;
    /* EDGE6 (ascension edge6.c): its extra IO mapping, made here on the
     * thread that initialises GCM, before the title maps anything else. */
    { extern void (*g_gcm_init_hook)(void); if (g_gcm_init_hook) g_gcm_init_hook(); }
    return CELL_OK;
}

/* EDGE6 (ascension edge6.c): map [ea, ea+size) at a fixed IO window just past
 * the 256 MB IO space (no local offset shares its pages, so the table-first
 * cellGcmResolveOffset finds it too), which gcm_io_alloc never hands out
 * (the title only learns IO offsets from MapMainMemory). Returns the IO offset
 * or 0xFFFFFFFF. Call it before the title's own mappings (g_gcm_init_hook). */
void (*g_gcm_init_hook)(void) = 0;
#define GCM_EXTRA_IO_BASE 0x10000000u
static u32 s_gcm_extra_io_size;
u32 gcm_map_extra(u32 ea, u32 size)
{
    size = (size + 0xFFFFFu) & ~0xFFFFFu;
    if (s_gcm_extra_io_size || !size || size > 0x01000000u || (ea & 0xFFFFFu)) return 0xFFFFFFFFu;
    IoMapping* m = find_free_mapping();
    if (!m) return 0xFFFFFFFFu;
    m->ea = ea; m->io = GCM_EXTRA_IO_BASE; m->size = size; m->active = 1;
    s_io_mapping_count++;
    populate_offset_table(ea, GCM_EXTRA_IO_BASE, size);
    s_gcm_extra_io_size = size;
    printf("[cellGcmSys] extra IO mapping (EDGE6): ea=0x%08X io=0x%08X size=0x%X\n", ea, GCM_EXTRA_IO_BASE, size);
    return GCM_EXTRA_IO_BASE;
}

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush. The command buffer is the start of the game's IO region. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    if (cmdSize < 0x10000)
        cmdSize = 0x10000;

    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    cellGcmInit(cmdSize, ioSize, ioAddress);

    u32 cmdbuf = ioAddress;                 /* FIFO lives at the IO region start */
    u32 cdata  = galloc ? galloc(16, 16) : 0;
    if (cdata && gwrite32) {
        gwrite32(cdata + 0x0, cmdbuf);              /* begin   */
        gwrite32(cdata + 0x4, cmdbuf + cmdSize);    /* end     */
        gwrite32(cdata + 0x8, cmdbuf);              /* current */
        /* Command-buffer-full callback: a guest OPD {func, toc} routed to
         * hle_gcm_callback -> cellGcm_fifo_recycle. Without a real callback the
         * ring never recycles and the FIFO wedges once `current` reaches `end`. */
        u32 opd = galloc ? galloc(8, 8) : 0;
        if (opd) {
            gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
            gwrite32(opd + 0x4, 0);                              /* toc (unused) */
            gwrite32(cdata + 0xC, opd);                          /* callback OPD */
        } else {
            gwrite32(cdata + 0xC, 0);
        }
        if (ctx_out_addr)
            gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
        s_gcm_ctx_out_ea = ctx_out_addr;   /* the title's gCellGcmCurrentContext */
        s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    }
    return cdata;
}

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_GCM_ERROR_INVALID_VALUE;

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void)
{
    /* Block until the pending flip completes (the present thread marks it done
     * via cellGcmTickFlip, ~once per display refresh). This throttles the title
     * to vsync; without it the guest loops unthrottled and many frames batch
     * into a single host present -> the console text stacks/duplicates. */
#ifdef _WIN32
    __declspec(dllimport) void __stdcall Sleep(unsigned long);
#endif  /* elsewhere: runtime/platform/win32_compat.h */
    for (int i = 0; i < 64 && s_flip_status == CELL_GCM_FLIP_STATUS_WAITING; i++)
        Sleep(1);
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
}

/* NID: 0xDF6476BD. The same wait without libgcm's command-buffer space check;
 * this HLE writes no command, so it is the same call. GH3 makes it ~36 times
 * a boot and got a faked CELL_OK (no wait at all) before this was registered. */
void cellGcmSetWaitFlipUnsafe(void) { cellGcmSetWaitFlip(); }

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    { static int n = 0;
      if (getenv("FLIP_DBG") && n++ < 12)
          fprintf(stderr, "[FLIP] ResetFlipStatus%c", 10); }
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
}

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; cellGcmTickFlip (the ticker's 60Hz vblank
     * beat) completes pending flips. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    { static int n = 0;
      if (getenv("FLIP_DBG") && n++ < 24)
          fprintf(stderr, "[FLIP] GetFlipStatus -> %u (0=DONE 1=WAITING)%c",
                  s_flip_status, 10); }
    return s_flip_status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip/user handler delivery.
 *
 * Tick producers publish pending work. The runner pumps it at HLE boundaries
 * or on a dedicated callback thread. The pump serializes guest handlers with
 * one another; it does not stop unrelated guest execution on other threads.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
#define GCM_PENDING_SET(bits)  InterlockedOr(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
static volatile LONG s_gcm_pumping = 0;
#define GCM_PUMP_TRY_ENTER() (InterlockedCompareExchange(&s_gcm_pumping, 1, 0) == 0)
#define GCM_PUMP_LEAVE()     InterlockedExchange(&s_gcm_pumping, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  atomic_fetch_or(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
static atomic_flag s_gcm_pumping = ATOMIC_FLAG_INIT;
#define GCM_PUMP_TRY_ENTER() (!atomic_flag_test_and_set_explicit(&s_gcm_pumping, memory_order_acquire))
#define GCM_PUMP_LEAVE()     atomic_flag_clear_explicit(&s_gcm_pumping, memory_order_release)
#endif

/* Called by the vblank ticker thread. NO guest code -- advance the vblank count
 * and mark a vblank+flip tick pending for the main thread to deliver. */
static u32 s_fifo_getoff;   /* tentative: defined below */
/* The park the walker last stopped on (GCM_PARK_SPU_HOLD_MS), ~0 = none. */
static u32 s_last_park_off = ~0u;
/* Every place the walker has parked. The title reuses the same few park
 * words frame after frame; when its CPU side is quick enough to patch one
 * before the walker gets there, the walker never parks on it, and the hold
 * has to recognise it by its address. */
#define GCM_PARK_SITES 16
static u32 s_park_sites[GCM_PARK_SITES]; static unsigned s_park_sites_n;
static int gcm_is_park_site(u32 off)
{
    for (unsigned i = 0; i < s_park_sites_n; i++) if (s_park_sites[i] == off) return 1;
    return 0;
}

void cellGcm_request_tick(void)
{
    s_vblank_count++;
    /* GCM_FLIP_NEEDS_FIFO=1: only mark a flip pending once the walker has
     * actually consumed up to `put`.
     *
     * A title's fifo-finish can use flip status as a fast path -- Twisted
     * Metal's reads it first and, if the flip is done, skips waiting for `get`
     * entirely. Completing a flip on the 60 Hz beat regardless of FIFO
     * progress therefore tells it the RSX has caught up when it has not, and
     * it recycles a FIFO block the walker is still inside, overwriting the
     * JUMP out of it. Off by default: a title whose FIFO never drains would
     * stop flipping entirely. */
    { static int need = -1;
      if (need < 0) { const char* e = getenv("GCM_FLIP_NEEDS_FIFO"); need = e ? atoi(e) : 0; }
      if (need) {
          u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
          GCM_PENDING_SET(s_fifo_getoff == put ? 3 : 1);
          return;
      } }
    GCM_PENDING_SET(3);
}

/* Deliver pending handlers on the calling host thread. Serialize the entire
 * claim-and-deliver sequence across threads: an HLE boundary and a host ticker
 * may both pump, and newer causes must not overtake already claimed callbacks.
 * A nonblocking guard also prevents nested callbacks from reentering. Leave
 * pending notifications untouched when another pump is active. */
void ppu_gcm_pump(void)
{
    if (!GCM_PUMP_TRY_ENTER()) return;
    long p = (long)GCM_PENDING_TAKE();
    u64 user = GCM_USER_PENDING_TAKE();
    /* GCM_PUMP_DBG=1: how often each guest handler is actually delivered. */
    { static int d = -1; static unsigned nv, nf, calls;
      if (d < 0) d = getenv("GCM_PUMP_DBG") ? 1 : 0;
      if (d) { calls++; if (p & 1) nv++; if (p & 2) nf++;
               if ((calls % 20000) == 0)
                   fprintf(stderr, "[gcm-pump] calls=%u vblank=%u flip=%u vcount=%u%c",
                           calls, nv, nf, s_vblank_count, 10); } }
    if ((p & 1) && s_vblank_handler_opd && g_ps3_guest_caller) {
        /* One call per vblank, not one per pump: the pending bit folds every
         * vblank since the last pump into one, and the pump only runs when a
         * guest thread passes through it. A handler that counts its calls
         * then loses time -- God of War: Ascension's movie clock ran at ~75%
         * while its main thread spun on SPU job counters, and the cinematic
         * fell behind its audio. A backlog past 8 (a long stall) is cut short
         * rather than replayed as a burst. GCM_VBLANK_COALESCE=1: the old way. */
        static int s_coalesce = -1;
        if (s_coalesce < 0) s_coalesce = getenv("GCM_VBLANK_COALESCE") ? 1 : 0;
        static u32 s_vblank_delivered;
        const u32 now = s_vblank_count;
        if (s_coalesce || now - s_vblank_delivered > 8u || s_vblank_delivered == 0)
            s_vblank_delivered = now - 1u;
        ydkj_restore_handler_opd(s_vblank_handler_opd, s_vblank_handler_code);
        while ((s32)(now - s_vblank_delivered) > 0) {
            s_vblank_delivered++;
            g_ps3_guest_caller(s_vblank_handler_opd, (uint64_t)s_vblank_delivered,
                               0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (p & 2) {
        s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
        if (s_flip_handler_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(s_flip_handler_opd, s_flip_handler_code);
            g_ps3_guest_caller(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (user) {
        u32 cmd = (u32)user;
        if (s_user_handler_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(s_user_handler_opd, (uint64_t)cmd, 0, 0, 0, 0, 0, 0, 0);
    }
    GCM_PUMP_LEAVE();
}

/* Back-compat: the old direct entry points now just mark a tick pending (so any
 * caller other than the ticker still works) -- delivery stays on the main thread. */
void cellGcmTickVBlank(void) { s_vblank_count++; GCM_PENDING_SET(1); }
/* Only deliver a flip completion when a flip is actually outstanding.
 *
 * Ticking unconditionally at 60 Hz fires the guest's flip handler even when
 * the guest never asked for a flip. A title whose handler touches the flip
 * status then livelocks: Tokyo Jungle's main loop is
 *
 *     do { usleep(100); } while (cellGcmGetFlipStatus() != DONE);
 *     cellGcmResetFlipStatus();
 *
 * and its handler runs code that puts the status back to WAITING, so the
 * poll never saw DONE and the game never rendered another frame. On hardware
 * the callback IS the flip interrupt -- no flip, no callback. */
void cellGcmTickFlip(void)
{
    if (s_flip_status == CELL_GCM_FLIP_STATUS_WAITING)
        GCM_PENDING_SET(2);
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
static u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* RSX get pointer (IO offset) + one-deep CALL return slot. The FIFO-wrap
 * recycle path teleports these when it resets a ring without a JUMP command. */
/* Milliseconds on a monotonic clock, for the runtime's own timeouts and rate
 * logs. Off Windows this returned 0, so every interval measured with it was
 * 0 there: rate logs never printed and timeouts never expired. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static u32 s_sema_offset = 0;   /* NV406E semaphore offset (label window) */
u32 g_gcm_method_ea = 0;        /* guest EA of the method argument being dispatched */
/* Backlog past which the drain stops honouring one-flip-per-tick and
 * catches up instead. Sized well above a frame's command list so a
 * title that keeps up never sees this path. */
#define GCM_FIFO_CATCHUP_BYTES 0x10000u
/* How long a self-JUMP park may stand with `put` somewhere else before the
 * drain gives up on it being patched and follows `put` instead. */
#define GCM_PARK_GRACE_MS 50ull
/* GCM_PARK_WAIT_MS=<ms> overrides it. God of War: Ascension moves `put` to the
 * end of a frame its SPU jobs are still filling and only then patches the park
 * into a JUMP to it; recompiled, those jobs take far longer than 50 ms, and
 * following put early skips the frame, the user command that releases the
 * render thread's frame slot with it. */
static unsigned long long gcm_park_grace_ms(void)
{
    static unsigned long long ms = ~0ull;
    if (ms == ~0ull) { const char* e = getenv("GCM_PARK_WAIT_MS");
                       ms = e ? strtoull(e, NULL, 0) : GCM_PARK_GRACE_MS; }
    return ms;
}
/* How close `current` must get to `end` before the runtime will recycle a ring
 * on the title's behalf. One gcmReserve is 8 bytes; a page of slack keeps a
 * title that is merely near the end from being touched. */
#define GCM_RECYCLE_SLACK 0x1000u
/* How far past the ring's start the walker must have read before the runtime
 * will recycle on the title's behalf. The guest resumes writing at `begin`, so
 * this is the amount of already-consumed head room it gets. */
#define GCM_RECYCLE_MARGIN 0x20000u
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */
static u32 s_fifo_getoff  = 0;
static u32 s_last_ring_off = 0;   /* last offset the walker actually consumed */
static u32 s_fifo_calloff = 0;

/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * libgcm binds: sub 1/6 = NV0039 (m2mf buffer copy), sub 2 = NV3062 (context
 * surface 2D), sub 3 = NV309E (swizzled surface), sub 4 = NV308A (image from
 * CPU / inline transfer), sub 5 = NV3089 (scaled image).
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u32 cellGcmResolveLocated(int local, u32 offset);

static struct {
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
} s_gcm2d;

static struct {
    u32 src_dma, dst_surf, fmt, op, clip_pt, clip_sz, out_pt, out_sz;
    u32 ds_dx, dt_dy, in_sz, in_fmt, in_off, in_uv;
} s_nv3089;

/* NV309E "swizzled surface" -- the destination object for a scaled-image blit
 * whose target is a swizzled (Morton-ordered) texture rather than a linear
 * surface. Rubber Ducky binds it to the same subchannel this file previously
 * assumed was always NV308A (image-from-CPU), so its SET_OFFSET was being read
 * as an NV308A "point" and the blit destination was never picked up at all.
 * The two are told apart by method 0x0300: NV308A has no such method. */
static struct {
    u32 fmt;        /* 0x0300: (log2h << 24) | (log2w << 16) | format */
    u32 offset;     /* 0x0304: destination offset                     */
    int active;
    int subch;      /* which subchannel NV309E is bound to. Per-subchannel:
                     * this title has NV309E on 4 and NV308A on 5, and a global
                     * flag made 5's NV308A POINT (also 0x0304) be swallowed as
                     * an NV309E offset, breaking every inline transfer. */
} s_nv309e;

/* Execute one NV3089 blit into the currently-bound NV3062 destination surface.
 * Point-sampled: ds/dx and dt/dy are 20.12 fixed point and are 0x100000 (1.0)
 * for the straight uploads this exists for; a scaled blit still lands, just
 * without filtering. */
/* Morton/Z-order offset for an NV309E swizzled surface: interleave the low
 * min(l2w,l2h) bits of x and y, then append the remaining bits of the larger
 * dimension above them. */
static u32 nv309e_swz(u32 x, u32 y, u32 l2w, u32 l2h)
{
    u32 n = l2w < l2h ? l2w : l2h, off = 0;
    for (u32 i = 0; i < n; i++)
        off |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    if (l2w > l2h)      off |= (x >> n) << (2 * n);
    else if (l2h > l2w) off |= (y >> n) << (2 * n);
    return off;
}

/* The draw engine renders on the host GPU, so the scene a title copies out
 * with NV3089 is not in guest memory, and what it copies in never reaches the
 * engine's surfaces. Around the guest-memory copy below: write the source
 * rectangle back from any engine surface first, and push the written
 * rectangle into any engine surface after. 32-bit pixels only. */
static void nv3089_engine_read(u32 in_pitch, u32 out_w, u32 out_h)
{
    if (!rsx_draw_engine_enabled() || !out_w || !out_h) return;
    const u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 */
    const u32 sx0 = u0 >> 4, sy0 = v0 >> 4;
    const u32 sx1 = (u32)((((u64)u0 << 8) + (u64)(out_w - 1) * s_nv3089.ds_dx) >> 20);
    const u32 sy1 = (u32)((((u64)v0 << 8) + (u64)(out_h - 1) * s_nv3089.dt_dy) >> 20);
    if (sx1 < sx0 || sy1 < sy0) return;
    const u32 loc = (s_nv3089.src_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_read(loc, s_nv3089.in_off + sy0 * in_pitch + sx0 * 4u, in_pitch,
                               sx1 - sx0 + 1u, sy1 - sy0 + 1u);
}

static void nv3089_engine_wrote(u32 dst_pitch, u32 x0, u32 y0, u32 x1, u32 y1)
{
    if (!rsx_draw_engine_enabled() || x1 <= x0 || y1 <= y0) return;
    const u32 loc = (s_gcm2d.dst_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_wrote(loc, s_gcm2d.dst_offset + y0 * dst_pitch + x0 * 4u, dst_pitch,
                                x1 - x0, y1 - y0);
}

/* Would a transfer touching [offset, offset + end) of a LOCAL surface run past
 * the end of local memory? Then it is dropped, and counted.
 *
 * Real RSX faults on such a transfer. Here every offset resolves into the 4 GB
 * guest window, which is committed on demand, so it silently wrote main
 * memory. God of War: Ascension's VRAM compaction (func_00374A70,
 * CopyLocalMemory) meets a wrapped negative size, prints "invalid size", and
 * asserts with a trap the lifter ignores, so it goes on blitting chunk after
 * chunk through 4 GB: thread stacks, the FIFO, and function descriptors, and
 * the run dies in "bctr to NULL". GCM_BLIT_GUARD=0 lets them through. */
static int gcm_local_extent_bad(int local, u32 offset, u64 end, const char* what)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_BLIT_GUARD"); on = !(e && *e == '0'); }
    if (!on || !local || (u64)offset + end <= (u64)s_config.localSize) return 0;
    static long n;
    const long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k <= 8 || (k & (k - 1)) == 0)
        printf("[GCM2D] %s past local memory dropped (#%ld): offset 0x%08X + 0x%llX > 0x%08X%c",
               what, k, offset, (unsigned long long)end, s_config.localSize, 10);
    return 1;
}

static void nv3089_blit(void)
{
    u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 out_x = s_nv3089.out_pt & 0xFFFF, out_y = s_nv3089.out_pt >> 16;
    u32 in_pitch = s_nv3089.in_fmt & 0xFFFF;
    u32 dst_pitch = s_gcm2d.pitch >> 16;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    /* NV3089_GATE=<file>: log every blit's geometry while <file> exists, so a
     * capture covers the scene of interest instead of the first N since boot. */
    { static const char* gate = (const char*)-1; static unsigned poll; static int open_gate;
      if (gate == (const char*)-1) gate = getenv("NV3089_GATE");
      if (gate) {
          if ((poll++ & 63u) == 0) { FILE* g = fopen(gate, "rb"); open_gate = g != NULL; if (g) fclose(g); }
          if (open_gate) {
              int _sl = (s_nv3089.src_dma != 0xFEED0001u), _dl = (s_gcm2d.dst_dma != 0xFEED0001u);
              printf("[NV3089G] fmt=0x%X out=%ux%u at %u,%u in=%ux%u uv=0x%08X ds=0x%X dt=0x%X src=0x%08X/%u dst=0x%08X/%u swz=%d op=%u in_fmt=0x%08X%c",
                     s_nv3089.fmt & 0xFF, out_w, out_h, out_x, out_y,
                     s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, s_nv3089.in_uv,
                     s_nv3089.ds_dx, s_nv3089.dt_dy,
                     cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch,
                     cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch,
                     s_nv309e.active, s_nv3089.op, s_nv3089.in_fmt, 10);
          }
      } }
    int gpu_copied = 0;   /* the GPU copied it into a live VRAM surface (LD_BLIT_GUEST_SKIP) */
    /* A copy out of a surface the live renderer drew exists only on the GPU;
     * mirror 1:1 blits there too (the guest-memory copy below still runs). */
    if (s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        rsx_live_draw_enabled()) {
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int sl = (s_nv3089.src_dma != 0xFEED0001u);
        const int dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
        const int gr =
        rsx_live_draw_blit(sl ? 0u : 1u, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                           dl ? 0u : 1u, s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                         + (s_nv3089.out_pt & 0xFFFF) * 4u, dst_pitch,
                           out_w, out_h);
        gpu_copied = dl && gr == 1;
    } else if (rsx_live_draw_enabled()) {
        /* Scaled: a resolve. Copy it on the GPU into the display buffer; fall
         * back to presenting the source when that cannot be done. */
        const u32 sl = s_nv3089.src_dma != 0xFEED0001u ? 0u : 1u;
        const u32 dl = s_gcm2d.dst_dma  != 0xFEED0001u ? 0u : 1u;
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int rok = rsx_live_draw_resolve_blit(sl, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                                        s_nv3089.ds_dx, s_nv3089.dt_dy, dl, s_gcm2d.dst_offset,
                                        dst_pitch, out_x, out_y, out_w, out_h);
        if (!rok && s_nv3089.in_uv <= 0x00100010u && !s_nv3089.out_pt)
            rsx_live_draw_note_resolve(sl, s_nv3089.in_off, dl, s_gcm2d.dst_offset);
    }
    /* LD_BLIT_GUEST_SKIP=1: a 1:1 blit the GPU copied between live surfaces
     * (source drawn on the GPU, destination a live VRAM surface) skips the
     * guest-memory copy: its source bytes in guest memory are not the picture,
     * and the renderer reads the destination from the GPU surface. Copies into
     * main memory (for the CPU or SPUs) and every other blit keep it.
     * NV3089_CPU_SKIP_AB=1: with LD_BLIT_GUEST_SKIP=1 that skip, else (a
     * ceiling test, picture may be wrong) every guest copy, in every other
     * [draws] window only. */
    { extern volatile int g_ld_blit_cpu_on; extern int ld_blit_ab(void);
      static int gs = -1; static volatile long long n_skip, n_keep;
      if (gs < 0) { const char* e = getenv("LD_BLIT_GUEST_SKIP"); gs = e && *e == '1'; }
      const int ab_off = ld_blit_ab() && !g_ld_blit_cpu_on;
      if (gs) {
          if (gpu_copied && (!ld_blit_ab() || ab_off)) {
              if ((++n_skip & 1023) == 0) fprintf(stderr, "[blit-skip] %lld guest copies skipped, %lld kept (A/B)%c", n_skip, n_keep, 10);
              return;
          }
          if (gpu_copied) n_keep++;
      } else if (ab_off && rsx_live_draw_enabled()) return; }
    /* Only the 32-bit colour formats are handled; anything else would need a
     * per-format converter and is better skipped loudly than written wrong. */
    u32 f = s_nv3089.fmt & 0xFF;
    /* NOTE: format 0x3 appears here as a downsample chain (1024x512 -> 1024x256
     * -> 512x128 -> 256x64 -> 128x32, all into one destination) that builds a
     * mip/blur pyramid, and skipping it leaves that texture empty. Treating it
     * as 4-byte A8R8G8B8 like 7/8/0xD is WRONG -- it floods the scene blue
     * (flat-blue coverage 17k -> 537k pixels), so its pixel layout differs.
     * Decode it properly before enabling; do not just add it to this list. */
    /* NV309E swizzled destination. Colour format 0x3 is A8R8G8B8 in the
     * scaled-image enum; this title uses it to build a mip/blur pyramid into a
     * swizzled texture (1024x512 -> 1024x256 -> ... all through NV309E).
     * Writing it LINEARLY into the NV3062 destination is what flooded the scene
     * blue -- wrong destination and wrong layout. */
    { u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
      static int en = -1;
      /* On by default: this is the mip/blur pyramid. Without it textures have
       * no mip levels, which shows up as aliased blocky tiles and a noise patch
       * where a minified surface should be, and the shell that samples the
       * pyramid draws untextured. NV309E_BLIT=0 disables. */
      if (en < 0) { const char* e = getenv("NV309E_BLIT"); en = e ? atoi(e) : 1; }
      if (en && f == 3 && s_nv309e.active && s_nv309e.offset &&
          (1u << l2w) == out_w && (1u << l2h) == out_h) {
        int sl = (s_nv3089.src_dma != 0xFEED0001u);
        u32 src2 = cellGcmResolveLocated(sl, s_nv3089.in_off);
        u32 dst2 = cellGcmResolveLocated(1, s_nv309e.offset);
        u32 u0b = s_nv3089.in_uv & 0xFFFF, v0b = s_nv3089.in_uv >> 16;
        { const u64 sv_last = (((u64)v0b << 8) + (u64)(out_h - 1) * s_nv3089.dt_dy) >> 20;
          const u64 su_last = (((u64)u0b << 8) + (u64)(out_w - 1) * s_nv3089.ds_dx) >> 20;
          if (gcm_local_extent_bad(sl, s_nv3089.in_off, sv_last * in_pitch + su_last * 4 + 4, "NV309E source") ||
              gcm_local_extent_bad(1, s_nv309e.offset, (u64)out_w * out_h * 4, "NV309E destination"))
              return; }
        nv3089_engine_read(in_pitch, out_w, out_h);
        /* Copy words verbatim through raw pointers rather than vm_read32/
         * vm_write32: this moves ~500k pixels per blit and several blits run per
         * frame, and the two byte swaps would cancel anyway. */
        { extern u8* vm_base;
          const u8* sbase = vm_base + src2;
          u8* dbase = vm_base + dst2;
          for (u32 y = 0; y < out_h; y++) {
              u64 sv = ((u64)v0b << 8) + (u64)y * s_nv3089.dt_dy;
              const u8* srow = sbase + (u32)(sv >> 20) * in_pitch;
              for (u32 x = 0; x < out_w; x++) {
                  u64 su = ((u64)u0b << 8) + (u64)x * s_nv3089.ds_dx;
                  memcpy(dbase + (u64)nv309e_swz(x, y, l2w, l2h) * 4,
                         srow + (u32)(su >> 20) * 4, 4);
              }
          } }
        { static int _n = 0; static int cap = -1;
          if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && _n < cap) { _n++;
            printf("[NV3089] swizzled %ux%u fmt=0x%X src=0x%08X -> NV309E dst=0x%08X%c",
                   out_w, out_h, f, src2, dst2, 10); } }
        return;
      } }
    /* 0x3 is A8R8G8B8 in the scaled-image format enum. This title uses it for
     * two different things: the swizzled mip/blur pyramid handled above, and
     * (with a LINEAR NV3062 destination) copies of the rendered scene surface
     * into the textures its water and effects sample -- e.g.
     * src=0xCE340000 (the scene) -> dst=0xC3746200, which is the single most
     * sampled blended texture in the frame. Skipping those left every one of
     * those textures empty. Same 4-byte pixel layout as the formats already
     * handled. */
    if (f != 3 /* A8R8G8B8 (scale-format enum) */ &&
        f != 7 /* A8R8G8B8 */ && f != 8 /* X8R8G8B8 */ && f != 0xD /* A8B8G8R8 */) {
        /* Log the GEOMETRY of a skipped blit, not just the format: a full-screen
         * copy from the scene surface to a registered display buffer is the
         * composite this title needs, and is worth telling apart from some
         * incidental small transfer. */
        { int _sl = (s_nv3089.src_dma != 0xFEED0001u);
          int _dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
          printf("[NV3089] SKIPPED fmt=0x%X  %ux%u  src=0x%08X(pitch %u,%s) -> dst=0x%08X(pitch %u,%s) at %u,%u%c",
                 f, out_w, out_h,
                 cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch, _sl?"LOCAL":"MAIN",
                 cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch, _dl?"LOCAL":"MAIN",
                 out_x, out_y, 10); }
        static int _w = 0;
        if (_w++ < 4) printf("[NV3089] unsupported colour format 0x%X, blit skipped\n", f);
        return;
    }
    int src_local = (s_nv3089.src_dma != 0xFEED0001u);
    int dst_local = (s_gcm2d.dst_dma  != 0xFEED0001u);
    u32 src = cellGcmResolveLocated(src_local, s_nv3089.in_off);
    u32 dst = cellGcmResolveLocated(dst_local, s_gcm2d.dst_offset);
    u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 start */
    /* Clip to SET_CLIP_POINT/SIZE (destination coordinates). A title tiling a
     * resolve in 512x512 blocks relies on it to stop at the surface edge:
     * Tornado Outbreak's bottom band is 512 rows tall with 208 left in the
     * display buffer, and writing all 512 ran on into the VRAM after it --
     * where its full-screen quad's vertices were staged, so the quad drew with
     * all-zero positions. A zero clip size means unclipped. */
    u32 cx0 = s_nv3089.clip_pt & 0xFFFF, cy0 = s_nv3089.clip_pt >> 16;
    u32 cx1 = (s_nv3089.clip_sz & 0xFFFF) ? cx0 + (s_nv3089.clip_sz & 0xFFFF) : 0xFFFFFFFFu;
    u32 cy1 = (s_nv3089.clip_sz >> 16)    ? cy0 + (s_nv3089.clip_sz >> 16)    : 0xFFFFFFFFu;
    /* Bytes per pixel from the SDK enums (CELL_GCM_TRANSFER_SCALE_FORMAT_*:
     * 7 = R5G6B5, 8 = Y8 -- not 32-bit as they were once labelled). When the
     * destination surface format agrees it is a plain copy of that width; a
     * real format conversion keeps the old 4-byte copy rather than guess. */
    u32 sbpp = (f == 7) ? 2u : (f == 8) ? 1u : 4u;
    u32 dbpp = (s_gcm2d.color_fmt == 4u /* R5G6B5 */) ? 2u
             : (s_gcm2d.color_fmt == 1u /* Y8 */)     ? 1u : 4u;
    u32 bpp = (sbpp == dbpp) ? sbpp : 4u;
    { const u64 sv_last = (((u64)v0 << 8) + (u64)(out_h - 1) * s_nv3089.dt_dy) >> 20;
      const u64 su_last = (((u64)u0 << 8) + (u64)(out_w - 1) * s_nv3089.ds_dx) >> 20;
      if (gcm_local_extent_bad(src_local, s_nv3089.in_off, sv_last * in_pitch + (su_last + 1) * bpp, "NV3089 source") ||
          gcm_local_extent_bad(dst_local, s_gcm2d.dst_offset,
                               (u64)(out_y + out_h - 1) * dst_pitch + (u64)(out_x + out_w) * bpp, "NV3089 destination"))
          return; }
    if (bpp == 4) nv3089_engine_read(in_pitch, out_w, out_h);
    /* Unscaled 32-bit rows are one copy each: the pixel loop goes through
     * vm_read32/vm_write32 (watch, bounds and SPU-reservation checks per word)
     * and was 9% of the RSX thread in God of War: Ascension's main menu. A row
     * whose destination holds an SPU reservation keeps the pixel loop, which
     * breaks it. */
    extern u8* vm_base;
    extern int spu_coh_is_reserved(u32 addr);
    const int row_copy = bpp == 4 && s_nv3089.ds_dx == 0x100000u;
    for (u32 y = 0; y < out_h; y++) {
        u32 dy = out_y + y;
        if (dy < cy0 || dy >= cy1) continue;
        u64 sv = ((u64)v0 << 8) + (u64)y * s_nv3089.dt_dy;         /* 20.12 */
        u32 sy = (u32)(sv >> 20);
        if (row_copy) {
            if (cx1 <= out_x) continue;
            const u32 xa = out_x < cx0 ? cx0 - out_x : 0;
            const u32 xb = (cx1 - out_x < out_w) ? cx1 - out_x : out_w;
            if (xa >= xb) continue;
            const u32 sx = (u32)((((u64)u0 << 8) + (u64)xa * s_nv3089.ds_dx) >> 20);
            const u32 s = src + sy * in_pitch + sx * 4u, d = dst + dy * dst_pitch + (out_x + xa) * 4u;
            const u32 len = (xb - xa) * 4u;
            int resv = 0;
            for (u32 l = d & ~127u; l < d + len && !resv; l += 128u) resv = spu_coh_is_reserved(l);
            if (!resv) { memmove(vm_base + d, vm_base + s, len); continue; }
        }
        for (u32 x = 0; x < out_w; x++) {
            u32 dx = out_x + x;
            if (dx < cx0 || dx >= cx1) continue;
            u64 su = ((u64)u0 << 8) + (u64)x * s_nv3089.ds_dx;
            u32 sx = (u32)(su >> 20);
            u32 s = src + sy * in_pitch + sx * bpp;
            u32 d = dst + dy * dst_pitch + dx * bpp;
            if (bpp == 4)      vm_write32(d, vm_read32(s));
            else if (bpp == 2) vm_write16(d, vm_read16(s));
            else               vm_write8(d, vm_read8(s));
        }
    }
    if (bpp == 4) {
        const u32 x0 = out_x > cx0 ? out_x : cx0, y0 = out_y > cy0 ? out_y : cy0;
        const u32 x1 = (out_x + out_w < cx1) ? out_x + out_w : cx1;
        const u32 y1 = (out_y + out_h < cy1) ? out_y + out_h : cy1;
        nv3089_engine_wrote(dst_pitch, x0, y0, x1, y1);
    }
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV3089] %ux%u fmt=0x%X src=0x%08X(pitch %u) -> dst=0x%08X(pitch %u) at %u,%u\n",
               out_w, out_h, f, src, in_pitch, dst, dst_pitch, out_x, out_y); } }
}

static void gcm_2d_method(u32 subch, u32 method, u32 data)
{
    /* GCM2D_TRACE=1: histogram of (subchannel, method) actually seen. The
     * subchannel a title binds each 2D object to is libgcm-version-specific and
     * SET_OBJECT binds are not tracked, so state landing on an unexpected
     * subchannel is silently dropped -- which looks like a blit using stale
     * destination state rather than like missing state. */
    { static int tr = -1;
      if (tr < 0) { const char* e = getenv("GCM2D_TRACE"); tr = e ? atoi(e) : 0; }
      if (tr) {
          static u8 seen[8][1024]; static u64 n = 0;
          u32 mi = (method >> 2) & 1023;
          if (subch < 8 && !seen[subch][mi]) {
              seen[subch][mi] = 1;
              printf("[GCM2D-TRACE] subch=%u method=0x%04X (first, data=0x%08X)%c",
                     subch, method, data, 10);
          }
          if (++n == 400000ull) {
              printf("[GCM2D-TRACE] --- subchannels used: ");
              for (u32 sc = 0; sc < 8; sc++) {
                  u32 c = 0; for (u32 m = 0; m < 1024; m++) c += seen[sc][m];
                  if (c) printf("%u(%u methods) ", sc, c);
              }
              printf("---%c", 10);
          }
      } }
    /* Subchannel bindings are libgcm-version-specific (SET_OBJECT binds are
     * not tracked): demosaic's SDK emits dest-surface on sub 3 and image-from-
     * CPU on sub 5 (cellGcmSetInlineTransfer: 0x4630C / 0xCA304 / 0xA400
     * headers); other versions use the classic 2 / 4. Accept both. */
    if (subch == 2 || subch == 3) {         /* NV3062 context surface 2D */
        switch (method) {
        case 0x0184: case 0x0188: s_gcm2d.dst_dma = data; return;
        case 0x0300: s_gcm2d.color_fmt  = data; return;
        case 0x0304: s_gcm2d.pitch      = data; return;
        case 0x030C: s_gcm2d.dst_offset = data; return;
        }
        return;
    }
    if (subch == 4 || subch == 5) {         /* NV308A image-from-CPU, or NV309E */
        /* NV309E swizzled-surface state. SET_FORMAT (0x0300) does not exist on
         * NV308A, so seeing it identifies the object bound here. */
        if (method == 0x0300) { s_nv309e.fmt = data; s_nv309e.active = 1;
                                s_nv309e.subch = (int)subch;
            { static int _n = 0; if (_n < 4 && ++_n)
                printf("[NV309E] format=0x%08X (log2 %ux%u fmt=0x%X)%c", data,
                       1u << ((data >> 16) & 0xFF), 1u << ((data >> 24) & 0xFF),
                       data & 0xFFFF, 10); }
            return; }
        if (method == 0x0304 && s_nv309e.active && s_nv309e.subch == (int)subch) {
            s_nv309e.offset = data; return; }
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return;
        case 0x0308: s_gcm2d.size_out = data; return;
        case 0x030C: return;                /* SIZE_IN */
        }
        if (method >= 0x0400 && method <= 0x07FC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers use format Y32 (4 bytes/px, one row per point.y).
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base = cellGcmResolveLocated(local, s_gcm2d.dst_offset);
            if (gcm_local_extent_bad(local, s_gcm2d.dst_offset, (u64)py * dst_pitch + (u64)px * 4 + 4, "NV308A write"))
                return;
            { static int _it = 0;
              if (_it++ < 6)
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u) = 0x%08X\n",
                         base + py * dst_pitch + px * 4, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, data); }
            vm_write32(base + py * dst_pitch + px * 4, data);
            return;
        }
        return;
    }
    /* NV3089 "scaled image from memory" -- the blit PSGL uses to move a texture
     * it has assembled elsewhere into the VRAM the sampler reads. Without it a
     * title's textures stay whatever the destination was before (zero), so
     * geometry renders flat no matter how correct the sampler is.
     *
     * Registers: 0x0184 source DMA, 0x0198 destination surface (the NV3062
     * state above), 0x0300 colour format, 0x0304 operation, 0x0308/0x030C clip
     * point/size, 0x0310/0x0314 out point/size, 0x0318/0x031C ds/dx dt/dy in
     * 20.12 fixed point, 0x0400 in size, 0x0404 (origin<<16)|pitch, 0x0408 in
     * offset, 0x040C in u/v start -- which is also the trigger. */
    if (subch == 6 || subch == 7) {
        switch (method) {
        case 0x0184: s_nv3089.src_dma   = data; return;
        case 0x0198: s_nv3089.dst_surf  = data; return;
        case 0x0300: s_nv3089.fmt       = data; return;
        case 0x0304: s_nv3089.op        = data; return;
        case 0x0308: s_nv3089.clip_pt   = data; return;
        case 0x030C: s_nv3089.clip_sz   = data; return;
        case 0x0310: s_nv3089.out_pt    = data; return;
        case 0x0314: s_nv3089.out_sz    = data; return;
        case 0x0318: s_nv3089.ds_dx     = data; return;
        case 0x031C: s_nv3089.dt_dy     = data; return;
        case 0x0400: s_nv3089.in_sz     = data; return;
        case 0x0404: s_nv3089.in_fmt    = data; return;
        case 0x0408: s_nv3089.in_off    = data; return;
        case 0x040C: s_nv3089.in_uv     = data; nv3089_blit(); return;
        }
        return;
    }
    /* NV0039 / NV309E: not implemented yet -- log first sightings. */
    /* GCM2D_DBG=<N> raises the cap and adds the data word: the fixed 8 showed
     * only that SOMETHING was unhandled, not enough to implement it. */
    static int warned = 0, wcap = -1;
    if (wcap < 0) { const char* e = getenv("GCM2D_DBG"); wcap = e ? atoi(e) : 8; }
    if (warned < wcap) { warned++;
        printf("[GCM2D-UNH] subch %u method 0x%04X data=0x%08X\n", subch, method, data); }
    if (0)
        printf("[cellGcmSys] FIFO subch %u method 0x%04X (unhandled 2D engine)\n",
               subch, method);
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    if (!s_flip_pending) return 0;
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put) return 0;   /* frame not fully drained yet */
    s_flip_pending = 0;
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

/* A JUMP/CALL whose target has no IO mapping must not be taken. Taking one
 * teleports the walker into unmapped space, where it can never reach `put`
 * again: the FIFO silently stops being processed, no fences are published, and
 * a title spinning on ctrl->ref waits forever with no indication why. Tokyo
 * Jungle did exactly this -- get left the 1 MB ring for IO 0x00F00000, which it
 * never mapped, while put sat at 0x604. Stop at the bad word and say so. */
static void gcm_fifo_bad_branch(const char* kind, u32 target, u32 word)
{
    static int n = 0;
    if (n++ < 8)
        fprintf(stderr, "[cellGcmSys] FIFO %s to unmapped IO 0x%08X (word 0x%08X) "
                "-- not taken, drain stops here\n", kind, target, word);
}

/* A bad branch nearly always means the walker mis-counted a method batch a few
 * dwords back and is now reading vertex data -- a float 1.0f is 0x3F800000,
 * which decodes as JUMP 0x1F800000. Dump the run-up: the command that consumed
 * the wrong count is in here. */
/* Last words the walker actually consumed, so a bad branch can show the decode
 * chain that led there rather than raw memory around it. */
#define GCM_TRACE_N 64
static u32 s_tr_off[GCM_TRACE_N], s_tr_w[GCM_TRACE_N];
static u32 s_tr_i = 0;
/* The walker's last GCM_BR_N branches (JUMP, CALL, RET): from, to, the word
 * and `put` when it was taken, and when. Printed at a resync: the step trace
 * above is full of the park word the walker re-read on every pass, while
 * this says how it got to where it stopped (a JUMP read from a segment that
 * was not all written yet lands it in an older lap of the ring, on a park
 * nobody will rewrite). A few stores per branch, a few hundred a frame. */
#define GCM_BR_N 32
unsigned long long ps3_qpc_us(void);
static struct { u32 from, to, w, put; unsigned long long t; } s_br[GCM_BR_N];
static u32 s_br_i;
static inline void gcm_br_note(u32 from, u32 to, u32 w, u32 put)
{
    const u32 k = s_br_i++ % GCM_BR_N;
    s_br[k].from = from; s_br[k].to = to; s_br[k].w = w; s_br[k].put = put; s_br[k].t = ps3_qpc_us();
}
static void gcm_br_dump(void)
{
    static int d = 0;
    if (d++ >= 3) return;
    const unsigned long long now = ps3_qpc_us();
    /* The word each branch was read from, read again now: one that has
     * changed since was read before its writer had written it. */
    for (u32 i = 0; i < GCM_BR_N; i++) {
        const u32 k = (s_br_i + i) % GCM_BR_N;
        if (!s_br[k].t) continue;
        const u32 ea = gcm_io2ea(s_br[k].from), now_w = ea ? vm_read32(ea) : 0xDEADDEADu;
        fprintf(stderr, "[FIFOBR] %lluus ago: 0x%08X -> 0x%08X (w=%08X%s%08X) put 0x%08X\n",
                now - s_br[k].t, s_br[k].from, s_br[k].to, s_br[k].w,
                now_w == s_br[k].w ? ", now same " : ", NOW CHANGED to ", now_w, s_br[k].put);
    }
    /* And the words around the last branch, as they are now. */
    const u32 last = (s_br_i + GCM_BR_N - 1) % GCM_BR_N;
    if (s_br[last].t)
        for (int d = -12; d <= 12; d++) {
            const u32 io = s_br[last].from + (u32)(d * 4), ea = gcm_io2ea(io);
            if (ea) fprintf(stderr, "[FIFOBR]   %+3d io=%08X w=%08X%s\n", d, io, vm_read32(ea), d ? "" : "   <-- last branch");
        }
}

/* A branch we cannot take leaves the walker pointing at whatever it
 * mis-decoded, and it never recovers -- get freezes for the rest of the
 * run and nothing renders. Resynchronise to `put` instead, the same trade
 * the unmapped-get path already makes: the commands between here and put
 * are lost, but the next frame is written from a clean boundary and the
 * FIFO lives. */
static void gcm_ref_push_at(u32 v, u32 getoff);

/* ---- SPU-built pushbuffers ------------------------------------------------
 * Some titles do not build the RSX command stream on the PPU at all. The
 * Orange Box is one: its libgcm context points at a 64 KB buffer in the
 * binary's own .bss that is never mapped into IO space, because that buffer is
 * where the PPU records draw commands for an SPU to consume. CB.SPU reads it
 * and DMAs the real pushbuffer into mapped main memory, then `put` is set past
 * the end of what it wrote.
 *
 * So libgcm is never told where the stream is, and the walker -- which starts
 * at IO 0 and has no JUMP to follow, because the SPU side did the binding --
 * scans tens of megabytes of unrelated memory, NOPs through the zeros (a zero
 * word is a valid NOP) and dies on the first vertex float it meets.
 *
 * The DMA is the only evidence there is, so record it. Each PUT that lands in
 * a mapped IO range is a candidate pushbuffer; the one that ends just short of
 * `put` is the block the RSX is actually being pointed at. */
#define GCM_SPU_BLK_N 16
static __declspec(align(64)) struct { u32 io_start, io_end; } s_spu_blk[GCM_SPU_BLK_N];
static __declspec(align(64)) int s_spu_blk_w = 0;   /* its own line: written on every recorded PUT */

/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact). */
static u32 gcm_ea2io_range(u32 ea, u32* room)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea >= m->ea && ea < m->ea + m->size) {
            if (room) *room = m->ea + m->size - ea;
            return m->io + (ea - m->ea);
        }
    }
    return 0xFFFFFFFFu;
}

void gcm_forget_spu_blocks(void)
{
    memset(s_spu_blk, 0, sizeof s_spu_blk);
    s_spu_blk_w = 0;
}

/* ---------------------------------------------------------------------------
 * Park episodes. A park is the title's self-JUMP at the head of a segment its
 * SPU jobs are still filling (God of War: Ascension's EDGE lists): the walker
 * stops on it until the job that writes the next segment rewrites it as a JUMP
 * onward. An episode runs from the first pass that stops on the word to the
 * pass that takes the rewritten JUMP. When the rewrite is an SPU DMA PUT (the
 * PUT hook below sees it) its time is recorded too, which splits an episode
 * into the SPU producing the segment and the walker noticing it.
 *
 *   GCM_PARK_LOG=1    one [park] line per episode (start, length, patch time,
 *                     offset, SPU slots at start and end), plus [gcm-rep] for
 *                     every report that is not a ZPASS count or that lands in
 *                     GCM_ROUND_RING's ring, and [gcm-flip] for each flip; all
 *                     on the ps3_qpc_us clock of [wid-time]/[task-time].
 *   GCM_PARK_STATS=1  a [park] summary every 5 s with the [walk] line.
 *   GCM_PARK_KICK=1   an SPU PUT that rewrites the park the walker stands on
 *                     wakes the walker at once, instead of at its next poll
 *                     (GCM_WALK_POLL_US) or a guest's fence kick. God of War:
 *                     Ascension's heavy scenes: taken 15 us after the patch
 *                     instead of ~2.8 ms (+1.8 fps), but 3 of 6 runs with it
 *                     on froze in the "walker ahead of the SPU" FIFO resync
 *                     (it read the next segment before the SPU had written
 *                     all of it), 0 of 6 without. Off.
 * All off by default. Episodes are tracked regardless (a few stores per park)
 * for cellGcm_park_clock_us, which the SPURS stats split SPU time by. */
unsigned long long ps3_qpc_us(void);
void cellGcm_fifo_kick(void);
extern int spurs_slot_snapshot(char* buf, int n) __attribute__((weak));
static int s_pk_log = -1, s_pk_stats, s_pk_kick;
/* GCM_PARK_KICK_AB=1: the kick (and the SPU hold with it) on and off in
 * turns, one [draws] window each (rsx_live_draw.c), for an A/B in one scene. */
volatile int g_gcm_park_kick_off;
/* GCM_PARK_KICK=2: the kick waits until the SPU that rewrote the park ends
 * its run (spurs_pm_worker calls cellGcm_park_kick_flush), so the rest of what
 * its job writes for that segment is in memory first. =1 woke the walker at
 * the PUT itself and it once read a CALL the job had not written yet. */
static _Thread_local int t_pk_kick_deferred;
/* GCM_PARK_HOLD_US=<us> (0 = off): a park an SPU job has rewritten is not
 * taken until the job that rewrote it has ended (cellGcm_park_job_end, where
 * the title's job front end reports it; else its SPURS run's end), or <us>
 * have passed, and that end wakes the walker. The rewrite is not the end of the segment: the
 * job's later DMAs write more of it (GCM_PARK_KICK=1, which woke the walker
 * at the rewriting PUT itself, read a CALL the job had not written yet and
 * froze 3 of 6 runs). A walker that looks between the two -- its poll landing
 * there, or the SPU thread preempted there, six of them on a 4-core PC --
 * decodes the rest of the segment from the previous lap of the ring: a float
 * taken for a JUMP into unmapped memory ("FIFO resync (unmapped-JUMP)"), or a
 * stale park nobody will rewrite, and the game freezes for good. The bound
 * keeps a job that waits on the walker from deadlocking it.
 * Up to GCM_PKH_N parks held at once (one per job that rewrote one); every
 * job marks its PUTs into the 64 KB after a park it rewrote, which is what
 * the [park-hold] line's "tail PUTs" counts (with the hold on or GCM_PARK_STATS). */
#define GCM_PKH_N 16
static volatile u32 s_pkh_ea[GCM_PKH_N];        /* parks rewritten by a job still going, 0 = free */
static long s_pkh_us = -1;
static _Thread_local int t_pkh_in_run;          /* between cellGcm_park_run_begin and the flush */
static _Thread_local u32 t_pkh_mask;            /* the s_pkh_ea slots this thread's run holds */
static _Thread_local u32 t_pkh_tail_ea;         /* the last park this run rewrote, 0 = none */
static volatile unsigned long long s_pkh_n, s_pkh_flush, s_pkh_timeout, s_pkh_wait_us, s_pkh_wait_max,
                                   s_pkh_rewrites, s_pkh_tail, s_pkh_full;
static long gcm_park_hold_us(void)
{
    if (s_pkh_us < 0) {
        const char* e = getenv("GCM_PARK_HOLD_US");
        const long v = e ? atol(e) : 0;
        s_pkh_us = v > 0 ? v : 0;
    }
    return s_pkh_us;
}
void cellGcm_park_run_begin(void) { t_pkh_in_run = 1; }
/* The end of a job that may have rewritten a park (a title's job front end
 * calls it: ascension n218_hle.c, the EDGE job): its segment is complete, so
 * its holds end here instead of at its run's end -- a run executes job after
 * job, and holding to its end kept the walker 3-4 ms per park. */
static void gw_pje_check(int where);   /* walker2 GW_PJE, below */
void cellGcm_park_job_end(void)
{
    gw_pje_check(0);
    t_pkh_tail_ea = 0;
    if (!t_pkh_mask) return;
    for (int i = 0; i < GCM_PKH_N; i++)
        if (t_pkh_mask & (1u << i)) __atomic_store_n(&s_pkh_ea[i], 0u, __ATOMIC_RELEASE);
    t_pkh_mask = 0;
    cellGcm_fifo_kick();
}
void cellGcm_park_kick_flush(void)
{
    int kick = 0;
    gw_pje_check(1);
    t_pkh_in_run = 0;
    t_pkh_tail_ea = 0;
    if (t_pkh_mask) {
        for (int i = 0; i < GCM_PKH_N; i++)
            if (t_pkh_mask & (1u << i)) __atomic_store_n(&s_pkh_ea[i], 0u, __ATOMIC_RELEASE);
        t_pkh_mask = 0;
        kick = 1;
    }
    if (t_pk_kick_deferred) { t_pk_kick_deferred = 0; kick = 1; }
    if (kick) cellGcm_fifo_kick();
}
static u32 s_pk_ring_lo, s_pk_ring_hi;          /* GCM_ROUND_RING, EA range */
static void gcm_park_env(void)
{
    if (s_pk_log >= 0) return;
    const char* e;
    s_pk_stats = (e = getenv("GCM_PARK_STATS")) != NULL && *e != '0';
    s_pk_kick  = (e = getenv("GCM_PARK_KICK")) != NULL ? (*e == '2' ? 2 : *e != '0') : 0;
    if ((e = getenv("GCM_ROUND_RING")) != NULL) {
        char* end = NULL; unsigned long long lo = strtoull(e, &end, 16), n = 0;
        if (end && *end == ',') n = strtoull(end + 1, NULL, 0);
        s_pk_ring_lo = (u32)lo; s_pk_ring_hi = (u32)(lo + n * 16ull);
    }
    s_pk_log = (e = getenv("GCM_PARK_LOG")) != NULL && *e != '0';
}
static u32 s_pk_off = ~0u;                      /* the park standing (IO), ~0 = none */
/* The two words every SPU PUT reads (gcm_note_spu_put), on a cache line of
 * their own: next to s_spu_blk_w, which every PUT into RSX memory writes,
 * each read missed. Written only when a park begins or ends. */
static __declspec(align(64)) struct { volatile u32 ea, word; char pad[56]; } s_pk_hot;
#define s_pk_ea   s_pk_hot.ea                  /* its EA for the PUT hook, 0 = none */
#define s_pk_word s_pk_hot.word                /* the self-JUMP word it held */
/* GCM_TEAR_TEST (spu_channels.c): where the park the walker stands on is. */
const volatile u32* cellGcm_park_ea_ptr(void) { return &s_pk_hot.ea; }
static unsigned long long s_pk_t0;              /* first pass that stopped on it */
static volatile unsigned long long s_pk_patch_t;   /* the SPU PUT that rewrote it, 0 = unseen */
static volatile long long s_pk_total_us;        /* completed episodes */
static volatile unsigned long long s_pk_since;  /* s_pk_t0 while one stands, else 0 */
static char s_pk_slots0[48];
static unsigned long long s_pks_n, s_pks_us, s_pks_max_us, s_pks_seen, s_pks_notice_us;

/* walker2 (2026-11-21, default off): park notice levers.
 *   GW_PJE=1      at every SPU job end (cellGcm_park_job_end, the natives' front ends) and SPURS run end
 *                 (cellGcm_park_kick_flush): if the walker stands on a park whose word has changed, kick it now
 *                 instead of at its next poll (GCM_WALK_POLL_US, 4 ms). The writes of the job that ended are
 *                 complete at that point.
 *   GW_PPOLL=<us> while the walker stands on a park, its poll is at most <us> (under 1000: yield-spin).
 *   GW_AB=1       test only: both off in every other 15 s window from the first present ([gw-ab] lines, window 0 OFF).
 * [gw] line every 5 s with the [walk] line: kicks from job ends / run ends. */
volatile int g_gw_on = 1;
static int s_gw_init, s_gw_ab, s_gw_pje, s_gw_ppoll;
static volatile long long s_gw_n[4];
static void gw_setup(void)
{
    const char* e;
    s_gw_pje = (e = getenv("GW_PJE")) != NULL && *e == '1';
    s_gw_ppoll = (e = getenv("GW_PPOLL")) != NULL ? atoi(e) : 0; if (s_gw_ppoll < 0) s_gw_ppoll = 0;
    s_gw_ab = (e = getenv("GW_AB")) != NULL && *e == '1';
    if (s_gw_ab) g_gw_on = 0;
    s_gw_init = 1;
    if (s_gw_pje || s_gw_ppoll || s_gw_ab)
        fprintf(stderr, "[gw] GW_PJE=%d GW_PPOLL=%d GW_AB=%d%c", s_gw_pje, s_gw_ppoll, s_gw_ab, 10);
}
static void gw_pje_check(int where)
{
    if (__builtin_expect(!s_gw_init, 0)) gw_setup();
    if (!s_gw_pje || !g_gw_on) return;
    const u32 pe = s_pk_ea;
    if (pe && vm_read32(pe) != s_pk_word) { __atomic_add_fetch(&s_gw_n[where], 1, __ATOMIC_RELAXED); cellGcm_fifo_kick(); }
}
/* boot_main's ticker: the poll bound while parked (0 = the usual one). */
int cellGcm_gw_park_poll_us(void)
{
    if (__builtin_expect(!s_gw_init, 0)) gw_setup();
    return s_gw_ppoll && g_gw_on && s_pk_ea ? s_gw_ppoll : 0;
}
/* rsx_live_draw_present: GW_AB windows. */
void cellGcm_gw_present(void)
{
    if (__builtin_expect(!s_gw_init, 0)) gw_setup();
    if (!s_gw_pje && !s_gw_ppoll && !s_gw_ab) return;
    static unsigned long long tw, t5; static int nwin; static unsigned fr;
    const unsigned long long now = ps3_qpc_us();
    fr++;
    if (!t5) t5 = now;
    if (now - t5 >= 5000000ull) {
        t5 = now;
        fprintf(stderr, "[gw] per 5s (%s): park kicks at job end %lld, at run end %lld%c", g_gw_on ? "ON" : "OFF",
                (long long)__atomic_exchange_n(&s_gw_n[0], 0, __ATOMIC_RELAXED),
                (long long)__atomic_exchange_n(&s_gw_n[1], 0, __ATOMIC_RELAXED), 10);
    }
    if (!s_gw_ab) return;
    if (!tw) { tw = now; fprintf(stderr, "[gw-ab] window 0 %s from frame %u%c", g_gw_on ? "ON" : "OFF", fr, 10); return; }
    if (now - tw >= 15000000ull) {
        tw = now; g_gw_on = !g_gw_on;
        fprintf(stderr, "[gw-ab] window %d %s from frame %u%c", ++nwin, g_gw_on ? "ON" : "OFF", fr, 10);
    }
}

/* Microseconds the walker has spent in park episodes so far, the standing one
 * included. Lock-free; a reader racing an episode's end can be off by that
 * episode for an instant (stats only). */
unsigned long long cellGcm_park_clock_us(unsigned long long now)
{
    const unsigned long long since = s_pk_since;
    long long t = s_pk_total_us;
    if (since && now > since) t += (long long)(now - since);
    return t > 0 ? (unsigned long long)t : 0ull;
}

static void gcm_park_end(int taken)
{
    if (s_pk_off == ~0u) return;
    const unsigned long long now = ps3_qpc_us();
    const unsigned long long d = now > s_pk_t0 ? now - s_pk_t0 : 0;
    const unsigned long long pt = s_pk_patch_t;
    s_pk_ea = 0;
    s_pk_since = 0;
    __atomic_add_fetch(&s_pk_total_us, (long long)d, __ATOMIC_RELAXED);
    s_pks_n++; s_pks_us += d; if (d > s_pks_max_us) s_pks_max_us = d;
    if (pt >= s_pk_t0 && pt) { s_pks_seen++; s_pks_notice_us += now > pt ? now - pt : 0; }
    if (s_pk_log > 0) {
        char s1[48] = "";
        if (spurs_slot_snapshot) spurs_slot_snapshot(s1, (int)sizeof s1);
        fprintf(stderr, "[park] t=%lluus d=%lluus patch=%lld off=0x%X %s slots=%s>%s\n",
                s_pk_t0, d, pt >= s_pk_t0 && pt ? (long long)(pt - s_pk_t0) : -1ll,
                s_pk_off, taken ? "taken" : "left", s_pk_slots0, s1);
    }
    s_pk_off = ~0u;
}

static void gcm_park_begin(u32 off)
{
    if (s_pk_off == off) return;                /* the same episode, another pass */
    gcm_park_end(0);                            /* one left standing: abandoned */
    gcm_park_env();
    s_pk_off = off;
    s_pk_t0 = ps3_qpc_us();
    s_pk_patch_t = 0;
    s_pk_since = s_pk_t0;
    const u32 ea = gcm_io2ea(off);
    s_pk_word = ea ? vm_read32(ea) : 0;
    s_pk_ea = ea;
    s_pk_slots0[0] = 0;
    if (s_pk_log > 0 && spurs_slot_snapshot) spurs_slot_snapshot(s_pk_slots0, (int)sizeof s_pk_slots0);
}

/* GCM_PARK_HOLD_US, the walker's side: the park it stands on has been
 * rewritten; 1 while the run that rewrote it is still going (and the bound
 * has not passed), so the walker stays parked this pass. */
static int gcm_park_hold_wait(void)
{
    static u32 held_ea;
    static unsigned long long held_t0;
    const u32 pe = s_pk_ea;
    if (!pe || !gcm_park_hold_us()) return 0;
    int open = 0;
    for (int i = 0; i < GCM_PKH_N; i++)
        if (__atomic_load_n(&s_pkh_ea[i], __ATOMIC_ACQUIRE) == pe) { open = 1; break; }
    const unsigned long long now = ps3_qpc_us();
    const unsigned long long pt = s_pk_patch_t;
    if (open && pt && now - pt < (unsigned long long)s_pkh_us) {
        if (held_ea != pe) { held_ea = pe; held_t0 = now; s_pkh_n++; }
        return 1;
    }
    if (held_ea == pe) {                         /* a hold ends: the run's end, or the bound */
        const unsigned long long d = now - held_t0;
        s_pkh_wait_us += d;
        if (d > s_pkh_wait_max) s_pkh_wait_max = d;
        if (open) s_pkh_timeout++; else s_pkh_flush++;
        held_ea = 0;
    }
    return 0;
}

void gcm_note_spu_put(u32 ea, u32 size)
{
    /* GCM_PARK_HOLD_US: a PUT of this run into the 64 KB after a park it
     * rewrote, i.e. more of that segment written after the rewrite. */
    { const u32 te = t_pkh_tail_ea;
      if (te && ea != te && ea - te < 0x10000u) __atomic_add_fetch(&s_pkh_tail, 1ull, __ATOMIC_RELAXED); }
    /* A PUT over the park word the walker stands on: it starts the next
     * segment (the rest of which the same job may still be writing). */
    { const u32 pe = s_pk_ea;
      if (pe && pe - ea < size && vm_read32(pe) != s_pk_word) {
          if (!s_pk_patch_t) s_pk_patch_t = ps3_qpc_us();
          if (t_pkh_in_run) {
              t_pkh_tail_ea = pe;
              __atomic_add_fetch(&s_pkh_rewrites, 1ull, __ATOMIC_RELAXED);
              if (gcm_park_hold_us()) {
                  int i = 0;
                  while (i < GCM_PKH_N && !__sync_bool_compare_and_swap(&s_pkh_ea[i], 0u, pe)) i++;
                  if (i < GCM_PKH_N) t_pkh_mask |= 1u << i;
                  else __atomic_add_fetch(&s_pkh_full, 1ull, __ATOMIC_RELAXED);
              }
          }
          if (s_pk_kick > 0 && !g_gcm_park_kick_off) {
              if (s_pk_kick == 2) t_pk_kick_deferred = 1;
              else cellGcm_fifo_kick();
          }
      } }
    if (!size || size > 0x40000u) return;
    u32 room = 0;
    u32 io = gcm_ea2io_range(ea, &room);
    if (io == 0xFFFFFFFFu || size > room) return;   /* not in RSX-visible memory */

    /* Coalesce with the previous block when an SPU emits one buffer in several
     * transfers -- otherwise only the last fragment is remembered and the
     * walker still starts in the middle of the stream. */
    int prev = (s_spu_blk_w + GCM_SPU_BLK_N - 1) % GCM_SPU_BLK_N;
    if (s_spu_blk[prev].io_end == io) { s_spu_blk[prev].io_end = io + size; return; }

    s_spu_blk[s_spu_blk_w].io_start = io;
    s_spu_blk[s_spu_blk_w].io_end   = io + size;
    s_spu_blk_w = (s_spu_blk_w + 1) % GCM_SPU_BLK_N;
}

/* The recorded block the RSX is being pointed at, or 0xFFFFFFFF.
 *
 * `put` sits at or just past the end of the block the title wants executed --
 * the gap is whatever libgcm appended after the SPU's data, a fence and its
 * argument in this title's case. Require the block to END within a small window
 * before `put` and to START before it, so an older double-buffered block (the
 * SPU alternates between two) is not chosen over the current one. */
static u32 gcm_spu_block_for_put(u32 put)
{
    u32 best = 0xFFFFFFFFu, best_end = 0;
    for (int i = 0; i < GCM_SPU_BLK_N; i++) {
        u32 s = s_spu_blk[i].io_start, e = s_spu_blk[i].io_end;
        if (!e || s >= put || e > put) continue;
        if (put - e > 0x40u) continue;              /* too far back to be current */
        if (e >= best_end) { best_end = e; best = s; }
    }
    return best;
}

/* A resync jumps `get` straight to `put`, which is what keeps the FIFO alive
 * when the walker cannot make progress -- but everything in between is
 * dropped, and if a SET_REFERENCE is in there the cellGcmFinish waiting on it
 * never wakes. Tokyo Jungle lost exactly one that way: two fences in the ring
 * (arg 0 at io 0x09A0, arg 1 at io 0x19E8), the walker parked before the
 * second, and the resync to put=0x19F0 stepped straight over it -- ref stayed
 * 0 while the title spun for ref==1 forever.
 *
 * Dropping DRAWS on a resync is survivable: a frame is lost. Dropping a FENCE
 * is not, it deadlocks the title. Sweep the skipped range first. */
/* GCM_RESYNC_DUMP=1 (off by default): in God of War: Ascension every resync
 * so far was the start of a freeze -- the frame it skips holds what the game
 * waits on (2 of 78 runs since 2026-10-28: a park left unpatched for the whole
 * GCM_PARK_WAIT_MS, and a CALL decoded from data). Before the first two
 * resyncs, say what the park was doing, what each SPU runs and where every
 * thread is (boot_main.cpp's watchdog dump), and again 3 s later, when the
 * freeze has set in. */
extern void spu_running_dump(const char* label);   /* spurs_policy.c */
extern void spurs_dump_state(const char* label);   /* cellSpurs.c */
/* boot_main.cpp's thread dump, registered there at startup (NULL in builds
 * without it). */
void (*g_ps3_dump_threads_fn)(const char* label) = NULL;
static DWORD WINAPI gcm_resync_dump_later(LPVOID unused)
{
    (void)unused;
    Sleep(3000);
    fprintf(stderr, "[resync-dump] 3 s after the resync: get 0x%08X\n", s_fifo_getoff);
    fflush(stderr);
    spu_running_dump("resync+3s");
    spurs_dump_state("resync+3s");
    if (g_ps3_dump_threads_fn) g_ps3_dump_threads_fn("resync+3s");
    return 0;
}
static void gcm_resync_dump(const char* why, u32 getoff, u32 put)
{
    static int on = -1, n = 0;
    if (on < 0) { const char* e = getenv("GCM_RESYNC_DUMP"); on = e && *e != '0'; }
    if (!on || n++ >= 2) return;
    const unsigned long long now = ps3_qpc_us();
    const u32 pk_ea = s_pk_ea;
    const unsigned long long pt = s_pk_patch_t;
    fprintf(stderr, "[resync-dump] %s at get 0x%08X put 0x%08X; park standing: off 0x%X for %lld us, "
                    "word was 0x%08X now 0x%08X, SPU rewrite seen %lld us after it began; "
                    "parks so far %llu (longest %llu us), SPU blocks last written:",
            why, getoff, put, s_pk_off,
            s_pk_off != ~0u && now > s_pk_t0 ? (long long)(now - s_pk_t0) : -1ll,
            s_pk_word, pk_ea ? vm_read32(pk_ea) : 0u,
            pt && pt >= s_pk_t0 ? (long long)(pt - s_pk_t0) : -1ll,
            s_pks_n, s_pks_max_us);
    for (int i = 0; i < GCM_SPU_BLK_N; i++) {
        const int k = (s_spu_blk_w + GCM_SPU_BLK_N - 1 - i) % GCM_SPU_BLK_N;
        if (s_spu_blk[k].io_end)
            fprintf(stderr, " %08X-%08X", s_spu_blk[k].io_start, s_spu_blk[k].io_end);
    }
    fprintf(stderr, "\n");
    /* The words around get, now and again after the SPU dump (>= 50 ms): any
     * that change were still being written when the walker read them -- the
     * walker ahead of the SPU (a bad CALL decoded from a segment in flight). */
    enum { W_LO = 256, W_N = 288 };   /* 1 KB before get (the walk that led here), 128 B after */
    u32 snap[W_N];
    for (int k = 0; k < W_N; k++) {
        const u32 ea = gcm_io2ea(getoff + (u32)((k - W_LO) * 4));
        snap[k] = ea ? vm_read32(ea) : 0xDEADDEADu;
    }
    fflush(stderr);
    spu_running_dump("resync");
    { int changed = 0;
      for (int k = 0; k < W_N; k++) {
          const u32 io = getoff + (u32)((k - W_LO) * 4), ea = gcm_io2ea(io);
          const u32 w = ea ? vm_read32(ea) : 0xDEADDEADu;
          if (w != snap[k]) {
              if (changed++ < 48)
                  fprintf(stderr, "[resync-dump] word io=0x%08X (get%+d) was 0x%08X, now 0x%08X\n",
                          io, (k - W_LO) * 4, snap[k], w);
          }
      }
      fprintf(stderr, "[resync-dump] %d of %d words around get changed while the SPUs were dumped\n",
              changed, (int)W_N); }
    /* With SPU_WTRACE=<lo>-<hi> over the command buffers: every PPU store and
     * SPU PUT that touched the words around get, newest first, with their
     * times -- who wrote the words the walker read, and when, against
     * now_us below (the walker read them just before this). */
    { extern uint32_t g_wtrace_span;
      extern void spu_wtrace_dump(uint32_t ea, uint32_t n, unsigned max, const char* tag);
      const u32 ea = gcm_io2ea(getoff - (u32)W_LO * 4u);
      if (g_wtrace_span && ea) {
          fprintf(stderr, "[resync-dump] writes into ea 0x%08X..0x%08X (get at 0x%08X), now_us %llu:\n",
                  ea, ea + (u32)W_N * 4u, gcm_io2ea(getoff), now);
          spu_wtrace_dump(ea, (u32)W_N * 4u, 48u, "resync");
      } }
    spurs_dump_state("resync");
    if (g_ps3_dump_threads_fn) g_ps3_dump_threads_fn("resync");
    HANDLE t = CreateThread(NULL, 0, gcm_resync_dump_later, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void gcm_fifo_resync_why(const char* why, u32* getoff, u32 put)
{
    gcm_br_dump();
    /* merge 0.1.17a: what OCCL holds now (rsx_live_draw.c; prints only while OCCL is set up, first 4 times) */
    { extern void rsx_live_draw_occl_freeze_dump(const char* why); rsx_live_draw_occl_freeze_dump(why); }
    gcm_resync_dump(why, *getoff, put);
    gcm_park_end(0);   /* a park standing is abandoned with the rest */
    static int n = 0;
    if (n++ < 8)
        fprintf(stderr, "[cellGcmSys] FIFO resync (%s) 0x%08X -> put 0x%08X\n",
                why, *getoff, put);
    /* If an SPU DMA'd a pushbuffer that ends just short of `put`, that is the
     * stream the RSX is being pointed at -- land at its START and walk it,
     * instead of landing on `put` and skipping it. Resyncing to `put` keeps the
     * FIFO alive but drops every command in between, and SET_BEGIN_END is one
     * of them: the draws still arrive, with no primitive type, and are dropped
     * by the backend as prim=0. */
    u32 land = gcm_spu_block_for_put(put);
    if (land == 0xFFFFFFFFu) land = put;
    else { static int b = 0; if (b < 8 && ++b)
        fprintf(stderr, "[cellGcmSys] resync landing on SPU pushbuffer "
                "0x%08X (put 0x%08X) instead of skipping to put\n", land, put); }

    /* Sweep only the range still being skipped; the walker will consume the
     * fences inside the block itself, and pushing them here too would publish
     * every one twice. */
    u32 from = (*getoff <= land) ? *getoff : s_last_ring_off;
    if (from < land) {
        unsigned rescued = 0;
        for (u32 io = from; io + 8 <= land; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w >> 29) != 0 || (w & 0x1FFCu) != 0x0050u) continue;
            u32 dea = gcm_io2ea(io + 4); if (!dea) continue;
            gcm_ref_push_at(vm_read32(dea), io);
            rescued++;
        }
        if (rescued) { static int m = 0; if (m < 8 && ++m)
            fprintf(stderr, "[cellGcmSys] resync rescued %u fence(s) from"
                    " 0x%08X..0x%08X\n", rescued, from, land); }
    }
    *getoff = land;
}

static void gcm_fifo_dump_around(u32 getoff)
{
    /* Only ever called on a recovery (bad branch, stuck get), and capped, so
     * it no longer hides behind GCM_RECDBG: the words the walker stopped on
     * are the one thing a run that wedged once in three can still tell us,
     * and GCM_RECDBG is far too loud to leave on for a 15-minute run. */
    static int d = 0;
    if (d++ >= 3) return;
    for (u32 t = 0; t < GCM_TRACE_N; t++) {
        u32 k = (s_tr_i + t) % GCM_TRACE_N;
        if (!s_tr_w[k]) continue;
        u32 w = s_tr_w[k];
        fprintf(stderr, "[FIFOSTEP] io=%08X w=%08X  type=%u method=0x%04X count=%u\n",
                s_tr_off[k], w, w >> 29, w & 0x1FFCu, (w >> 18) & 0x7FFu);
    }
    for (int k = -20; k <= 2; k++) {
        u32 io = getoff + (u32)(k * 4);
        u32 ea = gcm_io2ea(io);
        if (!ea) continue;
        fprintf(stderr, "[FIFODUMP] %+3d io=%08X w=%08X%s\n",
                k, io, vm_read32(ea), k ? "" : "   <-- bad word");
    }
}


static void gcm_refw_on_push(void);   /* cpu: SYS_USLEEP_REFWAIT, below */
static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        static int _o = 0;
        if (_o++ < 4) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    s_ref_qtail = t + 1;
    gcm_refw_on_push();
}

/* Global fence-publication counter: lets the lwmutex convoy trace correlate a
 * long lock hold with the number of paced fence publications inside it (the
 * 200us pacing x hundreds of one-ahead fences = the ~156ms holds). */
volatile long long g_gcm_ref_pub_count = 0;
/* Absolute microseconds from the QPC hardware counter -- the SAME origin in every
 * translation unit, so two probes in different files can be compared directly.
 * That is the whole point: every wrong conclusion about this stall came from
 * comparing orderings instead of times. */
unsigned long long ps3_qpc_us(void)
{
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (unsigned long long)(n.QuadPart / (f.QuadPart / 1000000));
}

/* SYS_USLEEP_REFWAIT (cpu, 2026-11-09; runtime/syscalls/sys_timer.c parses the list of poll sites, this file
 * only needs to know whether it is set): a guest thread that polls the ref register with short usleeps blocks in
 * cellGcm_ref_wait until the next moment a read of the register can see or do anything new, instead of spinning:
 * a fence publication (gcm_ref_publish_one: the walker's pass end, another thread's read) or the read gate of
 * cellGcm_ref_on_poll opening (s_ref_last_pub + ~200 us: the read publishes the next queued fence, or kicks the
 * walker when the queue is dry). Between those moments a read returns the same value and changes nothing, so the
 * guest sees the same values in the same order; only fewer identical reads. One waiting thread at a time (in God of
 * War: Ascension only the PPU main thread polls there: lr 0x375A78, the flip-pacing wait); a second one spins as
 * before. Counters: the [cpu-wait] line (cellSpurs.c).
 * The timer ends a sub-millisecond wait on the system's next timer tick (MEASURED here: a ~200 us gate wait lasted
 * 520 us p50), so a fence queued while the waiter blocks wakes it (gcm_refw_on_push), and a waiter that finds a
 * fence queued behind a closed gate yield-spins the rest of the gate as before: the queued fence is published at
 * the same moment as with the spin, the timer only covers the stretches with nothing to publish.
 * Side effect (host\cpu\NOTES.md 4): with nothing queued the poll's next read comes ~0.5 ms after the gate instead
 * of every ~200 us, and a dry read is what kicks a walker parked on a park an SPU rewrote (GCM_PARK_KICK is off), so
 * such a park can end up to ~0.3 ms later (the walker's own poll GCM_WALK_POLL_US stays the bound). Counters
 * (CPU_WAIT_STATS) only cost anything when on. */
#include "../../runtime/platform/cpu_wait.h"
static volatile LONGLONG s_ref_last_pub;       /* QPC of the last read-gate pass (cellGcm_ref_on_poll) */
static LARGE_INTEGER s_ref_qpf;
static volatile LONG s_rw_owner, s_rw_waiting;
static HANDLE s_rw_ev;                          /* auto-reset: a publication or a queued fence while s_rw_waiting */
static volatile LONGLONG s_rw_sig_qpc;          /* when that signal was given (wake latency) */
static __declspec(thread) long long t_rw_seen;  /* g_gcm_ref_pub_count at this thread's last ref read */
static __declspec(thread) LONGLONG t_rw_read_qpc;
static __declspec(thread) HANDLE t_rw_timer;
static volatile long long s_rw_calls, s_rw_now, s_rw_pub, s_rw_gate, s_rw_cap, s_rw_queued, s_rw_blocked, s_rw_spin,
                          s_rw_sets, s_rw_psets;
static cw_hist s_rw_wait_h, s_rw_lat_h, s_rw_qspin_h;
static int gcm_refw_on(void)
{
    static volatile int on = -1;
    int v = on;
    if (v < 0) {
        const char* e = getenv("SYS_USLEEP_REFWAIT");
        v = e && *e && strcmp(e, "0") != 0;
        if (!s_ref_qpf.QuadPart) QueryPerformanceFrequency(&s_ref_qpf);
        on = v;
    }
    return v;
}
/* On, and not CPU_WAIT_AB's off side (then every path below is the old one). */
static inline int gcm_refw_live(void) { return gcm_refw_on() && !cw_ab_off(); }
static void gcm_refw_note_read(void)
{
    if (!gcm_refw_live()) return;
    t_rw_seen = __atomic_load_n(&g_gcm_ref_pub_count, __ATOMIC_SEQ_CST);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    t_rw_read_qpc = n.QuadPart;
}
/* > 0: handled (the caller returns from its usleep), 0: not a ref poll here, spin as before. */
int cellGcm_ref_wait(unsigned cap_us)
{
    if (!gcm_refw_live() || !t_rw_read_qpc) return 0;
    const int st = cw_stats_on();
    const LONGLONG f = s_ref_qpf.QuadPart;
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    /* Only right after a ref read on this thread (a usleep of the same poll loop): 2 ms. */
    if (n.QuadPart - t_rw_read_qpc > f / 500) { if (st) __atomic_add_fetch(&s_rw_spin, 1, __ATOMIC_RELAXED); return 0; }
    if (InterlockedCompareExchange(&s_rw_owner, 1, 0) != 0) { if (st) __atomic_add_fetch(&s_rw_spin, 1, __ATOMIC_RELAXED); return 0; }
    if (!s_rw_ev) s_rw_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    if (!t_rw_timer) t_rw_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_rw_ev || !t_rw_timer) { InterlockedExchange(&s_rw_owner, 0); return 0; }
    if (st) __atomic_add_fetch(&s_rw_calls, 1, __ATOMIC_RELAXED);
    const LONGLONG t0 = n.QuadPart, cap = t0 + (LONGLONG)cap_us * f / 1000000, gap = f / 5000;   /* the gate */
    int blocked = 0, why = 0;   /* 1 publication, 2 gate open, 3 cap, 4 a queued fence (spun to the gate) */
    LONGLONG tb = 0;            /* when the blocked part ended */
    __atomic_store_n(&s_rw_waiting, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        if (__atomic_load_n(&g_gcm_ref_pub_count, __ATOMIC_SEQ_CST) != t_rw_seen) { why = 1; break; }
        QueryPerformanceCounter(&n);
        const LONGLONG due = s_ref_last_pub + gap;
        if (n.QuadPart >= due) { why = 2; break; }
        if (n.QuadPart >= cap) { why = 3; break; }
        /* A fence queued (s_rw_waiting was set before this read: a later push signals): the read at the gate
         * publishes it, so spin to the gate as without SYS_USLEEP_REFWAIT instead of the coarse timer. */
        if (__atomic_load_n(&s_ref_qhead, __ATOMIC_SEQ_CST) != __atomic_load_n(&s_ref_qtail, __ATOMIC_SEQ_CST)) {
            const LONGLONG s0 = n.QuadPart, end = due < cap ? due : cap;
            do { SwitchToThread(); QueryPerformanceCounter(&n); }
            while (n.QuadPart < end && __atomic_load_n(&g_gcm_ref_pub_count, __ATOMIC_SEQ_CST) == t_rw_seen);
            if (st) cw_hist_add(&s_rw_qspin_h, (n.QuadPart - s0) * 1000000 / f);
            why = 4; break;
        }
        const LONGLONG until = due < cap ? due : cap;
        LARGE_INTEGER rel; rel.QuadPart = -((until - n.QuadPart) * 10000000 / f);
        if (rel.QuadPart > -1) rel.QuadPart = -1;
        if (!SetWaitableTimer(t_rw_timer, &rel, 0, NULL, NULL, FALSE)) { why = 3; break; }
        HANDLE hs[2] = { s_rw_ev, t_rw_timer };
        const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        QueryPerformanceCounter(&n);
        tb = n.QuadPart;
        if (st && r == WAIT_OBJECT_0) {   /* signalled: how long after the publication / push (not a stale signal) */
            const LONGLONG p = s_rw_sig_qpc;
            if (p >= t0 && p <= n.QuadPart) cw_hist_add(&s_rw_lat_h, (n.QuadPart - p) * 1000000 / f);
        }
        blocked = 1;
    }
    __atomic_store_n(&s_rw_waiting, 0, __ATOMIC_SEQ_CST);
    InterlockedExchange(&s_rw_owner, 0);
    if (!st) return 1;
    if (!blocked && why != 4) __atomic_add_fetch(&s_rw_now, 1, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(why == 1 ? &s_rw_pub : why == 2 ? &s_rw_gate : why == 3 ? &s_rw_cap : &s_rw_queued, 1,
                           __ATOMIC_RELAXED);
        if (blocked) {
            __atomic_add_fetch(&s_rw_blocked, 1, __ATOMIC_RELAXED);
            cw_hist_add(&s_rw_wait_h, (tb - t0) * 1000000 / f);
        }
    }
    return 1;
}
/* A fence queued (gcm_ref_push_at, the walker): wake a blocked waiter, whose read at the gate publishes it. The
 * fence is in the queue before s_rw_waiting is read; the waiter sets s_rw_waiting before it reads the queue. */
static void gcm_refw_on_push(void)
{
    if (!gcm_refw_live()) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_rw_waiting, __ATOMIC_SEQ_CST)) {
        if (cw_stats_on()) {
            LARGE_INTEGER n; QueryPerformanceCounter(&n);
            s_rw_sig_qpc = n.QuadPart;
            __atomic_add_fetch(&s_rw_psets, 1, __ATOMIC_RELAXED);
        }
        SetEvent(s_rw_ev);
    }
}
/* For the [cpu-wait] line: this file's part, the window taken and zeroed. */
int cellGcm_refwait_line(char* buf, int len)
{
    if (!gcm_refw_on()) return snprintf(buf, (size_t)len, "refwait off");
    long long w50, w99, l50, l99, q50, q99;
    const long long calls = __atomic_exchange_n(&s_rw_calls, 0, __ATOMIC_RELAXED);
    const long long now = __atomic_exchange_n(&s_rw_now, 0, __ATOMIC_RELAXED);
    const long long pub = __atomic_exchange_n(&s_rw_pub, 0, __ATOMIC_RELAXED);
    const long long gate = __atomic_exchange_n(&s_rw_gate, 0, __ATOMIC_RELAXED);
    const long long capn = __atomic_exchange_n(&s_rw_cap, 0, __ATOMIC_RELAXED);
    const long long qd = __atomic_exchange_n(&s_rw_queued, 0, __ATOMIC_RELAXED);
    const long long blk = __atomic_exchange_n(&s_rw_blocked, 0, __ATOMIC_RELAXED);
    const long long spin = __atomic_exchange_n(&s_rw_spin, 0, __ATOMIC_RELAXED);
    const long long sets = __atomic_exchange_n(&s_rw_sets, 0, __ATOMIC_RELAXED);
    const long long psets = __atomic_exchange_n(&s_rw_psets, 0, __ATOMIC_RELAXED);
    cw_hist_take(&s_rw_wait_h, &w50, &w99);
    cw_hist_take(&s_rw_lat_h, &l50, &l99);
    const long long qn = cw_hist_take(&s_rw_qspin_h, &q50, &q99);
    return snprintf(buf, (size_t)len, "refwait %lld calls: %lld at once, ended by ref %lld / gate %lld / cap %lld / queued "
                    "fence %lld; blocked %lld (us p50/p99 %lld/%lld; wake after a signal p50/p99 %lld/%lld us); spun to the "
                    "gate %lld (us p50/p99 %lld/%lld); %lld not a ref poll (spun); signals: ref %lld, queued %lld",
                    calls, now, pub, gate, capn, qd, blk, w50, w99, l50, l99, qn, q50, q99, spin, sets, psets);
}

static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    /* stderr, deliberately: [DRAIN] uses printf and stdout is block-buffered
     * when redirected, so its interleaving with an stderr probe is not a
     * timeline. Same stream = same ordering. */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_REFPUB") ? 1 : 0;
      if (_rd) { static unsigned long _n = 0; if (++_n <= 24)
        fprintf(stderr, "[refpub] t=%lluus #%lu wrote 0x%08X -> readback 0x%08X\n", ps3_qpc_us(), _n,
                s_ref_q[h % GCM_REF_QLEN], vm_read32(GCM_CONTROL_GUEST_ADDR + 8)); } }
    s_ref_qhead = h + 1;
    if (!gcm_refw_live()) { g_gcm_ref_pub_count++; return; }
    /* SYS_USLEEP_REFWAIT: count, then look for a waiter (it sets s_rw_waiting, then reads the count: one of the
     * two sees the other). */
    __atomic_add_fetch(&g_gcm_ref_pub_count, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_rw_waiting, __ATOMIC_SEQ_CST)) {
        if (cw_stats_on()) {
            LARGE_INTEGER n; QueryPerformanceCounter(&n);
            s_rw_sig_qpc = n.QuadPart;
            __atomic_add_fetch(&s_rw_sets, 1, __ATOMIC_RELAXED);
        }
        SetEvent(s_rw_ev);
    }
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static void gcm_rsx_process_fifo_unlocked(void);
static const char* s_walk_why;   /* why the last walker pass stopped (gcm_walk_stats) */

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void*)s_gcm_kick_ev;
}

/* The present thread's idle wait. It used to be a flat Sleep(4), which meant
 * the kick above was created, signalled -- and waited on by nobody, so a
 * starved guest still paid the full tick. Waiting here instead costs nothing
 * when no one kicks (the timeout is the old sleep) and drains immediately when
 * someone does. */
void cellGcm_fifo_kick_wait(unsigned ms)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_KICK"); on = (e && *e == '0') ? 0 : 1; }
    HANDLE ev = on ? (HANDLE)cellGcm_fifo_kick_event() : NULL;
    if (ev) WaitForSingleObject(ev, ms);
    else    Sleep(ms);
}

/* Wake the present thread's walker now (a wait it stopped on may be over:
 * the SPURS tasks a held report waits for have gone back to WAIT_SIGNAL). */
void cellGcm_fifo_kick(void)
{
    if (s_gcm_kick_ev) SetEvent(s_gcm_kick_ev);
}


/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    /* s_ref_last_pub, s_ref_qpf: file scope for cellGcm_ref_wait (SYS_USLEEP_REFWAIT), same values as before. */
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock)) {
        gcm_refw_note_read();
        return;
    }
    if (!s_ref_qpf.QuadPart) QueryPerformanceFrequency(&s_ref_qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG min_gap = s_ref_qpf.QuadPart / 5000;          /* ~200us */
    int dry = 0;
    if (now.QuadPart - s_ref_last_pub >= min_gap) {
        gcm_ref_publish_one();
        s_ref_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    gcm_refw_note_read();
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
}

/* For the performance panel (LD_PERF_OVERLAY): time spent walking the FIFO
 * (the emulated RSX's command processing, live draw included), and time the
 * walker held a GET_REPORT for SPU tasks (GCM_REPORT_SPU_HOLD_MS). */
volatile long long g_perf_rsx_us, g_perf_hold_us;

/* Frame pacing for flips in the command stream. PS3_FPS_LIMIT=<fps> (or the
 * older GCM_FLIP_PACE_MS=<ms>) makes a flip wait until one frame period after
 * the previous one, with microsecond precision; the FIFO pass stops before the
 * flip and the next pass reads it again, as for a blocked ACQUIRE. God of War:
 * Ascension steps its game clock by a fixed 1/30 s per frame, so flips faster
 * than 30 a second make the game itself run faster. Returns 1 to wait. */
static long long gcm_flip_pace_left_us(long long* due_out, long long* now_out, long long* late_out);
/* PS3_FPS_LIMIT_MODE: where flips are paced. "walker" (flips in the command
 * stream wait in the FIFO walker), "direct" (flips requested by calling libgcm
 * wait on the calling thread), "both" (default). */
static int gcm_flip_pace_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char* e = getenv("PS3_FPS_LIMIT_MODE");
        /* Default "walker": sleeping a thread that called libgcm hung God of
         * War: Ascension on its loading screen (it flips from code the
         * runtime's vblank thread runs). */
        m = !e || !*e || !strcmp(e, "walker") ? 1 : !strcmp(e, "both") ? 3 : !strcmp(e, "direct") ? 2 : 1;
        fprintf(stderr, "[cellGcmSys] flip pacing mode %d\n", m);
    }
    return m;
}
static int gcm_flip_pace_wait(void);   /* below gcm_flip_pace_left_us (GCM_PACE_PRECISE) */

/* Microseconds until the next flip is due (0: it may go now, and the slot is
 * taken). */
/* Loading-screen turbo (LD_TURBO_LOAD, set per frame by rsx_live_draw.c):
 * God of War: Ascension loads a fixed amount per frame of its loading screen,
 * so at 60 frames a second Continue took 22-40 s with the PC idle; at 240 it
 * takes ~6 s. While the flag is up the vblank ticker (boot_main.cpp) and the
 * flip pacing run at LD_TURBO_HZ (default 240, 60..480) and Present does not
 * wait for vsync. */
volatile int g_ps3_load_turbo;
double ps3_load_turbo_hz(void)
{
    static double hz = -1.0;
    if (hz < 0.0) { const char* e = getenv("LD_TURBO_HZ"); hz = e ? atof(e) : 240.0;
                    if (hz < 60.0) hz = 60.0; if (hz > 480.0) hz = 480.0; }
    return hz;
}

static long long s_pace_period_us;   /* the cap's period, for the [pace] line */
/* In-game menu (rsx_ingame_menu.h): the frame-rate limit changed while playing. The new period waits here and the
 * pacer takes it at its next look, under its own lock; -1 = nothing waiting. fps <= 0: no cap. */
static volatile long long s_pace_us_req = -1;
void cellGcm_set_flip_pace(double fps)
{
    InterlockedExchange64(&s_pace_us_req, fps > 0.0 ? (long long)(1e6 / fps) : 0);
}
/* The limit in force (frames a second; 0: none, or not read yet). */
double cellGcm_flip_pace_fps(void)
{
    const long long req = s_pace_us_req;
    const long long us = req >= 0 ? req : s_pace_period_us;
    return us > 0 ? 1e6 / (double)us : 0.0;
}
/* due_out (when it returns > 0): the QPC microsecond the flip is due; now_out:
 * the clock it read (0 when there is no cap); late_out (when it returns 0: this
 * flip took the slot): how long after its due time it went, -1 when it was off
 * the grid (more than a period behind). Any may be NULL. */
static long long gcm_flip_pace_left_us(long long* due_out, long long* now_out, long long* late_out)
{
    static long long pace_us = -1, last_flip_us;
    static SRWLOCK lock = SRWLOCK_INIT;
    if (pace_us < 0) {
        const char* f = getenv("PS3_FPS_LIMIT");
        const char* e = getenv("GCM_FLIP_PACE_MS");
        pace_us = f && atof(f) > 0 ? (long long)(1e6 / atof(f))
                : e ? (long long)(atof(e) * 1000.0) : 0;
        s_pace_period_us = pace_us;
    }
    if (s_pace_us_req >= 0) {   /* in-game menu: a new limit (rare; the pacer's lock makes the change whole) */
        AcquireSRWLockExclusive(&lock);
        const long long req = InterlockedExchange64(&s_pace_us_req, -1);
        if (req >= 0) { pace_us = req; s_pace_period_us = req; last_flip_us = 0; }
        ReleaseSRWLockExclusive(&lock);
    }
    if (now_out) *now_out = 0;
    if (pace_us <= 0) return 0;
    const long long pace = g_ps3_load_turbo ? (long long)(1e6 / ps3_load_turbo_hz()) : pace_us;
#ifdef _WIN32
    static LARGE_INTEGER qpf; LARGE_INTEGER qpc;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc);
    const long long now = (long long)(qpc.QuadPart * 1000000.0 / (double)qpf.QuadPart);
#else
    extern unsigned long long ps3_ms_now(void);
    const long long now = (long long)ps3_ms_now() * 1000;
#endif
    AcquireSRWLockExclusive(&lock);
    long long left = 0;
    if (last_flip_us && now - last_flip_us < pace) {
        left = pace - (now - last_flip_us);
        if (due_out) *due_out = last_flip_us + pace;
    } else {
        /* Keep the cadence: the next flip is due one period after this one
         * was due, unless we fell more than a frame behind. */
        const int on_grid = last_flip_us && now - last_flip_us < 2 * pace;
        if (late_out) *late_out = on_grid ? now - (last_flip_us + pace) : -1;
        last_flip_us = on_grid ? last_flip_us + pace : now;
    }
    ReleaseSRWLockExclusive(&lock);
    if (now_out) *now_out = now ? now : 1;
    return left;
}

/* GCM_PACE_PRECISE (default 1; 0 = exactly the old behaviour): the frame cap
 * lets each flip go on time. A flip due later stops the walker pass
 * ('flip-pace'), and the pass ran again only on the ticker's next wake-up -- a
 * guest kick, the next vblank tick or the 4 ms poll, each a whole-millisecond
 * wait -- so capped flips went 0-4 ms late and the time between presents
 * wobbled (MSI Afterburner's wavy frame-time line; RTSS's limiter, a QPC spin
 * right before Present, is flat). Now a pass that stops on pace publishes the
 * flip's due time, and when it comes before the ticker's own wait would end,
 * the ticker (cellGcm_fifo_pace_wait, boot_main.cpp) sleeps on a
 * high-resolution waitable timer (Windows 10 1803+) to GCM_PACE_LEAD_US
 * (default 500) before it, still woken by a kick, then spins on QPC
 * (YieldProcessor, at most GCM_PACE_SPIN_US, default 600) to the due time,
 * and the walker lets the flip go. Never when no flip is held, never during
 * the loading-screen turbo; the guest's vblank ticks are not moved (the wait
 * never runs past the next tick; a poll's end it may pass by up to 1.5 ms,
 * as the old whole-ms wait could). No timer: the old wait.
 * [pace] per 5s (GCM_PACE_LOG=0 off), in both modes: how late held flips were
 * let go (release minus due, us), how late every flip on the cap's grid went
 * (held or not) and those the walker only reached after their due time, the
 * CPU-side interval between Present calls (what Afterburner / RTSS show, ms)
 * and how many reached 20 ms, the time from a flip's release to its Present
 * call, the precise wait's spin, how far the ticker's waits ran past the vblank
 * tick they began before (both modes), and how often a precise wait was cut at
 * the tick because the flip was due after it. */
static volatile long long s_pace_due_us;   /* due time of the flip this pass stopped at; 0: none */
static int s_pace_on = -1, s_pace_log = -1;
static long long s_pace_lead_us = 500, s_pace_spin_max_us = 600;
static HANDLE s_pace_timer;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#define PACE_SHORT_BINS 2000    /* 10 us bins, 0..20 ms */
#define PACE_IVL_BINS   10000   /* 10 us bins, 0..100 ms */
static struct {
    u32 late[PACE_SHORT_BINS]; long long late_max; u32 n_held, n_free;
    u32 alate[PACE_SHORT_BINS]; long long alate_max; u32 n_all, n_offgrid;   /* every flip on the grid */
    u32 clate[PACE_SHORT_BINS]; long long clate_max; u32 n_came;             /* not held: reached after its due time */
    u32 ivl[PACE_IVL_BINS];    long long ivl_max;  u32 n_ivl, n_ivl20; double ivl_sum, ivl_sq;
    u32 rel[PACE_SHORT_BINS];  long long rel_max;  u32 n_rel;
    u32 terr[PACE_SHORT_BINS]; long long terr_max; u32 n_terr;
    u32 tpast[PACE_SHORT_BINS]; long long tpast_max; u32 n_tpast, n_tpast500;   /* ticker waits that ran past their vblank tick: the old wait */
    u32 ppast[PACE_SHORT_BINS]; long long ppast_max; u32 n_ppast, n_ppast500;   /* ... the precise wait */
    u32 n_tclamp; long long tclamp_max;   /* precise waits cut at the tick (the flip was due after it) */
    u32 n_wait, n_kick; long long spin_us;
    long long win0, last_present, rel_t;
} s_pace;
static int s_pace_held;              /* the flip at the walker's head has been held for the cap */
static long long s_pace_held_due;

static long long gcm_pace_now_us(void)   /* the clock gcm_flip_pace_left_us uses */
{
#ifdef _WIN32
    static LARGE_INTEGER qpf; LARGE_INTEGER qpc;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc);
    return (long long)(qpc.QuadPart * 1000000.0 / (double)qpf.QuadPart);
#else
    extern unsigned long long ps3_ms_now(void);
    return (long long)ps3_ms_now() * 1000;
#endif
}

static int gcm_pace_precise(void)
{
    if (s_pace_on < 0) {
        const char* e = getenv("GCM_PACE_PRECISE");
        const int env_off = e && *e == '0';
        int on = !env_off;
        if ((e = getenv("GCM_PACE_LEAD_US")) && *e) s_pace_lead_us = atoll(e);
        if ((e = getenv("GCM_PACE_SPIN_US")) && *e) s_pace_spin_max_us = atoll(e);
        if (s_pace_lead_us < 0) s_pace_lead_us = 0; if (s_pace_lead_us > 4000) s_pace_lead_us = 4000;
        if (s_pace_spin_max_us < 0) s_pace_spin_max_us = 0; if (s_pace_spin_max_us > 4000) s_pace_spin_max_us = 4000;
#ifdef _WIN32
        if (on) {
            s_pace_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!s_pace_timer) on = 0;
        }
#else
        on = 0;
#endif
        if (on) fprintf(stderr, "[pace] precise flip release on (high-resolution timer to %lld us before the due time, "
                                "then a QPC spin of at most %lld us); GCM_PACE_PRECISE=0 turns it off%c",
                        s_pace_lead_us, s_pace_spin_max_us, 10);
        else fprintf(stderr, "[pace] precise flip release off (%s): the old wake-ups%c",
                     env_off ? "GCM_PACE_PRECISE=0" : "no high-resolution timer", 10);
        s_pace_on = on;
    }
    return s_pace_on;
}

static inline void gcm_pace_bin(u32* h, int nb, long long v, long long* mx, u32* n)
{
    if (v < 0) v = 0;
    const long long b = v / 10;
    h[b < nb ? b : nb - 1]++;
    if (v > *mx) *mx = v;
    (*n)++;
}
/* The q-quantile of a 10 us histogram, in us (the middle of its bin). */
static long long gcm_pace_q(const u32* h, int nb, u32 n, double q)
{
    if (!n) return 0;
    u32 k = (u32)(q * (double)n + 0.999999); if (k < 1) k = 1;
    u32 c = 0;
    for (int i = 0; i < nb; i++) { c += h[i]; if (c >= k) return (long long)i * 10 + 5; }
    return (long long)nb * 10;
}

/* The walker lets a flip go (the cap's slot is taken) at `now`, `late` us
 * after its due time (-1: off the grid). */
static void gcm_pace_note_release(long long now, long long late)
{
    if (!g_ps3_load_turbo) {
        if (s_pace_held) gcm_pace_bin(s_pace.late, PACE_SHORT_BINS, now - s_pace_held_due, &s_pace.late_max, &s_pace.n_held);
        else {
            s_pace.n_free++;
            if (late >= 0) gcm_pace_bin(s_pace.clate, PACE_SHORT_BINS, late, &s_pace.clate_max, &s_pace.n_came);
        }
        if (late >= 0) gcm_pace_bin(s_pace.alate, PACE_SHORT_BINS, late, &s_pace.alate_max, &s_pace.n_all);
        else s_pace.n_offgrid++;
    }
    s_pace_held = 0;
    s_pace.rel_t = now;
}

static int gcm_flip_pace_wait(void)
{
    if (!(gcm_flip_pace_mode() & 1)) return 0;
    long long due = 0, now = 0, late = -1;
    const long long left = gcm_flip_pace_left_us(&due, &now, &late);
    if (left > 0) {
        s_pace_held = 1; s_pace_held_due = due;
        if (!g_ps3_load_turbo && gcm_pace_precise()) s_pace_due_us = due;
        return 1;
    }
    if (now) gcm_pace_note_release(now, late);   /* now == 0: no cap */
    return 0;
}

#ifdef _WIN32
/* Sleep on the high-resolution timer until `wake`, or a kick. 1: kicked. */
static int gcm_pace_timer_wait(long long t, long long wake, long long limit)
{
    static int kick_on = -1;
    if (kick_on < 0) { const char* e = getenv("GCM_FIFO_KICK"); kick_on = (e && *e == '0') ? 0 : 1; }
    LARGE_INTEGER li; li.QuadPart = -(LONGLONG)((wake - t) * 10);   /* 100 ns units, relative */
    if (!SetWaitableTimer(s_pace_timer, &li, 0, NULL, NULL, FALSE)) return -1;
    HANDLE hs[2]; DWORD nh = 0;
    if (kick_on) hs[nh++] = (HANDLE)cellGcm_fifo_kick_event();
    hs[nh++] = s_pace_timer;
    const DWORD r = WaitForMultipleObjects(nh, hs, FALSE, (DWORD)((limit - t) / 1000 + 5));
    if (kick_on && r == WAIT_OBJECT_0) { CancelWaitableTimer(s_pace_timer); return 1; }
    if (r == WAIT_OBJECT_0 + nh - 1) gcm_pace_bin(s_pace.terr, PACE_SHORT_BINS, gcm_pace_now_us() - wake, &s_pace.terr_max, &s_pace.n_terr);
    return 0;
}
#endif

/* The ticker's wait (boot_main.cpp vblank_ticker), before it would sleep for
 * w_us (its next vblank tick or the end of its poll, whichever comes first);
 * tick_us: the time to the next vblank tick (not cut to the poll).
 * Returns 1 when it waited here: the last pass stopped on a flip held for the
 * cap that is due before the old wait could have ended. It waits to the due
 * time, or to the vblank tick when that comes first (spinning the last stretch
 * to it, as the old wait does), returns early on a kick. 0: the old wait. */
int cellGcm_fifo_pace_wait(double w_us, double tick_us)
{
    const long long due = s_pace_due_us;
    if (!due) return 0;
    s_pace_due_us = 0;   /* consumed: the next pass publishes it again if it stops on pace again */
#ifndef _WIN32
    (void)w_us; (void)tick_us; return 0;
#else
    if (s_pace_on <= 0 || !s_pace_timer) return 0;
    long long t = gcm_pace_now_us();
    const long long end = t + (w_us > 0.0 ? (long long)(w_us + 0.999) : 0);
    /* The old wait sleeps whole milliseconds and can end up to ~1 ms after its
     * end: when the flip is due later than that, it decides as before. */
    if (due >= end + 1500) return 0;
    /* A poll's end means nothing while the walker is held at the flip word (a
     * kick still ends the wait): the flip's due time instead. The next vblank
     * tick is never passed, in either case: when the flip is due after it, the
     * wait ends at the tick, and the walker's pass after the tick stops on the
     * flip again and publishes its due time for the next wait. */
    const long long tick = t + (tick_us > 0.0 ? (long long)tick_us : 0);
    long long target = due;
    if (target > tick) {
        target = tick;
        if (tick_us > w_us) {   /* the poll branch: this wait used to run on to the due time */
            s_pace.n_tclamp++;
            if (due - tick > s_pace.tclamp_max) s_pace.tclamp_max = due - tick;
        }
    }
    s_pace.n_wait++;
    if (target - s_pace_lead_us > t + 50) {
        const int k = gcm_pace_timer_wait(t, target - s_pace_lead_us, target);
        if (k < 0) return 0;
        if (k == 1) { s_pace.n_kick++; return 1; }   /* a kick: a walker pass runs now */
        t = gcm_pace_now_us();
    }
    /* The last stretch: spin on the clock (a sleep cannot end this precisely). */
    const long long s0 = t;
    while (t < target && t - s0 < s_pace_spin_max_us) {
        for (int i = 0; i < 16; i++) YieldProcessor();
        t = gcm_pace_now_us();
    }
    s_pace.spin_us += t - s0;
    return 1;
#endif
}

/* The ticker (boot_main.cpp), right after its wait, in both modes: the wait
 * (precise: cellGcm_fifo_pace_wait's, else the old one) began before the next
 * vblank tick and ended past_us after it. */
void cellGcm_pace_note_tick_past(double past_us, int precise)
{
    if (g_ps3_load_turbo) return;
    if (precise) {
        gcm_pace_bin(s_pace.ppast, PACE_SHORT_BINS, (long long)past_us, &s_pace.ppast_max, &s_pace.n_ppast);
        if (past_us >= 500.0) s_pace.n_ppast500++;
    } else {
        gcm_pace_bin(s_pace.tpast, PACE_SHORT_BINS, (long long)past_us, &s_pace.tpast_max, &s_pace.n_tpast);
        if (past_us >= 500.0) s_pace.n_tpast500++;
    }
}

/* Called right before each Present (rsx_live_draw.c ld_present), on the
 * walker thread. */
static u32 s_p2_833, s_p2_167, s_p2_333, s_p2_50;   /* gpupull step 3 [pace2] */
void cellGcm_pace_note_present(void)
{
    if (s_pace_log < 0) { const char* e = getenv("GCM_PACE_LOG"); s_pace_log = !(e && *e == '0'); }
    if (!s_pace_log) return;
    const long long now = gcm_pace_now_us();
    if (!g_ps3_load_turbo && s_pace.last_present) {
        const long long d = now - s_pace.last_present;
        gcm_pace_bin(s_pace.ivl, PACE_IVL_BINS, d, &s_pace.ivl_max, &s_pace.n_ivl);
        s_pace.ivl_sum += (double)d; s_pace.ivl_sq += (double)d * (double)d;
        if (d >= 20000) s_pace.n_ivl20++;
        if (d >= 8333) s_p2_833++;
        if (d >= 16667) s_p2_167++;
        if (d >= 33333) s_p2_333++;
        if (d >= 50000) s_p2_50++;
    }
    s_pace.last_present = now;
    if (s_pace.rel_t) {
        if (!g_ps3_load_turbo) gcm_pace_bin(s_pace.rel, PACE_SHORT_BINS, now - s_pace.rel_t, &s_pace.rel_max, &s_pace.n_rel);
        s_pace.rel_t = 0;
    }
    if (!s_pace.win0) { s_pace.win0 = now; return; }
    if (now - s_pace.win0 < 5000000) return;
    const u32 n = s_pace.n_ivl;
    const double mean = n ? s_pace.ivl_sum / n : 0.0;
    const double var = n ? s_pace.ivl_sq / n - mean * mean : 0.0;
    fprintf(stderr, "[pace] per 5s: precise %s, cap %.1f fps; flips %u (held %u, came late %u); release late us p50/p95/max"
                    " %lld/%lld/%lld; present interval ms p50/p95/p99/max %.2f/%.2f/%.2f/%.2f mean %.3f sd %.3f, >=20 ms %u of %u;"
                    " flip->present us p50/p95/max %lld/%lld/%lld; precise waits %u (kicked %u), spin %.1f ms,"
                    " timer late us p50/p95/max %lld/%lld/%lld; all flips late us p50/p95/max %lld/%lld/%lld (off grid %u);"
                    " came-late flips late us p50/p95/max %lld/%lld/%lld;"
                    " waits past the vblank tick: old %u, us p50/p99/max %lld/%lld/%lld, >=0.5 ms %u;"
                    " precise %u, us p50/p99/max %lld/%lld/%lld, >=0.5 ms %u; cut at the tick %u (up to %lld us)%c",
            s_pace_on > 0 ? "on" : "off", s_pace_period_us > 0 ? 1e6 / (double)s_pace_period_us : 0.0,
            s_pace.n_held + s_pace.n_free, s_pace.n_held, s_pace.n_free,
            gcm_pace_q(s_pace.late, PACE_SHORT_BINS, s_pace.n_held, 0.50), gcm_pace_q(s_pace.late, PACE_SHORT_BINS, s_pace.n_held, 0.95),
            s_pace.late_max,
            gcm_pace_q(s_pace.ivl, PACE_IVL_BINS, n, 0.50) / 1000.0, gcm_pace_q(s_pace.ivl, PACE_IVL_BINS, n, 0.95) / 1000.0,
            gcm_pace_q(s_pace.ivl, PACE_IVL_BINS, n, 0.99) / 1000.0, s_pace.ivl_max / 1000.0,
            mean / 1000.0, (var > 0.0 ? sqrt(var) : 0.0) / 1000.0, s_pace.n_ivl20, n,
            gcm_pace_q(s_pace.rel, PACE_SHORT_BINS, s_pace.n_rel, 0.50), gcm_pace_q(s_pace.rel, PACE_SHORT_BINS, s_pace.n_rel, 0.95),
            s_pace.rel_max,
            s_pace.n_wait, s_pace.n_kick, s_pace.spin_us / 1000.0,
            gcm_pace_q(s_pace.terr, PACE_SHORT_BINS, s_pace.n_terr, 0.50), gcm_pace_q(s_pace.terr, PACE_SHORT_BINS, s_pace.n_terr, 0.95),
            s_pace.terr_max,
            gcm_pace_q(s_pace.alate, PACE_SHORT_BINS, s_pace.n_all, 0.50), gcm_pace_q(s_pace.alate, PACE_SHORT_BINS, s_pace.n_all, 0.95),
            s_pace.alate_max, s_pace.n_offgrid,
            gcm_pace_q(s_pace.clate, PACE_SHORT_BINS, s_pace.n_came, 0.50), gcm_pace_q(s_pace.clate, PACE_SHORT_BINS, s_pace.n_came, 0.95),
            s_pace.clate_max,
            s_pace.n_tpast, gcm_pace_q(s_pace.tpast, PACE_SHORT_BINS, s_pace.n_tpast, 0.50),
            gcm_pace_q(s_pace.tpast, PACE_SHORT_BINS, s_pace.n_tpast, 0.99), s_pace.tpast_max, s_pace.n_tpast500,
            s_pace.n_ppast, gcm_pace_q(s_pace.ppast, PACE_SHORT_BINS, s_pace.n_ppast, 0.50),
            gcm_pace_q(s_pace.ppast, PACE_SHORT_BINS, s_pace.n_ppast, 0.99), s_pace.ppast_max, s_pace.n_ppast500,
            s_pace.n_tclamp, s_pace.tclamp_max, 10);
    { u32 k = (n + 99) / 100, left = k; double sum = 0.0;
      for (int b = PACE_IVL_BINS - 1; b >= 0 && left; b--) {
          u32 c = s_pace.ivl[b]; if (!c) continue; if (c > left) c = left;
          const double v = (b == PACE_IVL_BINS - 1) ? (double)s_pace.ivl_max : (double)b * 10.0 + 5.0;
          sum += v * c; left -= c;
      }
      const double low = k ? sum / (double)k / 1000.0 : 0.0;
      fprintf(stderr, "[pace2] per 5s: frames %u, >=8.33 ms %u, >=16.7 ms %u, >=33.3 ms %u, >=50 ms %u, slowest 1%% (%u) mean %.3f ms = %.1f fps%c",
              n, s_p2_833, s_p2_167, s_p2_333, s_p2_50, k, low, low > 0.0 ? 1000.0 / low : 0.0, 10);
      s_p2_833 = s_p2_167 = s_p2_333 = s_p2_50 = 0; }
    const long long lp = s_pace.last_present, rt = s_pace.rel_t;
    memset(&s_pace, 0, sizeof s_pace);
    s_pace.last_present = lp; s_pace.rel_t = rt; s_pace.win0 = now;
}

/* Set while this thread runs the FIFO walker (which holds s_gcm_fifo_lock and
 * must not sleep): a flip it decodes is paced by the walker instead. */
static PPU_THREAD_LOCAL int t_in_fifo_walker;

/* A flip the game requests by calling into libgcm directly (not through the
 * command stream): the calling thread waits for its slot, as it would for a
 * vsync flip on the real console. */
static void gcm_flip_pace_block(void)
{
    if (t_in_fifo_walker || !(gcm_flip_pace_mode() & 2)) return;
    for (;;) {
        const long long left = gcm_flip_pace_left_us(NULL, NULL, NULL);
        if (left <= 0) return;
#ifdef _WIN32
        if (left > 1500) Sleep((DWORD)((left - 1000) / 1000));
        else SwitchToThread();
#else
        usleep(left > 1000 ? (useconds_t)(left - 500) : 100);
#endif
    }
}

/* [walk] line, every 5 s: where the FIFO walker's time goes -- walking, or
 * idle after a pass that stopped for each reason (caught up with put: the
 * PPU has not written more; a blocked semaphore ACQUIRE: waiting for an SPU;
 * flip pacing; a held report; a park). Says whether the emulated RSX is the
 * bottleneck or is waiting for someone. GCM_WALK_STATS=0 turns it off. */
/* Running totals of the same split, never reset: [0] walking, [1+k] idle
 * after names[k]. rsx_live_draw.c's hitch line (LD_HITCH_MS) reads their
 * change over one slow frame. */
volatile unsigned long long g_gcm_walk_cum_us[8];
static void gcm_walk_stats(unsigned long long t0, unsigned long long t1)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_WALK_STATS"); on = !(e && *e == '0'); }
    if (!on) return;
    static const char* const names[] = { "caught-up", "sema-acquire", "flip-pace", "report-hold",
                                         "parked", "spu-hold", "other" };
    enum { NW = sizeof names / sizeof names[0] };
    static unsigned long long last_end, window, busy, idle[NW], n[NW];
    static int last_k = -1;
    if (last_k >= 0 && last_end) { idle[last_k] += t0 - last_end; n[last_k]++; g_gcm_walk_cum_us[1 + last_k] += t0 - last_end; }
    busy += t1 - t0;
    g_gcm_walk_cum_us[0] += t1 - t0;
    int k = NW - 1;
    for (int i = 0; i < NW - 1; i++)
        if (s_walk_why && strcmp(s_walk_why, names[i]) == 0) { k = i; break; }
    last_k = k; last_end = t1;
    if (!window) window = t0;
    if (t1 - window >= 5000000ull) {
        char line[512]; int o = snprintf(line, sizeof line, "[walk] per 5s: walking %llums; idle after",
                                         busy / 1000);
        for (int i = 0; i < NW; i++)
            if (n[i]) o += snprintf(line + o, sizeof line - (size_t)o, " %s %llums (%llu)",
                                    names[i], idle[i] / 1000, n[i]);
        printf("%s%c", line, 10);
        window = t1; busy = 0;
        for (int i = 0; i < NW; i++) idle[i] = n[i] = 0;
        /* GCM_PARK_STATS=1: the park episodes of the same 5 s. */
        if (s_pk_stats > 0)
            printf("[park] per 5s: %llu episodes, %llums parked (avg %lluus, max %lluus); "
                   "%llu rewritten by an SPU PUT, taken %lluus after it on average%c",
                   s_pks_n, s_pks_us / 1000, s_pks_n ? s_pks_us / s_pks_n : 0ull, s_pks_max_us,
                   s_pks_seen, s_pks_seen ? s_pks_notice_us / s_pks_seen : 0ull, 10);
        s_pks_n = s_pks_us = s_pks_max_us = s_pks_seen = s_pks_notice_us = 0;
        /* GCM_PARK_HOLD_US (or GCM_PARK_STATS): the same 5 s of park rewrites. */
        if (gcm_park_hold_us() || s_pk_stats > 0)
            printf("[park-hold] per 5s: %llu park rewrites by an SPU run, %llu tail PUTs after them; "
                   "walker held %llu times: %llu to the run's end, %llu to the %ldus bound "
                   "(avg %lluus, max %lluus)%s%c",
                   s_pkh_rewrites, s_pkh_tail, s_pkh_n, s_pkh_flush, s_pkh_timeout, s_pkh_us,
                   (s_pkh_flush + s_pkh_timeout) ? s_pkh_wait_us / (s_pkh_flush + s_pkh_timeout) : 0ull,
                   s_pkh_wait_max, s_pkh_full ? ", slots full" : "", 10);
        s_pkh_rewrites = s_pkh_tail = s_pkh_n = s_pkh_flush = s_pkh_timeout = 0;
        s_pkh_wait_us = s_pkh_wait_max = s_pkh_full = 0;
    }
}

/* LD_VTX_ASYNC_ZPASS (default on; only matters with LD_VTX_ASYNC): whether a
 * GET_REPORT waits for the queued vertex conversions first. God of War:
 * Ascension writes ~800 ZPASS (type 1) occlusion reports a frame, two or three
 * draws apart, so waiting at each one left the conversion workers nothing to
 * overlap (99% of jobs were joined at once).
 *
 * A type-1 report goes ahead without waiting, except one written into RsxSync's
 * ring of frame reports (GCM_ROUND_RING=<ea>,<count>, a title default), which
 * the game's SPU jobs act on. Every other report type, every label, semaphore,
 * reference, flip, user command, 2D write and the end of each pass still waits.
 *
 * Why that is safe. A queued conversion only READS guest memory (the vertex
 * spans its plan mapped) and writes the host upload ring; it never writes
 * guest memory, and the report it lets through carries nothing that depends on
 * it (a timestamp and the constant "visible" count). So no guest-visible value
 * can differ, whatever the guest does after reading the report. The one
 * possible difference is in the picture: a guest that took a ZPASS report as
 * proof that the RSX had fetched the vertices of every draw before it, and
 * rewrote those vertices within the microseconds before a worker converted
 * them, would see that one draw drawn from the new data for that one frame.
 * Real hardware writes a ZPASS report only after the draws before it have been
 * rasterized, so such a guest would be relying on that order; occlusion
 * results are read to decide what to draw next, and the buffers the SPUs
 * recycle are fenced by labels and RsxSync's reports, which still wait.
 * LD_VTX_ASYNC_ZPASS=0: every report waits, as before. */
volatile long long g_gcm_vx_report_skips;

/* MLAA_DIAG (2026-11-09, ascension host\mlaa\NOTES.md). The SPU MLAA start hang of God of War: Ascension: the walker
 * stops for good on a SEMAPHORE_ACQUIRE whose label the SPU post-process round never writes. When one ACQUIRE has
 * been waited on for 2, 8 and 30 s: the label (offset, EA, wanted and current value), the GET_REPORT holds for the
 * SPU tasks (how many, the longest, how many gave up with tasks still running), RsxSync's report ring and round
 * descriptor (GCM_ROUND_RING, GCM_ROUND_GATE: this title's addresses), and the SPURS tasks
 * (spu_taskset_diag_dump). Also one line for each of the first holds that give up with tasks still running. Only
 * prints in those cases. GCM_MLAA_DIAG=0: off. */
static int s_md_on = -1;
static volatile unsigned long long s_md_holds, s_md_hold_max_us, s_md_hold_gaveup, s_md_hold_long;
static volatile unsigned long long s_md_ring_holds, s_md_ring_max_us;   /* GCM_REPORT_SPU_HOLD_RING */
static int gcm_mlaa_diag_on(void)
{
    if (s_md_on < 0) { const char* e = getenv("GCM_MLAA_DIAG"); s_md_on = !(e && e[0] == '0'); }
    return s_md_on;
}
extern void spu_taskset_diag_dump(const char* why) __attribute__((weak));
static void gcm_mlaa_diag_mem(const char* what, u32 ea, u32 n)
{
    for (u32 o = 0; o < n; o += 32u) {
        char line[160]; int k = 0;
        for (u32 w = 0; w < 32u && o + w < n; w += 4u) k += snprintf(line + k, sizeof line - k, " %08X", vm_read32(ea + o + w));
        fprintf(stderr, "[mlaa-diag]   %s 0x%08X:%s%c", what, ea + o, line, 10);
    }
}
static void gcm_mlaa_diag_acquire(u32 off, u32 la, u32 want, u32 has, u32 get)
{
    if (!gcm_mlaa_diag_on()) return;
    extern unsigned long long ps3_ms_now(void);
    static u32 s_off = ~0u, s_want, s_get; static unsigned long long s_since, s_last; static int s_lvl;
    const unsigned long long now = ps3_ms_now();
    /* One wait = the same ACQUIRE re-polled pass after pass (every few ms while it blocks). */
    if (off != s_off || want != s_want || get != s_get || now - s_last > 500u) {
        s_off = off; s_want = want; s_get = get; s_since = now; s_lvl = 0;
    }
    s_last = now;
    static const unsigned long long steps[3] = { 2000u, 8000u, 30000u };
    if (s_lvl >= 3 || now - s_since < steps[s_lvl]) return;
    s_lvl++;
    fprintf(stderr, "[mlaa-diag] sema-acquire: the walker has waited %llu ms at get=0x%X for label offset 0x%X "
                    "(EA 0x%08X) to read 0x%08X; it reads 0x%08X%c", now - s_since, get, off, la, want, has, 10);
    fprintf(stderr, "[mlaa-diag]   GET_REPORT holds for SPU tasks so far: %llu, longest %llu us, %llu over 40 ms, "
                    "%llu gave up (GCM_REPORT_SPU_HOLD_MS) with tasks still running%c",
            s_md_holds, s_md_hold_max_us, s_md_hold_long, s_md_hold_gaveup, 10);
    fprintf(stderr, "[mlaa-diag]   ring reports held for the previous round: %llu, longest %llu us%c",
            s_md_ring_holds, s_md_ring_max_us, 10);
    gcm_mlaa_diag_mem("labels", (la & ~0x3Fu) - 0x40u, 0xC0u);
    const char* r = getenv("GCM_ROUND_RING");
    if (r) { char* end = NULL; const u32 lo = (u32)strtoull(r, &end, 16); u32 cnt = 0;
             if (end && *end == ',') cnt = (u32)strtoull(end + 1, NULL, 0);
             if (lo && cnt && cnt <= 64u) gcm_mlaa_diag_mem("report ring", lo, cnt * 16u); }
    const char* g = getenv("GCM_ROUND_GATE");
    if (g) { const u32 d = (u32)strtoull(g, NULL, 16); if (d) gcm_mlaa_diag_mem("round descriptor", d & ~0xFFu, 0x100u); }
    if (spu_taskset_diag_dump) spu_taskset_diag_dump("at the stuck ACQUIRE");
    fflush(stderr);
}
/* A GET_REPORT hold for the SPU tasks ended after held_us; gaveup: its time ran out with tasks still running. */
static void gcm_mlaa_diag_hold(unsigned long long held_us, int gaveup, long running, u32 get, u32 rtype, long cap_ms)
{
    s_md_holds++;
    if (held_us > s_md_hold_max_us) s_md_hold_max_us = held_us;
    if (held_us >= 40000u && ++s_md_hold_long <= 10u && !gaveup && gcm_mlaa_diag_on())
        fprintf(stderr, "[mlaa-diag] report-hold: SPU tasks held a GET_REPORT %llu us (cap %ld ms; long hold #%llu)%c",
                held_us, cap_ms, s_md_hold_long, 10);
    if (!gaveup) return;
    const unsigned long long n = ++s_md_hold_gaveup;
    if (!gcm_mlaa_diag_on() || (n > 20u && n % 500u)) return;
    fprintf(stderr, "[mlaa-diag] report-hold #%llu gave up after %llu us (cap %ld ms) with %ld SPU task(s) still running, "
                    "get=0x%X report type %u: the next report can now signal tasks still in this round%c",
            n, held_us, cap_ms, running, get, rtype, 10);
    if (n <= 3u && spu_taskset_diag_dump) spu_taskset_diag_dump("report-hold gave up");
}
/* GCM_REPORT_SPU_HOLD_RING: a frame's first ring report waited held_us for the previous round to end. */
static void gcm_mlaa_diag_ring_hold(unsigned long long held_us, long running, u32 get, long cap_ms)
{
    const unsigned long long n = ++s_md_ring_holds;
    if (held_us > s_md_ring_max_us) s_md_ring_max_us = held_us;
    if (!gcm_mlaa_diag_on() || (n > 10u && n % 1000u)) return;
    fprintf(stderr, "[mlaa-diag] ring report #%llu held %llu us at get=0x%X (cap %ld ms%s): the previous frame's "
                    "post-process round was still running (longest so far %llu us)%c",
            n, held_us, get, cap_ms, running > 0 ? ", gave up with tasks still running" : "", s_md_ring_max_us, 10);
}

static int gcm_report_needs_vtx_join(u32 v)
{
    static int skip_zpass = -1;
    static u32 ring_lo, ring_hi;
    if (skip_zpass < 0) {
        const char* e = getenv("LD_VTX_ASYNC_ZPASS");
        const char* r = getenv("GCM_ROUND_RING");
        unsigned long long lo = 0, n = 0;
        if (r) { char* end = NULL; lo = strtoull(r, &end, 16);
                 if (end && *end == ',') n = strtoull(end + 1, NULL, 0); }
        ring_lo = (u32)lo; ring_hi = (u32)(lo + n * 16ull);
        skip_zpass = !(e && e[0] == '0');
    }
    if (!skip_zpass || (v >> 24) != 1u) return 1;
    const int main_mem = s_report_dma == 0xFEED0001u || s_report_dma == 0xBAD68000u;
    if (main_mem) {
        const u32 rea = gcm_io2ea(v & 0xFFFFFFu);
        if (!rea || (rea >= ring_lo && rea < ring_hi)) return 1;
    }
    g_gcm_vx_report_skips++;
    return 0;
}

/* OCCL_RDIAG=1 (diagnostic, off by default; ascension/host/video/OCCLUSION-NOTES.md "Step 1"): who reads
 * the ZPASS report banks (EA 0x11238000-0x11264000: the two object banks and the PPU's special reports),
 * and how long after the walker's GET_REPORT wrote each slot. PPU loads are seen through PPU_RWATCH
 * (set it to 11238000,0x2C000: the lifted loads then go out of line; 16-byte vector loads stay unseen),
 * SPU DMA reads (GET, GETL elements, GETLLAR) of any image through spu_channels.c. Every 5 s, per reader
 * (PPU: guest function; SPU: image), an [ordiag] line: read events, slots read, the age of each slot read
 * since its last GET_REPORT in walker frames (0/1/2/3/4/5+/never) and in ms, the same for the first read
 * after a write, which words the PPU loads (+0 +4 +8 +12), and how many reads each written slot got before
 * its next write. The value word is written as today (0xFFFF). */
#define GCM_RD_LO 0x11238000u
#define GCM_RD_HI 0x11264000u
#define GCM_RD_NS ((GCM_RD_HI - GCM_RD_LO) / 16u)
#define GCM_RD_NR 24
typedef struct {
    u32 key; int src;
    u64 ev, slots, af[7], af1[7], am[9], am1[9], words[4], unwritten;
    u64 lag[4][6];   /* first slot of a read: frames since its GET (0/1/2/3+) x walker lag (0..4, 5+ or negative) */
    u32 ex_ea, ex_n, ex_lsa;
} gcm_rd_reader;
/* The game's frames: flips it queued (_cellGcmSetFlipCommand*) vs the flip markers the walker has reached
 * (s_flip_request_count): their difference is how many frames the walker is behind the game. */
volatile u32 g_gcm_ppu_flips;
static int s_rd_on = -1;
static SRWLOCK s_rd_lock = SRWLOCK_INIT;
static u64* s_rd_wt; static u32* s_rd_wf; static u8* s_rd_rc;
static u64 s_rd_rch[5], s_rd_writes, s_rd_t5;
static gcm_rd_reader s_rd_r[GCM_RD_NR]; static int s_rd_nr; static long s_rd_detail;
extern u32 rsx_live_draw_get_frames(void);
void (*volatile g_occl_rdiag_fn)(u32 ea, u32 n, int src, u32 who, u32 lsa);
static void gcm_rd_read(u32 ea, u32 n, int src, u32 who, u32 lsa)
{
    if (!n || ea >= GCM_RD_HI || ea + n <= GCM_RD_LO || !s_rd_wt) return;
    const u64 now = ps3_qpc_us();
    const u32 fr = rsx_live_draw_get_frames();
    AcquireSRWLockExclusive(&s_rd_lock);
    gcm_rd_reader* r = NULL;
    for (int i = 0; i < s_rd_nr; i++) if (s_rd_r[i].key == who && s_rd_r[i].src == src) { r = &s_rd_r[i]; break; }
    if (!r && s_rd_nr < GCM_RD_NR) { r = &s_rd_r[s_rd_nr++]; memset(r, 0, sizeof *r); r->key = who; r->src = src;
                                     r->ex_ea = ea; r->ex_n = n; r->ex_lsa = lsa; }
    if (r) {
        r->ev++;
        if (src == 0) r->words[(ea >> 2) & 3u]++;
        const u32 a = ea < GCM_RD_LO ? GCM_RD_LO : ea, e = ea + n > GCM_RD_HI ? GCM_RD_HI : ea + n;
        u32 d0 = ~0u; double ms0 = -1.0;
        for (u32 s = (a - GCM_RD_LO) >> 4; s <= (e - 1u - GCM_RD_LO) >> 4; s++) {
            r->slots++;
            if (!s_rd_wt[s]) { r->unwritten++; r->af[6]++; continue; }
            const u32 d = fr - s_rd_wf[s];
            const double ms = (double)(now - s_rd_wt[s]) / 1000.0;
            const int mb = ms < 1 ? 0 : ms < 2 ? 1 : ms < 4 ? 2 : ms < 8 ? 3 : ms < 16 ? 4 : ms < 33 ? 5 : ms < 66 ? 6 : ms < 133 ? 7 : 8;
            r->af[d > 5u ? 5u : d]++; r->am[mb]++;
            if (!s_rd_rc[s]) { r->af1[d > 5u ? 5u : d]++; r->am1[mb]++; }
            if (s_rd_rc[s] < 255u) s_rd_rc[s]++;
            if (d0 == ~0u) {
                d0 = d; ms0 = ms;
                const int lg = (int)(g_gcm_ppu_flips - s_flip_request_count);
                r->lag[d > 3u ? 3u : d][lg < 0 || lg > 4 ? 5 : lg]++;
            }
        }
        if (src != 4) { extern void rsx_live_draw_occl_read(u32 ea);   /* OCCL=1: what the read found */
          for (u32 s2 = a & ~15u; s2 < e; s2 += 16u) rsx_live_draw_occl_read(s2); }
        static long s_rd_puts;
        if (s_rd_detail < 160 || (s_rd_detail % 20000) == 0 || (src == 4 && s_rd_puts++ < 120))
            fprintf(stderr, "[ordiag] read %s who=0x%08X ea=0x%08X n=%u lsa=0x%05X frame=%u: first slot written %u frames / "
                            "%.2f ms before%c", src == 0 ? "ppu" : src == 1 ? "spu-get" : src == 2 ? "spu-getl" : src == 3 ? "spu-getllar" : "spu-put(lsa=value+8)",
                    who, ea, n, lsa, fr, d0, ms0, 10);
        s_rd_detail++;
    }
    ReleaseSRWLockExclusive(&s_rd_lock);
}
static void gcm_rd_init(void)
{
    if (s_rd_on >= 0) return;
    const char* e = getenv("OCCL_RDIAG");
    s_rd_on = e && e[0] == '1';
    if (!s_rd_on) return;
    s_rd_wt = (u64*)calloc(GCM_RD_NS, sizeof(u64)); s_rd_wf = (u32*)calloc(GCM_RD_NS, sizeof(u32));
    s_rd_rc = (u8*)calloc(GCM_RD_NS, 1);
    if (!s_rd_wt || !s_rd_wf || !s_rd_rc) { s_rd_on = 0; return; }
    g_occl_rdiag_fn = gcm_rd_read;
    fprintf(stderr, "[ordiag] OCCL_RDIAG=1: reads of EA 0x%08X-0x%08X timed against the walker's GET_REPORTs "
                    "(PPU needs PPU_RWATCH=11238000,0x2C000)%c", GCM_RD_LO, GCM_RD_HI, 10);
}
/* OCCL=1 (rsx_live_draw.c): a report's value word, written as GET_REPORT writes it, and read raw (no
 * PPU_RWATCH / OCCL_RDIAG hit). */
void gcm_occl_put_value(u32 rea, u32 v) { vm_write32(rea + 8u, v); }
u32 gcm_occl_peek_value(u32 rea) { u32 r; memcpy(&r, vm_base + rea + 8u, 4); return __builtin_bswap32(r); }
/* ...and replaced only if it still holds `expect` (the game's own 0xFFFFFFFF "pending" reset, an SPU PUT, must
 * win over a late count). The banks are read and written by plain DMA only (no lock-line reservations). */
int gcm_occl_cas_value(u32 rea, u32 expect, u32 v)
{
    volatile LONG* w = (volatile LONG*)(vm_base + rea + 8u);
    return InterlockedCompareExchange(w, (LONG)__builtin_bswap32(v), (LONG)__builtin_bswap32(expect)) ==
           (LONG)__builtin_bswap32(expect);
}
/* The walker, at each GET_REPORT into main memory. */
static void gcm_rd_write(u32 rea)
{
    if (rea < GCM_RD_LO || rea >= GCM_RD_HI) return;
    const u32 s = (rea - GCM_RD_LO) >> 4;
    const u64 now = ps3_qpc_us();
    AcquireSRWLockExclusive(&s_rd_lock);
    if (s_rd_wt[s]) s_rd_rch[s_rd_rc[s] > 4u ? 4u : s_rd_rc[s]]++;
    s_rd_wt[s] = now; s_rd_wf[s] = rsx_live_draw_get_frames(); s_rd_rc[s] = 0;
    s_rd_writes++;
    if (!s_rd_t5) s_rd_t5 = now;
    if (now - s_rd_t5 >= 5000000ull) {
        s_rd_t5 = now;
        fprintf(stderr, "[ordiag] per 5s: %llu slot writes; reads per written slot before its next write 0/1/2/3/4+: "
                        "%llu/%llu/%llu/%llu/%llu; flips queued by the game %u, reached by the walker %u, walker frames %u%c",
                s_rd_writes, s_rd_rch[0], s_rd_rch[1], s_rd_rch[2], s_rd_rch[3],
                s_rd_rch[4], g_gcm_ppu_flips, s_flip_request_count, rsx_live_draw_get_frames(), 10);
        for (int i = 0; i < s_rd_nr; i++) {
            gcm_rd_reader* r = &s_rd_r[i];
            if (!r->ev) continue;
            fprintf(stderr, "[ordiag]   %s who=0x%08X (e.g. ea=0x%08X n=%u lsa=0x%05X): %llu reads, %llu slots (never written %llu); "
                            "frames since the write 0/1/2/3/4/5+ %llu/%llu/%llu/%llu/%llu/%llu; ms <1/<2/<4/<8/<16/<33/<66/<133/more "
                            "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | first read after a write: frames %llu/%llu/%llu/%llu/%llu/%llu "
                            "ms %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | words +0/+4/+8/+12 %llu/%llu/%llu/%llu%c",
                    r->src == 0 ? "ppu" : r->src == 1 ? "spu-get" : r->src == 2 ? "spu-getl" : r->src == 3 ? "spu-getllar" : "spu-put", r->key,
                    r->ex_ea, r->ex_n, r->ex_lsa, r->ev, r->slots, r->unwritten,
                    r->af[0], r->af[1], r->af[2], r->af[3], r->af[4], r->af[5],
                    r->am[0], r->am[1], r->am[2], r->am[3], r->am[4], r->am[5], r->am[6], r->am[7], r->am[8],
                    r->af1[0], r->af1[1], r->af1[2], r->af1[3], r->af1[4], r->af1[5],
                    r->am1[0], r->am1[1], r->am1[2], r->am1[3], r->am1[4], r->am1[5], r->am1[6], r->am1[7], r->am1[8],
                    r->words[0], r->words[1], r->words[2], r->words[3], 10);
            fprintf(stderr, "[ordiag]     walker lag at the read (frames the game has queued and the walker not reached: "
                            "0/1/2/3/4/other) by frames since the slot's GET: 0: %llu/%llu/%llu/%llu/%llu/%llu | 1: "
                            "%llu/%llu/%llu/%llu/%llu/%llu | 2: %llu/%llu/%llu/%llu/%llu/%llu | 3+: %llu/%llu/%llu/%llu/%llu/%llu%c",
                    r->lag[0][0], r->lag[0][1], r->lag[0][2], r->lag[0][3], r->lag[0][4], r->lag[0][5],
                    r->lag[1][0], r->lag[1][1], r->lag[1][2], r->lag[1][3], r->lag[1][4], r->lag[1][5],
                    r->lag[2][0], r->lag[2][1], r->lag[2][2], r->lag[2][3], r->lag[2][4], r->lag[2][5],
                    r->lag[3][0], r->lag[3][1], r->lag[3][2], r->lag[3][3], r->lag[3][4], r->lag[3][5], 10);
            const u32 key = r->key; const int src = r->src; const u32 xe = r->ex_ea, xn = r->ex_n, xl = r->ex_lsa;
            memset(r, 0, sizeof *r); r->key = key; r->src = src; r->ex_ea = xe; r->ex_n = xn; r->ex_lsa = xl;
        }
        s_rd_writes = 0; memset(s_rd_rch, 0, sizeof s_rd_rch);
    }
    ReleaseSRWLockExclusive(&s_rd_lock);
}

/* GCM_FIFO_BATCH (default on): a method packet every word of which takes the
 * GCM_WALK_LEAN path (subchannel 0, the lean gates on, none of the methods the
 * walker acts on itself: 0x50, 0x1A8, 0x17C8, 0x1800, 0x1D70, 0x1D74) and
 * whose data words sit in one 1 MB IO page, four words or more (a shorter one
 * gains nothing), is walked as one unit: one IO-table
 * lookup for the packet instead of one per word, and the lean tests once per
 * packet. Each word then reaches rsx_live_draw_method (and, under
 * GCM_RSX_STATE=1 or for 0x1D6C, rsx_process_method) with the same method,
 * the same argument and the same g_gcm_method_ea, in the same order, as the
 * per-word loop gives it: within one IO page every word's IO-table lookup is
 * the first word's plus its offset. Anything else keeps the per-word loop.
 * GCM_FIFO_BATCH=0: the per-word loop for every packet.
 * GCM_FIFO_BATCH_VERIFY=1: each batched packet is also decoded the per-word
 * way and compared (method, argument, address, lean decision); mismatches are
 * counted on a [fifo-batch] line every 4096 batched packets (diagnostic). */
/* W2_PKSTATS=1 (ascension host/walk/walk2): subchannel-0 method packets by the class of their first method --
 * packets, data words, non-incrementing packets, packets taking the GCM_FIFO_BATCH path, and data-word counts in
 * buckets 0 / 1 / 2 / 3 / 4-7 / 8-15 / 16-63 / 64+. rsx_live_draw.c prints and clears them per [draws] window. */
extern volatile int g_w2_pkstats;
unsigned long long g_w2_pk[8][12];
/* W2_FIFO (walk2 cut 3, rsx_live_draw.c): NOP / constant / program packets stored whole. */
extern volatile int g_w2_fifo_act;
int rsx_live_draw_w2_packet(u32 cls, u32 type, u32 method, u32 count, u32 dea0);
int rsx_live_draw_w2_nop(u32 count, u32 last_arg);
static inline u32 w2_pk_class(u32 m)
{
    if (m == 0x0100u) return 0;                                                /* NOP */
    if (m >= 0x1EFCu && m < 0x1F80u) return 1;                                 /* constant upload */
    if ((m >= 0x0B80u && m < 0x0C00u) || m == 0x1E9Cu || m == 0x1EA0u) return 2; /* program upload */
    if (m == 0x1824u || m == 0x1808u || m == 0x1814u) return 3;                /* DRAW_INDEX_ARRAY, BEGIN_END, DRAW_ARRAYS */
    if (m >= 0x1A00u && m < 0x1C00u) return 4;                                 /* textures */
    if (m == 0x1D6Cu || m == 0x1D70u || m == 0x1D74u || m == 0x1800u || m == 0x17C8u || m < 0x0100u) return 5;  /* labels, reports */
    if (m >= 0x1500u && m < 0x1A00u) return 6;                                 /* 0x1500-0x19FF rest */
    return 7;
}
static inline void w2_pk_note(u32 type, u32 m, u32 count)
{
    unsigned long long* c = g_w2_pk[w2_pk_class(m)];
    c[0]++; c[1] += count; c[2] += type == 2;
    c[4 + (count == 0 ? 0 : count < 4 ? count : count < 8 ? 4 : count < 16 ? 5 : count < 64 ? 6 : 7)]++;
}
static int s_fifo_batch = -1, s_fifo_batch_verify;
static unsigned long long s_fifo_batch_pk, s_fifo_batch_words, s_fifo_batch_bad;
static int gcm_fifo_batch_on(void)
{
    if (s_fifo_batch < 0) {
        const char* e = getenv("GCM_FIFO_BATCH");
        const char* v = getenv("GCM_FIFO_BATCH_VERIFY");
        s_fifo_batch_verify = v && *v && *v != '0';
        s_fifo_batch = !(e && e[0] == '0');
    }
    return s_fifo_batch;
}
/* A word the walker acts on itself after (or instead of) the lean calls. */
static inline int gcm_walk_special(u32 m)
{
    return m == 0x50u || m == 0x1A8u || m == 0x17C8u || m == 0x1800u || m == 0x1D70u || m == 0x1D74u;
}
/* Does a packet of `count` words starting at `method` (type 0: incrementing,
 * type 2: non-incrementing) carry one of those methods? */
static int gcm_packet_has_special(u32 type, u32 method, u32 count)
{
    static const u32 sp[6] = { 0x50u, 0x1A8u, 0x17C8u, 0x1800u, 0x1D70u, 0x1D74u };
    if (type != 0) return gcm_walk_special(method);
    const u32 last = method + (count - 1u) * 4u;
    for (int k = 0; k < 6; k++)
        if (sp[k] >= method && sp[k] <= last) return 1;
    return 0;
}
/* GCM_METHOD_HASH=1: a hash of every (method, argument) the walker hands the
 * live engine, printed with the word count at every flip word -- the same
 * stream decoded two ways must give the same lines. Diagnostic, off. */
static int s_mhash_on = -1;
static unsigned long long s_mhash = 0xCBF29CE484222325ull, s_mhash_n, s_mhash_frame;
static inline void gcm_mhash(u32 m, u32 a)
{
    unsigned long long h = s_mhash;
    h = (h ^ m) * 0x100000001B3ull;
    h = (h ^ a) * 0x100000001B3ull;
    s_mhash = h; s_mhash_n++;
}
static void gcm_mhash_frame(void)
{
    if (s_mhash_on <= 0) return;
    printf("[mhash] flip %llu: %llu words, hash %016llX%c", ++s_mhash_frame, s_mhash_n, s_mhash, 10);
    s_mhash = 0xCBF29CE484222325ull; s_mhash_n = 0;
}


void cellGcm_rsx_process_fifo(void)
{
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    const unsigned long long t0 = ps3_qpc_us();
    t_in_fifo_walker = 1;
    s_pace_due_us = 0;   /* GCM_PACE_PRECISE: set again only if this pass stops on a held flip */
    gcm_rsx_process_fifo_unlocked();
    t_in_fifo_walker = 0;
    const unsigned long long t1 = ps3_qpc_us();
    __atomic_add_fetch(&g_perf_rsx_us, (long long)(t1 - t0), __ATOMIC_RELAXED);
    gcm_walk_stats(t0, t1);
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
}

static void gcm_rsx_process_fifo_unlocked(void)
{
    /* The walk's diagnostic gates, read once. A pass runs thousands of times
     * a second (every kick and every ticker iteration) and each getenv is a
     * hashed, string-compared lookup: these six were ~1% of the RSX thread
     * in God of War: Ascension's combat. The runtime's getenv caches values
     * for the whole run anyway, so reading them once changes nothing. */
    static int s_dbg_init = 0, s_dbg_rate, s_dbg_ctx, s_dbg_drain, s_dbg_scanref, s_dbg_recdbg;
    /* GCM_WALK_LEAN (default on): an ordinary NV4097 word on subchannel 0
     * goes straight to the live engine and rsx_process_method, past the
     * per-word diagnostic gates, when none of them is on (and the shared
     * draw engine is off). Those gates are ten tests and three re-reads of
     * the argument on each of ~150,000 words a frame; with all of them off
     * the full path does exactly these two calls for such a word. The words
     * the walker itself acts on after rsx_process_method (reference, report
     * DMA and GET_REPORT, CLEAR_REPORT_VALUE, the back-end and texture-read
     * label releases) keep the full path. GCM_WALK_LEAN=0: the full path. */
    /* GCM_RSX_STATE (default 0 under GCM_WALK_LEAN): rsx_process_method's
     * register shadow (s_state below) only feeds the null backend's
     * callbacks while the live engine draws -- log lines and the clear
     * colour of a GDI paint the swap chain covers. Its effects the guest
     * sees are the back-end/texture-read label releases (0x1D70/0x1D74,
     * at the offset 0x1D6C set) and the reference (0x50); lean words other
     * than 0x1D6C skip it (the others take the full path). Off with
     * RSX_TRACE, which traces it, and with PS3_MILESTONE_OUT, whose
     * rsx:draw_* counts it makes. GCM_RSX_STATE=1: every word, as before. */
    static int s_walk_lean, s_walk_stateless;
    if (!s_dbg_init) {
        { const char* lv = getenv("GCM_VAL_WATCH"); const char* ln = getenv("GCM_WALK_LEAN");
          s_walk_lean = !(ln && ln[0] == '0') && !(lv && strtoul(lv, 0, 16)) &&
                        !getenv("GCM_SUBCH_GATE") && !getenv("GCM_OBJDBG") &&
                        !getenv("RSX_LIVE_FEED_DBG") &&
                        rsx_live_draw_enabled() && !rsx_draw_engine_enabled(); }
        { const char* st = getenv("GCM_RSX_STATE");
          s_walk_stateless = s_walk_lean && !(st && st[0] == '1') && !getenv("RSX_TRACE") &&
                             !getenv("PS3_MILESTONE_OUT"); }
        s_dbg_rate    = getenv("GCM_RATE") != NULL;
        s_dbg_ctx     = getenv("GCM_CTXDBG") != NULL;
        s_dbg_drain   = getenv("GCM_DRAINDBG") != NULL;
        s_dbg_scanref = getenv("GCM_SCANREF") != NULL;
        s_dbg_recdbg  = getenv("GCM_RECDBG") != NULL;
        { const char* e = getenv("GCM_METHOD_HASH"); s_mhash_on = e && *e && *e != '0'; }
        s_dbg_init = 1;
    }
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      if (s_dbg_rate) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    static rsx_state s_state;
    static int  s_inited = 0;
    extern u32 g_rsx_last_reference;

    /* A statically-linked libgcm (PS3 firmware modules -- ps1_netemu) never calls
     * cellGcmSetupContext, so there is no CellGcmContextData EA to gate on: it gets
     * its ring through sys_rsx_context_allocate instead. The walk below only needs
     * `put` and the IO table, both of which the syscall layer fills; the one part
     * that does read the context (the ring-recycle heuristic) is separately guarded
     * by s_gcm_ctx_out_ea, which stays 0 on that path. */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }

    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    /* The words behind `put` were stored by another thread (the title, or the
     * recycle below) before it stored `put`. On x86 the hardware keeps loads
     * in order; on arm64 it does not, so without an acquire here the ring
     * word can be fetched before `put` was and come back stale -- the walker
     * then runs off the end of the previous frame's bytes until it meets a
     * word that is not a command, and parks there forever. */
    atomic_thread_fence(memory_order_acquire);
    /* The park the walker stood on has been rewritten: its episode ends with
     * this pass, which is when the walker sees it -- unless the job that
     * rewrote it is still writing the segment (GCM_PARK_HOLD_US). */
    int park_held = 0;
    if (s_pk_off != ~0u && s_pk_ea && vm_read32(s_pk_ea) != s_pk_word) {
        park_held = gcm_park_hold_wait();
        if (!park_held) gcm_park_end(1);
    }

    /* GCM_CTXDBG=1: which context the title is actually driving. Ours is written
     * once at init; if the title repoints gCellGcmCurrentContext at a buffer of
     * its own, every field below moves with it and the FIFO the runtime walks
     * stops being the one the title fills. Reported on change, not per tick. */
    if (s_dbg_ctx && s_gcm_ctx_out_ea) {
        static u32 last_ptr = 0xFFFFFFFFu, last_cur = 0xFFFFFFFFu;
        u32 ptr = vm_read32(s_gcm_ctx_out_ea);
        u32 cur = ptr ? vm_read32(ptr + 0x8) : 0;
        if (ptr != last_ptr || cur != last_cur) {
            last_ptr = ptr; last_cur = cur;
            fprintf(stderr, "[CTX] gCellGcmCurrentContext@0x%08X -> 0x%08X  "
                    "begin=%08X end=%08X current=%08X callback=%08X  (ours=0x%08X)\n",
                    s_gcm_ctx_out_ea, ptr,
                    ptr ? vm_read32(ptr + 0x0) : 0, ptr ? vm_read32(ptr + 0x4) : 0,
                    cur, ptr ? vm_read32(ptr + 0xC) : 0, s_gcm_context_ea);
            fflush(stderr);
        }
    }

    u32 start_off = s_fifo_getoff;
    if (s_dbg_drain) {
        static int n = 0;
        if (n++ % 60 == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    /* GCM_SCANREF=1 (one-shot): list every NV406E_SET_REFERENCE actually
     * present in 0..put. "The title waits for a fence we never published" has
     * two very different causes -- we missed the command, or it was never
     * submitted -- and only a direct scan of the ring separates them. */
    { static int scanned = 0;
      if (!scanned && s_dbg_scanref && put >= 0x19F0) { scanned = 1;
        unsigned found = 0;
        for (u32 io = 0; io + 8 <= put; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w & 0x1FFCu) == 0x0050u && (w >> 29) == 0) {
                u32 dea = gcm_io2ea(io + 4);
                fprintf(stderr, "[SCANREF] io=%08X w=%08X count=%u arg=%08X\n",
                        io, w, (w >> 18) & 0x7FFu, dea ? vm_read32(dea) : 0xDEADu);
                found++;
            }
        }
        fprintf(stderr, "[SCANREF] %u SET_REFERENCE in 0..%08X\n", found, put);
      } }

    /* GCM_FIFO_SNAP=N: dump N snapshots of the raw words at both ends of the
     * ring -- what `get` is about to decode, and what the title just wrote at
     * `put`. When get and put drift apart, the two windows say immediately
     * whether get is grinding through NOPs/garbage or the title is writing
     * somewhere get can never reach. The [FIFOSTEP] trace only ever fired on a
     * bad branch, which is exactly the case where the drift is silent. */
    { static int snap = -1;
      if (snap < 0) { const char* e = getenv("GCM_FIFO_SNAP"); snap = e ? atoi(e) : 0; }
      if (snap > 0 && put) {
          snap--;
          fprintf(stderr, "[SNAP] get=%08X put=%08X\n", s_fifo_getoff, put);
          for (int k = 0; k < 16; k++) {
              u32 gio = s_fifo_getoff + (u32)(k * 4), pio = put + (u32)(k * 4);
              u32 gea = gcm_io2ea(gio), pea = gcm_io2ea(pio);
              fprintf(stderr, "[SNAP]  get+%-3d io=%08X w=%08X   put+%-3d io=%08X w=%08X\n",
                      k * 4, gio, gea ? vm_read32(gea) : 0xDEADDEADu,
                      k * 4, pio, pea ? vm_read32(pea) : 0xDEADDEADu);
          }
          fflush(stderr);
      } }

    int budget = 0x100000;                    /* words per tick cap */
    /* GCM_DRAINDBG also reports WHY each pass stopped. A drain that ends far
     * short of `put` every tick is the signature of a FIFO that can never
     * catch up, and the reason is the whole diagnosis. */
    const char* why = "caught-up";
    while (s_fifo_getoff != put && budget-- > 0) {
        u32 ea = gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            /* get is sitting on an IO offset with no mapping -- the title
             * branched into a region and then unmapped it, or the offset was
             * never valid. Breaking silently (what this did) leaves get stuck
             * there FOREVER: it can never reach put, the FIFO stops draining,
             * no fences publish, and anything waiting on ctrl->ref hangs with
             * nothing logged. Resynchronise to put instead. Commands between
             * here and put are lost, which is bad, but a permanently dead FIFO
             * is worse -- and now it says so. */
            static u32 last_bad; static int n;
            if (s_fifo_getoff != last_bad || n < 4) {
                last_bad = s_fifo_getoff;
                if (n++ < 16)
                    fprintf(stderr, "[cellGcmSys] FIFO get=0x%08X has no IO "
                            "mapping -- resyncing to put=0x%08X\n",
                            s_fifo_getoff, put);
            }
            s_fifo_getoff = put;
            why = "no-io-mapping";
            break;
        }
        /* GCM_PARK_HOLD_US: stay on the rewritten park this pass. */
        if (park_held && s_fifo_getoff == s_pk_off) { why = "parked"; break; }
        u32 w = vm_read32(ea);
        /* Only remember offsets BELOW put -- those are positions in the
         * ring the title is writing. A park in another region is where the
         * walker gave up, not how far it got through the ring. */
        if (s_fifo_getoff < put) s_last_ring_off = s_fifo_getoff + 4;
        s_tr_off[s_tr_i % GCM_TRACE_N] = s_fifo_getoff;
        s_tr_w[s_tr_i % GCM_TRACE_N] = w;
        s_tr_i++;
        u32 type = w >> 29;

        if ((w & 0xFFFF0000u) == 0xFEAD0000u && gcm_flip_pace_wait()) {
            why = "flip-pace";   /* not consumed: the next pass reads it again */
            break;
        }
        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            extern s32 cellGcmSetFlipCommand(u32 bufferId);
            /* LD_VTX_ASYNC: no vertex conversion outlives a flip. */
            rsx_live_draw_vtx_join();
            gcm_mhash_frame();
            s_fifo_getoff += 4;
            /* Count the flip WORDS the title actually writes into the FIFO,
             * separately from the presents that result. When a title stops
             * appearing on screen the first question is which side stopped:
             * this counter still climbing while presents do not is a runtime
             * problem, both stopping together is the guest's. */
            { static unsigned long long flips = 0; static int dbg = -1;
              if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
              ++flips;
              if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
                  fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                          flips, 10);
              if (s_pk_log > 0) fprintf(stderr, "[gcm-flip] t=%lluus%c", ps3_qpc_us(), 10); }
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
                /* Queued by _cellGcmSetFlipCommand, which already did the
                 * guest-visible half of the request. */
                gcm_set_current_display_buffer(w & 7u);
                s_flip_request_count++;
                if (gcm_present_at_flip()) g_gcm_flip_present_hook();
                else s_flip_pending = 1;
            } else {
                cellGcmSetFlipCommand(w & 0xFFu);
            }
            /* ...unless the FIFO is badly backlogged. One flip per drain is
             * right while `get` is keeping up with `put`; when it is megabytes
             * behind it is a deadlock, because the title's ring can only be
             * reclaimed as `get` advances. VF5 submits ~5x faster than one
             * flip per drain consumes, so its 4 MB ring fills, its own
             * buffer-full callback can never free space, and AMGL prints
             * "Command Buffer Overflow" forever. Past the threshold keep
             * draining -- the intermediate presents are frames we are too far
             * behind to show anyway. No-op for a title that is keeping up. */
            if (put < s_fifo_getoff ||
                put - s_fifo_getoff < GCM_FIFO_CATCHUP_BYTES)
                { why = "flip"; break; }
            continue;
        }

        if (type == 1) {                       /* JUMP: 0x20000000 | offset */
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stderr, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, w & 0x1FFFFFFCu, put); }
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("JUMP", tgt, w); gcm_fifo_dump_around(s_fifo_getoff); gcm_fifo_resync_why("unmapped-JUMP", &s_fifo_getoff, put); break; }
              /* A JUMP to its own address is the "park the RSX here" idiom:
               * the title leaves it at the write head so the GPU stops if it
               * catches up, and overwrites it when the next segment is
               * appended. Taking it re-reads the same word forever -- 38.8 M
               * times in one Twisted Metal run, which is the drain burning the
               * CPU the guest needs. Stop this pass instead; the next one
               * re-reads the word, so a patched jump is still followed. */
              if (tgt == s_fifo_getoff) {
                  const u32 pk_at = s_fifo_getoff;   /* park episode (GCM_PARK_LOG) */
                  /* Parked. If the title has meanwhile moved `put` somewhere
                   * else, it has switched to its other segment and left this
                   * park standing -- it only patches a park when it reuses that
                   * block. Waiting for a patch that will not come freezes the
                   * FIFO for the rest of the run, so follow the write head. */
                  /* ...but not at once. A title that links its segments by
                   * patching the park moves `put` first and patches second:
                   * inFAMOUS 2 ends each of its main-memory command blocks in
                   * a park, writes the next commands at the ring start, sets
                   * put, and only then rewrites the park as a JUMP to them. A
                   * drain that lands between the two saw put elsewhere, went
                   * straight to it, and dropped everything from the ring
                   * start to put -- the frame's fence with it, so the title
                   * waited on that label for the rest of the run. Give the
                   * patch time to arrive, and resync only for a park that
                   * has stood with the same put for longer than any title
                   * takes between the two stores. */
                  /* The wait belongs to ONE park episode (merge 0.1.17a). This
                   * record used to survive the episode: the walker left the
                   * park when the title patched it, but park_off / park_put /
                   * park_since stayed, so the next park at the same site with
                   * the same put (God of War: Ascension keeps put at 0x00A80000
                   * and parks at segment heads such as 0x005500A4) counted
                   * from the OLD episode's start and, once that was more than
                   * GCM_PARK_WAIT_MS ago, resynced on its first pass -- a
                   * normal park of a few ms, the frame skipped, the game frozen
                   * for good (m17-s1-user: resync 1.3 ms after the walker
                   * arrived; every other resync came after the full 60 s).
                   * s_pk_off is the standing episode (gcm_park_begin below on
                   * every parked pass; gcm_park_end when the walker leaves it
                   * by any route, or at a resync): a park the walker does not
                   * already stand on starts its own wait. */
                  if (put != s_fifo_getoff) {
                      extern unsigned long long ps3_ms_now(void);
                      static u32 park_off, park_put;
                      static unsigned long long park_since;
                      const unsigned long long now = ps3_ms_now();
                      if (park_off != s_fifo_getoff || park_put != put || s_pk_off != s_fifo_getoff) {
                          park_off = s_fifo_getoff; park_put = put; park_since = now;
                      } else if (now - park_since >= gcm_park_grace_ms()) {
                          park_off = 0;
                          gcm_fifo_dump_around(s_fifo_getoff);
                          gcm_fifo_resync_why("jump-park", &s_fifo_getoff, put);
                      }
                  }
                  s_last_park_off = s_fifo_getoff;
                  if (!gcm_is_park_site(s_fifo_getoff))
                      s_park_sites[s_park_sites_n++ % GCM_PARK_SITES] = s_fifo_getoff;
                  if (s_park_sites_n > GCM_PARK_SITES) s_park_sites_n = GCM_PARK_SITES;
                  if (s_fifo_getoff == pk_at) gcm_park_begin(pk_at);
                  why = "parked";
                  break;
              }
              /* GCM_PARK_SPU_HOLD_MS=<ms>: a park the title has now patched
               * into a JUMP starts its next segment, the next frame. A real
               * RSX spends a frame's time drawing it; this one spends none,
               * so the frame's reports land while the SPU work they feed is
               * still on the last one. Hold the patched park while SPURS
               * tasks are running (out of WAIT_SIGNAL), for up to <ms>.
               * God of War: Ascension skips its post-process ACQUIRE on the
               * first frame and relies on that time: RsxSync's signal for
               * round 2 was eaten by tasks still in round 1's barriers. Off
               * by default. */
              if (s_fifo_getoff == s_last_park_off || gcm_is_park_site(s_fifo_getoff)) {
                  static long hold = -1; static unsigned long long held_since;
                  if (hold < 0) { const char* e = getenv("GCM_PARK_SPU_HOLD_MS"); hold = e ? atol(e) : 0; }
                  extern long spu_taskset_tasks_running(void) __attribute__((weak));
                  if (hold > 0 && !g_gcm_park_kick_off && spu_taskset_tasks_running && spu_taskset_tasks_running() > 0) {
                      extern unsigned long long ps3_ms_now(void);
                      const unsigned long long now = ps3_ms_now();
                      if (!held_since) held_since = now;
                      if (now - held_since < (unsigned long long)hold) { why = "spu-hold"; break; }
                  }
                  { static int lg = -1; if (lg < 0) lg = getenv("GCM_HOLD_LOG") ? 1 : 0;
                    if (lg) { extern unsigned long long ps3_qpc_us(void);
                      fprintf(stderr, "[gcm-hold] t=%lluus park=0x%X parked=%d held=%llums\n",
                              ps3_qpc_us(), s_fifo_getoff, s_fifo_getoff == s_last_park_off,
                              held_since ? ps3_ms_now() - held_since : 0ull); } }
                  held_since = 0;
                  s_last_park_off = ~0u;
              }
              if (s_fifo_getoff == s_pk_off) gcm_park_end(1);   /* the park, rewritten, taken */
              gcm_br_note(s_fifo_getoff, tgt, w, put);
              s_fifo_getoff = tgt; }
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("CALL", tgt, w); gcm_fifo_resync_why("unmapped-CALL", &s_fifo_getoff, put); break; }
              s_fifo_calloff = s_fifo_getoff + 4;
              gcm_br_note(s_fifo_getoff, tgt, w, put);
              s_fifo_getoff  = tgt; }
            continue;
        }
        if (w == 0x00020000u) {                /* RET */
            gcm_br_note(s_fifo_getoff, s_fifo_calloff, w, put);
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = 0;
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            if (__builtin_expect(g_w2_pkstats, 0) && subch == 0) w2_pk_note(type, method, count);   /* W2_PKSTATS */
            /* TCONST_FIFO=1: how the title actually streams vertex constants.
             * NV4097_SET_TRANSFORM_CONSTANT is a 32-register window; if a block
             * arrives NON-INCREMENTING then every dword carries the same method
             * and any handler that derives the constant slot from the method
             * offset writes them all on top of each other. */
            { static int tf = -1;
              if (tf < 0) { const char* e = getenv("TCONST_FIFO"); tf = e ? atoi(e) : 0; }
              if (tf && subch == 0) {
                  static u32 hist[2048]; static u32 tot[2048]; static int dumped = 0;
                  u32 mi = (method >> 2) & 2047;
                  hist[mi]++; tot[mi] += count;
                  static u64 seen = 0;
                  if (++seen == 200000ull && !dumped) { dumped = 1;
                      fprintf(stderr, "[TCFIFO] method histogram (blocks, dwords):%c", 10);
                      for (u32 k = 0; k < 2048; k++) if (hist[k])
                          fprintf(stderr, "[TCFIFO]   0x%04X  %8u blocks %10u dwords%c",
                                  k << 2, hist[k], tot[k], 10);
                  }
              } }
            /* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
             *
             * These were falling through to rsx_process_method as "unknown method" --
             * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
             * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
             * RSX is supposed to write and never sees it move, so it never appends its
             * next FIFO segment and the GPU sits on a park forever.
             *
             * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
             * the method, so the next pass re-reads it -- which is what the hardware does
             * while it waits. Semaphore offsets index the same label window
             * cellGcmGetLabelAddress hands out. */
            if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) {
                /* LD_VTX_ASYNC: a RELEASE tells the guest the draws before it
                 * are done with their memory. */
                rsx_live_draw_vtx_join();
                int sem_blocked = 0;
                for (u32 i = 0; i < count; i++) {
                    u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea) break;
                    u32 m = (type == 0) ? method + i * 4 : method;
                    u32 v = vm_read32(dea);
                    u32 la = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
                    { static int sn = 0; if (s_dbg_recdbg && sn++ < 12)
                        fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X\n", m, v, s_sema_offset); }
                    /* GCM_SEMALOG=1: every RELEASE, and each ACQUIRE once per (offset, value)
                     * with what the label holds then. */
                    { static int sl = -1; static u32 last_off = ~0u, last_v = ~0u;
                      if (sl < 0) sl = getenv("GCM_SEMALOG") ? 1 : 0;
                      if (sl && m == 0x6Cu)
                          printf("[sema] t=%llu RELEASE off=0x%X <- 0x%08X get=0x%X%c", (unsigned long long)ps3_ms_now(), s_sema_offset, v, s_fifo_getoff, 10);
                      if (sl && m == 0x68u && (s_sema_offset != last_off || v != last_v)) {
                          last_off = s_sema_offset; last_v = v;
                          printf("[sema] t=%llu ACQUIRE off=0x%X want 0x%08X has 0x%08X get=0x%X%c", (unsigned long long)ps3_ms_now(),
                                 s_sema_offset, v, vm_read32(la), s_fifo_getoff, 10); } }
                    if (m == 0x64u)      s_sema_offset = v;
                    else if (m == 0x6Cu) vm_write32(la, v);
                    else if (m == 0x68u && vm_read32(la) != v) {
            /* GCM_SEMA_ACQUIRE=1 makes ACQUIRE actually block, which is what
             * the hardware does. Off by default: a title whose label nothing
             * ever writes would wedge the drain permanently, and RELEASE on
             * its own is the half that unblocks a waiting guest. */
            static int blk = -1;
            if (blk < 0) { const char* e = getenv("GCM_SEMA_ACQUIRE"); blk = e ? atoi(e) : 0; }
            if (blk) { sem_blocked = 1; gcm_mlaa_diag_acquire(s_sema_offset, la, v, vm_read32(la), s_fifo_getoff); break; }
        }
                }
                if (sem_blocked) { why = "sema-acquire"; break; }
                s_fifo_getoff += 4 + count * 4;
                continue;
            }
            /* GCM_FLIP_PACE_MS=<ms>: a flip in the command stream waits until that
             * long after the previous one, the way a vsync flip holds a real RSX
             * (the pass stops here without consuming the method, as a blocked
             * ACQUIRE does). Off by default. Recompiled, the RSX side runs far
             * faster than the SPU work it is meant to overlap: God of War:
             * Ascension's frame 2 reports used to land while its SPU post-process
             * tasks were still on frame 1, and RsxSync's signal for the next
             * round was eaten by their barrier waits. */
            if ((((subch << 13) | method) == 0xE920u || ((subch << 13) | method) == 0xE924u) &&
                gcm_flip_pace_wait()) { why = "flip-pace"; break; }
            /* GCM_REPORT_SPU_HOLD_MS=<ms>: a GET_REPORT waits, for up to <ms>,
             * while SPURS tasks are out of WAIT_SIGNAL or still have a signal
             * they have not picked up. A title that turns reports into task
             * signals (God of War: Ascension's RsxSync job signals its
             * post-process taskset once per report) counts on the RSX taking
             * a frame's time between two of them; this one takes none, and
             * when the PPU side is quick the next report lands while the
             * tasks are still in the last round's barriers, which eat the
             * signal: the round never ends and the GPU waits on its label
             * forever. Like a blocked ACQUIRE, the pass stops before the
             * method and the next pass re-reads it. Off by default. */
            if (subch == 0 && (type == 0 ? (method <= 0x1800u && 0x1800u < method + count * 4u)
                                         : method == 0x1800u)) {
                static long rhold = -1; static unsigned long long rheld_since; static unsigned long rheld_n;
                if (rhold < 0) { const char* e = getenv("GCM_REPORT_SPU_HOLD_MS"); rhold = e ? atol(e) : 0; }
                extern long spu_taskset_tasks_running(void) __attribute__((weak));
                /* Only the reports a task signal can hang on: a ZPASS pixel
                 * count (type 1) feeds no signal, and Ascension's level issues
                 * ~800 of them a frame, each held while any task ran -- the
                 * RSX sat idle for the post-process window every frame.
                 * GCM_REPORT_HOLD_ALL=1: hold every type, as before. */
                static int rhold_all = -1;
                if (rhold_all < 0) rhold_all = getenv("GCM_REPORT_HOLD_ALL") ? 1 : 0;
                u32 rtype = 0, rword = 0;
                { const u32 ri = type == 0 ? (0x1800u - method) / 4u : 0u;
                  const u32 rdea = gcm_io2ea(s_fifo_getoff + 4 + ri * 4);
                  if (rdea) { rword = vm_read32(rdea); rtype = rword >> 24; } }
                /* GCM_REPORT_SPU_HOLD_RING (default on while the hold is on and GCM_ROUND_RING is set; ascension
                 * host\mlaa\NOTES.md): the first report of a frame into RsxSync's ring -- the one RsxSync turns
                 * into the signal that starts the next post-process round -- waits like the placeholders while
                 * the tasks still run the previous frame's round. The placeholder after a frame's second ring
                 * report is not always held: in 16% of God of War: Ascension's frames here (SPU MLAA on) the
                 * walker passed it before RsxSync had woken the tasks, and only the frame pacing then kept the
                 * next frame's report from signalling tasks still in their barrier (a lost round: the walker
                 * waits for its label for good). Enough for a ~4 ms round, not for a slow PC's. =0: off;
                 * =2 (test only): the ring hold alone, the placeholders are not held. */
                static int rring = -1; static u32 rr_lo, rr_hi, rr_flip = ~0u;
                if (rring < 0) {
                    const char* e = getenv("GCM_REPORT_SPU_HOLD_RING");
                    const char* r = getenv("GCM_ROUND_RING");
                    unsigned long long lo = 0, n = 0;
                    if (r) { char* end = NULL; lo = strtoull(r, &end, 16);
                             if (end && *end == ',') n = strtoull(end + 1, NULL, 0); }
                    rr_lo = (u32)lo; rr_hi = (u32)(lo + n * 16ull);
                    rring = !(e && e[0] == '0') && rr_lo && rr_hi > rr_lo ? (e && e[0] == '2' ? 2 : 1) : 0;
                }
                int ring_rep = 0, ring_first = 0;
                if (rring && rtype == 1u && (s_report_dma == 0xFEED0001u || s_report_dma == 0xBAD68000u)) {
                    const u32 rea = gcm_io2ea(rword & 0xFFFFFFu);
                    if (rea >= rr_lo && rea < rr_hi) { ring_rep = 1; ring_first = s_flip_request_count != rr_flip; }
                }
                if (ring_rep && !rhold_all) {
                    if (rhold > 0 && ring_first && spu_taskset_tasks_running && spu_taskset_tasks_running() > 0) {
                        const unsigned long long now = ps3_qpc_us();
                        if (!rheld_since) rheld_since = now;
                        if (now - rheld_since < (unsigned long long)rhold * 1000ull) { why = "report-hold"; break; }
                    }
                    if (rheld_since) {
                        const unsigned long long now = ps3_qpc_us();
                        gcm_mlaa_diag_ring_hold(now - rheld_since, spu_taskset_tasks_running ? spu_taskset_tasks_running() : 0,
                                                s_fifo_getoff, rhold);
                        __atomic_add_fetch(&g_perf_hold_us, (long long)(now - rheld_since), __ATOMIC_RELAXED);
                        rheld_since = 0;
                    }
                    rr_flip = s_flip_request_count;
                } else
                if (rhold > 0 && (rhold_all || rtype != 1) && rring != 2) {
                    /* QPC, not ps3_ms_now: GetTickCount64 steps by ~15.6 ms,
                     * which is a frame. */
                    const unsigned long long now = ps3_qpc_us();
                    long rrun = 0; int rgaveup = 0;
                    if (spu_taskset_tasks_running && (rrun = spu_taskset_tasks_running()) > 0) {
                        if (!rheld_since) rheld_since = now;
                        if (now - rheld_since < (unsigned long long)rhold * 1000ull) { why = "report-hold"; break; }
                        rgaveup = 1;
                    }
                    if (rheld_since) gcm_mlaa_diag_hold(now - rheld_since, rgaveup, rrun, s_fifo_getoff, rtype, rhold);
                    { static int lg = -1; if (lg < 0) lg = getenv("GCM_HOLD_LOG") ? 1 : 0;
                      if (lg && rheld_since)
                        fprintf(stderr, "[report-hold] t=%lluus #%lu get=0x%X held=%lluus\n",
                                now, ++rheld_n, s_fifo_getoff, now - rheld_since); }
                    if (rheld_since) __atomic_add_fetch(&g_perf_hold_us, (long long)(now - rheld_since), __ATOMIC_RELAXED);
                    rheld_since = 0;
                }
            }
            /* GCM_FIFO_BATCH: the whole packet as one unit (see above). */
            if (s_walk_lean && subch == 0 && count >= 4u && gcm_fifo_batch_on()) {
                const u32 io0 = s_fifo_getoff + 4u, io1 = io0 + (count - 1u) * 4u;
                const u32 dea0 = (io0 >> 20) == (io1 >> 20) &&
                                 !gcm_packet_has_special(type, method, count) ? gcm_io2ea(io0) : 0u;
                if (dea0) {
                    extern u32 g_gcm_method_ea;
                    const u32 step = type == 0 ? 4u : 0u;
                    if (s_fifo_batch_verify) {
                        unsigned bad = 0;
                        for (u32 i = 0; i < count; i++) {
                            const u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                            const u32 m = (type == 0) ? method + i * 4 : method;
                            const int lean = s_walk_lean && subch == 0 && !gcm_walk_special(m);
                            if (dea != dea0 + i * 4u || m != method + i * step || !lean ||
                                vm_read32(dea) != vm_read32(dea0 + i * 4u)) bad++;
                        }
                        s_fifo_batch_bad += bad != 0;
                        if (bad && s_fifo_batch_bad <= 8)
                            fprintf(stderr, "[fifo-batch] MISMATCH get=0x%X method=0x%04X count=%u type=%u: %u words%c",
                                    s_fifo_getoff, method, count, type, bad, 10);
                    }
                    /* W2_FIFO: a NOP / constant / program packet as one store (same register-file results) */
                    if (__builtin_expect(g_w2_fifo_act, 0) && s_walk_stateless) {
                        const u32 w2c = method == 0x0100u ? 1u : (method >= 0x1EFCu && method < 0x1F80u) ? 2u :
                                        (method >= 0x0B80u && method < 0x0C00u) ? 3u : 0u;
                        if (w2c && rsx_live_draw_w2_packet(w2c, type, method, count, dea0)) {
                            extern u32 g_gcm_method_ea;
                            if (s_mhash_on > 0) for (u32 i = 0; i < count; i++) gcm_mhash(method + i * step, vm_read32(dea0 + i * 4u));
                            g_gcm_method_ea = dea0 + (count - 1u) * 4u;
                            s_fifo_getoff += 4 + count * 4;
                            continue;
                        }
                    }
                    s_fifo_batch_pk++; s_fifo_batch_words += count;
                    if (__builtin_expect(g_w2_pkstats, 0)) g_w2_pk[w2_pk_class(method)][3]++;   /* W2_PKSTATS */
                    if (s_fifo_batch_verify && (s_fifo_batch_pk & 4095u) == 0)
                        fprintf(stderr, "[fifo-batch] %llu packets, %llu words batched; %llu mismatching packets%c",
                                s_fifo_batch_pk, s_fifo_batch_words, s_fifo_batch_bad, 10);
                    u32 m = method;
                    for (u32 i = 0; i < count; i++, m += step) {
                        const u32 dea = dea0 + i * 4u;
                        const u32 arg = vm_read32(dea);
                        g_gcm_method_ea = dea;
                        if (s_mhash_on > 0) gcm_mhash(m, arg);
                        rsx_live_draw_method(m, arg);
                        if (!s_walk_stateless || m == 0x1D6Cu)
                            rsx_process_method(&s_state, m, arg);
                    }
                    s_fifo_getoff += 4 + count * 4;
                    continue;
                }
            }
            /* W2_FIFO: a NOP packet of 1-3 words (the batch path above takes 4+) as one store. */
            if (__builtin_expect(g_w2_fifo_act, 0) && s_walk_lean && s_walk_stateless && subch == 0 && method == 0x0100u &&
                count && count < 4u && (type == 2 || count == 1)) {
                u32 dl = 0; int ok = 1;
                for (u32 i = 0; i < count; i++) { dl = gcm_io2ea(s_fifo_getoff + 4 + i * 4); if (!dl) { ok = 0; break; } }
                if (ok && rsx_live_draw_w2_nop(count, vm_read32(dl))) {
                    extern u32 g_gcm_method_ea;
                    if (s_mhash_on > 0) for (u32 i = 0; i < count; i++) gcm_mhash(method, vm_read32(gcm_io2ea(s_fifo_getoff + 4 + i * 4)));
                    g_gcm_method_ea = dl;
                    s_fifo_getoff += 4 + count * 4;
                    continue;
                }
            }
            for (u32 i = 0; i < count; i++) {
                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                if (!dea) break;
                u32 m = (type == 0) ? method + i * 4 : method;
                /* GCM_WALK_LEAN: an ordinary state word (see the top). */
                if (s_walk_lean && subch == 0 && m != 0x50u && m != 0x1A8u && m != 0x17C8u &&
                    m != 0x1800u && m != 0x1D70u && m != 0x1D74u) {
                    extern u32 g_gcm_method_ea;
                    const u32 arg = vm_read32(dea);
                    g_gcm_method_ea = dea;
                    if (s_mhash_on > 0) gcm_mhash(m, arg);
                    rsx_live_draw_method(m, arg);
                    if (!s_walk_stateless || m == 0x1D6Cu)
                        rsx_process_method(&s_state, m, arg);
                    continue;
                }
                /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
                 * splits into subchannel 7 / method 0x0B00 -- so it was landing
                 * in the 2D engine path and being dropped as unrecognised. It
                 * is not a 2D method at all: it tells the RSX to raise a
                 * USER_CMD interrupt, which is what ps1_netemu's flip path
                 * blocks on. */
                { static int sd = -1; static unsigned long s7 = 0;
                  if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
                  if (sd && subch == 7 && (s7++ < 16))
                      fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n",
                              m, vm_read32(dea)); }
                if (subch == 7 && m == 0x0B00u) {
                    extern void rsx_raise_user_cmd(u32 arg);
                    rsx_live_draw_vtx_join();   /* LD_VTX_ASYNC */
                    rsx_raise_user_cmd(vm_read32(dea));
                    /* A title that registered its handler with the HLE
                     * cellGcmSetUserHandler has no lv2 interrupt thread
                     * reading that event; its handler runs from the pump.
                     * The `continue` skips the 0xEB00 case below, so queue
                     * it for the pump here. */
                    if (s_user_handler_opd) cellGcmQueueUserCommand(vm_read32(dea));
                    continue;
                }
                /* GCM_SUBCH1_3D=1: treat subchannel 1 as the 3D object too.
                 * The title issues NV4097 methods on it -- 0x1A80..0x1AC0 is
                 * SET_TEXTURE_OFFSET for units 4-6 -- and the 2D path silently
                 * discards whatever it does not recognise, so that state never
                 * reaches the renderer. SET_OBJECT binds are not tracked, which
                 * is what makes the subchannel-to-engine mapping a guess. */
                /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
                 * title binds to a subchannel is what says which engine that
                 * subchannel drives; comparing subchannel 1 against subchannel 0
                 * turns the routing below from a guess into a reading. */
                { static int od = -1;
                  if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
                  if (od && m == 0) {
                      static unsigned seen[8]; static int have[8];
                      const unsigned h = vm_read32(dea);
                      if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                          have[subch] = 1; seen[subch] = h;
                          printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                                 subch, h, 10); fflush(stdout);
                      } } }
                /* The subchannel is a binding slot, not an engine selector: a
                 * title may bind NV4097 to something other than subchannel 0.
                 * Twisted Metal uses subchannel 1, and caner (canersaka) hit
                 * the same thing in Yakuza Dead Souls, where SPU-built command
                 * lists bind NV4097 elsewhere -- his rsx_live_draw.c masks the
                 * subchannel out of the method for exactly this reason.
                 *
                 * gcm_2d_method only ever handles subchannels 2..7 (NV3062,
                 * NV308A/NV309E, NV3089), so anything arriving on 1 was being
                 * dropped on the floor -- for this title that included
                 * SET_SHADER_PROGRAM, leaving one stale fragment program bound
                 * for every draw in the game. Treat the subchannels the 2D path
                 * does not claim as 3D. GCM_SUBCH1_2D=1 restores the old split.
                 */
                static int s1_2d = -1;
                if (s1_2d < 0) { const char* e = getenv("GCM_SUBCH1_2D"); s1_2d = e ? atoi(e) : 0; }
                /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
                 * non-zero subchannel -- what each binding slot actually carries. */
                { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
                  if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
                  const u32 cm3 = m & 0x1FFCu;
                  /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
                  { static u32 vw = 1;
                    if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
                    if (vw && vm_read32(dea) == vw) {
                        static int vn; if (vn++ < 64)
                            printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, s_fifo_getoff, 10);
                    } }
                  /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
                  if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                                       cm3 == 0x1A20)) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, vm_read32(dea), 10);
                  }
                  if (sg && subch) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, vm_read32(dea), 10);
                  } }
                /* Mirror the whole method stream into the live NV4097->D3D12
                 * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
                 * It wants the raw method with its subchannel bits -- it
                 * canonicalises with & 0x1FFC itself. */
                { static int live = -1;
                  if (live < 0) live = rsx_live_draw_enabled();
                  extern u32 g_gcm_method_ea;
                  g_gcm_method_ea = dea;   /* where this argument came from (LD_TEXOFF_DBG) */
                  if (live) {
                      if (s_mhash_on > 0) gcm_mhash((subch << 13) | m, vm_read32(dea));
                      rsx_live_draw_method((subch << 13) | m, vm_read32(dea));
                  } }

                /* And into the platform-neutral register-file draw engine,
                 * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
                 * It wants the raw method with its subchannel bits for the
                 * same reason the live engine does, and masks them itself. */
                { static int eng = -1;
                  if (eng < 0) eng = rsx_draw_engine_enabled();
                  if (eng) rsx_draw_engine_method((subch << 13) | m, vm_read32(dea)); }

                /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
                 * for any run, live engine or not, so the method stream and the
                 * backend's draw callbacks can be compared in one measurement. */
                { static int fdbg = -1;
                  if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
                  if (fdbg) {
                      static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
                      const u32 cm = m & 0x1FFCu;
                      if      (cm == 0x1808u) n_be++;
                      else if (cm == 0x1814u) n_va++;
                      else if (cm == 0x1824u) n_ia++;
                      if (((++tick) % 200000) == 0)
                          printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                                 n_be, n_va, n_ia), fflush(stdout);
                  } }

                /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
                 * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
                 * truncates them.  Reconstruct the full address the same way
                 * the draw-engine call above does. */
                { const u32 mfull = (subch << 13) | m;
                if (mfull == 0xEB00u || mfull == 0xEB04u) {
                    rsx_live_draw_vtx_join();   /* LD_VTX_ASYNC */
                    cellGcmQueueUserCommand(vm_read32(dea));
                } else if (mfull == 0xE920u || mfull == 0xE924u) {
                    rsx_live_draw_vtx_join();   /* LD_VTX_ASYNC */
                    if (s_pk_log > 0) fprintf(stderr, "[gcm-flip] t=%lluus%c", ps3_qpc_us(), 10);
                    gcm_mhash_frame();
                    cellGcmSetFlipCommand(vm_read32(dea) & 7u);
                } else if (subch == 0 || (subch == 1 && !s1_2d)) {
                    /* LD_VTX_ASYNC: a reference, a back-end or texture-read
                     * label release and a report each show the guest how far
                     * the RSX has got: no conversion may still be pending
                     * (except at a ZPASS report: gcm_report_needs_vtx_join). */
                    if (m == 0x50u || m == 0x1D70u || m == 0x1D74u ||
                        (m == 0x1800u && gcm_report_needs_vtx_join(vm_read32(dea))))
                        rsx_live_draw_vtx_join();
                    rsx_process_method(&s_state, m, vm_read32(dea));
                    /* NV406E_SET_REFERENCE: queue the fence value for PACED
                     * publication (gcm_ref_publish below) instead of letting a
                     * later fence in the same batch overwrite it. */
                    if (m == 0x50) gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff);
                    /* NV4097_GET_REPORT: (type << 24) | offset into the report
                     * area. Sample counts are not measured, so a ZPASS query
                     * reports "visible" -- the conservative answer; reading 0
                     * tells the title every queried object is occluded. */
                    if (m == 0x1A8) s_report_dma = vm_read32(dea);
                    /* GCM_ZPASS_INC=<n>: the ZPASS counter accumulates n per
                     * GET_REPORT (every query "sees" n more samples) and
                     * CLEAR_REPORT_VALUE (0x17C8, type 1) resets it, as the RSX
                     * counter does; a title that subtracts two reports otherwise
                     * reads 0, "occluded". A probe until samples are counted. */
                    static long s_zinc = -1; static u32 s_zcount;
                    if (s_zinc < 0) { const char* e = getenv("GCM_ZPASS_INC"); s_zinc = e ? atol(e) : 0; }
                    if (m == 0x17C8 && vm_read32(dea) == 1u) {
                        s_zcount = 0;
                        { static int zc = 0; if (s_zinc && zc++ < 4) printf("[zpass] CLEAR_REPORT_VALUE zpass%c", 10); }
                    }
                    if (m == 0x1800) {
                        const u32 v = vm_read32(dea), off = v & 0xFFFFFFu, idx = off / 16u;
                        if (s_zinc && (v >> 24) == 1u) s_zcount += (u32)s_zinc;
                        /* GCM_ZPASS_INC=-1: every query reads 0 (an A/B of "occluded") */
                        u32 val = (v >> 24) == 1u ? (s_zinc < 0 ? 0u : s_zinc ? s_zcount : 0xFFFFu) : 0u;
                        /* GCM_ZPASS_OCC_AB=1 (a ceiling test): every ZPASS query reads 0
                         * ("occluded") in every other [draws] window (g_ld_zocc_on). */
                        { extern volatile int g_ld_zocc_on; if (g_ld_zocc_on && (v >> 24) == 1u) val = 0u; }
                        const u64 ts = gcm_report_timestamp();
                        { extern volatile long g_gcm_report_count; g_gcm_report_count++; }
                        const int main_mem = s_report_dma == 0xFEED0001u || s_report_dma == 0xBAD68000u;
                        { /* GCM_REPORTLOG=<n>: the first n reports (default 6), with the time. */
                          static long rn = 0, rmax = -1;
                          if (rmax < 0) { const char* e = getenv("GCM_REPORTLOG"); rmax = e ? atol(e) : 6; }
                          if (rn++ < rmax) printf("[GET_REPORT] t=%lluus type=%u off=0x%X dma=0x%08X%c",
                                                  ps3_qpc_us(), v >> 24, off, s_report_dma, 10);
                          /* GCM_PARK_LOG=1: the reports the SPU side acts on (not the
                           * ~800 ZPASS counts a frame), with the park lines. */
                          if (s_pk_log < 0) gcm_park_env();
                          if (s_pk_log > 0) {
                              const u32 rea = main_mem ? gcm_io2ea(off) : 0;
                              const int ring = rea && rea >= s_pk_ring_lo && rea < s_pk_ring_hi;
                              if ((v >> 24) != 1u || ring)
                                  fprintf(stderr, "[gcm-rep] t=%lluus type=%u off=0x%X ring=%d%c",
                                          ps3_qpc_us(), v >> 24, off, ring, 10);
                          }
                          /* GCM_REPORTFF=1: a GET_REPORT word still 0xFFFFFFFF (a placeholder
                           * not patched yet): where it is, its neighbours, and what the same
                           * word holds when the next reports go by. */
                          static int ff = -1; static u32 ff_ea[8]; static int ff_n;
                          if (ff < 0) ff = getenv("GCM_REPORTFF") ? 1 : 0;
                          if (ff) {
                              for (int k = 0; k < ff_n; k++)
                                  printf("[reportff] later ea=0x%08X now 0x%08X%c", ff_ea[k], vm_read32(ff_ea[k]), 10);
                              ff_n = 0;
                              if (v == 0xFFFFFFFFu) {
                                  printf("[reportff] get=0x%X ea=0x%08X:", s_fifo_getoff, dea);
                                  for (int k = -8; k <= 8; k++) printf(" %08X", vm_read32(dea + 4u * (u32)k));
                                  printf("%c", 10);
                                  if (ff_n < 8) ff_ea[ff_n++] = dea;
                                  const u32 tea = gcm_io2ea(off);
                                  printf("[reportff] target ea=0x%08X before:", tea);
                                  for (int k = -4; k < 8; k++) printf(" %08X", tea ? vm_read32((tea & ~3u) + 4u * (u32)k) : 0u);
                                  printf("%c", 10);
                              }
                          } }
                        /* GCM_REPORT_VALIDATE (default on; =0 off): a report whose
                         * type is not one the RSX has (1-5: ZPASS, ZCULL stats) or
                         * whose offset is not 16-byte aligned is not written. God of
                         * War: Ascension's command words for reports are patched in
                         * place, and the walker read one still holding a placeholder
                         * (type 0xFF, offset 0xFFEECF): the 16-byte report went to
                         * IO 0xFFEECF -> 0x11FFEECF, inside the SPU job builder's
                         * staging buffer, and the next module-216 job list carried a
                         * timestamp as a DMA element (the in-game freeze). */
                        static int s_rep_val = -1;
                        if (s_rep_val < 0) { const char* e = getenv("GCM_REPORT_VALIDATE"); s_rep_val = !(e && e[0] == '0'); }
                        const u32 rtype = v >> 24;
                        const int rep_bad = s_rep_val && (rtype < 1u || rtype > 5u || (off & 15u));
                        if (rep_bad) {
                            static int bn = 0;
                            if (bn < 16 && ++bn)
                                printf("[GET_REPORT] skipped: type=%u off=0x%06X (not a report the RSX writes) get=0x%X%c",
                                       rtype, off, s_fifo_getoff, 10);
                        } else if (main_mem) {
                            /* CellGcmReportData, big-endian: u64 timer, u32 value, u32 padding. */
                            const u32 rea = gcm_io2ea(off);
                            if (s_rd_on < 0) gcm_rd_init();   /* OCCL_RDIAG */
                            if (rea) {
                                /* OCCL=1 (rsx_live_draw.c): a bank report's value word is the true count of
                                 * its query, written when the query has resolved; here 0xFFFF, or the word left
                                 * as it is (it holds the slot's previous true count). Off: g_occl_apply = 0. */
                                int wv = 1;
                                { extern volatile int g_occl_apply;
                                  if (g_occl_apply && rtype == 1u) {
                                      extern int rsx_live_draw_occl_get(u32 off, u32 rea, u32* val);
                                      u32 ov = val;
                                      const int r = rsx_live_draw_occl_get(off, rea, &ov);
                                      if (r == 0) wv = 0; else if (r > 0) val = ov;
                                  } }
                                vm_write32(rea, (u32)(ts >> 32));
                                vm_write32(rea + 4u, (u32)ts);
                                if (wv) vm_write32(rea + 8u, val);
                                vm_write32(rea + 12u, 0u);
                                if (s_rd_on > 0) gcm_rd_write(rea);
                            } else { static int un = 0; if (un < 4 && ++un) printf("[GET_REPORT] main-memory offset 0x%X is not IO-mapped%c", off, 10); }
                        } else if (idx < CELL_GCM_MAX_REPORT_COUNT) {
                            s_report_data[idx].timestamp = ts;
                            s_report_data[idx].value = val;
                        }
                        /* SPU code polls these reports (a Wws job list's RSX
                         * barrier): let the SPURS kernel look again now. */
                        { extern void spurs_external_event(void); spurs_external_event(); }
                    }
                } else {
                    /* LD_VTX_ASYNC: 2D blits and inline transfers write guest
                     * memory, which may hold vertices a queued draw reads. */
                    rsx_live_draw_vtx_join();
                    gcm_2d_method(subch, m, vm_read32(dea));
                }
                }
            }
            s_fifo_getoff += 4 + count * 4;
            continue;
        }
        { static int _uw = 0; if (_uw++ < 16 && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
    }
    /* LD_VTX_ASYNC: the pass ends with every vertex conversion done, before
     * the GET/drained publication below and before anything outside the
     * walker (vblank, flip handlers, guest polls) can look. */
    rsx_live_draw_vtx_join();

    /* Walker watchdog. Everything above recovers from a *recognised* stall --
     * a branch we cannot take, a park the title abandoned. A walker that stops
     * for any other reason simply freezes `get` for the rest of the run and
     * nothing renders again, and which of those happens is a race, so the same
     * boot renders one time in five. If `get` has not moved across several
     * passes while `put` is somewhere else, there is work the walker will
     * never reach: resynchronise to the write head. Same trade as the
     * unmapped-get path -- commands are lost, the FIFO lives. */
    /* The stuck-detector above only fires when `get` stops MOVING. A walker
     * going in circles moves plenty -- it just never arrives. Twisted Metal
     * and flOw both write one ring and their JUMPs lead to `put`; VF5 does
     * not. AMGL leaves libgcm's default ring at IO 0 parked on its own JUMP
     * loop and drives the RSX from a second buffer it mapped at IO 0xA00000,
     * so the walker circulates the default ring forever, `get` never reaches
     * `put`, the title's ring is never reclaimed, and AMGL prints "Command
     * Buffer Overflow" until it runs off the end of its own mapping.
     *
     * A drain that burns its ENTIRE word budget without arriving is that
     * signature: no legitimate frame leaves a million words pending, and a
     * genuine burst clears within a tick or two. Circling does not, so
     * require several passes in a row before following the write head.
     *
     * ponytail: consecutive-budget-burn is a heuristic, not a proof. The real
     * fix is knowing which buffer the RSX is bound to -- that needs the
     * SetQueueHandler / context-switch path modelled, not a walker rule. */
    { static int burned = 0, passes = -1;
      if (passes < 0) { const char* e = getenv("GCM_FIFO_CIRCLE_PASSES");
                        passes = e ? atoi(e) : 4; }
      if (passes && budget <= 0 && s_fifo_getoff != put) {
          if (++burned >= passes) {
              static int n = 0;
              if (n++ < 8)
                  fprintf(stderr, "[cellGcmSys] FIFO walker circling (get=0x%08X, "
                          "put=0x%08X, full budget burned x%d) -- following the "
                          "write head\n", s_fifo_getoff, put, burned);
              gcm_fifo_resync_why("circling", &s_fifo_getoff, put);
              burned = 0;
          }
      } else burned = 0; }

    /* A pass that stopped on a self-JUMP park is not stuck: the park has its
     * own grace before it gives up (gcm_park_grace_ms, GCM_PARK_WAIT_MS), and
     * this rule -- eight passes, some 30 ms -- overrode it. God of War:
     * Ascension parks the RSX while its SPU jobs fill the next segment and
     * patches the park afterwards; resyncing to put dropped those segments,
     * the GPU reports in them were never written, the RsxSync workload's
     * barrier never passed, and the frames stopped after the fifth. */
    /* A blocked semaphore ACQUIRE (GCM_SEMA_ACQUIRE=1) is not stuck either: the
     * GPU waits there by design until the label is released. Ascension's
     * frame waits on a label its SPU post-process tasks write when they are
     * done with the frame; following put dropped the frame mid-wait. Nor is
     * a patched park or a report held for SPU work (GCM_PARK_SPU_HOLD_MS,
     * GCM_REPORT_SPU_HOLD_MS), each of which bounds its own wait. */
    { static u32 last = 0xFFFFFFFFu; static int stuck = 0;
      if (why && (strcmp(why, "parked") == 0 || strcmp(why, "sema-acquire") == 0 || strcmp(why, "flip-pace") == 0 || strcmp(why, "spu-hold") == 0 || strcmp(why, "report-hold") == 0)) { stuck = 0; last = s_fifo_getoff; }
      else if (s_fifo_getoff == last && s_fifo_getoff != put) {
          if (++stuck >= 8) { gcm_fifo_dump_around(s_fifo_getoff);
                             gcm_fifo_resync_why("stuck-get", &s_fifo_getoff, put); stuck = 0; }
      } else { stuck = 0; last = s_fifo_getoff; } }
    s_walk_why = why;
    /* OCCL=2 (rsx_live_draw.c): deferred ZPASS counts are submitted and landed before the walker stops. */
    { extern volatile int g_occl_apply;
      if (g_occl_apply) { extern void rsx_live_draw_occl_pass_end(void); rsx_live_draw_occl_pass_end(); } }
    /* A park episode ends when the walker has left the park word by any route
     * (a rewrite that is not a JUMP is walked through, not taken). */
    if (s_pk_off != ~0u && s_fifo_getoff != s_pk_off) gcm_park_end(1);

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    if (s_dbg_drain) {
        static int n2 = 0;
        if (n2++ % 60 == 0)
            fprintf(stderr, "[DRAINEND] %s: %08X -> %08X (%u bytes), put=%08X\n",
                    why, start_off, s_fifo_getoff, s_fifo_getoff - start_off, put);
    }
    /* Recycle the TITLE'S OWN command buffer when its callback will not.
     *
     * gCellGcmCurrentContext points at the title's context, not the one
     * cellGcmInit handed back: Virtua Fighter 5's is a 512 KB ring at EA
     * 0x4AE00000, and the buffer-full callback AMGL installs (0x006A8030) only
     * prints "[AMGL]:[ERROR] Command Buffer Overflow!" and returns 0 -- which
     * libgcm's gcmReserve reads as "retry", so the title writes straight past
     * `end` into unmapped memory and never renders again.
     *
     * Hardware never reaches that state because the ring is recycled once the
     * RSX has consumed it. Do the same here, on the title's context, and only
     * when it is provably safe: the walker has drained everything the title
     * submitted (get == put) and `current` is within one reserve of `end`.
     * A title whose own callback recycles never reaches that state, so this is
     * inert for every port that works today.
     *
     * ponytail: append the JUMP at `current` the way the SDK's own callback
     * does, rather than tracking a second copy of the ring's geometry. */
    if (s_gcm_ctx_out_ea) {
        u32 ctx = vm_read32(s_gcm_ctx_out_ea);
        u32 begin = ctx ? vm_read32(ctx + 0x0) : 0;
        u32 end   = ctx ? vm_read32(ctx + 0x4) : 0;
        u32 cur   = ctx ? vm_read32(ctx + 0x8) : 0;
        /* `get == put` -- a fully drained FIFO -- was too strict. It holds
         * while a title is only clearing, and stops holding the moment its SPU
         * work starts producing real command volume: the walker then always has
         * a backlog, the recycle never fires, and the ring overflows again.
         * That is exactly where Virtua Fighter 5 stalls once its "SPU Delegate"
         * group starts feeding the renderer.
         *
         * Hardware does not need a drained FIFO either -- only that the bytes
         * about to be overwritten have already been read. So require the walker
         * to be a margin PAST `begin` instead: the head of the ring is consumed,
         * which is the region the guest is about to write. */
        u32 io_begin_chk = begin ? gcm_ea2io(begin) : 0xFFFFFFFFu;
        int head_consumed =
            (io_begin_chk != 0xFFFFFFFFu) &&
            (s_fifo_getoff == put ||
             (s_fifo_getoff > io_begin_chk &&
              s_fifo_getoff - io_begin_chk >= GCM_RECYCLE_MARGIN));
        /* Already overrun. gcmReserve writes past `end` when its callback
         * declines to recycle (AMGL's only prints), and once the guest is out
         * there the walker follows into memory that is not the ring: `get`
         * stops advancing, head_consumed can never become true again, and the
         * title is wedged for good -- which is exactly where Virtua Fighter 5
         * stops, at the same flip every run however long it is left.
         *
         * There is nothing left to protect at that point. Recycling loses
         * whatever was written past the end; not recycling loses the rest of
         * the run. */
        /* A pass that stopped at a flip has a backlog on purpose (the next
         * frame), and teleporting `get` to `begin` would drop it -- fence
         * writes included, so the title then waits on a reference that never
         * comes (The Simpsons Arcade Game froze at its 4th recycle). The next
         * pass drains to `put` and recycles then. */
        if (!strcmp(why, "flip") && s_fifo_getoff != put) head_consumed = 0;
        /* Only once the title has been SEEN to overrun. Near `end` is also
         * where a title whose callback does recycle sits just before calling
         * it: Tornado Outbreak's steps its context through 64 KB segments of a
         * larger ring, and recycling under it from this thread rewound put/get
         * behind its callback's bookkeeping -- which then waited forever for a
         * GET that could not arrive. A callback that declines (AMGL's) always
         * overruns first, so the first overrun is recycled by the path above
         * and near-end recycling is enabled from then on. */
        static int s_title_overran = 0;
        if (begin && end > begin && cur >= end) { head_consumed = 1; s_title_overran = 1; }
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {
            u32 io_begin = gcm_ea2io(begin);
            if (io_begin != 0xFFFFFFFFu) {
                u32 jmp_at = (cur + 4 <= end) ? cur : end - 4;
                vm_write32(jmp_at, 0x20000000u | io_begin);   /* JUMP -> begin */
                vm_write32(ctx + 0x8, begin);                 /* current = begin */
                vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);  /* put */
                s_fifo_getoff = io_begin;                          /* get */
                put = io_begin;
                static int n = 0;
                if (n++ < 8)
                    fprintf(stderr, "[cellGcmSys] recycled the title's own ring "
                            "(ctx=0x%08X begin=0x%08X end=0x%08X) -- its callback "
                            "does not%c", ctx, begin, end, 10);
            }
        }
    }

    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    /* GCM_GET_EQ_PUT=1: publish `get` as having reached `put` rather than where
     * the walker actually is. A probe, not a fix -- it removes the back-pressure
     * a title uses to avoid overwriting commands the GPU has not read. It exists
     * to answer one question for a title that reports its ring full while the
     * walker says it is drained: is the title reading `get`, or something else?
     * If the overflow survives this, `get` is not what it consults. */
    { static int eq = -1;
      if (eq < 0) { const char* e = getenv("GCM_GET_EQ_PUT"); eq = e ? atoi(e) : 0; }
      vm_write32(GCM_CONTROL_GUEST_ADDR + 4, eq ? put : s_fifo_getoff); }

    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    u16 io_page = s_io_address_table[page];
    if (io_page == 0xFFFF) return 0xFFFFFFFFu;
    return ((u32)io_page << 20) | (ea & 0xFFFFFu);
}

void cellGcm_fifo_recycle(u32 ctx_ea)
{
    if (!ctx_ea) return;
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (current <= begin) return;                       /* nothing to recycle */

    if (getenv("CELLMARK_BLINKDBG"))
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stderr, "[REC>] tid=%lu begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* Do what the SDK's default command-buffer-full callback does: append a
     * JUMP-to-begin at the write head and move `put` to begin. The FIFO walker
     * consumes the tail, follows the jump, and idles at begin; then it's safe
     * for the guest to write from begin (that region was consumed long ago). */
    u32 io_begin = gcm_ea2io(begin);
    if (io_begin != 0xFFFFFFFFu) {
        vm_write32(current, 0x20000000u | io_begin);            /* JUMP begin  */
        /* The jump has to be in guest memory before `put` moves behind the
         * walker's `get`. Program order is not enough on arm64: the walker
         * may observe the new `put`, find `get` != `put`, and read the old
         * word at `current` before the jump lands there. */
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);        /* put = begin */
    }

    /* Wait (bounded ~2s) for the walker to consume the tail + take the jump so
     * no commands are lost; a stalled ticker degrades to dropped commands. */
    int spins = 0;
    while (g_gcm_fifo_drained_ea != begin && spins < 2000) { Sleep(1); spins++; }
    if (spins >= 2000) {
        static int warned = 0;
        if (warned++ < 4)
            printf("[cellGcmSys] fifo recycle: drain stalled (drained=0x%08X begin=0x%08X)\n",
                   g_gcm_fifo_drained_ea, begin);
    }

    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */

    if (s_recdbg)
        fprintf(stderr, "[REC<] tid=%lu cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
}

/* A host that brings its backend up on another thread -- a frame clock that
 * opens the window while the title boots -- can register the draw engine after
 * the title has already called cellGcmSetDisplayBuffer. The engine then knows
 * no scanout, and every flip presents whatever surface happens to be bound:
 * Twisted Metal on Linux showed an offscreen target instead of its composited
 * display buffer. rsx_draw_engine_init calls this to catch up. */
void cellGcm_replay_display_buffers_to_engine(void)
{
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i])
            rsx_draw_engine_set_display_buffer(i, 0, s_display_buffers[i].offset,
                                               s_display_buffers[i].pitch,
                                               s_display_buffers[i].width,
                                               s_display_buffers[i].height);
}

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Tell the live engine too, or it has no registered scanout to present and
     * falls back to whatever surface happens to be current. Display buffers are
     * always in RSX local memory (location 0). */
    if (rsx_live_draw_enabled())
        rsx_live_draw_set_display_buffer(bufferId, 0, offset, pitch, width, height);
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_set_display_buffer(bufferId, 0, offset, pitch, width, height);

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* NID: 0xEAA52F23 */
/* How many display buffers the title has actually registered. cellResc needs
 * it to know how far to rotate its flip target; nothing else about the set is
 * anyone else's business, so this stays a count rather than exposing the array. */
u32 cellGcm_display_buffer_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i]) n++;
    return n;
}

static s32 gcm_flip_request(u32 bufferId, int in_fifo)
{
    /* A direct flip (not a marker the walker will reach): wait for its frame
     * slot here, on the game's thread (PS3_FPS_LIMIT). */
    if (!in_fifo) gcm_flip_pace_block();
    /* GCM_FLIPCOUNT=1: every flip, with a timestamp. FLIP_DBG caps at 20 lines,
     * which answers "did it ever flip?" and not "is it still flipping, and how
     * fast?" -- the question that matters when a title stops appearing on
     * screen. Virtua Fighter 5 does not use the in-FIFO 0xFEADxxxx flip word at
     * all (that counter stays at zero for the whole run); it flips through
     * cellGcmSetFlip, so this is where its frame rate actually is. */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      if (dbg) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long n = 0, t0 = 0;
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          ++n;
          if (n <= 400ull || (n % 10ull) == 0)
              fprintf(stderr, "[flip] %llu at %llu ms%c", n, now - t0, 10);
      } }

    /* GCM_FLIP_BT=<n>: dump the flipping thread's guest stack for the flips
     * around n. When a title renders correctly for a while and then stops
     * submitting -- no error, no unresolved import, no overflow -- the only
     * thing separating the last good frame from the first missing one is what
     * the render path was doing on each. This is the window onto that. */
    { static long long at = -2;
      if (at == -2) { const char* e = getenv("GCM_FLIP_BT"); at = e ? atoll(e) : -1; }
      if (at >= 0) {
          static unsigned long long m = 0;
          ++m;
          if ((long long)m >= at - 5 && (long long)m <= at + 5) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (g_active_ctx) {
                  char tag[48];
                  snprintf(tag, sizeof tag, "flip#%llu", m);
                  ppu_dump_guest_stack(g_active_ctx, tag);
                  { extern void ppu_dump_bctrl_ring(uint32_t, const char*);
                    ppu_dump_bctrl_ring((uint32_t)g_active_ctx->thread_id, tag); }
              }
          }
      } }

    { static int _n=0; if (getenv("FLIP_DBG") && _n++ < 20)
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;


    /* in_fifo: the flip sits in the command stream as a GCM_FLIP_MARKER, and
     * the drain does this half when it reaches it (see the 0xFEAD case). */
    if (!in_fifo) {
        gcm_set_current_display_buffer(bufferId);
        s_flip_pending = 1;   /* ticker: present BEFORE the next drain */
        s_flip_request_count++;
    }
    /* Flip requested but not yet shown: a subsequent cellGcmSetWaitFlip blocks
     * until the present thread's cellGcmTickFlip marks it done (vsync). */
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke via OPD resolution, not a raw call into guest code.
     *
     * On hardware this callback fires when the flip COMPLETES, at vsync --
     * which is what cellGcmTickFlip does. Calling it again here, at request
     * time, is a compatibility wart: some titles need the early edge, but a
     * title that issues its first flip while still initialising then runs its
     * handler against half-built state. Tokyo Jungle faults that way, in
     * func_001F2D3C, dereferencing a singleton it has not constructed yet.
     * GCM_FLIPCB_ONTICK=1 leaves the callback to the tick alone. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);  /* head 0 = primary display */

    return CELL_OK;
}

s32 cellGcmSetFlipCommand(u32 bufferId) { return gcm_flip_request(bufferId, 0); }

/* The real _cellGcmSetFlipCommand appends the flip to the command buffer, so
 * the RSX flips once it has executed everything queued before it. Firing it
 * at call time instead presented whatever the drain had reached by then --
 * and the title has not even flushed its frame's tail yet: The Simpsons Arcade
 * Game calls it with ctx->current ~2.4 KB past put, so frames showed without
 * their last draws (a dialog, a dimming overlay) for one flip. Write a marker
 * at ctx->current instead; the drain stops at it and fires the flip there,
 * exactly as it does for the 0xFEAD words a statically-linked libgcm emits.
 * Returns 0 when there is no room or ctx is not a mapped command buffer. */
static int gcm_flip_into_fifo(u32 ctx, u32 bufferId)
{
    if (!ctx || bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId]) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Never near the end of a ring the runtime owns: those bytes are the
     * title's reserve before its ring wraps, and the recycle above keys on
     * them. A context the title manages itself only needs room for the
     * marker: God of War: Ascension flips with ~4 KB left in each 64 KB
     * segment it hands out, so asking for the reserve there sent half its
     * flips around the FIFO (immediate, ahead of the frame, unpaced). */
    const u32 cb_opd0 = vm_read32(ctx + 0xC);
    const int runtime_ring0 = cb_opd0 && vm_read32(cb_opd0) == GCM_FIFO_CALLBACK_SENTINEL_EA;
    if (cur + 8 + (runtime_ring0 ? GCM_RECYCLE_SLACK : 0u) > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, GCM_FLIP_MARKER | bufferId);
    /* Wrap here, on the guest thread, once the ring is nearly used -- what the
     * SDK's buffer-full callback does. Otherwise the drain thread recycles it
     * (see "recycled the title's own ring"), rewriting ctx->current and put
     * while the guest may be between advancing current and flushing: put
     * then lands on begin, the guest's closing fence is never executed, and
     * it spins on `ref` for good. Safe when the walker has left the head of
     * the ring well behind, which is the region the guest writes next.
     *
     * Only for a ring the runtime owns, i.e. one whose buffer-full callback is
     * still the runtime's (cellGcmSetupContext installs it). A title with its
     * own callback manages its own buffers, and its context need not be a ring
     * at all: Killzone 3 flips on a 0x30000-byte draw context whose `end`
     * moves down as it allocates from the top, so it always looks nearly full.
     * Rewinding it to `begin` in mid-frame cut the frame short, dropping the
     * fence release in its tail, and the title then waited for it forever. */
    const u32 begin = vm_read32(ctx), io_begin = gcm_ea2io(begin);
    const u32 cb_opd = vm_read32(ctx + 0xC);
    const int runtime_ring = cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA;
    if (runtime_ring && end - (cur + 4) < GCM_FLIP_WRAP_BYTES && io_begin != 0xFFFFFFFFu &&
        s_fifo_getoff >= io_begin + GCM_RECYCLE_MARGIN) {
        vm_write32(cur + 4, 0x20000000u | io_begin);   /* JUMP -> begin */
        vm_write32(ctx + 8, begin);
        return 1;
    }
    vm_write32(ctx + 8, cur + 4);
    return 1;
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    vm_write32(GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE, labelValue);

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    gcm_flip_pace_block();
    gcm_set_current_display_buffer(bufferId);
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_request_count++;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. See cellGcmSetFlipCommand for why
     * GCM_FLIPCB_ONTICK exists. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    return s_current_display_buffer_id;
}

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_flip_handler_opd = (u32)(size_t)handler;
    s_flip_handler = handler;
    { u32 c = s_flip_handler_opd ? vm_read32(s_flip_handler_opd) : 0; if (c) s_flip_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_vblank_handler_opd = (u32)(size_t)handler;
    s_vblank_handler = handler;
    { u32 c = s_vblank_handler_opd ? vm_read32(s_vblank_handler_opd) : 0; if (c) s_vblank_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
}

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    gcm_update_handler_mask();
}

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* NID: 0x0E6B0DFF */
/* Mirror the host tables into the guest window the title will index. */
static void gcm_publish_offset_tables(void)
{
    for (u32 i = 0; i < GCM_OFFSET_TABLE_PAGES; i++) {
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + i * 2, s_io_address_table[i]);
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + i * 2, s_ea_address_table[i]);
    }
}

s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    u32 ea = (u32)(uintptr_t)table;
    if (!ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_publish_offset_tables();
    s_offset_tables_published = 1;

    /* The guest struct is two 4-byte EAs, not two host pointers. */
    vm_write32(ea + 0, GCM_OFFSET_TABLE_IO_ADDR);
    vm_write32(ea + 4, GCM_OFFSET_TABLE_EA_ADDR);
    return CELL_OK;
}

/* NID: 0xDB769B32 */
#define GCM_IO_NONE 0xFFFFFFFFu   /* gcm_io_alloc: no free IO space */
static u32 gcm_io_alloc(u32 ea, u32 size);   /* defined below */

s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    if (!off_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* A2O_DBG=<N>: the EA->offset conversions a title asks for. A texture bound
     * at an offset nothing ever writes usually means the offset came from an EA
     * in a space we classified wrongly, and that is only visible here. */
    { static int cap = -1, k = 0;
      if (cap < 0) { const char* e = getenv("A2O_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && k < cap) { k++;
        u32 nz = 0;
        for (u32 i = 0; i < 0x400u; i += 13) if (vm_base && vm_base[address + i]) nz++;
        printf("[A2O] ea=0x%08X  (first 1KB nonzero=%u/79)\n", address, nz); } }

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        if ((loff >> 20) < sizeof(s_local_offset_page)) s_local_offset_page[loff >> 20] = 1;
        vm_write32(off_ea, loff);
        return CELL_OK;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        vm_write32(off_ea, (io_page << 20) | (address & 0xFFFFF));
        return CELL_OK;
    }

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        vm_write32(off_ea, address - s_config.ioAddress);
        return CELL_OK;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use.
     *
     * The bound used to be a flat `< 0x40000000`, which is not a description of
     * anything -- RSX local memory is already handled at the top of this
     * function against its real base, and the raw-SPU window at 0x30000000 sits
     * *below* the old bound, so it was not keeping that out either. What it did
     * do was exclude titles whose RSX-visible heap lives high: Virtua Fighter 5
     * maps 0x4A900000..0x4B400000 and then hands us 0x4B400040, which fell past
     * 0x40000000, skipped the auto-map, and failed -- 976 times a boot, leaving
     * the title with no offsets to draw with (20 draws in five minutes).
     *
     * So the test is now what it always meant: anything below RSX local memory
     * is main memory. Local is checked first and returns above, so reaching
     * here with an address under localAddress means an unmapped main-memory
     * page, which is exactly the case this fallback exists for. */
    /* NULL is not a buffer. Auto-mapping it costs an IO page and, worse, hands
     * the title a real-looking offset for guest address 0 -- so an EA the title
     * computed as NULL (a failed allocation, an out-param it never filled)
     * silently becomes a valid RSX offset instead of an error it could notice.
     * The Orange Box's chooser asks for exactly this, and the page it burned was
     * the one the following cellGcmMapMainMemory would otherwise have taken. */
    if (address < 0x100000u) {
        printf("[cellGcmSys] AddressToOffset REFUSED: ea 0x%08X is not a mappable "
               "address (page 0)\n", address);
        vm_write32(off_ea, 0);
        return CELL_GCM_ERROR_INVALID_VALUE;
    }

    u32 auto_map_limit = s_config.localAddress ? s_config.localAddress : 0x40000000u;
    if (address < auto_map_limit) {
        u32 ea_page = address & ~0xFFFFFu;
        gcm_io_lock();
        u32 io      = gcm_io_alloc(ea_page, 0x100000u);
        if (io == GCM_IO_NONE) { gcm_io_unlock(); vm_write32(off_ea, 0); return CELL_GCM_ERROR_FAILURE; }
        populate_offset_table(ea_page, io, 0x100000u);
        /* Record it in the mapping list too, not just the offset table.
         *
         * These are two different structures answering two different questions,
         * and only the offset table was being filled in. The FIFO walker
         * resolves io -> ea through the MAPPING LIST (find_io_mapping_by_io),
         * so an auto-mapped range was invisible to it: a JUMP into one reported
         * "no IO mapping", the walker resynced straight to put, and every fence
         * in the skipped range was lost -- which strands a title spinning on
         * ctrl->ref with no way to see why. Both paths have to agree about what
         * is mapped. */
        IoMapping* m = find_free_mapping();
        if (m) {
            m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
            s_io_mapping_count++;
        } else {
            printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                   "the FIFO will not be able to walk io 0x%08X\n", io);
        }
        gcm_io_unlock();
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        vm_write32(off_ea, io | (address & 0xFFFFFu));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    vm_write32(off_ea, 0);
    { static int _d = -1; if (_d < 0) _d = getenv("A2O_FAILDBG") ? 1 : 0;
      if (_d) { static int _n = 0; if (_n < 12 && ++_n)
          fprintf(stderr, "[A2O] FAILED ea=0x%08X (not in local or IO space)%c",
                  address, 10); } }
    return CELL_GCM_ERROR_FAILURE;
}

/* IO-offset space bump allocator for main-memory mappings. Starts PAST the
 * Init command-buffer region: handing out io 0 (the old s_local_mem_allocated
 * bump, which begins at 0) remapped the FIFO's own io page -- demosaic's
 * cellGcmMapMainMemory(sys heap) got offset 0, io2ea(0) then resolved into its
 * heap (all zeros) and the RSX walker consumed every frame's commands as
 * no-ops (no draws, label fence never written, guest waited forever). */
/* First fit among the IO pages no mapping holds (s_ea_address_table, which an
 * unmap clears), from the end of that region up, as the RSX's own IO page table
 * is handed out. This was a bump allocator that never took back what an unmap
 * freed. God of War: Ascension maps and unmaps ~70 MB at every area load and
 * checkpoint reload, so after a few the offsets ran past the 256 MB aperture
 * and then past 512 MB; the title keeps them in 29 bits, so its unmaps of
 * io 0x20700000.. arrived as 0x00700000.. and failed ("Ice::Render::
 * UnmapMainMemory (0x00700000) failed"), its address-to-offset answers became
 * unusable ("iooffset == -1", 80k times) and vertex buffers, fragment programs
 * and their inline-transfer constants (NV308A to offset 0xFFFFFFFC) pointed at
 * the wrong memory: garbled scenery until a restart (2026-11-03). Until the
 * first unmap this hands out the offsets the bump allocator did. The EDGE6
 * window (gcm_map_extra) holds its pages in the table, so it is skipped too.
 * Tried first: io = ea - ioAddress, where that range is free -- the pairing
 * the bump allocator gave this title's contiguous heap, so a page that comes
 * back always maps the same memory again and caches keyed by offset stay true.
 * Only the 256 MB RSX IO aperture is handed out; past it the map fails, as
 * libgcm's does (call with s_io_lock held). */
#define GCM_IO_PAGES (0x10000000u >> 20)
static int gcm_io_pages_free(u32 p, u32 pages)
{
    if (p + pages > GCM_IO_PAGES) return 0;
    for (u32 k = 0; k < pages; k++)
        if (s_ea_address_table[p + k] != 0xFFFF) return 0;
    return 1;
}
static u32 gcm_io_alloc(u32 ea, u32 size)
{
    const u32 pages = (size + 0xFFFFFu) >> 20;
    u32 first = (s_config.ioSize + 0xFFFFFu) >> 20;
    if (first < 1) first = 1;
    /* GCM_IO_BUMP=1 (test only): the old bump allocator, to reproduce the leak
     * in an A/B with one executable. */
    { static int bump = -1; static u32 next;
      if (bump < 0) { const char* e = getenv("GCM_IO_BUMP"); bump = e && e[0] == '1'; }
      if (bump) {
          if (next < (first << 20)) next = first << 20;
          if (s_gcm_extra_io_size && next < GCM_EXTRA_IO_BASE + s_gcm_extra_io_size &&
              next + (pages << 20) > GCM_EXTRA_IO_BASE)
              next = GCM_EXTRA_IO_BASE + s_gcm_extra_io_size;
          const u32 io = next;
          next += pages << 20;
          return io;
      } }
    if (s_config.ioAddress && ea >= s_config.ioAddress) {
        const u32 p = (ea - s_config.ioAddress) >> 20;
        if (p >= first && gcm_io_pages_free(p, pages)) return p << 20;
    }
    for (u32 p = first; p + pages <= GCM_IO_PAGES; ) {
        u32 k = 0;
        while (k < pages && s_ea_address_table[p + k] == 0xFFFF) k++;
        if (k == pages) return p << 20;
        p += k + 1;
    }
    static int warned;
    if (warned++ < 8)
        printf("[cellGcmSys] WARNING: the 256 MB IO aperture has no room for 0x%X bytes (ea 0x%08X): map refused\n", size, ea);
    return GCM_IO_NONE;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    /* Log the REQUEST before validating it. Every rejection below used to
     * happen before the only printf in this function, so a title whose
     * mapping we refused looked -- in the log -- like a title that never asked.
     * Virtua Fighter 5 then fails cellGcmAddressToOffset ~976 times a boot for
     * addresses immediately past the region it did get, draws 20 times instead
     * of thousands, and never flips; the refusal that would explain it was
     * invisible. Same lesson as the savedata early returns. */
    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    if (!offset) {
        printf("[cellGcmSys] MapMainMemory REFUSED: offset out-param is NULL\n");
        return CELL_GCM_ERROR_INVALID_VALUE;
    }
    /* Size and EA must both be 1MB aligned. */
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if ((ea & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: ea 0x%08X is not 1MB aligned\n", ea);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }

    /* A free slot first, then the IO region: allocating the region before
     * the slot check leaked IO space on every refused call. */
    gcm_io_lock();
    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        gcm_io_unlock();
        printf("[cellGcmSys] WARNING: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    /* IO space past the Init command-buffer window (gcm_io_alloc). */
    u32 io_offset = gcm_io_alloc(ea, size);
    if (io_offset == GCM_IO_NONE) { gcm_io_unlock(); return CELL_GCM_ERROR_FAILURE; }

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io_offset, size);
    gcm_io_unlock();

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Logged before validation, and every failure path named -- same reason as
     * cellGcmMapMainMemory above. Two of these returned silently AFTER the log
     * line as well, so even a call that got that far could vanish. */
    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X / io 0x%08X not 1MB aligned\n", ea, io);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    gcm_io_lock();
    if (find_mapping_by_ea(ea) != NULL) {
        gcm_io_unlock();
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X is already mapped\n", ea);
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        gcm_io_unlock();
        printf("[cellGcmSys] MapEaIoAddress REFUSED: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io, size);
    gcm_io_unlock();

    return CELL_OK;
}

/* NID: 0x626E8518 */
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags)
{
    /* RPCS3 cellGcmSys.cpp accepts exactly STRICT_ORDERING (not an arbitrary
     * bit mask). The software FIFO already reads shared guest memory in FIFO
     * order after its acquire fence; there is no relaxed DMA mapping path to
     * select here. Preserve that path rather than pretending to enable a new
     * ordering mode by storing an unused flag. */
    if (flags != CELL_GCM_IOMAP_FLAG_STRICT_ORDERING)
        return CELL_GCM_ERROR_INVALID_VALUE;
    if ((ea | io | size) & 0xFFFFFu || !size)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    const u64 ea_end = (u64)ea + size;
    const u64 io_end = (u64)io + size;
    /* The HLE models the standard 256 MiB RSX IO aperture. Widen sums before
     * checking bounds so wrapping inputs cannot publish aliases at page 0. */
    if (ea_end > 0x100000000ull || io_end > 0x10000000ull)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* The legacy mapper checks only an identical EA base. Preflight complete
     * intervals on both sides so a rejected flagged map changes neither the
     * mapping list nor either direction of the translation table. */
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (m->active &&
            (((u64)ea < (u64)m->ea + m->size && ea_end > m->ea) ||
             ((u64)io < (u64)m->io + m->size && io_end > m->io)))
            return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }
    return cellGcmMapEaIoAddress(ea, io, size);
}

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    gcm_io_lock();
    IoMapping* mapping = find_mapping_by_ea(ea);
    if (!mapping) {
        gcm_io_unlock();
        return CELL_GCM_ERROR_FAILURE;
    }

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;
    gcm_io_unlock();

    return CELL_OK;
}

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io)
{
    gcm_io_lock();
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping) {
        gcm_io_unlock();
        return CELL_GCM_ERROR_FAILURE;
    }

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;
    gcm_io_unlock();

    return CELL_OK;
}

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        return s_config.localAddress + offset;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return s_config.localAddress + offset;
}

/* IO-table-first resolve: skip the local-page shortcut and ask the IO offset
 * table, falling back to VRAM only if the page is unmapped. cellGcmResolveOffset
 * prefers local for any page the guest ever derived from a local EA, which is
 * right for a title that really does keep the data in VRAM but wrong for one
 * that builds its textures in main memory on pages whose numbers happen to
 * collide. Returns 0 when the IO table has no mapping, so a caller can tell
 * "not IO-mapped" from "IO-mapped at 0". */
u32 cellGcmResolveIO(u32 offset)
{
    u32 page = offset >> 20;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return 0;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory.
 *
 * A MAIN offset asks the IO table first. cellGcmResolveOffset would send it
 * to VRAM whenever the guest once derived a local offset in the same 1 MB
 * page number, and a title with both a large IO map and a full VRAM has
 * many such pages: Twisted Metal's menu fragment programs at IO 0x08B96040
 * and 0x0F75E800 and a G-buffer texture at IO 0x02A1F380 were read out of
 * VRAM -- texture bytes that decode as a NOP program, so the menu's UI drew
 * transparent black. An offset the IO table does not map keeps the old
 * guess. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local)
        return s_config.localAddress + offset;
    const u32 ea = cellGcmResolveIO(offset);
    return ea ? ea : cellGcmResolveOffset(offset);
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (index >= CELL_GCM_MAX_LABEL_COUNT) {
        printf("[cellGcmSys] WARNING: GetLabelAddress index %u out of range\n", index);
        return NULL;
    }
    /* EDGE6_DUMP (ascension edge6.c): which label indices the title asks for. */
    { static int s_l = -1; static u8 s_seen[256];
      if (s_l < 0) { const char* e = getenv("EDGE6_DUMP"); s_l = e && *e == '1'; }
      if (s_l && !s_seen[index]) { s_seen[index] = 1;
          fprintf(stderr, "[edge6] cellGcmGetLabelAddress(%u)\n", index); } }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* NID: 0x97FC4B73 */
/* NID: 0x99D397AC. The value NV4097_GET_REPORT stored at this index (the
 * walker fills it). Unregistered, every call returned a faked 0. */
u32 cellGcmGetReport(u32 type, u32 index)
{
    { static int n = 0;
      if (n++ < 8 || (n % 2000) == 0) printf("[cellGcmSys] GetReport(type=%u, index=%u) -> %u%c", type, index,
                          index < CELL_GCM_MAX_REPORT_COUNT ? s_report_data[index].value : 0u, 10); }
    if (index >= CELL_GCM_MAX_REPORT_COUNT) return 0;
    return s_report_data[index].value;
}

u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u)\n",
           index, location, offset, size, pitch);

    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;

    return CELL_OK;
}

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;

    return CELL_OK;
}

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    s_tiles[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u)\n", index);
    s_zcull[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    s_tiles[index].bound = 0;
    return CELL_OK;
}

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x107BF789
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (_n < 4 && ++_n) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data area — similar to report data, 16 bytes per slot */
static u8 s_notify_data[256 * 16];

void* cellGcmGetNotifyDataAddress(u32 index)
{
    if (index >= 256) return NULL;
    return &s_notify_data[index * 16];
}

/* Timestamp location — returns CELL_GCM_LOCATION_LOCAL or MAIN */
/* The Location suffix names an ARGUMENT, not an out-parameter: it is the
 * caller saying which memory the report lives in, CELL_GCM_LOCATION_LOCAL or
 * _MAIN, exactly as cellGcmGetReportDataLocation and
 * cellGcmGetReportDataAddressLocation take it three hundred lines below. This
 * one alone was declared u32* and treated the value as a guest address to
 * write the location INTO, which inverts the parameter and loses the return.
 *
 * A title passing CELL_GCM_LOCATION_MAIN therefore had a 1 taken for an
 * effective address, and vm_write32 stored four bytes at guest address 1.
 * Yakuza: Dead Souls does that during display setup and dies there.
 *
 * It returns the timestamp, like cellGcmGetTimeStamp, whose body this is with
 * a location the report table does not distinguish. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID 0x21397818 is gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
/* NID: 0x21397818 */
/* PPU_DELAY_AB=<us> (TEST ONLY): in every other [draws] window
 * (rsx_live_draw.c g_ld_ppu_delay_on, "ppudelay window ON|OFF") the guest main
 * thread spins <us> microseconds when it flips: how much the frame rate depends
 * on the main thread's time per frame (ON vs OFF fps). Other threads that flip
 * (the runtime's vblank thread on loading screens) are left alone. */
static void gcm_ppu_delay_test(void)
{
    static long us = -1;
    if (us < 0) { const char* e = getenv("PPU_DELAY_AB"); us = e ? atol(e) : 0; if (us < 0) us = 0; }
    extern volatile int g_ld_ppu_delay_on;
    if (!us || !g_ld_ppu_delay_on) return;
    extern DWORD host_ppu_main_thread_id(void);
    if (GetCurrentThreadId() != host_ppu_main_thread_id()) return;
    const unsigned long long t0 = ps3_qpc_us();
    while (ps3_qpc_us() - t0 < (unsigned long long)us) YieldProcessor();
}

s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    { extern volatile u32 g_gcm_ppu_flips; g_gcm_ppu_flips++; }   /* OCCL_RDIAG: the game's frames */
    gcm_ppu_delay_test();   /* PPU_DELAY_AB */
    /* VDEC_TIMELOG=1 (with cellVdec's lines): when the title queues each flip. */
    { static int s_tl = -1; if (s_tl < 0) s_tl = getenv("VDEC_TIMELOG") ? 1 : 0;
      if (s_tl) printf("[flip-time] t=%lluus buffer=%u\n", ps3_qpc_us(), bufferId); }
    if (gcm_flip_into_fifo((u32)(uintptr_t)ctx, bufferId))
        return gcm_flip_request(bufferId, 1);
    { static int n = 0; if (getenv("GCM_FLIPCOUNT") && n++ < 24) {
        const u32 c = (u32)(uintptr_t)ctx;
        fprintf(stderr, "[flip-direct] _cellGcmSetFlipCommand ctx=0x%X begin=0x%X end=0x%X cur=0x%X io=0x%X\n",
                c, c ? vm_read32(c) : 0, c ? vm_read32(c + 4) : 0, c ? vm_read32(c + 8) : 0,
                c ? gcm_ea2io(vm_read32(c + 8)) : 0); } }
    return cellGcmSetFlipCommand(bufferId);
}

/* Queue "wait until label[labelIndex] == labelValue" in the title's command
 * buffer: SEMAPHORE_OFFSET then SEMAPHORE_ACQUIRE on the NV406E channel
 * (subchannel 0), as RPCS3's _cellGcmSetFlipCommandWithWaitLabel does. Returns
 * 0 when there is no room. */
static int gcm_wait_label_into_fifo(u32 ctx, u32 labelIndex, u32 labelValue)
{
    if (!ctx || labelIndex >= CELL_GCM_MAX_LABEL_COUNT) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Room for these 12 bytes and the flip marker after them (and, in a ring
     * the runtime owns, its reserve: see gcm_flip_into_fifo). */
    const u32 cb_opd = vm_read32(ctx + 0xC);
    const int runtime_ring = cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA;
    if (cur + 12 + 8 + (runtime_ring ? GCM_RECYCLE_SLACK : 0u) > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, (2u << 18) | 0x0064u);          /* 0x64 OFFSET, 0x68 ACQUIRE */
    vm_write32(cur + 4, labelIndex * GCM_LABEL_STRIDE);
    vm_write32(cur + 8, labelValue);
    vm_write32(ctx + 8, cur + 12);
    return 1;
}

/* NID: 0xD8F88E1A
 *
 * The flip waits, in the command stream, for the label to reach the value,
 * then flips when the RSX gets there. This used to write the label itself and
 * flip at once, on the calling thread: the label meant to say "the SPU
 * post-process is done" was set early, and the frame was presented before the
 * RSX had drawn it (God of War: Ascension flips this way in game). Being in
 * the stream also puts these flips under PS3_FPS_LIMIT's pacing.
 * GCM_WAITLABEL_IMMEDIATE=1: the old way. */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    { extern volatile u32 g_gcm_ppu_flips; g_gcm_ppu_flips++; }   /* OCCL_RDIAG: the game's frames */
    gcm_ppu_delay_test();   /* PPU_DELAY_AB */
    static int immediate = -1;
    if (immediate < 0) immediate = getenv("GCM_WAITLABEL_IMMEDIATE") ? 1 : 0;
    const u32 c = (u32)(uintptr_t)ctx;
    if (!immediate && bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM && s_display_buffer_set[bufferId] &&
        gcm_wait_label_into_fifo(c, labelIndex, labelValue)) {
        if (gcm_flip_into_fifo(c, bufferId))
            return gcm_flip_request(bufferId, 1);
        /* No room for the marker after all: flip directly, the wait stays queued. */
        return cellGcmSetFlipCommand(bufferId);
    }
    { static int n = 0; if (getenv("GCM_FLIPCOUNT") && n++ < 24)
        fprintf(stderr, "[flip-direct] WithWaitLabel ctx=0x%X begin=0x%X end=0x%X cur=0x%X io=0x%X label=%u\n",
                c, c ? vm_read32(c) : 0, c ? vm_read32(c + 4) : 0, c ? vm_read32(c + 8) : 0,
                c ? gcm_ea2io(vm_read32(c + 8)) : 0, labelIndex); }
    return cellGcmSetFlipCommandWithWaitLabel(bufferId, labelIndex, labelValue);
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Guest out-params, like every other pointer parameter in this file. */
    vm_write32((uint32_t)(uintptr_t)address, s_config.localAddress);
    vm_write32((uint32_t)(uintptr_t)size,    s_config.localSize);

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           s_config.localAddress, s_config.localSize);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    s_gcm_initialized = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = 0xFEED0000u;
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_user_handler     = NULL;
    s_second_v_handler = NULL;
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_last_flip_time = 0;
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* Return tile info for a given index */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

/* Return zcull info for a given index */
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;

    /* On hardware this re-points gCellGcmCurrentContext at the default context
     * cellGcmInit built. Zeroing a host-side struct is not that: a title that
     * calls it to go back to the big default buffer stays on whatever smaller
     * segment it had switched to.
     *
     * GCM_DEFAULT_CTX_REPOINT=1 does the real thing -- write our context's EA
     * into the guest variable cellGcmInit's ctx_out pointed at, and rewind that
     * context. Off by default because a title managing its own segment chain
     * may hold pointers into the old one; this exists to be measured, not
     * assumed.
     *
     * Measured once now, on Virtua Fighter 5, which is the case this was
     * written for. VF5's AMGL keeps its own command buffers (it repoints
     * gCellGcmCurrentContext at a 512 KB segment of its own inside the 4 MB it
     * maps) and calls this entry to recover when one fills. Getting a host-side
     * struct zeroed instead leaves it on the exhausted segment, so it allocates
     * another, fills that, and asks again:
     *
     *     [AMGL]:[ERROR] Command Buffer Overflow!   x91, 636 calls to here
     *
     * With the repoint: **0 overflows**, and the command packets reaching the
     * draw engine go from 12,756 to 15,097 over the same five minutes. Still
     * one title's evidence, so still opt-in -- but it is no longer unmeasured,
     * and a port whose title manages its own segments should try it. */
    if (getenv("GCM_DEFAULT_CTX_REPOINT") && s_gcm_ctx_out_ea && s_gcm_context_ea) {
        vm_write32(s_gcm_context_ea + 0x8, vm_read32(s_gcm_context_ea + 0x0));
        vm_write32(s_gcm_ctx_out_ea, s_gcm_context_ea);
        printf("[cellGcmSys] SetDefaultCommandBuffer: repointed "
               "gCellGcmCurrentContext@0x%08X -> 0x%08X\n",
               s_gcm_ctx_out_ea, s_gcm_context_ea);
    }
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* Default command word size: 0x400 (1024) words */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return 0x400;
}

/* Default segment word size: 0x100 (256) words */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return 0x100;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    s_flip_status = status;
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
}

/* Called when a FIFO consumer retires a user-interrupt method. Publishing
 * the cause and pending marker is atomic; guest code runs later in the pump.
 * Like the driver cause register, multiple pending commands coalesce. */
void cellGcmQueueUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
    GCM_USER_PENDING_STORE((1ULL << 32) | cmd);
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    (void)location;  /* We only have one report area */

    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    return s_report_data[index].value;
}

/* ---------------------------------------------------------------------------
 * VRAM upload registry
 *
 * A title whose texture uploads land at an address that does not match the
 * offset it later programs into SET_TEXTURE_OFFSET leaves the sampler reading
 * untouched memory. Recording what was uploaded lets the texture path find the
 * real bytes by size instead of trusting the offset. Diagnostic bridge: it is
 * a lookup, not a fix for the offset bookkeeping itself.
 * -----------------------------------------------------------------------*/
#define RSX_UPLOAD_LOG 256
static struct { u32 dst, size; int claimed; } s_uploads[RSX_UPLOAD_LOG];
static u32 s_upload_n;

void rsx_note_vram_upload(u32 dst_ea, u32 size)
{
    if (!size) return;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].dst     = dst_ea;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].size    = size;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].claimed = 0;
    s_upload_n++;
}

/* OLDEST unclaimed upload of exactly `want` bytes, claimed on return. Matching
 * by size alone hands every same-sized surface the same image; consuming them in
 * order pairs the Nth bind of a size with the Nth upload of that size, which is
 * the order a title loads and then draws its materials in. Returns 0 if none. */
u32 rsx_find_vram_upload(u32 want)
{
    u32 n = s_upload_n < RSX_UPLOAD_LOG ? s_upload_n : RSX_UPLOAD_LOG;
    u32 first = s_upload_n > RSX_UPLOAD_LOG ? s_upload_n - RSX_UPLOAD_LOG : 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (first + i) % RSX_UPLOAD_LOG;
        if (s_uploads[idx].size == want && !s_uploads[idx].claimed) {
            s_uploads[idx].claimed = 1;
            return s_uploads[idx].dst;
        }
    }
    return 0;
}

/* Let the same pairing repeat next frame. */
void rsx_reset_upload_claims(void)
{
    for (u32 i = 0; i < RSX_UPLOAD_LOG; i++) s_uploads[i].claimed = 0;
}

/* ---------------------------------------------------------------------------
 * sys_rsx_* bridge (libs/video/sys_rsx.c)
 *
 * A PS3 firmware module links libgcm statically and drives RSX through the lv2
 * syscalls instead of importing cellGcmSys, so none of the HLE entry points above
 * ever run. Everything the FIFO walker needs is still here -- it just has to be
 * reachable from the syscall layer. These four accessors are that surface; the
 * walker, the method decoder and rsx_live_draw are untouched.
 * -----------------------------------------------------------------------*/

/* Bring the GCM state up the way cellGcmInit would, minus the IO window (the
 * driver maps that itself with sys_rsx_context_iomap). Returns the guest base of
 * RSX local memory, which is what sys_rsx_memory_allocate hands back. */
u32 cellGcm_syscall_bringup(u32 local_size)
{
    if (!s_gcm_initialized)
        cellGcmInit(0x10000, 0, 0);
    if (local_size)
        s_config.localSize = local_size;
    s_gcm_syscall_mode = 1;
    return s_config.localAddress;
}

/* Guest EA of the put/get/ref triple. The driver gets this as
 * sys_rsx_context_allocate's lpar_dma_control + 0x40 (verified in the image:
 * `lwz r3,0x18(r9)` / `addi r3,r3,64`), so the syscall returns this minus 0x40
 * and the driver's own flushes land exactly where the walker reads. */
u32 cellGcm_control_guest_addr(void) { return GCM_CONTROL_GUEST_ADDR; }

/* sys_rsx_context_iomap / iounmap: the same 1MB-page offset table
 * cellGcmMapEaIoAddress fills, which is what gcm_io2ea() and every offset
 * resolution read. */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
{
    gcm_io_lock();
    populate_offset_table(ea, io, size);
    /* A free slot, not s_io_mappings[count]: an unmap leaves a hole, and the
     * count then indexes a live slot. */
    IoMapping* m = find_free_mapping();
    if (m) {
        m->ea = ea; m->io = io; m->size = size; m->active = 1;
        s_io_mapping_count++;
    }
    if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
    gcm_io_unlock();
}

void cellGcm_syscall_iounmap(u32 io, u32 size)
{
    gcm_io_lock();
    u32 page = io >> 20;
    u32 ea = (page < 65536 && s_ea_address_table[page] != 0xFFFF)
             ? ((u32)s_ea_address_table[page] << 20) : 0;
    if (ea) clear_offset_table(ea, io, size);
    /* The mapping list entry goes too: gcm_io_alloc takes cleared table pages
     * as free, and a stale active entry would then claim pages handed out again. */
    IoMapping* m = find_mapping_by_io(io);
    if (m && m->size <= size) { m->active = 0; s_io_mapping_count--; }
    gcm_io_unlock();
}

/* sys_rsx_context_attribute(0x001): the driver's explicit FIFO reset. Move the
 * walker with it -- leaving s_fifo_getoff where it was makes the next drain walk
 * from a stale offset through whatever the driver has since overwritten. */
void cellGcm_syscall_set_fifo(u32 put, u32 get)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get);
    s_fifo_getoff  = get;
    s_fifo_calloff = 0;
}
