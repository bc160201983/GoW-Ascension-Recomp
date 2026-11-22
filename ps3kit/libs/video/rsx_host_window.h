/* The game window on Windows, shared by the RSX backends that own one
 * (rsx_null_backend.c, which live draw presents into, and rsx_d3d12_backend.c).
 *
 *   PS3_PARENT_HWND=<hwnd>   render inside another program's window (a
 *                            launcher), as a child filling its client area
 *   PS3_WINDOW_SIZE=<w>x<h>  the window's client size (the picture is
 *                            stretched to it); default the render size
 *   PS3_FULLSCREEN=1         borderless full screen; F11 or Alt+Enter toggles
 *   PS3_WINDOW_ICON=<.ico>   the window's icon
 *   PS3_ESC_QUITS=0          Escape does not close the game
 *   PS3_DPI_AWARE=0          let Windows scale the window (blurry above 100%)
 *
 * Embedded, full screen applies to the host's top-level window, the host
 * window's size is followed, and its closing ends the game. */
#ifndef RSX_HOST_WINDOW_H
#define RSX_HOST_WINDOW_H
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HWND s_hw_parent;
static int  s_hw_fullscreen;
static RECT s_hw_restore;
static LONG s_hw_restore_style;

/* PS3_DISPLAY_MATCH (default on; =0 off): full screen with vsync shows a frame
 * only on the monitor's refresh, so a frame rate that does not divide it can
 * never be even. 60 FPS on the user's 75 Hz screen: three frames shown for
 * one refresh (13.3 ms), the fourth for two (26.7 ms) -- a judder 15 times a
 * second while the counter reads a steady 60; 144 and 165 Hz screens do the
 * same. Going full screen switches that monitor to the same resolution at a
 * refresh the frame cap divides (PS3_FPS_LIMIT: 60 -> 240/180/120/60, the
 * highest the monitor offers), until the game leaves full screen or exits.
 * Not with PS3_VSYNC=0 (a variable-refresh screen shows each frame when it is
 * ready) or no frame cap. */
static WCHAR s_hw_mode_device[32];   /* the monitor switched, "" when none */
static void rsx_host_window_mode_restore(void)
{
    if (!s_hw_mode_device[0]) return;
    ChangeDisplaySettingsExW(s_hw_mode_device, NULL, NULL, 0, NULL);
    printf("[display] %ls back to its own refresh\n", s_hw_mode_device);
    s_hw_mode_device[0] = 0;
}
/* Logs the monitor's refresh against the frame cap; switch: full screen is
 * starting, so switch to a matching refresh (PS3_DISPLAY_MATCH). */
static void rsx_host_window_mode_match(HMONITOR mon, int do_switch)
{
    MONITORINFOEXW mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return;
    DEVMODEW cur; memset(&cur, 0, sizeof cur); cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return;
    const char* e = getenv("PS3_FPS_LIMIT");
    const unsigned cap = e ? (unsigned)atoi(e) : 0;
    const char* v = getenv("PS3_VSYNC");
    const int vsync = !(v && v[0] == '0');
    const unsigned hz = cur.dmDisplayFrequency;
    if (!cap || hz <= 1 || hz % cap == 0 || !vsync) {
        printf("[display] %ls %lux%lu at %u Hz, frame cap %u: %s\n", mi.szDevice, cur.dmPelsWidth, cur.dmPelsHeight,
               hz, cap, !cap ? "no cap" : !vsync ? "no vsync (variable refresh shows frames as they come)"
                        : hz % cap == 0 ? "frames shown evenly" : "unknown refresh");
        return;
    }
    DEVMODEW best; memset(&best, 0, sizeof best);
    for (DWORD i = 0;; i++) {
        DEVMODEW m; memset(&m, 0, sizeof m); m.dmSize = sizeof m;
        if (!EnumDisplaySettingsW(mi.szDevice, i, &m)) break;
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight ||
            m.dmBitsPerPel != cur.dmBitsPerPel || m.dmDisplayFrequency % cap || m.dmDisplayFrequency > 240) continue;
        if (m.dmDisplayFrequency > best.dmDisplayFrequency) best = m;
    }
    e = getenv("PS3_DISPLAY_MATCH");
    const int match = !(e && e[0] == '0');
    if (!do_switch || !match || !best.dmDisplayFrequency) {
        char tail[96];
        if (!best.dmDisplayFrequency) snprintf(tail, sizeof tail, "no %lux%lu mode at a multiple of %u Hz",
                                               cur.dmPelsWidth, cur.dmPelsHeight, cap);
        else if (!match) snprintf(tail, sizeof tail, "PS3_DISPLAY_MATCH=0");
        else snprintf(tail, sizeof tail, "in a window; full screen switches to %lu Hz", best.dmDisplayFrequency);
        printf("[display] %ls %lux%lu at %u Hz, frame cap %u: frames cannot be shown evenly (%s)\n", mi.szDevice,
               cur.dmPelsWidth, cur.dmPelsHeight, hz, cap, tail);
        return;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    const LONG r = ChangeDisplaySettingsExW(mi.szDevice, &best, NULL, CDS_FULLSCREEN, NULL);
    if (r == DISP_CHANGE_SUCCESSFUL) {
        wcsncpy(s_hw_mode_device, mi.szDevice, 31); s_hw_mode_device[31] = 0;
        static int hooked;
        if (!hooked && ++hooked) atexit(rsx_host_window_mode_restore);
    }
    printf("[display] %ls %lux%lu at %u Hz, frame cap %u: %s %lu Hz while in full screen%s\n", mi.szDevice,
           cur.dmPelsWidth, cur.dmPelsHeight, hz, cap, r == DISP_CHANGE_SUCCESSFUL ? "switched to" : "could not switch to",
           best.dmDisplayFrequency, r == DISP_CHANGE_SUCCESSFUL ? " (frames shown evenly)" : "");
}

static void rsx_host_window_set_fullscreen(HWND hwnd, int on)
{
    if (s_hw_parent) hwnd = GetAncestor(s_hw_parent, GA_ROOT);
    if (!hwnd || on == s_hw_fullscreen) return;
    if (on) {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowRect(hwnd, &s_hw_restore);
        s_hw_restore_style = GetWindowLongA(hwnd, GWL_STYLE);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        rsx_host_window_mode_match(mon, 1);   /* PS3_DISPLAY_MATCH, before the window takes the screen */
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        rsx_host_window_mode_restore();
        SetWindowLongA(hwnd, GWL_STYLE, s_hw_restore_style ? s_hw_restore_style
                                                           : (LONG)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        SetWindowPos(hwnd, NULL, s_hw_restore.left, s_hw_restore.top,
                     s_hw_restore.right - s_hw_restore.left, s_hw_restore.bottom - s_hw_restore.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    s_hw_fullscreen = on;
}

/* The process works in the monitor's real pixels (per-monitor DPI aware).
 * Without it, with Windows' display scaling at 125%, the full-screen window
 * got 2048 x 1152 of a 2560 x 1440 screen: the frame was stretched down to
 * that and Windows enlarged it back by 1.25, blurring it twice -- a 1440p
 * frame looked softer than 720p. PS3_DPI_AWARE=0: as before. Called before
 * the process makes its first window; not when embedded in a launcher's
 * window, whose process decides. */
static void rsx_host_window_dpi_aware(void)
{
    const char* e = getenv("PS3_DPI_AWARE");
    if (e && e[0] == '0') return;
    typedef BOOL (WINAPI *set_context_fn)(HANDLE);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    set_context_fn set_context = user32
        ? (set_context_fn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703+) */
    const int v2 = set_context && set_context((HANDLE)(intptr_t)-4);
    if (!v2) SetProcessDPIAware();
    HDC dc = GetDC(NULL);
    printf("[window] DPI aware (%s): screen %dx%d\n", v2 ? "per monitor" : "system",
           dc ? GetDeviceCaps(dc, DESKTOPHORZRES) : 0, dc ? GetDeviceCaps(dc, DESKTOPVERTRES) : 0);
    if (dc) ReleaseDC(NULL, dc);
}

static HWND rsx_host_window_create(const char* cls, unsigned width, unsigned height, const char* title)
{
    const char* e = getenv("PS3_PARENT_HWND");
    HWND parent = e && *e ? (HWND)(uintptr_t)strtoull(e, NULL, 0) : NULL;
    HWND hwnd = NULL;
    if (parent && IsWindow(parent)) {
        RECT pr; GetClientRect(parent, &pr);
        /* WS_EX_NOPARENTNOTIFY: creating or destroying the child must not send
         * WM_PARENTNOTIFY across processes to the host's thread, which may be
         * waiting for this game to end (the launcher closing it). */
        hwnd = CreateWindowExA(WS_EX_NOPARENTNOTIFY, cls, title, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                               pr.right > 0 ? pr.right : (int)width, pr.bottom > 0 ? pr.bottom : (int)height,
                               parent, NULL, GetModuleHandle(NULL), NULL);
        if (hwnd) {
            s_hw_parent = parent;
            printf("[window] rendering inside window 0x%llX\n", (unsigned long long)(uintptr_t)parent);
            SetFocus(hwnd);
            return hwnd;
        }
        printf("[window] could not render inside 0x%llX (%lu): own window\n",
               (unsigned long long)(uintptr_t)parent, GetLastError());
    }
    rsx_host_window_dpi_aware();
    unsigned cw = width, ch = height;
    e = getenv("PS3_WINDOW_SIZE");
    { unsigned ew = 0, eh = 0;
      if (e && sscanf(e, "%ux%u", &ew, &eh) == 2 && ew >= 320 && eh >= 180 && ew <= 16384 && eh <= 16384) {
          cw = ew; ch = eh; } }
    RECT wr = {0, 0, (LONG)cw, (LONG)ch};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, cls, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                           NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!hwnd) return NULL;
    e = getenv("PS3_WINDOW_ICON");
    if (e && *e) {
        HICON ico_b = (HICON)LoadImageA(NULL, e, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        HICON ico_s = (HICON)LoadImageA(NULL, e, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
        if (ico_b) SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)ico_b);
        if (ico_s) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ico_s);
    }
    e = getenv("PS3_FULLSCREEN");
    if (e && e[0] == '1') rsx_host_window_set_fullscreen(hwnd, 1);
    else rsx_host_window_mode_match(MonitorFromWindow(s_hw_parent ? s_hw_parent : hwnd, MONITOR_DEFAULTTONEAREST), 0);
    return hwnd;
}

/* The window messages handled here: F11 / Alt+Enter, and a click that gives
 * an embedded window the keyboard. Returns 1 when the message is consumed. */
static int rsx_host_window_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { rsx_host_window_set_fullscreen(hwnd, !s_hw_fullscreen); return 1; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { rsx_host_window_set_fullscreen(hwnd, !s_hw_fullscreen); return 1; }
        break;
    case WM_LBUTTONDOWN:
        if (s_hw_parent) SetFocus(hwnd);
        break;
    case WM_CLOSE: case WM_DESTROY:   /* the monitor's own refresh back before the game winds down */
        rsx_host_window_mode_restore();
        break;
    }
    return 0;
}

/* The in-game settings menu (rsx_ingame_menu.h) owns Esc while its panel is open: Esc closes the panel, never the
 * game (the flag stays up for a few frames after closing, for an Esc still in the message queue). */
__declspec(selectany) volatile long g_rsx_menu_owns_esc;
static int rsx_host_window_esc_quits(void)
{
    if (g_rsx_menu_owns_esc) return 0;
    const char* e = getenv("PS3_ESC_QUITS");
    return !(e && e[0] == '0');
}

/* Embedded: follow the host window's size. Returns 1 once the host is gone. */
static int rsx_host_window_follow(HWND hwnd)
{
    if (!s_hw_parent || !hwnd) return 0;
    if (!IsWindow(s_hw_parent)) return 1;
    RECT pr, cr;
    GetClientRect(s_hw_parent, &pr);
    GetClientRect(hwnd, &cr);
    if (pr.right > 0 && pr.bottom > 0 && (pr.right != cr.right || pr.bottom != cr.bottom))
        MoveWindow(hwnd, 0, 0, pr.right, pr.bottom, TRUE);
    return 0;
}

#endif /* _WIN32 */
#endif /* RSX_HOST_WINDOW_H */
