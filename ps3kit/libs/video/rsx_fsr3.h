/* AMD FSR 3.1 frame generation for the D3D12 live-draw renderer (ar-fsr, 2026-11-10).
 * Included once by rsx_live_draw.c. Everything is behind environment switches, all off by default:
 *
 *   FSR3_FG=1          frame generation: the swap chain is made through AMD's frame-generation proxy and every game
 *                      frame gets one generated frame in between (shown frames = 2 x game frames). Needs VSync
 *                      (PS3_VSYNC=1) and the game held at half the monitor's refresh (PS3_FPS_LIMIT), and the
 *                      "Disable SPU MLAA" patch: with MLAA the depth at present time is the NEXT frame's.
 *   FSR3_FG_FORCE=1    start it even without the MLAA patch (tests only)
 *   FSR3_DIR=<dir>     where amd_fidelityfx_loader_dx12.dll and its effect DLLs are (default: beside the exe)
 *   FSR3_FG_SPIN=1     AMD's default pacing (a busy-waiting thread); default: hybrid spin + fence waits
 *   FSR3_FG_CHECK=1    FSR's debug checking of the inputs
 *   FSR3_FG_DEBUG=1    FSR's debug tear / reset lines on the generated frames
 *   FSR3_SCALE=1       FSR 3.1 upscaling (af-7): scene frames are rendered with a sub-pixel jitter (the same in the depth
 *                      pre-pass and the EQUAL colour pass: it advances at the next scene's pre-pass, not at the flip)
 *                      and FSR's temporal upscaler makes the window-size picture in place of SMAA (menus, movies,
 *                      loading screens keep the old path). The swap chain follows the window's size.
 *   FSR3_SCALE_SHARP=<0..1>  FSR's own sharpening (default 0: the port's RCAS pass still runs after it)
 *
 * The idea and the measurements behind it come from Ascension-R's FSR 3.1 plan
 * (docs/notes-2026-11-07-fsr31-plan.md, https://github.com/rigaz29/Ascension-R):
 *   Copyright (c) 2026 Ryan Bagas -- MIT License:
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
 *   copyright notice and this permission notice shall be included in all copies or substantial portions of the
 *   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 *   LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 *   SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 * No code of theirs is copied here (their glue targets another runtime); the API use follows AMD's FidelityFX SDK
 * 2.4.0 headers (MIT, third_party/ffx, licence in third_party/ffx/LICENSE.md). */
#ifndef RSX_FSR3_H
#define RSX_FSR3_H
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include "third_party/ffx/api/include/ffx_api.h"
#include "third_party/ffx/api/include/ffx_api_types.h"
#include "third_party/ffx/framegeneration/include/ffx_framegeneration.h"
#include "third_party/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "third_party/ffx/upscalers/include/ffx_upscale.h"

#define FSR3_LISTS 3
static struct {
    int want;                     /* FSR3_FG=1 asked for */
    int on;                       /* proxy + context made */
    HMODULE dll;
    PfnFfxCreateContext Create; PfnFfxDestroyContext Destroy; PfnFfxConfigure Configure;
    PfnFfxQuery Query; PfnFfxDispatch Dispatch;
    ffxContext sc_ctx, fg_ctx;
    IDXGISwapChain4* proxy;
    ID3D12Device* dev; ID3D12CommandQueue* q;
    ID3D12CommandAllocator* alloc[FSR3_LISTS]; ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence; HANDLE ev; unsigned long long fv[FSR3_LISTS], fnext; unsigned ai;
    ID3D12Resource* depth; ID3D12Resource* mv;     /* R32_FLOAT (inverted: 0 = far), R16G16_FLOAT pixels */
    unsigned w, h;                                 /* display = render size (af-0) */
    unsigned long long frame_id, last_us, win_us;
    int was_enabled;
    unsigned win_game, win_gen, win_off, fails;
    /* af-1: the scene's camera and depth, taken at the frame's first depth-EQUAL colour draw (the pre-pass has laid
     * the whole depth by then, and the EQUAL pass writes none): FSR3_FG_CAMERA (default 1 with FSR3_FG). */
    int cam_on;                   /* -1 unread */
    int have;                     /* this frame has a scene camera + depth */
    double m[16], mprev[16];      /* clip = M * world (row-major M[r*4+c]); columns are c264..c267 */
    int have_prev;
    float s[3], t[3];             /* the guest viewport (window = ndc * s + t) */
    float hx, hy;                 /* host pixels per guest viewport pixel */
    unsigned dw, dh;              /* the depth copy's size (host pixels) */
    ID3D12Resource* rawdepth;     /* the depth snapshot's copy, R32_FLOAT, NON_PIXEL_SHADER_RESOURCE */
    ID3D12RootSignature* rs; ID3D12PipelineState* pso; ID3D12DescriptorHeap* heap;
    int cs_failed;
    unsigned win_scene, resets;
    float near_, far_, fovy, pos[3], up[3], right[3], fwd[3];
    /* af-3: moving objects from the title's own motion-blur velocity draws (FSR3_OBJECTS=1, needs motion blur on):
     * their vertex programs read the previous frame's position (v7) through c335..c338; each such draw is drawn a
     * second time, unstretched and unclamped (c467.x = 0, c466.z = huge in a copy of its constants), into vel, a float
     * target cleared to FSR3_VEL_NONE where nothing moved. Its texcoord 0 = (cur - prev ndc) * c466.y + c466.y. */
    int obj_on;
    ID3D12Resource* vel; ID3D12DescriptorHeap* vel_rtv; unsigned vel_w, vel_h;
    unsigned long long vel_stamp;  /* frame_id + 1 of the frame the target was cleared for */
    int vel_have;                  /* this frame has velocity draws in vel */
    float c466y;
    unsigned win_vel_draws, win_vel_skip;
    unsigned win_vel_noreuse;      /* af-final: velocity draws kept off LD_DRAW_REUSE's fast path (so they get their twin) */
    unsigned obj_seen, novel_scene; /* af-final guard: any velocity draw yet; scene frames in a row without one */
    /* the floor: FSR3_FG_FLOOR (default 25; 0 none) game frames a second over 1.5 s, twice in a row, turns generation
     * off (passthrough) until the rate is 2 over it again (Ascension-R's rule: avoid generating from a low base) */
    float floor_fps; int below; unsigned long long fl_us; unsigned fl_n, fl_low; unsigned win_floor;
    /* 0.1.26: the floor's second trip turns generation off for the session (it flipped on and off before: the RX 580
     * tester went 60 -> 7 fps with generation on) */
    unsigned fl_trips; int fl_stuck;
    /* af-7: upscaling */
    int scale_want, scale_on, native;
    ffxContext up_ctx;
    ID3D12Resource* upout;          /* bb size, R8G8B8A8_UNORM, rests in UNORDERED_ACCESS */
    int phases, jit_index, jit_advance;
    float jx, jy, cap_jx, cap_jy;   /* current jitter (render pixels); the one the captured scene used */
    unsigned long long mv_stamp, cam_stamp, up_last_us;
    int cam_reset, up_prev_ok;
    unsigned win_up, win_up_skip;
    /* in-game menu (rsx_ingame_menu.h, ovl): frame generation switched off by the player (the proxy stays and passes
     * real frames through); frames left for which generation is held because the menu's panel is on screen; the pace
     * frame generation chose at start and the limit the player had before it (frames a second, 0 = none); totals of
     * frames with / without generation and of upscaled frames since start (the menu's proof lines). */
    int user_off, menu_hold;
    int live_init;                /* the upscaler is being made by the menu, not by the start-up switches */
    double fg_cap, user_cap;
    unsigned long long tot_gen, tot_off, tot_up, tot_vel;
    /* ovl3: this frame's generation decision (fsr3_fg_decide; made once per frame, also by the in-game overlay layer
     * before it draws) and the UI layer handed to AMD's swap chain (fsr3_fg_ui: the panel + FPS counter, composed by
     * the swap chain onto the real AND the generated frame, so they never go through frame generation) */
    unsigned long long dec_stamp; int dec_enable, dec_game, dec_hold;
    ID3D12Resource* ui_res; unsigned long long ui_stamp; int ui_reg, ui_fail;
} s_fsr3;
#define FSR3_VEL_NONE 30000.0f

static unsigned long long fsr3_now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

static int fsr3_env_on(const char* n) { const char* e = getenv(n); return e && e[0] && e[0] != '0'; }

/* af-final: whether a game patch is on, asked from the game itself (host/ascension_patches.cpp's
 * ascension_patch_on, which applies the table's defaults when the launcher sets no PS3_PATCHES). A build without it
 * links fsr3_patch_unknown in its place (-1: unknown), and then the PS3_PATCHES text decides as before. */
int ascension_patch_on(const char* name);
int fsr3_patch_unknown(const char* name) { (void)name; return -1; }
#pragma comment(linker, "/alternatename:ascension_patch_on=fsr3_patch_unknown")
static int fsr3_patch(const char* name, const char* text)
{
    const int r = ascension_patch_on(name);
    if (r >= 0) return r;
    const char* pt = getenv("PS3_PATCHES");
    return pt && strstr(pt, text) ? 1 : 0;
}

static int fsr3_dll_load(void);
/* FSR3_FG asked for, the DLLs found and the MLAA patch on: 1. Logs why not otherwise. */
/* 0.1.26: the live-draw GPU's name (rsx_live_draw.c fills it when the device is made) */
static wchar_t s_ld_gpu_name[128];
static int fsr3_old_gpu(void)
{
    wchar_t n[128]; size_t i = 0;
    for (; i < 127 && s_ld_gpu_name[i]; i++) n[i] = (wchar_t)towlower(s_ld_gpu_name[i]);
    n[i] = 0;
    if (!n[0]) return 0;
    /* NVIDIA before Turing: GTX 10 / 9 / 7, GT, MX, Quadro P/M/K (GTX 16 and RTX are fine) */
    if (wcsstr(n, L"gtx 10") || wcsstr(n, L"gtx 9") || wcsstr(n, L"gtx 7") || wcsstr(n, L"geforce gt ") ||
        wcsstr(n, L"geforce mx") || wcsstr(n, L"quadro p") || wcsstr(n, L"quadro m") || wcsstr(n, L"quadro k")) return 1;
    /* Intel integrated (Arc is fine) */
    if (wcsstr(n, L"intel") && !wcsstr(n, L"arc")) return 1;
    /* AMD before RDNA: RX 4xx/5xx (three digits; RX 5500-5700 have four), Vega, R5/R7/R9, HD, and the APUs'
     * plain 'Radeon(TM) Graphics' */
    const wchar_t* rx = wcsstr(n, L"rx ");
    if (rx && (rx[3] == L'4' || rx[3] == L'5')) {
        int d = 0; while (rx[3 + d] >= L'0' && rx[3 + d] <= L'9') d++;
        if (d == 3) return 1;
    }
    if (wcsstr(n, L"vega") || wcsstr(n, L"radeon r5") || wcsstr(n, L"radeon r7") || wcsstr(n, L"radeon r9") ||
        wcsstr(n, L"radeon hd") || wcsstr(n, L"radeon(tm) graphics") || wcsstr(n, L"radeon graphics")) return 1;
    return 0;
}

static int fsr3_fg_load(void)
{
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    s_fsr3.want = fsr3_env_on("FSR3_FG");
    s_fsr3.scale_want = fsr3_env_on("FSR3_SCALE");
    /* af-9: FSR3_NATIVE_AA=1: the upscaler at 1x (output = the render size, never the window's): anti-aliasing in
     * place of SMAA */
    s_fsr3.native = fsr3_env_on("FSR3_NATIVE_AA");
    if (s_fsr3.native) s_fsr3.scale_want = 1;
    if (!s_fsr3.want && !s_fsr3.scale_want) return 0;
    { const char* e = getenv("FSR3_FG_CAMERA"); s_fsr3.cam_on = !(e && e[0] == '0'); }
    s_fsr3.obj_on = s_fsr3.cam_on && fsr3_env_on("FSR3_OBJECTS");
    if (s_fsr3.obj_on) {
        if (fsr3_patch("Disable Motion Blur", "Disable Motion Blur")) {   /* af-final: the game's state, defaults included */
            printf("[fsr3] moving objects off: the 'Disable Motion Blur' patch is on (the title's velocity draws are needed)\n");
            s_fsr3.obj_on = 0;
        }
    }
    /* af-9: upscaling keeps a history of past frames; with the camera's motion only, Kratos and enemies (moved by the
     * SPUs, not the camera) leave smeared copies of themselves (user report 2026-11-10, run afru5b). So upscaling and
     * native AA need the moving objects' motion; FSR3_SCALE_FORCE=1 starts them anyway (tests). */
    if (s_fsr3.scale_want && !s_fsr3.obj_on && !fsr3_env_on("FSR3_SCALE_FORCE")) {
        printf("[fsr3] upscaling / native AA off: without moving objects' motion (FSR3_OBJECTS=1 and motion blur on) "
               "Kratos and enemies smear\n");
        s_fsr3.scale_want = 0; s_fsr3.native = 0;
        if (!s_fsr3.want) return 0;
    }
    if (!fsr3_patch("Disable SPU MLAA - God of War: Ascension (01)", "Disable SPU MLAA") && !fsr3_env_on("FSR3_FG_FORCE")) {
        printf("[fsr3] frame generation / upscaling off: MLAA is on (needs the 'Disable SPU MLAA' patch: with MLAA the "
               "depth at present time is the next frame's)\n");
        return 0;
    }
    /* af-final: frame generation paces the game at half the refresh; without Unlock FPS the game is held at 30 and the
     * shown rate is uneven (e.g. 60 on 75 Hz), so it needs that patch */
    if (s_fsr3.want && !fsr3_patch("Unlock FPS", "Unlock FPS") && !fsr3_env_on("FSR3_FG_FORCE")) {
        printf("[fsr3] frame generation off: it needs the 'Unlock FPS' patch (the game is held at 30 without it)\n");
        s_fsr3.want = 0;
        if (!s_fsr3.scale_want) return 0;
    }
    { const char* v = getenv("PS3_VSYNC");
      if (v && v[0] == '0') printf("[fsr3] warning: PS3_VSYNC=0 -- frame generation is meant for VSync on\n"); }
    /* 0.1.26: frame generation needs an RX 5000 / RTX 20 class GPU or newer (AMD's minimum); on older ones (RX 580
     * tester: 7-17 fps with it, 60 without) it only costs. FSR3_FG_OLDGPU=1 keeps it on. */
    if (s_fsr3.want && fsr3_old_gpu() && !fsr3_env_on("FSR3_FG_OLDGPU")) {
        printf("[fsr3] frame generation off: this GPU (%ls) is older than AMD's minimum (Radeon RX 5000 / GeForce RTX 20); "
               "FSR3_FG_OLDGPU=1 keeps it on\n", s_ld_gpu_name);
        s_fsr3.want = 0;
        if (!s_fsr3.scale_want) return 0;
    }
    if (!fsr3_dll_load()) return 0;
    done = 1;
    return 1;
}

/* AMD's loader DLL and its entry points (once; also from the in-game menu when FSR is switched on while playing).
 * 1 when they are there. Leaves the DLL search path on FSR3_DIR: the caller puts it back (SetDllDirectoryW(NULL))
 * once its contexts are made. */
static int fsr3_dll_load(void)
{
    static int st = -1;
    if (st >= 0) return st;
    st = 0;
    wchar_t dir[MAX_PATH] = L"";
    const char* d = getenv("FSR3_DIR");
    if (d && d[0]) MultiByteToWideChar(CP_UTF8, 0, d, -1, dir, MAX_PATH);
    else {
        GetModuleFileNameW(NULL, dir, MAX_PATH);
        wchar_t* s = wcsrchr(dir, L'\\'); if (s) *s = 0;
    }
    SetDllDirectoryW(dir);   /* the loader finds amd_fidelityfx_framegeneration_dx12.dll there */
    wchar_t path[MAX_PATH + 64];
    _snwprintf(path, MAX_PATH + 63, L"%ls\\amd_fidelityfx_loader_dx12.dll", dir); path[MAX_PATH + 63] = 0;
    s_fsr3.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!s_fsr3.dll) {
        printf("[fsr3] frame generation off: amd_fidelityfx_loader_dx12.dll not found in %ls (error %lu)\n",
               dir, (unsigned long)GetLastError());
        return 0;
    }
    s_fsr3.Create    = (PfnFfxCreateContext)(void*)GetProcAddress(s_fsr3.dll, "ffxCreateContext");
    s_fsr3.Destroy   = (PfnFfxDestroyContext)(void*)GetProcAddress(s_fsr3.dll, "ffxDestroyContext");
    s_fsr3.Configure = (PfnFfxConfigure)(void*)GetProcAddress(s_fsr3.dll, "ffxConfigure");
    s_fsr3.Query     = (PfnFfxQuery)(void*)GetProcAddress(s_fsr3.dll, "ffxQuery");
    s_fsr3.Dispatch  = (PfnFfxDispatch)(void*)GetProcAddress(s_fsr3.dll, "ffxDispatch");
    if (!s_fsr3.Create || !s_fsr3.Destroy || !s_fsr3.Configure || !s_fsr3.Query || !s_fsr3.Dispatch) {
        printf("[fsr3] frame generation off: the loader DLL lacks the ffx entry points\n");
        return 0;
    }
    st = 1;
    return 1;
}

static ID3D12Resource* fsr3_tex(ID3D12Device* dev, unsigned w, unsigned h, DXGI_FORMAT f, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = f; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = NULL;   /* committed: zero-filled (0 = far with the inverted depth, no motion) */
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&r))) return NULL;
    r->lpVtbl->SetName(r, name);
    return r;
}

static struct FfxApiResource fsr3_res(ID3D12Resource* r, unsigned w, unsigned h, uint32_t fmt)
{
    struct FfxApiResource x; memset(&x, 0, sizeof x);
    x.resource = r;
    x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D; x.description.format = fmt;
    x.description.width = w; x.description.height = h; x.description.depth = 1; x.description.mipCount = 1;
    x.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_UAV;
    x.state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return x;
}

/* ---- af-1: camera motion from depth -------------------------------------------------------------------------- */
static int fsr3_inv4(const double* m, double* o)   /* general 4x4 inverse, row-major; 0 when singular */
{
    double inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    double det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (det == 0.0 || det != det) return 0;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return 1;
}

static void fsr3_norm3(const double* a, float* o)
{
    double l = sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); if (l <= 0) l = 1;
    o[0] = (float)(a[0]/l); o[1] = (float)(a[1]/l); o[2] = (float)(a[2]/l);
}

/* The camera's pose, near, far and vertical field of view from clip = M * world (M = P * V). The eye maps to clip
 * x = y = w = 0; view depth is row 3; the vertical scale of row 1 against row 3 is 1 / tan(fovy / 2). */
static void fsr3_pose(void)
{
    const double* m = s_fsr3.m;
    const double* r0 = m; const double* r1 = m + 4; const double* r2 = m + 8; const double* r3 = m + 12;
    double c[3];   /* eye: intersection of the planes r0, r1, r3 (r . (x,1) = 0) */
    { const double a[3] = {r0[0], r0[1], r0[2]}, b[3] = {r1[0], r1[1], r1[2]}, d[3] = {r3[0], r3[1], r3[2]};
      double bxd[3] = {b[1]*d[2]-b[2]*d[1], b[2]*d[0]-b[0]*d[2], b[0]*d[1]-b[1]*d[0]};
      double dxa[3] = {d[1]*a[2]-d[2]*a[1], d[2]*a[0]-d[0]*a[2], d[0]*a[1]-d[1]*a[0]};
      double axb[3] = {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
      const double det = a[0]*bxd[0] + a[1]*bxd[1] + a[2]*bxd[2];
      if (det != 0.0)
          for (int i = 0; i < 3; i++) c[i] = -(r0[3]*bxd[i] + r1[3]*dxa[i] + r3[3]*axb[i]) / det;
      else c[0] = c[1] = c[2] = 0; }
    double f[3] = {r3[0], r3[1], r3[2]};
    const double fl = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    fsr3_norm3(f, s_fsr3.fwd);
    double ri[3], u[3];
    for (int i = 0; i < 3; i++) { ri[i] = r0[i]; u[i] = r1[i]; }
    { double k = ri[0]*s_fsr3.fwd[0] + ri[1]*s_fsr3.fwd[1] + ri[2]*s_fsr3.fwd[2];
      for (int i = 0; i < 3; i++) ri[i] -= k * s_fsr3.fwd[i];
      k = u[0]*s_fsr3.fwd[0] + u[1]*s_fsr3.fwd[1] + u[2]*s_fsr3.fwd[2];
      for (int i = 0; i < 3; i++) u[i] -= k * s_fsr3.fwd[i]; }
    const double ul = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    fsr3_norm3(ri, s_fsr3.right); fsr3_norm3(u, s_fsr3.up);
    for (int i = 0; i < 3; i++) s_fsr3.pos[i] = (float)c[i];
    s_fsr3.fovy = (ul > 0 && fl > 0) ? (float)(2.0 * atan(fl / ul)) : 0.9f;
    if (!(s_fsr3.fovy > 0.05f && s_fsr3.fovy < 3.0f)) s_fsr3.fovy = 0.9f;
    /* near / far: where the window depth along the forward axis is 0 and 1 (window z = s2 * clip.z / clip.w + t2) */
    double nf[2];
    for (int k = 0; k < 2; k++) {
        const double v = (double)k, s2 = s_fsr3.s[2], t2 = s_fsr3.t[2];
        const double a = r2[0]*c[0] + r2[1]*c[1] + r2[2]*c[2] + r2[3], b = (r2[0]*f[0] + r2[1]*f[1] + r2[2]*f[2]) / (fl ? fl : 1);
        const double cc = r3[0]*c[0] + r3[1]*c[1] + r3[2]*c[2] + r3[3], d = fl;
        const double den = s2 * b - (v - t2) * d;
        nf[k] = den != 0.0 ? ((v - t2) * cc - s2 * a) / den : 0.0;
    }
    s_fsr3.near_ = nf[0] > 1e-4 ? (float)nf[0] : 0.1f;
    s_fsr3.far_ = nf[1] > s_fsr3.near_ ? (float)nf[1] : s_fsr3.near_ * 10000.0f;
    if (s_fsr3.far_ > 1e7f || s_fsr3.far_ != s_fsr3.far_) s_fsr3.far_ = 1e7f;
}

static int fsr3_cs_init(void)
{
    if (s_fsr3.pso) return 1;
    if (s_fsr3.cs_failed) return 0;
    s_fsr3.cs_failed = 1;
    static const char src[] =
        "cbuffer C : register(b0) { row_major float4x4 inv_cur; row_major float4x4 prev; float4 s; float4 t; };\n"
        "Texture2D<float> D : register(t0);\n"
        "Texture2D<float4> V : register(t1);\n"
        "cbuffer C2 : register(b1) { float4 o; };\n"
        "RWTexture2D<float2> MV : register(u0);\n"
        "RWTexture2D<float> DO : register(u1);\n"
        "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
        "    uint w, h; MV.GetDimensions(w, h);\n"
        "    if (id.x >= w || id.y >= h) return;\n"
        "    uint dw, dh; D.GetDimensions(dw, dh);\n"
        "    const uint kx = max(dw / w, 1u), ky = max(dh / h, 1u);\n"
        "    const uint2 b0 = uint2(id.x * dw / w, id.y * dh / h);\n"   /* the depth copy can be larger: nearest of its block */
        "    float d = 1.0;\n"
        "    for (uint y = 0; y < ky && y < 4; y++) for (uint x = 0; x < kx && x < 4; x++)\n"
        "        d = min(d, D[min(b0 + uint2(x, y), uint2(dw - 1, dh - 1))]);\n"
        "    DO[id.xy] = d;\n"
        "    const float2 win = (float2(id.xy) + 0.5) / float2(s.w, t.w);\n"      /* guest viewport pixels */
        "    const float3 ndc = float3((win - t.xy) / s.xy, (d - t.z) / s.z);\n"
        "    float4 p = mul(inv_cur, float4(ndc, 1.0));\n"
        "    float4 q = mul(prev, p);\n"
        "    float2 mv = 0.0;\n"
        "    if (abs(q.w) > 1e-6 && abs(p.w) > 1e-12) {\n"
        "        const float2 wp = (q.xy / q.w) * s.xy + t.xy;\n"
        "        mv = (wp - win) * float2(s.w, t.w);\n"                       /* host pixels, previous - current */
        "    }\n"
        "    if (o.x > 0.0) {\n"                                                /* objects: the title's motion where it drew */
        "        uint vw, vh; V.GetDimensions(vw, vh);\n"
        "        const uint vx = max(vw / w, 1u), vy = max(vh / h, 1u);\n"
        "        for (uint y = 0; y < vy && y < 4; y++) for (uint x = 0; x < vx && x < 4; x++) {\n"
        "            const float4 ve = V[min(uint2(id.x * vw / w + x, id.y * vh / h + y), uint2(vw - 1, vh - 1))];\n"
        "            if (ve.x < 10000.0) { mv = -((ve.xy - o.y) / o.y) * s.xy * float2(s.w, t.w); x = 99; y = 99; }\n"
        "        }\n"
        "    }\n"
        "    MV[id.xy] = mv;\n"
        "}\n";
    ID3DBlob* cs = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(src, sizeof src - 1, "fsr3_mv", NULL, NULL, "cs", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
        printf("[fsr3] camera motion shader: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        return 0;
    }
    D3D12_DESCRIPTOR_RANGE rg[2]; memset(rg, 0, sizeof rg);
    rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 2;
    rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 2; rg[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER rp[3]; memset(rp, 0, sizeof rp);
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[2].Constants.Num32BitValues = 4;
    rp[2].Constants.ShaderRegister = 1; rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 40;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = rg; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd, 0, sizeof rd); rd.NumParameters = 3; rd.pParameters = rp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { cs->lpVtbl->Release(cs); return 0; }
    ID3D12Device* dev = s_fsr3.dev;
    HRESULT hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                                  &IID_ID3D12RootSignature, (void**)&s_fsr3.rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) { cs->lpVtbl->Release(cs); return 0; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_fsr3.rs;
    pd.CS.pShaderBytecode = cs->lpVtbl->GetBufferPointer(cs); pd.CS.BytecodeLength = cs->lpVtbl->GetBufferSize(cs);
    hr = dev->lpVtbl->CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_fsr3.pso);
    cs->lpVtbl->Release(cs);
    if (FAILED(hr)) { s_fsr3.pso = NULL; return 0; }
    D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fsr3.heap))) { s_fsr3.pso = NULL; return 0; }
    s_fsr3.cs_failed = 0;
    return 1;
}

/* The depth copy at w x h (made again when the size changes), its descriptors and the motion target. */
static int fsr3_raw(unsigned w, unsigned h)
{
    if (s_fsr3.rawdepth && s_fsr3.dw == w && s_fsr3.dh == h) return 1;
    /* depth and motion go to FSR at the swap chain's size (a smaller scene depth is sampled up) */
    if (!fsr3_cs_init()) return 0;
    if (s_fsr3.rawdepth) {   /* wait for the GPU before dropping the old ones */
        if (s_fsr3.fnext) {
            s_fsr3.fence->lpVtbl->SetEventOnCompletion(s_fsr3.fence, s_fsr3.fnext, s_fsr3.ev);
            WaitForSingleObject(s_fsr3.ev, 2000);
        }
        s_fsr3.rawdepth->lpVtbl->Release(s_fsr3.rawdepth); s_fsr3.rawdepth = NULL;
    }
    ID3D12Device* dev = s_fsr3.dev;
    s_fsr3.rawdepth = fsr3_tex(dev, w, h, DXGI_FORMAT_R32_FLOAT, L"fsr3-scene-depth");
    if (!s_fsr3.rawdepth) return 0;
    s_fsr3.dw = w; s_fsr3.dh = h;
    const UINT step = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0; s_fsr3.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fsr3.heap, &h0);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    dev->lpVtbl->CreateShaderResourceView(dev, s_fsr3.rawdepth, &sv, h0);
    { D3D12_CPU_DESCRIPTOR_HANDLE h1 = h0; h1.ptr += step;   /* t1: the velocity target, or a null view */
      D3D12_SHADER_RESOURCE_VIEW_DESC vv = sv; vv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      dev->lpVtbl->CreateShaderResourceView(dev, s_fsr3.vel, &vv, h1); }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv; memset(&uv, 0, sizeof uv);
    uv.Format = DXGI_FORMAT_R16G16_FLOAT; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    h0.ptr += 2 * step;
    dev->lpVtbl->CreateUnorderedAccessView(dev, s_fsr3.mv, NULL, &uv, h0);
    uv.Format = DXGI_FORMAT_R32_FLOAT; h0.ptr += step;
    dev->lpVtbl->CreateUnorderedAccessView(dev, s_fsr3.depth, NULL, &uv, h0);
    printf("[fsr3] scene depth %ux%u -> depth and camera motion at %ux%u\n", w, h, s_fsr3.w, s_fsr3.h);
    return 1;
}

/* The velocity target at w x h (R32G32B32A32_FLOAT, RENDER_TARGET), its RTV, and t1 in the pass's heap. */
static int fsr3_vel_target(unsigned w, unsigned h)
{
    if (s_fsr3.vel && s_fsr3.vel_w == w && s_fsr3.vel_h == h) return 1;
    if (!s_fsr3.heap || !s_fsr3.rawdepth) return 0;   /* the camera pass first */
    ID3D12Device* dev = s_fsr3.dev;
    if (s_fsr3.vel) {
        if (s_fsr3.fnext) {
            s_fsr3.fence->lpVtbl->SetEventOnCompletion(s_fsr3.fence, s_fsr3.fnext, s_fsr3.ev);
            WaitForSingleObject(s_fsr3.ev, 2000);
        }
        return 0;   /* size changed mid-run: no objects (keeps the old target alive for the lists in flight) */
    }
    if (!s_fsr3.vel_rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fsr3.vel_rtv))) return 0;
    }
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv; memset(&cv, 0, sizeof cv); cv.Format = rd.Format;
    cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = FSR3_VEL_NONE;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    &cv, &IID_ID3D12Resource, (void**)&s_fsr3.vel))) { s_fsr3.vel = NULL; return 0; }
    s_fsr3.vel->lpVtbl->SetName(s_fsr3.vel, L"fsr3-velocity");
    s_fsr3.vel_w = w; s_fsr3.vel_h = h;
    D3D12_CPU_DESCRIPTOR_HANDLE r0; s_fsr3.vel_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fsr3.vel_rtv, &r0);
    dev->lpVtbl->CreateRenderTargetView(dev, s_fsr3.vel, NULL, r0);
    const UINT step = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h1; s_fsr3.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fsr3.heap, &h1); h1.ptr += step;
    D3D12_SHADER_RESOURCE_VIEW_DESC vv; memset(&vv, 0, sizeof vv);
    vv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; vv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    vv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; vv.Texture2D.MipLevels = 1;
    dev->lpVtbl->CreateShaderResourceView(dev, s_fsr3.vel, &vv, h1);
    printf("[fsr3] moving objects: velocity target %ux%u\n", w, h);
    return 1;
}

/* At present, in the prepare list: motion from the depth copy (previous - current camera), into s_fsr3.mv. */
static void fsr3_cam_motion(ID3D12GraphicsCommandList* l)
{
    float k[40];
    double inv[16];
    if (!fsr3_inv4(s_fsr3.m, inv)) { for (int i = 0; i < 16; i++) inv[i] = (i % 5) ? 0.0 : 1.0; }
    const double* pv = s_fsr3.have_prev ? s_fsr3.mprev : s_fsr3.m;
    for (int i = 0; i < 16; i++) { k[i] = (float)inv[i]; k[16 + i] = (float)pv[i]; }
    /* output (swap chain) pixels per guest viewport pixel */
    const float ox = s_fsr3.hx * (float)s_fsr3.w / (float)s_fsr3.dw, oy = s_fsr3.hy * (float)s_fsr3.h / (float)s_fsr3.dh;
    k[32] = s_fsr3.s[0]; k[33] = s_fsr3.s[1]; k[34] = s_fsr3.s[2] != 0 ? s_fsr3.s[2] : 1.0f; k[35] = ox;
    k[36] = s_fsr3.t[0]; k[37] = s_fsr3.t[1]; k[38] = s_fsr3.t[2]; k[39] = oy;
    D3D12_RESOURCE_BARRIER b[2]; memset(b, 0, sizeof b);
    for (int i = 0; i < 2; i++) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition.pResource = i ? s_fsr3.depth : s_fsr3.mv;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    l->lpVtbl->ResourceBarrier(l, 2, b);
    l->lpVtbl->SetDescriptorHeaps(l, 1, &s_fsr3.heap);
    l->lpVtbl->SetComputeRootSignature(l, s_fsr3.rs);
    l->lpVtbl->SetPipelineState(l, s_fsr3.pso);
    l->lpVtbl->SetComputeRoot32BitConstants(l, 0, 40, k, 0);
    const int use_vel = s_fsr3.obj_on && s_fsr3.vel && s_fsr3.vel_have;
    { const float o2[4] = { use_vel ? 1.0f : 0.0f, s_fsr3.c466y != 0.0f ? s_fsr3.c466y : 0.5f, 0.0f, 0.0f };
      l->lpVtbl->SetComputeRoot32BitConstants(l, 2, 4, o2, 0); }
    D3D12_RESOURCE_BARRIER vb; memset(&vb, 0, sizeof vb);
    vb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; vb.Transition.pResource = s_fsr3.vel;
    vb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    vb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    vb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s_fsr3.vel) l->lpVtbl->ResourceBarrier(l, 1, &vb);
    D3D12_GPU_DESCRIPTOR_HANDLE g0; s_fsr3.heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_fsr3.heap, &g0);
    l->lpVtbl->SetComputeRootDescriptorTable(l, 1, g0);
    l->lpVtbl->Dispatch(l, (s_fsr3.w + 7) / 8, (s_fsr3.h + 7) / 8, 1);
    vb.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    vb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s_fsr3.vel) l->lpVtbl->ResourceBarrier(l, 1, &vb);
    for (int i = 0; i < 2; i++) {
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    l->lpVtbl->ResourceBarrier(l, 2, b);
}

/* Once per frame: the pose and whether the camera cut (a reset). stamp = frame_id + 1 of the frame it was done for. */
static int fsr3_frame_camera(void)
{
    if (s_fsr3.cam_stamp == s_fsr3.frame_id + 1u) return s_fsr3.cam_reset;
    s_fsr3.cam_stamp = s_fsr3.frame_id + 1u;
    fsr3_pose();
    if (s_fsr3.have_prev) {   /* a cut: the matrix moved by a quarter of its size or more in one frame */
        double dn = 0, nn = 0;
        for (int i = 0; i < 16; i++) { const double d = s_fsr3.m[i] - s_fsr3.mprev[i]; dn += d * d; nn += s_fsr3.m[i] * s_fsr3.m[i]; }
        if (dn > 0.0625 * nn) { s_fsr3.have_prev = 0; s_fsr3.resets++; }
    }
    s_fsr3.cam_reset = !s_fsr3.have_prev;
    return s_fsr3.cam_reset;
}

static ffxReturnCode_t fsr3_fg_dispatch_cb(ffxDispatchDescFrameGeneration* p, void* user)
{
    (void)user;
    return s_fsr3.Dispatch(&s_fsr3.fg_ctx, &p->header);
}

/* In place of CreateSwapChainForHwnd: the frame-generation proxy (an IDXGISwapChain4) and the generation context.
 * 1 and *out set when made; 0 = make the plain swap chain as before. */
static int fsr3_fg_make_swapchain(void* factory, ID3D12Device* dev, ID3D12CommandQueue* q, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* scd, IDXGISwapChain1** out)
{
    if (!fsr3_fg_load() || !s_fsr3.want) return 0;
    struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 sd; memset(&sd, 0, sizeof sd);
    sd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    IDXGISwapChain4* proxy = NULL;
    sd.swapchain = &proxy; sd.hwnd = hwnd; sd.desc = scd; sd.fullscreenDesc = NULL;
    sd.dxgiFactory = (IDXGIFactory*)factory; sd.gameQueue = q;
    struct ffxCreateContextDescFrameGenerationSwapChainVersionDX12 sv; memset(&sv, 0, sizeof sv);
    sv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    sv.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    sd.header.pNext = &sv.header;
    ffxReturnCode_t rc = s_fsr3.Create(&s_fsr3.sc_ctx, &sd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !proxy) {
        printf("[fsr3] frame generation off: the proxy swap chain was not made (ffx %u)\n", (unsigned)rc);
        return 0;
    }
    /* Pacing: AMD's default spins a high-priority thread, which costs this CPU-bound game frames (Ascension-R
     * measured 28.52 -> 29.01 flips/s and 113% -> 25% CPU with these two). */
    if (!fsr3_env_on("FSR3_FG_SPIN")) {
        FfxApiSwapchainFramePacingTuning t; memset(&t, 0, sizeof t);
        t.safetyMarginInMs = 0.1f; t.varianceFactor = 0.1f; t.allowHybridSpin = true; t.hybridSpinTime = 2;
        t.allowWaitForSingleObjectOnFence = true;
        struct ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 kv; memset(&kv, 0, sizeof kv);
        kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING; kv.ptr = &t;
        rc = s_fsr3.Configure(&s_fsr3.sc_ctx, &kv.header);
        if (rc != FFX_API_RETURN_OK) printf("[fsr3] pacing tuning not taken (ffx %u)\n", (unsigned)rc);
    }
    struct ffxCreateBackendDX12Desc be; memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = dev;
    struct ffxCreateContextDescFrameGenerationVersion fv; memset(&fv, 0, sizeof fv);
    fv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION; fv.version = FFX_FRAMEGENERATION_VERSION;
    struct ffxCreateContextDescFrameGeneration fd; memset(&fd, 0, sizeof fd);
    fd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fd.flags = (s_fsr3.cam_on ? 0 : FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) |
               (fsr3_env_on("FSR3_FG_CHECK") ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0);
    fd.displaySize.width = scd->Width; fd.displaySize.height = scd->Height;
    fd.maxRenderSize = fd.displaySize;
    fd.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fd.header.pNext = &fv.header; fv.header.pNext = &be.header;
    rc = s_fsr3.Create(&s_fsr3.fg_ctx, &fd.header, NULL);
    if (rc != FFX_API_RETURN_OK) {
        printf("[fsr3] frame generation off: context not made (ffx %u); plain swap chain\n", (unsigned)rc);
        s_fsr3.Destroy(&s_fsr3.sc_ctx, NULL);   /* releases the proxy */
        return 0;
    }
    s_fsr3.dev = dev; s_fsr3.q = q; s_fsr3.w = scd->Width; s_fsr3.h = scd->Height;
    { const char* e = getenv("FSR3_FG_FLOOR"); s_fsr3.floor_fps = e ? (float)atof(e) : 25.0f; }
    /* The pace (FSR3_FG_AUTOCAP, default on): with VSync each shown frame should stay a whole number of refreshes, so the
     * game runs at half the monitor's refresh -- or a quarter past 120 Hz, as a PS3 game flips 60 at most (144 -> 36,
     * 240 -> 60). FSR3_FG_REFRESH=<Hz> gives the rate by hand (a variable-refresh monitor). A lower PS3_FPS_LIMIT stays. */
    const char* vs0 = getenv("PS3_VSYNC");
    if ((fsr3_env_on("FSR3_FG_AUTOCAP") || !getenv("FSR3_FG_AUTOCAP")) && !(vs0 && vs0[0] == '0')) {
        double hz = 0;
        const char* r = getenv("FSR3_FG_REFRESH");
        if (r && atof(r) > 1) hz = atof(r);
        else {
            MONITORINFOEXW mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
            DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
            if (hwnd && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi) &&
                EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                hz = dm.dmDisplayFrequency == 59 ? 59.94 : (double)dm.dmDisplayFrequency;
        }
        if (hz > 1) {
            double cap = hz / 2.0;
            while (cap > 60.5) cap /= 2.0;
            const char* l = getenv("PS3_FPS_LIMIT");
            const double have = l ? atof(l) : 0.0;
            s_fsr3.fg_cap = cap; s_fsr3.user_cap = have;   /* in-game menu: both paces, for switching while playing */
            if (have <= 0.0 || have > cap + 0.01) {
                char v[32]; snprintf(v, sizeof v, "%.3f", cap);
                SetEnvironmentVariableA("PS3_FPS_LIMIT", v); _putenv_s("PS3_FPS_LIMIT", v);
                printf("[fsr3] game paced at %.2f frames/s for a %.2f Hz display (was %s)\n", cap, hz, l ? l : "no cap");
            }
        }
    }
    int ok = 1;
    for (int i = 0; i < FSR3_LISTS && ok; i++)
        ok = SUCCEEDED(dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       &IID_ID3D12CommandAllocator, (void**)&s_fsr3.alloc[i]));
    ok = ok && SUCCEEDED(dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_fsr3.alloc[0], NULL,
                         &IID_ID3D12GraphicsCommandList, (void**)&s_fsr3.list));
    if (ok) s_fsr3.list->lpVtbl->Close(s_fsr3.list);
    ok = ok && SUCCEEDED(dev->lpVtbl->CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_fsr3.fence));
    s_fsr3.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_fsr3.depth = fsr3_tex(dev, s_fsr3.w, s_fsr3.h, DXGI_FORMAT_R32_FLOAT, L"fsr3-depth");
    s_fsr3.mv = fsr3_tex(dev, s_fsr3.w, s_fsr3.h, DXGI_FORMAT_R16G16_FLOAT, L"fsr3-motion");
    if (!ok || !s_fsr3.ev || !s_fsr3.depth || !s_fsr3.mv) {
        printf("[fsr3] frame generation off: own resources not made\n");
        s_fsr3.Destroy(&s_fsr3.fg_ctx, NULL); s_fsr3.Destroy(&s_fsr3.sc_ctx, NULL);
        return 0;
    }
    const char* ver = "?";
    { struct ffxQueryGetProviderVersion pv; memset(&pv, 0, sizeof pv);
      pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
      if (s_fsr3.Query(&s_fsr3.fg_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) ver = pv.versionName; }
    s_fsr3.proxy = proxy;
    s_fsr3.on = 1;
    *out = (IDXGISwapChain1*)proxy;   /* IDXGISwapChain4 derives from IDXGISwapChain1 */
    printf("[fsr3] frame generation on: %ux%u, provider %s, pacing %s\n", s_fsr3.w, s_fsr3.h, ver,
           fsr3_env_on("FSR3_FG_SPIN") ? "AMD default (spin)" : "hybrid spin + fence waits");
    fflush(stdout);
    return 1;
}

/* Right before Present on the proxy. game: a game frame (not a movie, not the loading turbo). */
static void fsr3_frame_end(void);
/* ovl3: whether this frame gets a generated frame. Made once per frame (frame_id), the first time it is asked: by the
 * in-game overlay layer before it draws (ld_fgui_frame) or else right before Present -- both then agree. The floor
 * is counted here, once per frame. */
static int fsr3_fg_decide(int game)
{
    if (s_fsr3.dec_stamp == s_fsr3.frame_id + 1 && s_fsr3.dec_game == game) return s_fsr3.dec_enable;
    const int first = s_fsr3.dec_stamp != s_fsr3.frame_id + 1;
    const unsigned long long now = fsr3_now_us();
    /* camera mode: only frames with a scene camera and depth (menus, movies, loading screens: passed through) */
    const int scene = s_fsr3.cam_on ? (s_fsr3.have && s_fsr3.rawdepth && s_fsr3.pso) : 1;
    /* the floor (counted on game frames only) */
    if (first) {
    if (game && s_fsr3.floor_fps > 0.0f) {
        if (!s_fsr3.fl_us) s_fsr3.fl_us = now;
        s_fsr3.fl_n++;
        if (now - s_fsr3.fl_us >= 1500000ull) {
            const float r = (float)s_fsr3.fl_n * 1e6f / (float)(now - s_fsr3.fl_us);
            if (!s_fsr3.below) {
                s_fsr3.fl_low = r < s_fsr3.floor_fps ? s_fsr3.fl_low + 1 : 0;
                if (s_fsr3.fl_low >= 2) {
                    s_fsr3.below = 1;
                    if (++s_fsr3.fl_trips >= 2 && !s_fsr3.fl_stuck) {
                        s_fsr3.fl_stuck = 1;
                        printf("[fsr3] frame generation off for this session: the game fell below %.0f frames/s twice with it "
                               "(this PC runs better without it: set AMD FSR 3.1 to Off in the launcher)\n", s_fsr3.floor_fps);
                    } else if (!s_fsr3.fl_stuck) printf("[fsr3] below the floor (%.1f frames/s): generation paused\n", r);
                }
            }
            else if (!s_fsr3.fl_stuck && r >= s_fsr3.floor_fps + 2.0f) { s_fsr3.below = 0; s_fsr3.fl_low = 0; }
            s_fsr3.fl_us = now; s_fsr3.fl_n = 0;
        }
    }
    if (s_fsr3.below) s_fsr3.win_floor++;
    s_fsr3.dec_hold = s_fsr3.menu_hold > 0;
    if (s_fsr3.menu_hold > 0) s_fsr3.menu_hold--;
    }
    /* in-game menu: no generated frames when the player switched generation off; and none while its panel is on
     * screen (+ a few frames after) ONLY when the UI layer cannot be used (menu_hold, fallback) */
    const int menu_no = s_fsr3.user_off || s_fsr3.dec_hold;
    s_fsr3.dec_enable = game && scene && !s_fsr3.below && !menu_no;
    s_fsr3.dec_stamp = s_fsr3.frame_id + 1; s_fsr3.dec_game = game;
    return s_fsr3.dec_enable;
}
/* ovl3: the UI layer for this Present (res = the overlay's own texture, display size, premultiplied alpha, in
 * PIXEL_SHADER_RESOURCE; NULL = none: the overlay was drawn into the frame itself). AMD's swap chain copies it at
 * Present (internal double buffering) and composes it onto the real and the generated frame. 0 when refused. */
static int fsr3_fg_ui(ID3D12Resource* res)
{
    if (!s_fsr3.on || s_fsr3.ui_fail) return res == NULL;
    if (!res && !s_fsr3.ui_reg) return 1;   /* nothing registered: nothing to take away */
    struct ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 u; memset(&u, 0, sizeof u);
    u.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    if (res) {
        u.uiResource = fsr3_res(res, s_fsr3.w, s_fsr3.h, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM);
        u.uiResource.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
        u.uiResource.state = FFX_API_RESOURCE_STATE_PIXEL_READ;
        u.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                  FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    }
    const ffxReturnCode_t rc = s_fsr3.Configure(&s_fsr3.sc_ctx, &u.header);
    if (rc != FFX_API_RETURN_OK) {
        if (res) { s_fsr3.ui_fail = 1; printf("[fsr3] UI layer not taken by the swap chain (ffx %u): overlay drawn into the frame, generation rests under the panel\n", (unsigned)rc); }
        return 0;
    }
    if (res && !s_fsr3.ui_reg) { static int logs; if (logs++ < 4) printf("[fsr3] UI layer on: the overlay is composed onto real and generated frames\n"); fflush(stdout); }
    s_fsr3.ui_reg = res != NULL;
    return 1;
}
static void fsr3_fg_before_present(int game)
{
    if (!s_fsr3.on) {   /* upscaling alone: the frame's bookkeeping only */
        if (s_fsr3.scale_on) { s_fsr3.frame_id++; fsr3_frame_end(); }
        return;
    }
    const unsigned long long now = fsr3_now_us();
    const float dt = s_fsr3.last_us ? (float)(now - s_fsr3.last_us) / 1000.0f : 16.7f;
    s_fsr3.last_us = now;
    const int enable = fsr3_fg_decide(game);
    /* ovl3: the overlay's layer when the overlay drew into it this frame (ld_fgui_frame: only on frames with
     * generation), else none */
    (void)fsr3_fg_ui(s_fsr3.ui_stamp == s_fsr3.frame_id + 1 ? s_fsr3.ui_res : NULL);
    s_fsr3.ui_res = NULL;
    int reset = !s_fsr3.was_enabled;
    if (enable && s_fsr3.cam_on) {
        if (fsr3_frame_camera()) reset = 1;
        s_fsr3.win_scene++;
    }
    s_fsr3.frame_id++;
    struct ffxConfigureDescFrameGeneration c; memset(&c, 0, sizeof c);
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = s_fsr3.proxy;
    c.frameGenerationCallback = fsr3_fg_dispatch_cb;
    c.frameGenerationEnabled = enable ? true : false;
    c.allowAsyncWorkloads = false;
    c.flags = fsr3_env_on("FSR3_FG_DEBUG") ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
                                              FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS) : 0;
    c.generationRect.left = 0; c.generationRect.top = 0;
    c.generationRect.width = (int)s_fsr3.w; c.generationRect.height = (int)s_fsr3.h;
    c.frameID = s_fsr3.frame_id;
    ffxReturnCode_t rc = s_fsr3.Configure(&s_fsr3.fg_ctx, &c.header);
    if (rc != FFX_API_RETURN_OK && s_fsr3.fails++ < 8) printf("[fsr3] configure failed (ffx %u)\n", (unsigned)rc);
    if (enable) {
        const unsigned i = s_fsr3.ai; s_fsr3.ai = (s_fsr3.ai + 1) % FSR3_LISTS;
        if (s_fsr3.fv[i] && s_fsr3.fence->lpVtbl->GetCompletedValue(s_fsr3.fence) < s_fsr3.fv[i]) {
            s_fsr3.fence->lpVtbl->SetEventOnCompletion(s_fsr3.fence, s_fsr3.fv[i], s_fsr3.ev);
            WaitForSingleObject(s_fsr3.ev, 2000);
        }
        s_fsr3.alloc[i]->lpVtbl->Reset(s_fsr3.alloc[i]);
        s_fsr3.list->lpVtbl->Reset(s_fsr3.list, s_fsr3.alloc[i], NULL);
        struct ffxDispatchDescFrameGenerationPrepareV2 p; memset(&p, 0, sizeof p);
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = s_fsr3.frame_id; p.flags = c.flags; p.commandList = s_fsr3.list;
        p.renderSize.width = s_fsr3.w; p.renderSize.height = s_fsr3.h;
        p.jitterOffset.x = 0; p.jitterOffset.y = 0;
        p.motionVectorScale.x = 1.0f; p.motionVectorScale.y = 1.0f;   /* pixels */
        p.frameTimeDelta = dt;
        p.reset = reset ? true : false;
        p.cameraNear = 0.25f; p.cameraFar = 4000.0f; p.cameraFovAngleVertical = 0.9f; p.viewSpaceToMetersFactor = 1.0f;
        p.depth = fsr3_res(s_fsr3.depth, s_fsr3.w, s_fsr3.h, FFX_API_SURFACE_FORMAT_R32_FLOAT);
        p.motionVectors = fsr3_res(s_fsr3.mv, s_fsr3.w, s_fsr3.h, FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
        p.cameraUp[1] = 1.0f; p.cameraRight[0] = 1.0f; p.cameraForward[2] = 1.0f;
        if (s_fsr3.cam_on) {
            if (s_fsr3.mv_stamp != s_fsr3.frame_id) fsr3_cam_motion(s_fsr3.list);   /* the upscaler may have made it */
            p.cameraNear = s_fsr3.near_; p.cameraFar = s_fsr3.far_; p.cameraFovAngleVertical = s_fsr3.fovy;
            for (int k = 0; k < 3; k++) { p.cameraPosition[k] = s_fsr3.pos[k]; p.cameraUp[k] = s_fsr3.up[k];
                                          p.cameraRight[k] = s_fsr3.right[k]; p.cameraForward[k] = s_fsr3.fwd[k]; }
        }
        rc = s_fsr3.Dispatch(&s_fsr3.fg_ctx, &p.header);
        if (rc != FFX_API_RETURN_OK && s_fsr3.fails++ < 8) printf("[fsr3] prepare failed (ffx %u)\n", (unsigned)rc);
        s_fsr3.list->lpVtbl->Close(s_fsr3.list);
        ID3D12CommandList* l = (ID3D12CommandList*)s_fsr3.list;
        s_fsr3.q->lpVtbl->ExecuteCommandLists(s_fsr3.q, 1, &l);
        s_fsr3.fv[i] = ++s_fsr3.fnext;
        s_fsr3.q->lpVtbl->Signal(s_fsr3.q, s_fsr3.fence, s_fsr3.fv[i]);
        s_fsr3.win_gen++; s_fsr3.tot_gen++;
    } else { s_fsr3.win_off++; s_fsr3.tot_off++; }
    s_fsr3.was_enabled = enable;
    fsr3_frame_end();
    s_fsr3.win_game++;
    if (!s_fsr3.win_us) s_fsr3.win_us = now;
    else if (now - s_fsr3.win_us >= 10000000ull) {
        const double sec = (double)(now - s_fsr3.win_us) / 1e6;
        printf("[fsr3] %.1f game frames/s (%u with generation, %u without; %u scene cameras, %u cuts) -> ~%.1f shown/s; near %.3g far %.3g fovy %.1f deg; objects %s: %u velocity draws (%u kept off draw reuse), %u skipped; %u below the floor\n",
               (double)s_fsr3.win_game / sec, s_fsr3.win_gen, s_fsr3.win_off, s_fsr3.win_scene, s_fsr3.resets,
               (double)(s_fsr3.win_game + s_fsr3.win_gen) / sec, s_fsr3.near_, s_fsr3.far_, s_fsr3.fovy * 57.2958f,
               s_fsr3.obj_on ? "on" : "off", s_fsr3.win_vel_draws, s_fsr3.win_vel_noreuse, s_fsr3.win_vel_skip, s_fsr3.win_floor);
        s_fsr3.win_floor = 0;
        s_fsr3.win_vel_draws = s_fsr3.win_vel_skip = s_fsr3.win_vel_noreuse = 0;
        s_fsr3.win_game = s_fsr3.win_gen = s_fsr3.win_off = s_fsr3.win_scene = s_fsr3.resets = 0; s_fsr3.win_us = now;
    }
}
/* The frame's end (after its present): its camera becomes the previous one; the next frame looks for its own. */
static void fsr3_frame_end(void)
{
    if (s_fsr3.cam_on) {
        if (s_fsr3.have) { memcpy(s_fsr3.mprev, s_fsr3.m, sizeof s_fsr3.m); s_fsr3.have_prev = 1; }
        else s_fsr3.have_prev = 0;
        /* af-final guard: native AA / upscaling need the objects' motion. If no scene frame ever had a velocity draw
         * (motion blur really off, or the title changed), go back to SMAA after ~4 s of scene frames and say why. */
        if (s_fsr3.scale_on && s_fsr3.have && !s_fsr3.obj_seen) {
            if (s_fsr3.vel_have) s_fsr3.obj_seen = 1;
            else if (++s_fsr3.novel_scene >= 240u && !fsr3_env_on("FSR3_SCALE_FORCE")) {
                s_fsr3.scale_on = 0;
                printf("[fsr3] native AA / upscaling off: 240 scene frames without one velocity draw (is motion blur "
                       "disabled?); back to SMAA\n");
                fflush(stdout);
            }
        }
        s_fsr3.have = 0;
    }
    s_fsr3.vel_have = 0;
    if (s_fsr3.scale_on && !s_fsr3.on) {
        const unsigned long long now = fsr3_now_us();
        if (!s_fsr3.win_us) s_fsr3.win_us = now;
        else if (now - s_fsr3.win_us >= 10000000ull) {
            printf("[fsr3] upscaling: %u frames upscaled, %u not (no scene camera or size), jitter phases %d; %u velocity draws (%u kept off draw reuse), %u skipped\n",
                   s_fsr3.win_up, s_fsr3.win_up_skip, s_fsr3.phases, s_fsr3.win_vel_draws, s_fsr3.win_vel_noreuse, s_fsr3.win_vel_skip);
            s_fsr3.win_up = s_fsr3.win_up_skip = 0; s_fsr3.win_vel_draws = s_fsr3.win_vel_skip = s_fsr3.win_vel_noreuse = 0; s_fsr3.win_us = now;
        }
    }
}

/* af-final: a frame held back (LD_PSO_LOAD_HOLD, pipelines compiling) is never presented: it still ends, and its
 * camera / depth / jitter are dropped (have = 0, so the next shown frame starts FSR's history again) instead of being
 * reused by the next frame whose pre-pass already moved to the next jitter (review af-10). */
static void fsr3_frame_held(void)
{
    if (!(s_fsr3.on || s_fsr3.scale_on)) return;
    s_fsr3.frame_id++;
    s_fsr3.have = 0;
    s_fsr3.was_enabled = 0;
    fsr3_frame_end();
}

/* ---- af-7: upscaling --------------------------------------------------------------------------------------------- */
static void fsr3_jitter_set(void)
{
    if (s_fsr3.phases <= 0) { s_fsr3.jx = s_fsr3.jy = 0; return; }
    struct ffxQueryDescUpscaleGetJitterOffset q; memset(&q, 0, sizeof q);
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = s_fsr3.jit_index; q.phaseCount = s_fsr3.phases; q.pOutX = &s_fsr3.jx; q.pOutY = &s_fsr3.jy;
    if (s_fsr3.Query(&s_fsr3.up_ctx, &q.header) != FFX_API_RETURN_OK) s_fsr3.jx = s_fsr3.jy = 0;
}

/* After the swap chain is made: the upscaler's context for render rw x rh -> the swap chain's bw x bh. */
static void fsr3_scale_init(ID3D12Device* dev, ID3D12CommandQueue* q, unsigned rw, unsigned rh, unsigned bw, unsigned bh)
{
    /* in-game menu (live_init): switched on while playing, where the start-up switches said no FSR */
    if (!(s_fsr3.live_init ? fsr3_dll_load() : fsr3_fg_load()) || !s_fsr3.scale_want) return;
    if (!s_fsr3.cam_on) { printf("[fsr3] upscaling off: it needs the scene camera (FSR3_FG_CAMERA=0)\n"); return; }
    if (rw > bw || rh > bh) { printf("[fsr3] upscaling off: render %ux%u above the swap chain %ux%u\n", rw, rh, bw, bh); return; }
    /* af-10: from a smaller picture (e.g. 1280x720 -> 2560x1440) Kratos still breaks up in fights even with the
     * objects' motion (their motion comes from the title's 640x360 blur pass: too coarse); MEASURED no speed gain on a
     * fast PC either. Only native AA (same size in and out) by default; FSR3_SCALE_FORCE=1 allows it (tests). */
    if ((rw < bw || rh < bh) && !fsr3_env_on("FSR3_SCALE_FORCE")) {
        printf("[fsr3] upscaling off: %ux%u -> %ux%u would upscale; only native AA (render = output, e.g. the default 2x "
               "picture on a 1440p screen) is on offer (FSR3_SCALE_FORCE=1 for tests)\n", rw, rh, bw, bh);
        return;
    }
    if (!s_fsr3.dev) { s_fsr3.dev = dev; s_fsr3.q = q; s_fsr3.w = bw; s_fsr3.h = bh; }
    if (!s_fsr3.depth) s_fsr3.depth = fsr3_tex(dev, bw, bh, DXGI_FORMAT_R32_FLOAT, L"fsr3-depth");
    if (!s_fsr3.mv) s_fsr3.mv = fsr3_tex(dev, bw, bh, DXGI_FORMAT_R16G16_FLOAT, L"fsr3-motion");
    if (!s_fsr3.fence) {
        dev->lpVtbl->CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_fsr3.fence);
        s_fsr3.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    struct ffxCreateBackendDX12Desc be; memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = dev;
    struct ffxCreateContextDescUpscaleVersion uv; memset(&uv, 0, sizeof uv);
    uv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION; uv.version = FFX_UPSCALER_VERSION;
    struct ffxCreateContextDescUpscale ud; memset(&ud, 0, sizeof ud);
    ud.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    ud.flags = FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               (fsr3_env_on("FSR3_FG_CHECK") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
    ud.maxRenderSize.width = rw; ud.maxRenderSize.height = rh;
    ud.maxUpscaleSize.width = bw; ud.maxUpscaleSize.height = bh;
    ud.header.pNext = &uv.header; uv.header.pNext = &be.header;
    ffxReturnCode_t rc = s_fsr3.Create(&s_fsr3.up_ctx, &ud.header, NULL);
    if (rc != FFX_API_RETURN_OK) { printf("[fsr3] upscaling off: context not made (ffx %u)\n", (unsigned)rc); return; }
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = bw; rd.Height = bh; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    NULL, &IID_ID3D12Resource, (void**)&s_fsr3.upout))) {
        printf("[fsr3] upscaling off: output not made\n"); s_fsr3.Destroy(&s_fsr3.up_ctx, NULL); return;
    }
    s_fsr3.upout->lpVtbl->SetName(s_fsr3.upout, L"fsr3-upscaled");
    struct ffxQueryDescUpscaleGetJitterPhaseCount pc; memset(&pc, 0, sizeof pc);
    pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pc.renderWidth = rw; pc.displayWidth = bw; pc.pOutPhaseCount = &s_fsr3.phases;
    if (s_fsr3.Query(&s_fsr3.up_ctx, &pc.header) != FFX_API_RETURN_OK || s_fsr3.phases <= 0) s_fsr3.phases = 8;
    s_fsr3.jit_index = 0; fsr3_jitter_set();
    const char* ver = "?";
    { struct ffxQueryGetProviderVersion pv; memset(&pv, 0, sizeof pv);
      pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
      if (s_fsr3.Query(&s_fsr3.up_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) ver = pv.versionName; }
    s_fsr3.scale_on = 1;
    printf("[fsr3] upscaling on: %ux%u -> %ux%u, provider %s, %d jitter phases\n", rw, rh, bw, bh, ver, s_fsr3.phases);
    fflush(stdout);
}

/* Draw-time jitter for a scene draw (reads the camera, on a scene-sized target). prepass: it writes depth with no
 * colour (the next scene's depth pre-pass): the jitter advances there once after each captured scene, so the pre-pass
 * and its EQUAL colour pass share one offset. Returns the offset in render pixels. */
static void fsr3_jitter_for_draw(int prepass, float* ox, float* oy)
{
    if (prepass && s_fsr3.jit_advance) {
        s_fsr3.jit_advance = 0;
        s_fsr3.jit_index = (s_fsr3.jit_index + 1) % (s_fsr3.phases > 0 ? s_fsr3.phases : 1);
        fsr3_jitter_set();
    }
    *ox = s_fsr3.jx; *oy = s_fsr3.jy;
}

static uint32_t fsr3_ffx_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

/* In the frame's list l, before present: the presented scene colour (src, in RENDER_TARGET, sw x sh) upscaled into
 * upout (left in PIXEL_SHADER_RESOURCE; the caller puts it back). 0: not this frame (the old path). */
static int fsr3_upscale(ID3D12GraphicsCommandList* l, ID3D12Resource* src, unsigned sw, unsigned sh)
{
    if (!s_fsr3.scale_on || !s_fsr3.have || !s_fsr3.rawdepth || !s_fsr3.pso) { s_fsr3.win_up_skip++; s_fsr3.up_prev_ok = 0; return 0; }
    if (sw != s_fsr3.dw || sh != s_fsr3.dh || sw > s_fsr3.w || sh > s_fsr3.h) { s_fsr3.win_up_skip++; s_fsr3.up_prev_ok = 0; return 0; }
    D3D12_RESOURCE_DESC d; src->lpVtbl->GetDesc(src, &d);
    const uint32_t fmt = fsr3_ffx_format(d.Format);
    if (fmt == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        static int logs; if (logs++ < 2) printf("[fsr3] upscaling: colour format %d not handled\n", (int)d.Format);
        s_fsr3.win_up_skip++; return 0;
    }
    const int reset = fsr3_frame_camera() || !s_fsr3.up_prev_ok;
    fsr3_cam_motion(l);
    s_fsr3.mv_stamp = s_fsr3.frame_id + 1u;   /* frame generation's prepare (after frame_id++) uses it as it is */
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    l->lpVtbl->ResourceBarrier(l, 1, &b);
    const unsigned long long now = fsr3_now_us();
    struct ffxDispatchDescUpscale u; memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = l;
    u.color = fsr3_res(src, sw, sh, fmt); u.color.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    u.depth = fsr3_res(s_fsr3.rawdepth, s_fsr3.dw, s_fsr3.dh, FFX_API_SURFACE_FORMAT_R32_FLOAT);
    u.motionVectors = fsr3_res(s_fsr3.mv, s_fsr3.w, s_fsr3.h, FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
    u.output = fsr3_res(s_fsr3.upout, s_fsr3.w, s_fsr3.h, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM);
    u.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    u.jitterOffset.x = s_fsr3.cap_jx; u.jitterOffset.y = s_fsr3.cap_jy;
    u.motionVectorScale.x = 1.0f; u.motionVectorScale.y = 1.0f;   /* display pixels */
    u.renderSize.width = sw; u.renderSize.height = sh;
    u.upscaleSize.width = s_fsr3.w; u.upscaleSize.height = s_fsr3.h;
    { const char* e = getenv("FSR3_SCALE_SHARP"); const float sh0 = e ? (float)atof(e) : 0.0f;
      u.enableSharpening = sh0 > 0.0f; u.sharpness = sh0 > 1.0f ? 1.0f : sh0; }
    u.frameTimeDelta = s_fsr3.up_last_us ? (float)(now - s_fsr3.up_last_us) / 1000.0f : 16.7f;
    s_fsr3.up_last_us = now;
    u.preExposure = 1.0f;
    u.reset = reset ? true : false;
    u.cameraNear = s_fsr3.near_; u.cameraFar = s_fsr3.far_; u.cameraFovAngleVertical = s_fsr3.fovy;
    u.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t rc = s_fsr3.Dispatch(&s_fsr3.up_ctx, &u.header);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    l->lpVtbl->ResourceBarrier(l, 1, &b);
    if (rc != FFX_API_RETURN_OK) {
        if (s_fsr3.fails++ < 8) printf("[fsr3] upscale failed (ffx %u)\n", (unsigned)rc);
        s_fsr3.win_up_skip++; s_fsr3.up_prev_ok = 0; return 0;
    }
    b.Transition.pResource = s_fsr3.upout;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    l->lpVtbl->ResourceBarrier(l, 1, &b);
    s_fsr3.up_prev_ok = 1;
    s_fsr3.win_up++; s_fsr3.tot_up++;
    return 1;
}

/* In-game menu: anti-aliasing switched off while playing. The caller has waited for the GPU to be idle (nothing
 * recorded or queued still names the context or its output). Depth, motion, the fence and the camera pass stay for
 * the next switch-on (and for frame generation). */
static void fsr3_scale_shutdown(void)
{
    s_fsr3.scale_on = 0;
    if (s_fsr3.upout) {
        s_fsr3.Destroy(&s_fsr3.up_ctx, NULL);
        s_fsr3.up_ctx = NULL;
        s_fsr3.upout->lpVtbl->Release(s_fsr3.upout); s_fsr3.upout = NULL;
    }
    s_fsr3.scale_want = 0; s_fsr3.native = 0;
    s_fsr3.up_prev_ok = 0; s_fsr3.phases = 0; s_fsr3.jx = s_fsr3.jy = 0; s_fsr3.jit_advance = 0;
}
/* FSR3_PP_MS=1 (af-9, tests): the GPU time of the present section (FSR's upscale or SMAA/the present pass) from
 * timestamps; a ring of 8 frames, read 6 frames later; the mean printed every 10 s. */
static struct { int st; ID3D12QueryHeap* qh; ID3D12Resource* rb; double freq; unsigned long long n;
                double sum; unsigned cnt; unsigned long long win_us; int open; } s_ppms;
static void fsr3_ppms_begin(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12GraphicsCommandList* l)
{
    if (s_ppms.st == 0) {
        s_ppms.st = -1;
        if (!fsr3_env_on("FSR3_PP_MS")) return;
        D3D12_QUERY_HEAP_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 16;
        if (FAILED(dev->lpVtbl->CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void**)&s_ppms.qh))) return;
        D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd; memset(&rd, 0, sizeof rd);
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 16 * 8; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        NULL, &IID_ID3D12Resource, (void**)&s_ppms.rb))) return;
        UINT64 f = 0; q->lpVtbl->GetTimestampFrequency(q, &f); if (!f) return;
        s_ppms.freq = (double)f; s_ppms.st = 1;
        printf("[fsr3] FSR3_PP_MS: timing the present section on the GPU\n");
    }
    if (s_ppms.st != 1) return;
    const unsigned slot = (unsigned)(s_ppms.n & 7u);
    if (s_ppms.n >= 6) {   /* the frame 6 back is done on the GPU by now (at most 3 in flight) */
        const unsigned rs = (unsigned)((s_ppms.n - 6) & 7u);
        D3D12_RANGE r = { rs * 16, rs * 16 + 16 }; UINT64* p = NULL;
        if (SUCCEEDED(s_ppms.rb->lpVtbl->Map(s_ppms.rb, 0, &r, (void**)&p)) && p) {
            const UINT64 a = p[rs * 2], b = p[rs * 2 + 1];
            if (b > a && b - a < (UINT64)s_ppms.freq) { s_ppms.sum += (double)(b - a) * 1000.0 / s_ppms.freq; s_ppms.cnt++; }
            D3D12_RANGE w = { 0, 0 }; s_ppms.rb->lpVtbl->Unmap(s_ppms.rb, 0, &w);
        }
    }
    l->lpVtbl->EndQuery(l, s_ppms.qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    s_ppms.open = 1;
}
static void fsr3_ppms_end(ID3D12GraphicsCommandList* l, const char* what)
{
    if (s_ppms.st != 1 || !s_ppms.open) return;
    s_ppms.open = 0;
    const unsigned slot = (unsigned)(s_ppms.n & 7u);
    l->lpVtbl->EndQuery(l, s_ppms.qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    l->lpVtbl->ResolveQueryData(l, s_ppms.qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, s_ppms.rb, slot * 16);
    s_ppms.n++;
    const unsigned long long now = fsr3_now_us();
    if (!s_ppms.win_us) s_ppms.win_us = now;
    else if (now - s_ppms.win_us >= 10000000ull) {
        if (s_ppms.cnt) printf("[pp-ms] present section on the GPU: %.3f ms mean over %u frames (%s)\n", s_ppms.sum / s_ppms.cnt, s_ppms.cnt, what);
        s_ppms.sum = 0; s_ppms.cnt = 0; s_ppms.win_us = now;
    }
}
static void fsr3_upscale_done(ID3D12GraphicsCommandList* l)   /* upout back to UNORDERED_ACCESS */
{
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = s_fsr3.upout;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    l->lpVtbl->ResourceBarrier(l, 1, &b);
}
#endif
