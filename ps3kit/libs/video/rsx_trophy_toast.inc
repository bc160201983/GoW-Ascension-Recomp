/* ---- host/trophies (2026-11-22): the trophy popup ------------------------------------------------------------------
 * Included by rsx_ingame_menu.h (after the overlay's layer code). When the game unlocks a trophy (np_trophy_data.c's
 * event ring) a PS3-style card shows in the top-right corner for a few seconds: the trophy's icon (the player's own
 * TROP###.PNG, decoded with Windows' WIC), a small cup in the grade's colour, "Trophy unlocked" and the trophy's name.
 * Never takes input, never pauses. Its picture is made on its own worker thread (as the panel's); under frame
 * generation it goes into the overlay's layer (no ghosting, generation keeps running); without that layer it is drawn
 * into the frame (no generation pause for a popup). Several unlocks show one after another.
 *   LD_TROPHY_POPUP=0      no popup (the trophies are still kept)
 *   LD_TROPHY_POPUP_S=<s>  how long one shows (default 4.5, 1..30) */
#include "../network/np_trophy_data.h"
#include <wincodec.h>
#pragma comment(lib, "ole32.lib")

#define SRV_TOAST (SRV_MENU + 1)
typedef char ld_toast_slot_fits[(SRV_TOAST < SRV_CPU_HEAP_SLOTS) ? 1 : -1];

typedef struct { int id; char grade; u32 bw, bh; unsigned seq; } ld_toast_req;
static struct {
    int st;                                   /* 0 not looked at, 1 on, -1 off */
    ID3D12PipelineState *pso, *pso_plain;
    unsigned long long show_us;               /* how long one shows */
    np_trophy_event q[16]; int qn;            /* waiting after the one shown */
    int cur; char cur_grade;                  /* the one shown (-1: none) */
    unsigned long long t0;                    /* when it first showed (0: its picture is not there yet) */
    unsigned req_seq;
    ID3D12Resource *tex, *up; u32 w, h, tex_bw, tex_bh; unsigned tex_seq;
    /* the worker */
    HANDLE th, ev; CRITICAL_SECTION cs; int wst;
    ld_toast_req want; int want_set;
    ID3D12Resource* done; u32 dw, dh, dpitch, dbw, dbh; unsigned dseq; double ms;
    unsigned long long shown;                 /* log */
} s_toast = { 0, NULL, NULL, 0, {{0}}, 0, -1 };

/* 0 en, 1 es, 2 pt, 3 fr: the console language as CELL_SYSUTIL_LANG numbers (2 French, 3 Spanish, 7/17 Portuguese,
 * 20/21 the US disc's Latin-American Spanish); the trophy texts follow it too (np_trophy_data.c nt_lang_file) */
static int ld_toast_lang(void)
{
    const char* e = getenv("PS3_SYS_LANG");
    const int v = e && *e >= '0' && *e <= '9' ? atoi(e) : 1;
    return (v == 3 || v == 20 || v == 21) ? 1 : (v == 7 || v == 17) ? 2 : v == 2 ? 3 : 0;
}
static const char* ld_toast_text(int what)   /* 0 the heading, 1..4 grade P G S B */
{
    static const char* const k[4][5] = {
        { "Trophy unlocked", "Platinum", "Gold", "Silver", "Bronze" },
        { "Trofeo desbloqueado", "Platino", "Oro", "Plata", "Bronce" },
        { "Trof\xC3\xA9u desbloqueado", "Platina", "Ouro", "Prata", "Bronze" },
        { "Troph\xC3\xA9" "e d\xC3\xA9" "bloqu\xC3\xA9", "Platine", "Or", "Argent", "Bronze" } };
    return k[ld_toast_lang()][what];
}
static int ld_toast_grade_ix(char g) { return g == 'P' ? 1 : g == 'G' ? 2 : g == 'S' ? 3 : 4; }
static unsigned ld_toast_grade_rgb(char g)
{
    return g == 'P' ? 0xB9D3EEu : g == 'G' ? 0xE8C25Au : g == 'S' ? 0xC9CED6u : 0xC8834Eu;
}

/* the trophy's PNG -> BGRA straight alpha, side x side (WIC, Fant scaling). 1 = done. */
static int ld_toast_icon(const char* path_utf8, int side, u8* out)
{
    static const GUID clsid_factory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
    static const GUID iid_factory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
    static const GUID fmt_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };
    wchar_t wp[MAX_PATH * 2];
    if (!path_utf8[0] || !MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wp, MAX_PATH * 2)) return 0;
    IWICImagingFactory* f = NULL; IWICBitmapDecoder* d = NULL; IWICBitmapFrameDecode* fr = NULL;
    IWICBitmapScaler* sc = NULL; IWICFormatConverter* cv = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&clsid_factory, NULL, CLSCTX_INPROC_SERVER, &iid_factory, (void**)&f))) return 0;
    if (SUCCEEDED(f->lpVtbl->CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(d->lpVtbl->GetFrame(d, 0, &fr)) &&
        SUCCEEDED(f->lpVtbl->CreateBitmapScaler(f, &sc)) &&
        SUCCEEDED(sc->lpVtbl->Initialize(sc, (IWICBitmapSource*)fr, (UINT)side, (UINT)side, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->lpVtbl->CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(cv->lpVtbl->Initialize(cv, (IWICBitmapSource*)sc, &fmt_bgra, WICBitmapDitherTypeNone, NULL, 0.0,
                                         WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->lpVtbl->CopyPixels(cv, NULL, (UINT)side * 4u, (UINT)side * (UINT)side * 4u, out)))
        ok = 1;
    if (cv) cv->lpVtbl->Release(cv);
    if (sc) sc->lpVtbl->Release(sc);
    if (fr) fr->lpVtbl->Release(fr);
    if (d) d->lpVtbl->Release(d);
    f->lpVtbl->Release(f);
    return ok;
}

/* a small trophy cup centred at (cx, cy), about 2*r tall */
static void ld_toast_cup(ld_mc* m, float cx, float cy, float r, unsigned rgb)
{
    mc_rrect(m, cx - r * 0.62f, cy - r * 0.95f, cx + r * 0.62f, cy + r * 0.15f, r * 0.42f, rgb, 1.0f);    /* bowl */
    mc_ring(m, cx - r * 0.98f, cy - r * 0.78f, cx - r * 0.38f, cy - r * 0.12f, r * 0.30f, rgb, 1.0f, r * 0.16f);   /* handles */
    mc_ring(m, cx + r * 0.38f, cy - r * 0.78f, cx + r * 0.98f, cy - r * 0.12f, r * 0.30f, rgb, 1.0f, r * 0.16f);
    mc_rrect(m, cx - r * 0.12f, cy + r * 0.05f, cx + r * 0.12f, cy + r * 0.55f, r * 0.05f, rgb, 1.0f);    /* stem */
    mc_rrect(m, cx - r * 0.48f, cy + r * 0.52f, cx + r * 0.48f, cy + r * 0.80f, r * 0.08f, rgb, 1.0f);    /* base */
}

/* The card's picture for trophy id at output height bh (premultiplied RGBA in a new upload buffer). */
static int ld_toast_paint(const ld_toast_req* r, ID3D12Resource** up_out, u32* w_out, u32* h_out, u32* pitch_out)
{
    const double CH0 = 84.0, M0 = 14.0, CWMAX = 620.0;
    double k = (double)r->bh / 720.0;
    if ((CWMAX + 2.0 * M0) * k > (double)r->bw * 0.7) k = (double)r->bw * 0.7 / (CWMAX + 2.0 * M0);
    if (k < 0.5) k = 0.5;
#define U(x) ((float)((x) * k))
    const np_trophy_def* td = np_trophy_data_get(r->id);
    if (!td) return 0;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    /* own fonts (the panel's worker owns s_mf); the card is as wide as the name needs (420..620 at 720 lines) */
    HFONT f_head = mc_font(13.5 * k, FW_SEMIBOLD, L"Segoe UI"), f_name = mc_font(18.0 * k, FW_SEMIBOLD, L"Segoe UI");
    double CW0 = 420.0;
    { const float nw = mc_text(dc, f_name, 0, 0, 0, td->name[0] ? td->name : "?", 0, 0.0f, 0);
      const double need = (10.0 + 64.0 + 14.0 + 16.0) + (double)nw / k;
      if (need > CW0) CW0 = need > CWMAX ? CWMAX : need; }
    const int W = (int)ceil((CW0 + 2.0 * M0) * k), H = (int)ceil((CH0 + 2.0 * M0) * k);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    u8* alpha = (u8*)calloc((size_t)W * (size_t)H, 1);
    if (!bmp || !bits || !alpha) { if (bmp) DeleteObject(bmp); free(alpha); DeleteObject(f_head); DeleteObject(f_name); DeleteDC(dc); return 0; }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * (size_t)H * 4u);
    SetBkMode(dc, TRANSPARENT);
    ld_mc mm = { (u8*)bits, alpha, W, H }; ld_mc* m = &mm;
    const unsigned gc = ld_toast_grade_rgb(td->grade);
    const float X = U(M0), Y = U(M0), CW = U(CW0), CH = U(CH0), R = U(12);
    mc_shadow(m, X, Y, X + CW, Y + CH, R, U(M0), 0.55f);
    mc_rrect(m, X, Y, X + CW, Y + CH, R, 0x110D0Cu, 0.92f);
    mc_rrect_ex(m, X, Y, X + CW, Y + CH, R, 0x4A1014u, 0.45f, 0.0f, 0.0f, -1e9f, 1e9f);   /* a warm wash, left */
    mc_ring(m, X, Y, X + CW, Y + CH, R, gc, 0.70f, U(1.3));
    /* the icon */
    const float IS = U(64), IX = X + U(10), IY = Y + (CH - IS) * 0.5f;
    const int side = (int)(IS + 0.5f);
    mc_rrect(m, IX - U(1), IY - U(1), IX + IS + U(1), IY + IS + U(1), U(6), 0x000000u, 0.6f);
    u8* ic = side > 0 ? (u8*)malloc((size_t)side * (size_t)side * 4u) : NULL;
    char ipath[MAX_PATH * 2]; np_trophy_data_icon_path(r->id, ipath, sizeof ipath);
    if (ic && ld_toast_icon(ipath, side, ic)) {
        GdiFlush();
        const int ox = (int)(IX + 0.5f), oy = (int)(IY + 0.5f);
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) {
                const u8* p = ic + ((size_t)y * (size_t)side + (size_t)x) * 4u;
                mc_put(m, ox + x, oy + y, ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0], (float)p[3] / 255.0f);
            }
    } else ld_toast_cup(m, IX + IS * 0.5f, IY + IS * 0.5f, IS * 0.36f, gc);
    free(ic);
    /* the cup in the grade's colour, the heading, the name */
    const float TX = IX + IS + U(14);
    ld_toast_cup(m, TX + U(7), Y + U(27), U(7.5), gc);
    char head[96];
    snprintf(head, sizeof head, "%s  \xC2\xB7  %s", ld_toast_text(0), ld_toast_text(ld_toast_grade_ix(td->grade)));
    GdiFlush();
    mc_text(dc, f_head, TX + U(20), Y + U(27), 0xCFC6BCu, head, 0, X + CW - U(14) - (TX + U(20)), 1);
    mc_text(dc, f_name, TX, Y + U(56), 0xF2EAE0u, td->name[0] ? td->name : "?", 0, X + CW - U(14) - TX, 1);
    GdiFlush();
#undef U
    int ok = 0;
    const u32 pitch = ((u32)W * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (u64)pitch * (u64)H; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = NULL; u8* map = NULL;
    D3D12_RANGE none = {0, 0};
    if (g.dev && SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&up))) {
        if (SUCCEEDED(up->lpVtbl->Map(up, 0, &none, (void**)&map))) {
            for (int y = 0; y < H; y++) {
                u8* o = map + (size_t)y * pitch;
                const u8* p = (const u8*)bits + (size_t)y * (size_t)W * 4u;
                const u8* a = alpha + (size_t)y * (size_t)W;
                for (int x = 0; x < W; x++, o += 4, p += 4) {
                    const u32 al = a[x];
                    o[0] = (u8)((p[2] * al + 127u) / 255u); o[1] = (u8)((p[1] * al + 127u) / 255u);
                    o[2] = (u8)((p[0] * al + 127u) / 255u); o[3] = (u8)al;
                }
            }
            up->lpVtbl->Unmap(up, 0, NULL);
            ok = 1;
        } else { up->lpVtbl->Release(up); up = NULL; }
    }
    SelectObject(dc, old_bmp);
    DeleteObject(f_head); DeleteObject(f_name);
    DeleteObject(bmp); DeleteDC(dc); free(alpha);
    *up_out = up; *w_out = (u32)W; *h_out = (u32)H; *pitch_out = pitch;
    return ok;
}

static DWORD WINAPI ld_toast_worker(LPVOID arg)
{
    (void)arg;
    const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(s_toast.ev, INFINITE);
        ld_toast_req r; int have = 0;
        EnterCriticalSection(&s_toast.cs);
        if (s_toast.want_set) { r = s_toast.want; s_toast.want_set = 0; have = 1; }
        LeaveCriticalSection(&s_toast.cs);
        if (!have) continue;
        const unsigned long long t0 = fsr3_now_us();
        ID3D12Resource* up = NULL; u32 w = 0, h = 0, pitch = 0;
        const int ok = ld_toast_paint(&r, &up, &w, &h, &pitch);
        EnterCriticalSection(&s_toast.cs);
        if (s_toast.done) s_toast.done->lpVtbl->Release(s_toast.done);
        s_toast.done = ok ? up : NULL;
        if (!ok && up) up->lpVtbl->Release(up);
        s_toast.dw = w; s_toast.dh = h; s_toast.dpitch = pitch; s_toast.dbw = r.bw; s_toast.dbh = r.bh; s_toast.dseq = r.seq;
        s_toast.ms = (double)(fsr3_now_us() - t0) / 1000.0;
        LeaveCriticalSection(&s_toast.cs);
        if (!ok) fprintf(stderr, "[trophy] popup picture for %d failed\n", r.id);
    }
    (void)co;
    return 0;
}

static int ld_toast_init(void)
{
    if (s_toast.st) return s_toast.st > 0;
    s_toast.st = -1;
    const char* e = getenv("LD_TROPHY_POPUP");
    if (e && e[0] == '0') { fprintf(stderr, "[trophy] popup off (LD_TROPHY_POPUP=0)\n"); return 0; }
    if (np_trophy_data_count() <= 0) { s_toast.st = 0; return 0; }   /* the set is not loaded (yet): look again later */
    static const char k_toast_ps[] =
        "Texture2D src : register(t0);\n"
        "SamplerState point_clamp : register(s0);\n"
        "cbuffer PP : register(b0) { float4 pp_uv; float4 pp_opt; };\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 ps(V i) : SV_Target { return src.SampleLevel(point_clamp, i.uv, 0.0) * saturate(pp_opt.z); }\n";
    ID3DBlob* ps = ld_pp_init() && s_pp.copy_vs ? ld_pp_compile(k_toast_ps, sizeof k_toast_ps - 1u, "trophy_toast", "ps", "ps_5_0") : NULL;
    s_toast.pso = ps ? ld_pp_pipeline_ex(s_pp.copy_vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, 1) : NULL;
    s_toast.pso_plain = ps ? ld_pp_pipeline_ex(s_pp.copy_vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, 0) : NULL;
    if (ps) ps->lpVtbl->Release(ps);
    if (!s_toast.pso || !s_toast.pso_plain) { fprintf(stderr, "[trophy] no pipeline: no popup\n"); return 0; }
    InitializeCriticalSection(&s_toast.cs);
    s_toast.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_toast.th = s_toast.ev ? CreateThread(NULL, 0, ld_toast_worker, NULL, 0, NULL) : NULL;
    if (!s_toast.th) { fprintf(stderr, "[trophy] no worker thread: no popup\n"); return 0; }
    SetThreadPriority(s_toast.th, THREAD_PRIORITY_BELOW_NORMAL);
    { const char* d = getenv("LD_TROPHY_POPUP_S"); double sec = d && d[0] ? atof(d) : 4.5;
      if (sec < 1.0) sec = 1.0;
      if (sec > 30.0) sec = 30.0;
      s_toast.show_us = (unsigned long long)(sec * 1e6); }
    s_toast.cur = -1;
    s_toast.st = 1;
    fprintf(stderr, "[trophy] popup ready (%.1f s each, language %s)\n", (double)s_toast.show_us / 1e6,
            ld_toast_lang() == 1 ? "es" : ld_toast_lang() == 2 ? "pt" : ld_toast_lang() == 3 ? "fr" : "en");
    return 1;
}

static void ld_toast_request(u32 bw, u32 bh)
{
    ld_toast_req r; r.id = s_toast.cur; r.grade = s_toast.cur_grade; r.bw = bw; r.bh = bh; r.seq = ++s_toast.req_seq;
    EnterCriticalSection(&s_toast.cs);
    s_toast.want = r; s_toast.want_set = 1;
    LeaveCriticalSection(&s_toast.cs);
    SetEvent(s_toast.ev);
}

/* Once per presented frame: new events, timing. 1 = the card is to be drawn this frame. */
static unsigned long long ld_toast_total_us(void) { return 250000ull + s_toast.show_us + 300000ull; }
static int ld_toast_tick(void)
{
    if (s_toast.st <= 0) {
        if (s_toast.st < 0) return 0;
        np_trophy_event peek;
        if (s_toast.qn == 0 && !np_trophy_event_take(&peek)) return 0;   /* nothing yet: stay cheap */
        if (s_toast.qn == 0) { s_toast.q[0] = peek; s_toast.qn = 1; }
        if (!ld_toast_init()) { if (s_toast.st < 0) s_toast.qn = 0; return 0; }
    }
    np_trophy_event e;
    while (np_trophy_event_take(&e)) {
        if (s_toast.qn < (int)(sizeof s_toast.q / sizeof s_toast.q[0])) s_toast.q[s_toast.qn++] = e;
    }
    const unsigned long long now = fsr3_now_us();
    if (s_toast.cur >= 0 && s_toast.t0 && now - s_toast.t0 >= ld_toast_total_us()) s_toast.cur = -1;
    if (s_toast.cur < 0 && s_toast.qn > 0) {
        s_toast.cur = s_toast.q[0].id; s_toast.cur_grade = s_toast.q[0].grade;
        memmove(s_toast.q, s_toast.q + 1, (size_t)(--s_toast.qn) * sizeof s_toast.q[0]);
        s_toast.t0 = 0;
        if (g.bb_w >= 320u && g.bb_h >= 180u) ld_toast_request(g.bb_w, g.bb_h);
    }
    return s_toast.cur >= 0;
}

/* The card over the frame's rect (0, 0, dw, dh) of swap-chain buffer bbi (RENDER_TARGET). */
static void ld_toast_draw(u32 bbi, u32 dw, u32 dh)
{
    if (s_toast.st <= 0 || s_toast.cur < 0 || dw < 320u || dh < 180u) return;
    /* a finished picture: into a fresh texture (the old one is retired, never rewritten) */
    { ID3D12Resource* up; u32 w, h, pitch, bw, bh; unsigned seq; double ms;
      EnterCriticalSection(&s_toast.cs);
      up = s_toast.done; s_toast.done = NULL; w = s_toast.dw; h = s_toast.dh; pitch = s_toast.dpitch;
      bw = s_toast.dbw; bh = s_toast.dbh; seq = s_toast.dseq; ms = s_toast.ms;
      LeaveCriticalSection(&s_toast.cs);
      if (up) {
          D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
          D3D12_RESOURCE_DESC rd = {0};
          rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
          rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
          ID3D12Resource* tex = NULL;
          if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                  D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&tex))) up->lpVtbl->Release(up);
          else {
              if (s_toast.up) retire_texture(s_toast.up);
              if (s_toast.tex) retire_texture(s_toast.tex);
              s_toast.up = up; s_toast.tex = tex;
              D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
              src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
              src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
              src.PlacedFootprint.Footprint.Width = w; src.PlacedFootprint.Footprint.Height = h;
              src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = pitch;
              dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
              LD_CL(CopyTextureRegion, &dst, 0, 0, 0, &src, NULL);
              ld_pp_barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
              srv_write(SRV_TOAST, tex);
              s_toast.w = w; s_toast.h = h; s_toast.tex_bw = bw; s_toast.tex_bh = bh; s_toast.tex_seq = seq;
              if (s_toast.shown++ < 8)
                  fprintf(stderr, "[trophy] popup for trophy %d (%c): picture %ux%u for %ux%u made in %.1f ms on the worker\n",
                          s_toast.cur, s_toast.cur_grade, w, h, bw, bh, ms);
          }
      } }
    if (!s_toast.tex || s_toast.tex_seq != s_toast.req_seq) return;      /* this trophy's picture is not there yet */
    if (s_toast.tex_bw != g.bb_w || s_toast.tex_bh != g.bb_h) {           /* the window changed size: a new picture */
        if (s_toast.tex_bw != 0) { s_toast.tex_bw = 0; ld_toast_request(g.bb_w, g.bb_h); }
        return;
    }
    if (s_toast.w > dw || s_toast.h > dh) return;
    const unsigned long long now = fsr3_now_us();
    if (!s_toast.t0) s_toast.t0 = now;   /* its time starts with the first drawn frame */
    const unsigned long long t = now - s_toast.t0;
    float e;
    if (t < 250000ull) { const float x = (float)t / 250000.0f; e = 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
    else if (t < 250000ull + s_toast.show_us) e = 1.0f;
    else { const float x = mc_c01((float)(t - 250000ull - s_toast.show_us) / 300000.0f); e = (1.0f - x) * (1.0f - x); }
    if (e <= 0.004f) return;
    const float k = (float)g.bb_h / 720.0f;
    const u32 margin = (u32)(10.0f * k + 0.5f);                            /* the card's own picture has its shadow room */
    const u32 slide = (u32)((1.0f - e) * 36.0f * k + 0.5f);
    /* the FPS counter (ld_perf_overlay: 136 px wide at x = width - 144, y 8) sits in the same corner: the card goes left of it */
    u32 room = margin;
    { const char* pe = s_ld_perf_on < 0 ? getenv("LD_PERF_OVERLAY") : NULL;
      if (s_ld_perf_draw && (s_ld_perf_on > 0 || (s_ld_perf_on < 0 && pe && pe[0] && pe[0] != '0'))) room = 144u + margin / 2u; }
    const u32 x = dw > s_toast.w + room ? dw - s_toast.w - room + slide : 0;
    const u32 y = margin;
    if (x + s_toast.w > dw) return;
    const float c[8] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, e, 0.0f };
    D3D12_GPU_VIRTUAL_ADDRESS va;
    if (!ld_pp_room(c, 2, 1, 0, &va)) return;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = ld_pp_table(SRV_TOAST, SRV_WHITE, SRV_WHITE);
    if (!table.ptr) return;
    /* the layer is copied into, not blended: while the panel is up there, the card waits (its time runs on) */
    if (ld_fgui_begin()) {
        if (s_menu.open || ld_menu_fading()) return;
        ld_pp_draw(s_toast.pso_plain, table, va, rtv_handle(LD_RTV_PP + LD_PP_UI), x, y, s_toast.w, s_toast.h);
    } else
        ld_pp_draw(s_toast.pso, table, va, rtv_handle(bbi), x, y, s_toast.w, s_toast.h);
}
