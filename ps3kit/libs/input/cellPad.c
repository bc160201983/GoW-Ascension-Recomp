/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows (the
 * PS3RECOMP_PAD_SDL2 CMake option does it): needed for PlayStation pads,
 * which XInput cannot see.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  /* This file has no main(); keep SDL from renaming one. */
  #define SDL_MAIN_HANDLED
  #if defined(__has_include)
    #if __has_include(<SDL2/SDL.h>)
      #include <SDL2/SDL.h>
    #else
      #include <SDL.h>   /* the Windows VC package puts the headers in include/ */
    #endif
  #else
    #include <SDL2/SDL.h>
  #endif
  #ifdef _WIN32
    /* The keyboard fallback below uses the Win32 API. */
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
  #endif
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Both axes of one stick together, the way RPCS3 does it (Emu/Io/PadHandler.cpp:
 * NormalizeStickDeadzone + ConvertToSquirclePoint): a ROUND deadzone, then the
 * round range of a modern stick stretched toward the near-square range of a
 * DualShock 3, which PS3 games are tuned for. Without it a diagonal flick
 * reached only ~62% on each axis (per-axis deadzone, round gate), and the
 * right-stick evade did not always fire (user, 2026-11-11). Factor as in
 * RPCS3's "Squircling Factor" (default 4000 there; a full diagonal then
 * reaches ~88% per axis). PS3_PAD_SQUIRCLE=<factor>, 0 = the old per-axis
 * conversion. Y: XInput up is positive, PS3 up is 0. */
static void pad_xinput_stick_pair(short raw_x, short raw_y, short deadzone, u8* out_x, u8* out_y)
{
    static int factor = -1;
    if (factor < 0) { const char* e = getenv("PS3_PAD_SQUIRCLE"); factor = e ? atoi(e) : 4000; if (factor < 0) factor = 0; }
    if (!factor) {
        *out_x = pad_xinput_stick_to_u8(raw_x, deadzone);
        *out_y = (u8)(256 - pad_xinput_stick_to_u8(raw_y, deadzone));
        return;
    }
    const float x = raw_x / 32767.0f, y = raw_y / 32767.0f;
    const float dz = deadzone / 32767.0f;
    const float r0 = sqrtf(x * x + y * y);
    if (r0 <= dz) { *out_x = 128; *out_y = 128; return; }   /* exactly 128: 127 aliases SELECT+START */
    float r = (r0 - dz) / (1.0f - dz);
    if (r > 1.0f) r = 1.0f;
    /* direction from the raw axes (no trig: an untouched axis stays exactly 128) */
    const float ux = x / r0, uy = y / r0, s2 = 2.0f * ux * uy;   /* s2 = sin(2 * angle) */
    r *= 1.0f + s2 * s2 / ((float)factor / 1000.0f);
    const float nx = r * ux, ny = -r * uy;
    int vx = (int)lroundf((nx + 1.0f) * 127.5f), vy = (int)lroundf((ny + 1.0f) * 127.5f);
    *out_x = (u8)(vx < 0 ? 0 : vx > 255 ? 255 : vx);
    *out_y = (u8)(vy < 0 ? 0 : vy > 255 ? 255 : vy);
}
#endif /* PAD_BACKEND_XINPUT (continued after the keyboard fallback) */

/* Keyboard fallback for port 0, shared by the XInput and SDL2 backends.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return 1;
    /* Rendering inside a launcher's window (PS3_PARENT_HWND): that window in
     * front counts as ours. */
    { static HWND host = (HWND)-1;
      if (host == (HWND)-1) {
          const char* e = getenv("PS3_PARENT_HWND");
          HWND p = e && *e ? (HWND)(uintptr_t)strtoull(e, NULL, 0) : NULL;
          host = p ? GetAncestor(p, GA_ROOT) : NULL;
      }
      return host && fg == host; }
}

/* The controls of the launcher's Controls page (launcher/app/config.py
 * CONTROL_GROUPS): a PS3 button, or one direction of a stick. */
static const struct { const char* id; u16 btn; int stick; int dir; } s_pad_ctl[] = {
    { "ls_up", 0, 1, 0 }, { "ls_down", 0, 1, 1 }, { "ls_left", 0, 1, 2 }, { "ls_right", 0, 1, 3 },
    { "rs_up", 0, 2, 0 }, { "rs_down", 0, 2, 1 }, { "rs_left", 0, 2, 2 }, { "rs_right", 0, 2, 3 },
    { "cross", CELL_PAD_CTRL_CROSS, 0, 0 },   { "square", CELL_PAD_CTRL_SQUARE, 0, 0 },
    { "triangle", CELL_PAD_CTRL_TRIANGLE, 0, 0 }, { "circle", CELL_PAD_CTRL_CIRCLE, 0, 0 },
    { "l1", CELL_PAD_CTRL_L1, 0, 0 }, { "r1", CELL_PAD_CTRL_R1, 0, 0 },
    { "l2", CELL_PAD_CTRL_L2, 0, 0 }, { "r2", CELL_PAD_CTRL_R2, 0, 0 },
    { "l3", CELL_PAD_CTRL_L3, 0, 0 }, { "r3", CELL_PAD_CTRL_R3, 0, 0 },
    { "up", CELL_PAD_CTRL_UP, 0, 0 }, { "down", CELL_PAD_CTRL_DOWN, 0, 0 },
    { "left", CELL_PAD_CTRL_LEFT, 0, 0 }, { "right", CELL_PAD_CTRL_RIGHT, 0, 0 },
    { "start", CELL_PAD_CTRL_START, 0, 0 }, { "select", CELL_PAD_CTRL_SELECT, 0, 0 },
};
#define PAD_NCTL ((int)(sizeof s_pad_ctl / sizeof s_pad_ctl[0]))

static int pad_ctl_index(const char* id, size_t n)
{
    for (int i = 0; i < PAD_NCTL; i++)
        if (strlen(s_pad_ctl[i].id) == n && strncmp(s_pad_ctl[i].id, id, n) == 0) return i;
    return -1;
}

/* A key name as the launcher writes it (config.py TK_KEYS / KEY_PRESETS). */
static int pad_vk_from_name(const char* s, size_t n)
{
    static const struct { const char* name; int vk; } names[] = {
        { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Tab", VK_TAB }, { "Backspace", VK_BACK },
        { "Escape", VK_ESCAPE }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT },
        { "Right", VK_RIGHT }, { "LShift", VK_LSHIFT }, { "RShift", VK_RSHIFT }, { "LCtrl", VK_LCONTROL },
        { "RCtrl", VK_RCONTROL }, { "LAlt", VK_LMENU }, { "RAlt", VK_RMENU }, { "Comma", VK_OEM_COMMA },
        { "Period", VK_OEM_PERIOD }, { "Slash", VK_OEM_2 }, { "Semicolon", VK_OEM_1 }, { "Quote", VK_OEM_7 },
        { "LBracket", VK_OEM_4 }, { "RBracket", VK_OEM_6 }, { "Minus", VK_OEM_MINUS }, { "Equals", VK_OEM_PLUS },
        { "Grave", VK_OEM_3 }, { "Backslash", VK_OEM_5 }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE },
        { "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
        { "CapsLock", VK_CAPITAL }, { "Mouse1", VK_LBUTTON }, { "Mouse2", VK_RBUTTON },
        { "Mouse3", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 }, { "Mouse5", VK_XBUTTON2 },
    };
    if (n == 1 && s[0] >= 'A' && s[0] <= 'Z') return s[0];
    if (n == 1 && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    if (n == 1 && s[0] >= '0' && s[0] <= '9') return s[0];
    if (n == 4 && strncmp(s, "Num", 3) == 0 && s[3] >= '0' && s[3] <= '9') return VK_NUMPAD0 + (s[3] - '0');
    if ((n == 2 || n == 3) && s[0] == 'F') {
        const int f = atoi(s + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].name) == n && strncmp(names[i].name, s, n) == 0) return names[i].vk;
    return 0;
}

/* PS3_KEYMAP (the launcher's Controls page, passed once this string is in the
 * executable): "cross=Space,K;square=J,Mouse1;ls_up=W;...", up to 4 keys a
 * control. Unset: the old fixed map below. */
static int s_keymap[PAD_NCTL][4];
static int s_keymap_on = -1;
static void pad_keymap_load(void)
{
    s_keymap_on = 0;
    const char* e = getenv("PS3_KEYMAP");
    if (!e || !*e) return;
    memset(s_keymap, 0, sizeof s_keymap);
    int bound = 0;
    for (const char* p = e; *p; ) {
        const char* end = strchr(p, ';'); if (!end) end = p + strlen(p);
        const char* eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            const int c = pad_ctl_index(p, (size_t)(eq - p));
            int k = 0;
            for (const char* q = eq + 1; q < end && c >= 0; ) {
                const char* comma = memchr(q, ',', (size_t)(end - q)); if (!comma) comma = end;
                const int vk = pad_vk_from_name(q, (size_t)(comma - q));
                if (vk && k < 4) { s_keymap[c][k++] = vk; bound++; }
                q = comma < end ? comma + 1 : end;
            }
        }
        p = *end ? end + 1 : end;
    }
    s_keymap_on = bound > 0;
    printf("[cellPad] PS3_KEYMAP: %d key(s) bound\n", bound);
}

/* The keyboard as one pad, into *hs. Returns 0 (and a centred, released pad)
 * when no window of this process is in front, so a key held while alt-tabbing
 * never stays down. */
static int pad_read_keyboard(PadHostState* hs)
{
    memset(hs, 0, sizeof *hs);
    hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off || !pad_host_window_focused()) return 0;
    if (s_keymap_on < 0) pad_keymap_load();

    u16 btns = 0;
    int dir[3][4] = { { 0 } };
    if (s_keymap_on) {
        for (int c = 0; c < PAD_NCTL; c++)
            for (int k = 0; k < 4 && s_keymap[c][k]; k++)
                if (GetAsyncKeyState(s_keymap[c][k]) & 0x8000) {
                    if (s_pad_ctl[c].stick) dir[s_pad_ctl[c].stick][s_pad_ctl[c].dir] = 1;
                    else btns |= s_pad_ctl[c].btn;
                }
    } else {
        /* The old fixed map: arrows = d-pad (the left stick follows it),
         * Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1, 1/2 = L2/R2,
         * Enter = START, Tab = SELECT. */
        static const struct { int vk; u16 btn; } map[] = {
            { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
            { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
            { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
            { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
            { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
            { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
            { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
        };
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;
        dir[1][0] = !!(btns & CELL_PAD_CTRL_UP);   dir[1][1] = !!(btns & CELL_PAD_CTRL_DOWN);
        dir[1][2] = !!(btns & CELL_PAD_CTRL_LEFT); dir[1][3] = !!(btns & CELL_PAD_CTRL_RIGHT);
    }
    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)(dir[1][2] && !dir[1][3] ? 0 : dir[1][3] && !dir[1][2] ? 255 : 128);
    hs->analog_ly = (u8)(dir[1][0] && !dir[1][1] ? 0 : dir[1][1] && !dir[1][0] ? 255 : 128);
    hs->analog_rx = (u8)(dir[2][2] && !dir[2][3] ? 0 : dir[2][3] && !dir[2][2] ? 255 : 128);
    hs->analog_ry = (u8)(dir[2][0] && !dir[2][1] ? 0 : dir[2][1] && !dir[2][0] ? 255 : 128);
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    hs->press_up = (btns & CELL_PAD_CTRL_UP) ? 255 : 0;         hs->press_down = (btns & CELL_PAD_CTRL_DOWN) ? 255 : 0;
    hs->press_left = (btns & CELL_PAD_CTRL_LEFT) ? 255 : 0;     hs->press_right = (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
    hs->press_cross = (btns & CELL_PAD_CTRL_CROSS) ? 255 : 0;   hs->press_circle = (btns & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
    hs->press_square = (btns & CELL_PAD_CTRL_SQUARE) ? 255 : 0; hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_l1 = (btns & CELL_PAD_CTRL_L1) ? 255 : 0;         hs->press_r1 = (btns & CELL_PAD_CTRL_R1) ? 255 : 0;
    { static int said = 0;
      if (!said && (btns || hs->analog_lx != 128 || hs->analog_ly != 128 || hs->analog_rx != 128 || hs->analog_ry != 128)) {
          said = 1;
          printf("[cellPad] keyboard input on port 0 (%s map)\n", s_keymap_on ? "PS3_KEYMAP" : "fixed");
          fflush(stdout); } }
    return 1;
}

/* PS3_PAD_MERGE (default 1; 0 = the old way: one host controller per port, the
 * keyboard only when port 0 has none): every connected controller AND the
 * keyboard drive port 0, the one pad a single-player game reads. Before, a
 * second pad (an Xbox pad beside a paired DualSense) went to port 1, which the
 * game never reads, and any controller on port 0 switched the keyboard off.
 * Buttons combine; each stick and trigger comes from whichever source moves it
 * most. Ports 1-3 report nothing connected. */
static void pad_merge_port0(const PadHostState* kb, int kb_on)
{
    PadHostState m;
    memset(&m, 0, sizeof m);
    m.analog_lx = m.analog_ly = m.analog_rx = m.analog_ry = 128;
    int bl = 0, br = 0;
    for (int i = 0; i <= PAD_MAX_HOST_PORTS; i++) {
        const PadHostState* s = i < PAD_MAX_HOST_PORTS ? &s_host_state[i] : kb;
        if (i < PAD_MAX_HOST_PORTS ? !s->connected : !kb_on) continue;
        m.buttons |= s->buttons;
        const int dl = abs((int)s->analog_lx - 128) + abs((int)s->analog_ly - 128);
        const int dr = abs((int)s->analog_rx - 128) + abs((int)s->analog_ry - 128);
        if (dl > bl) { bl = dl; m.analog_lx = s->analog_lx; m.analog_ly = s->analog_ly; }
        if (dr > br) { br = dr; m.analog_rx = s->analog_rx; m.analog_ry = s->analog_ry; }
#define PAD_MAXU8(f) if (s->f > m.f) m.f = s->f
        PAD_MAXU8(trigger_l2); PAD_MAXU8(trigger_r2);
        PAD_MAXU8(press_right); PAD_MAXU8(press_left); PAD_MAXU8(press_up); PAD_MAXU8(press_down);
        PAD_MAXU8(press_triangle); PAD_MAXU8(press_circle); PAD_MAXU8(press_cross); PAD_MAXU8(press_square);
        PAD_MAXU8(press_l1); PAD_MAXU8(press_r1);
#undef PAD_MAXU8
    }
    m.connected = 1;   /* the keyboard is always there, as before */
    s_host_state[0] = m;
    for (int i = 1; i < PAD_MAX_HOST_PORTS; i++) s_host_state[i].connected = 0;
}
#endif

#if PAD_BACKEND_XINPUT
static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        pad_xinput_stick_pair(gp->sThumbLX, gp->sThumbLY, PAD_STICK_DEADZONE,
                              &s_host_state[i].analog_lx, &s_host_state[i].analog_ly);
        pad_xinput_stick_pair(gp->sThumbRX, gp->sThumbRY, PAD_STICK_DEADZONE,
                              &s_host_state[i].analog_rx, &s_host_state[i].analog_ry);

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

#ifdef _WIN32
/* PS3_PADMAP (the launcher's Controls page): "cross=a;circle=b;l2=lt;...", the
 * controller button each PS3 button is ("none" = no button). Controls it does
 * not name keep the standard layout below, so unset = the map as before.
 * PS3_PAD_DEADZONE=<percent>: a round dead zone on both sticks (unset: none,
 * as before). */
enum { PADSRC_NONE = -1, PADSRC_LT = 1000, PADSRC_RT = 1001 };
static int s_padmap[PAD_NCTL];
static float s_pad_deadzone;
static int s_padmap_loaded;
static void pad_padmap_load(void)
{
    static const struct { const char* name; int src; } srcs[] = {
        { "a", SDL_CONTROLLER_BUTTON_A }, { "b", SDL_CONTROLLER_BUTTON_B }, { "x", SDL_CONTROLLER_BUTTON_X },
        { "y", SDL_CONTROLLER_BUTTON_Y }, { "lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
        { "rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, { "lt", PADSRC_LT }, { "rt", PADSRC_RT },
        { "ls", SDL_CONTROLLER_BUTTON_LEFTSTICK }, { "rs", SDL_CONTROLLER_BUTTON_RIGHTSTICK },
        { "back", SDL_CONTROLLER_BUTTON_BACK }, { "start", SDL_CONTROLLER_BUTTON_START },
        { "guide", SDL_CONTROLLER_BUTTON_GUIDE }, { "up", SDL_CONTROLLER_BUTTON_DPAD_UP },
        { "down", SDL_CONTROLLER_BUTTON_DPAD_DOWN }, { "left", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
        { "right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT }, { "none", PADSRC_NONE },
    };
    /* The standard layout (Xbox names; the PlayStation pad's buttons sit in
     * the same places). */
    static const struct { const char* ctl; const char* src; } defaults[] = {
        { "cross", "a" }, { "circle", "b" }, { "square", "x" }, { "triangle", "y" }, { "l1", "lb" },
        { "r1", "rb" }, { "l2", "lt" }, { "r2", "rt" }, { "l3", "ls" }, { "r3", "rs" }, { "up", "up" },
        { "down", "down" }, { "left", "left" }, { "right", "right" }, { "start", "start" }, { "select", "back" },
    };
    s_padmap_loaded = 1;
    for (int c = 0; c < PAD_NCTL; c++) s_padmap[c] = PADSRC_NONE;
    const char* e = getenv("PS3_PADMAP");
    for (int pass = 0; pass < 2; pass++) {
        const char* list = pass ? e : NULL;
        if (pass == 0) {
            for (unsigned d = 0; d < sizeof defaults / sizeof defaults[0]; d++) {
                const int c = pad_ctl_index(defaults[d].ctl, strlen(defaults[d].ctl));
                for (unsigned k = 0; k < sizeof srcs / sizeof srcs[0]; k++)
                    if (c >= 0 && strcmp(srcs[k].name, defaults[d].src) == 0) s_padmap[c] = srcs[k].src;
            }
            continue;
        }
        if (!list || !*list) break;
        for (const char* p = list; *p; ) {
            const char* end = strchr(p, ';'); if (!end) end = p + strlen(p);
            const char* eq = memchr(p, '=', (size_t)(end - p));
            if (eq) {
                const int c = pad_ctl_index(p, (size_t)(eq - p));
                const size_t n = (size_t)(end - eq - 1);
                for (unsigned k = 0; k < sizeof srcs / sizeof srcs[0] && c >= 0 && !s_pad_ctl[c].stick; k++)
                    if (strlen(srcs[k].name) == n && strncmp(srcs[k].name, eq + 1, n) == 0) s_padmap[c] = srcs[k].src;
            }
            p = *end ? end + 1 : end;
        }
    }
    const char* dz = getenv("PS3_PAD_DEADZONE");
    if (dz && *dz) {
        float v = (float)atof(dz) / 100.0f;
        s_pad_deadzone = v < 0.0f ? 0.0f : v > 0.9f ? 0.9f : v;
    }
    printf("[cellPad] controller map: %s, stick dead zone %.0f%%\n", e && *e ? "PS3_PADMAP" : "standard",
           s_pad_deadzone * 100.0f);
}

static void pad_deadzone_apply(int* x, int* y)
{
    if (s_pad_deadzone <= 0.0f) return;
    const float fx = (float)*x / 32767.0f, fy = (float)*y / 32767.0f;
    const float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= s_pad_deadzone) { *x = *y = 0; return; }
    const float out = ((mag > 1.0f ? 1.0f : mag) - s_pad_deadzone) / (1.0f - s_pad_deadzone);
    const float k = out / mag * 32767.0f;
    int nx = (int)(fx * k), ny = (int)(fy * k);
    *x = nx < -32768 ? -32768 : nx > 32767 ? 32767 : nx;
    *y = ny < -32768 ? -32768 : ny > 32767 ? 32767 : ny;
}

/* 2026-11-12: the RPCS3-style squircle of pad_xinput_stick_pair for the SDL2 backend. Release builds read pads
 * through SDL2 (PS3RECOMP_PAD_SDL2=ON), so 0.1.27's squircle (XInput only) never reached players: a full diagonal
 * stayed at ~71% per axis (user + tester, Steam Deck with a generic pad: right-stick evade less responsive, a
 * stick QTE not taken). Input: SDL axes after the launcher's round dead zone (y down = positive = PS3 down).
 * Centre -> exactly 128. PS3_PAD_SQUIRCLE=<factor> (default 4000), 0 = the old per-axis conversion. */
static void pad_sdl_stick_pair(int x, int y, u8* out_x, u8* out_y)
{
    static int factor = -1;
    if (factor < 0) { const char* e = getenv("PS3_PAD_SQUIRCLE"); factor = e ? atoi(e) : 4000; if (factor < 0) factor = 0; }
    if (!factor) { *out_x = pad_sdl_axis_to_u8(x); *out_y = pad_sdl_axis_to_u8(y); return; }
    const float fx = x / 32767.0f, fy = y / 32767.0f;
    const float r0 = sqrtf(fx * fx + fy * fy);
    if (r0 <= 0.0f) { *out_x = 128; *out_y = 128; return; }
    float r = r0 > 1.0f ? 1.0f : r0;
    const float ux = fx / r0, uy = fy / r0, s2 = 2.0f * ux * uy;   /* s2 = sin(2 * angle) */
    r *= 1.0f + s2 * s2 / ((float)factor / 1000.0f);
    int vx = (int)lroundf((r * ux + 1.0f) * 127.5f), vy = (int)lroundf((r * uy + 1.0f) * 127.5f);
    *out_x = (u8)(vx < 0 ? 0 : vx > 255 ? 255 : vx);
    *out_y = (u8)(vy < 0 ? 0 : vy > 255 ? 255 : vy);
}
#endif

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
                if (s_sdl_controllers[i])
                    printf("[cellPad] controller %d connected: %s\n", i, SDL_GameControllerName(s_sdl_controllers[i]));
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                printf("[cellPad] controller %d disconnected\n", i);
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
#ifdef _WIN32
        if (!s_padmap_loaded) pad_padmap_load();
        {
            const int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            const int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            int src_l2 = PADSRC_NONE, src_r2 = PADSRC_NONE;
            for (int c = 0; c < PAD_NCTL; c++) {
                const int src = s_padmap[c];
                if (s_pad_ctl[c].stick || src == PADSRC_NONE) continue;
                const int on = src == PADSRC_LT ? lt > 3000 : src == PADSRC_RT ? rt > 3000
                             : SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)src);
                if (on) btns |= s_pad_ctl[c].btn;
                if (s_pad_ctl[c].btn == CELL_PAD_CTRL_L2) src_l2 = src;
                if (s_pad_ctl[c].btn == CELL_PAD_CTRL_R2) src_r2 = src;
            }
            s_host_state[i].buttons = btns;
            int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
            int rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
            int ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
            pad_deadzone_apply(&lx, &ly);
            pad_deadzone_apply(&rx, &ry);
            pad_sdl_stick_pair(lx, ly, &s_host_state[i].analog_lx, &s_host_state[i].analog_ly);
            pad_sdl_stick_pair(rx, ry, &s_host_state[i].analog_rx, &s_host_state[i].analog_ry);
            /* An analog trigger keeps its travel; L2/R2 on a button is all or nothing. */
            s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(src_l2 == PADSRC_LT ? lt : src_l2 == PADSRC_RT ? rt
                                                               : (btns & CELL_PAD_CTRL_L2) ? 32767 : 0);
            s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(src_r2 == PADSRC_LT ? lt : src_r2 == PADSRC_RT ? rt
                                                               : (btns & CELL_PAD_CTRL_R2) ? 32767 : 0);
            s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
            s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
            s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
            s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
            s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
            s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
            s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
            s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
            s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
            s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
            continue;
        }
#endif
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        /* The game window is not an SDL window, so SDL would otherwise treat
         * the app as unfocused and could drop controller input. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef _WIN32
        /* 2026-11-11 (tester: pad dead after some cutscenes until a restart): without a message loop on the thread
         * that initialised SDL, Windows' device-change notices were never read, so a pad that dropped for a moment
         * (wireless, rumble) was never found again. SDL's own joystick thread reads them. PS3_PAD_JOYTHREAD=0: off. */
        { const char* e = getenv("PS3_PAD_JOYTHREAD");
          if (!(e && e[0] == '0')) SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1"); }
#endif
#ifdef _WIN32
        /* Xbox pads: SDL's RawInput driver (and WGI) hands input only to the
         * thread that initialised SDL -- it reads its helper window's raw input
         * queue -- but the game reads the pad from its own threads, so an Xbox
         * controller was found and opened and never moved. SDL's XInput driver
         * polls from any thread. PlayStation pads use HIDAPI either way.
         * PS3_PAD_RAWINPUT=1 keeps SDL's default. */
        { const char* e = getenv("PS3_PAD_RAWINPUT");
          if (!(e && e[0] == '1')) {
              SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
              SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
          } }
#endif
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
                printf("[cellPad] SDL_InitSubSystem failed: %s\n", SDL_GetError());
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            if (s_sdl_controllers[i])
                printf("[cellPad] SDL controller %d: %s (%s)\n", i,
                       SDL_GameControllerName(s_sdl_controllers[i]),
                       SDL_GameControllerPath(s_sdl_controllers[i]) ? SDL_GameControllerPath(s_sdl_controllers[i]) : "?");
        }
    }
    printf("[cellPad] SDL2 backend: %d joystick(s) found\n", num);
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* PS3_SKIP_MOVIES=1: while a prerendered movie is decoding (cellVdec), press
 * the title's own skip button on the first pad -- short pulses, so a title
 * that wants a fresh press sees one. God of War: Ascension skips its movies
 * on CROSS. The pulses stop as soon as the movie stops decoding, so nothing
 * leaks into the game after it. PS3_SKIP_MOVIES_BUTTON=<CELL_PAD_CTRL mask>
 * picks another button. */
/* God of War: Ascension (BCES01741 1.00), its movie skip through the pause
 * menu. Recognised by the code it reads (IsMoviePlaying at 0x3C87A8, the pause
 * setter at 0x15958), so no other title or version is touched. Game state used:
 *   movie playing = 0x9967CC && 0x82B68C && 0x82B690 (what 0x3C87A8 returns),
 *   pause menu    = byte 0x83D1B5 (pause reason 1, set by 0x15958(1, on) from
 *                   the pause-menu commands at 0x222FD4/0x223004; reason 0 is
 *                   the movie itself, and 0x83D1B8 = any reason, is 1 all
 *                   through a movie -- run r3, 2026-11-07),
 *   single player = 0x8A11D4 == 2 ("set singleplayer"; shell 1, multiplayer
 *                   other: their pause menus have no Skip item, never driven).
 * Steps: movie playing 0.8 s unpaused -> SELECT; once paused, DOWN (one item:
 * Continue -> Skip Cinematic); CROSS; CROSS again on "Are you sure?" (Yes).
 * Every press is 50 ms and every step waits for the state it needs; the moment
 * the movie is no longer playing, any press is dropped and nothing more is
 * sent. Before DOWN the highlighted item must be Continue, before each CROSS it
 * must be Skip Cinematic (read from the game, below); if it is not, nothing more
 * is pressed for that movie (it plays on; Quit Game is never risked). */
static u32 pad_gow_r32(u32 a) { return vm_read32(a); }

/* The pause menu's highlighted item, read from the game (runs r7/r8, 2026-11-07):
 * the menu is a behaviour script run by bhvrSMIClient objects (vtable word
 * 0x00661928 at +4). The pause-menu script is the one whose header reads
 * 00010012 20121217 0000FB80 00000EF8 (+0x38 of the client points at it);
 * 14 clients run it, and the cursor's client is the one whose current state
 * (+0x54) is not 0: 0x1F8 = Continue, 0x2F8 = Skip Cinematic, 0x360 = Quit Game
 * (snapshots with the cursor on each item, both directions; the dialog keeps
 * 0x2F8). Found by a scan of the heap once per menu opening, then read
 * directly. 0 = not found (or not exactly one). */
#define GOW_ST_CONTINUE 0x1F8u
#define GOW_ST_SKIP     0x2F8u
static int pad_gow_is_menu_client(u32 c)
{
    if (c < 0x10000000u || c >= 0x20000000u || pad_gow_r32(c + 4) != 0x00661928u) return 0;
    const u32 scr = pad_gow_r32(c + 0x38);
    if (scr < 0x10000000u || scr >= 0x20000000u) return 0;
    return pad_gow_r32(scr) == 0x00010012u && pad_gow_r32(scr + 4) == 0x20121217u &&
           pad_gow_r32(scr + 8) == 0x0000FB80u && pad_gow_r32(scr + 12) == 0x00000EF8u;
}
static u32 pad_gow_find_cursor(void)
{
#ifdef _WIN32
    u32 found = 0; int n = 0;
    for (u32 ga = 0x10000000u; ga < 0x20000000u; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ga, &mi, sizeof mi)) break;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        u32 gend = end - (uintptr_t)vm_base > 0x20000000u ? 0x20000000u : (u32)(end - (uintptr_t)vm_base);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && gend > ga) {
            const u32* w = (const u32*)(vm_base + ga);
            for (u32 k = 1; k < (gend - ga) / 4; k++)
                if (w[k] == 0x28196600u) {               /* 0x00661928 big-endian */
                    const u32 c = ga + 4 * (k - 1);
                    if (pad_gow_is_menu_client(c) && pad_gow_r32(c + 0x54)) { found = c; n++; }
                }
        }
        if (gend <= ga) break;
        ga = gend;
    }
    return n == 1 ? found : 0;
#else
    return 0;
#endif
}

static int pad_gow_menu_skip(unsigned long long now)
{
    static int gow = -1;
    if (gow < 0) {
        gow = pad_gow_r32(0x222FD0) == 0x38800001u && pad_gow_r32(0x3C87A8) == 0x3C800099u && pad_gow_r32(0x3C87B0) == 0x808467CCu &&
              pad_gow_r32(0x15958) == 0x3CC00084u && pad_gow_r32(0x15970) == 0x9883008Cu;
        if (gow) printf("[cellPad] movie skip: God of War: Ascension -- single-player movies through the pause menu\n");
    }
    if (!gow) return 0;
    static int st, tries; static unsigned long long t, since, cool, p_until; static u16 p_mask;
    static u32 cur;   /* the pause menu's cursor client, 0 = not found */
    const int sp = pad_gow_r32(0x8A11D4) == 2;
    const int mp = pad_gow_r32(0x9967CC) && pad_gow_r32(0x82B68C) && pad_gow_r32(0x82B690);
    const int paused = vm_read8(0x83D1B5) != 0;   /* the pause menu is up */
    const u32 item = (cur && pad_gow_is_menu_client(cur)) ? pad_gow_r32(cur + 0x54) : 0;
    #define GOW_PRESS(m, what) do { p_mask = (m); p_until = now + 50; \
        printf("[cellPad] movie skip: %s (menu state 0x%X)\n", what, item); fflush(stdout); } while (0)
    #define GOW_GIVE_UP(why) do { st = 9; p_until = 0; \
        printf("[cellPad] movie skip: stopped, nothing more pressed: %s (menu state 0x%X)\n", why, item); fflush(stdout); } while (0)
    if (!mp) {
        if (st) { printf("[cellPad] movie skip: the movie stopped (step %d)\n", st); fflush(stdout); }
        st = 0; tries = 0; since = 0; p_until = 0;
        return 0;
    }
    if (!sp && !st) return 0;
    if (now < p_until) {
        s_host_state[0].connected = 1;
        s_host_state[0].buttons |= p_mask;
        if (p_mask & CELL_PAD_CTRL_CROSS) s_host_state[0].press_cross = 255;
        return 1;
    }
    switch (st) {
    case 0:
        if (paused || tries >= 4 || now < cool) { since = 0; break; }
        if (!since) since = now;
        if (now - since >= 800) { GOW_PRESS(CELL_PAD_CTRL_SELECT, "SELECT (pause menu)"); st = 1; t = now; }
        break;
    case 1:   /* wait for the pause menu */
        if (paused) { st = 2; t = now; tries++; cur = 0; }
        else if (now - t > 1500) { st = 0; cool = now + 1500; since = 0; }
        break;
    case 2:   /* menu up: find its cursor; it must be on Continue. The menu fades in for about
               * a second and takes no input meanwhile (a DOWN 350 ms after the flag was lost,
               * run r4): DOWN at 1.2 s. */
        if (!paused) { st = 0; cool = now + 1000; since = 0; break; }
        if (!cur && now - t >= 600) {
            cur = pad_gow_find_cursor();
            printf("[cellPad] movie skip: pause-menu cursor client 0x%08X\n", cur); fflush(stdout);
            if (!cur) { GOW_GIVE_UP("pause-menu cursor not found"); break; }
        }
        if (cur && now - t >= 1200) {
            if (item != GOW_ST_CONTINUE) { GOW_GIVE_UP("the cursor is not on Continue"); break; }
            GOW_PRESS(CELL_PAD_CTRL_DOWN, "DOWN (Skip Cinematic)"); st = 3; t = now;
        }
        break;
    case 3:   /* the cursor must now be on Skip Cinematic, and only then CROSS */
        if (!paused) { GOW_GIVE_UP("the pause menu closed"); break; }
        if (now - t >= 500) {
            if (item != GOW_ST_SKIP) { GOW_GIVE_UP("the highlighted item is not Skip Cinematic"); break; }
            GOW_PRESS(CELL_PAD_CTRL_CROSS, "CROSS (Skip Cinematic)"); st = 4; t = now;
        }
        break;
    case 4:   /* "Are you sure you want to Skip?" (the cursor state stays on Skip) */
        if (!paused) { GOW_GIVE_UP("the pause menu closed"); break; }
        if (now - t >= 700) {
            if (item != GOW_ST_SKIP) { GOW_GIVE_UP("the highlighted item is not Skip Cinematic"); break; }
            GOW_PRESS(CELL_PAD_CTRL_CROSS, "CROSS (Yes)"); st = 5; t = now;
        }
        break;
    case 5:   /* the movie should stop now; if it does not, nothing more is pressed */
        if (now - t > 2000) GOW_GIVE_UP("the movie went on after Yes");
        break;
    default:  /* 9: gave up for this movie */
        break;
    }
    #undef GOW_PRESS
    #undef GOW_GIVE_UP
    if (now < p_until) {
        s_host_state[0].connected = 1;
        s_host_state[0].buttons |= p_mask;
        if (p_mask & CELL_PAD_CTRL_CROSS) s_host_state[0].press_cross = 255;
    }
    return 1;
}

/* PS3_MENU_TRACE=1 (debug): log the pause-menu cursor state whenever it changes while the
 * pause menu is up, to map states to items. */
static void pad_gow_menu_trace(void)
{
    static int on = -1; static u32 cur, last = 0xFFFFFFFFu; static int was;
    if (on < 0) { const char* e = getenv("PS3_MENU_TRACE"); on = e && e[0] == '1'; }
    if (!on) return;
    if (pad_gow_r32(0x15970) != 0x9883008Cu) return;
    const int paused = vm_read8(0x83D1B5) != 0;
    if (paused && !was) { cur = 0; last = 0xFFFFFFFFu; }
    was = paused;
    if (!paused) return;
    if (!cur || !pad_gow_is_menu_client(cur)) cur = pad_gow_find_cursor();
    const u32 v = cur ? pad_gow_r32(cur + 0x54) : 0;
    if (v != last) { printf("[cellPad] menu trace: client 0x%08X state 0x%X\n", cur, v); fflush(stdout); last = v; }
}

static void pad_movie_skip(void)
{
    static int on = -1; static u16 button = CELL_PAD_CTRL_CROSS;
    if (on < 0) {
        const char* e = getenv("PS3_SKIP_MOVIES");
        on = e && e[0] == '1';
        const char* b = getenv("PS3_SKIP_MOVIES_BUTTON");
        if (b && *b) button = (u16)strtoul(b, NULL, 0);
        if (on) printf("[cellPad] movies are skipped (button 0x%04X)\n", button);
    }
    if (!on) return;
    extern volatile unsigned long long g_vdec_seq_start_ms, g_vdec_last_au_ms;
    const unsigned long long start = g_vdec_seq_start_ms, last = g_vdec_last_au_ms;
    const unsigned long long now = pad_now_ms();
    /* God of War: Ascension in single player: CROSS does not skip a movie
     * there; the game's own way is its pause menu (SELECT): "Continue / Skip
     * Cinematic / Quit Game", then "Are you sure you want to Skip?" (CROSS =
     * Yes). That is driven here, from the game's own state, never on timing. */
    pad_gow_menu_trace();
    if (pad_gow_menu_skip(now)) return;
    /* No press once the decoder has paused for 80 ms, and each press only
     * 60 ms (still several of the title's pad reads): with 120 ms presses
     * going on 300 ms past the last decoded unit, the press that ended a
     * movie could still be down when the game came back -- CROSS is jump,
     * and a tester's Kratos jumped as the opening's loading movie ended,
     * onto a floor not drawn yet (2026-11-03). */
    if (!start || now - start < 1000 || !last || now - last > 80) return;
    /* 60 ms down, 340 ms up. */
    if ((now - start) % 400 >= 60) return;
    s_host_state[0].connected = 1;
    s_host_state[0].buttons |= button;
    if (button & CELL_PAD_CTRL_CROSS) s_host_state[0].press_cross = 255;
    { static unsigned long long said; if (now - said > 2000) { said = now;
        printf("[cellPad] skipping a movie\n"); fflush(stdout); } }
}

/* PS3_PEEK=<hexEA>[:w|h|b],...: debug -- print each listed guest word when its
 * value changes (checked every 50 ms from the pad poll), to find the game
 * state that marks a cinematic or a movie. Off unless set. */
static void pad_peek(void)
{
    static int n = -1; static u32 ea[16]; static char wd[16]; static u32 last[16];
    static unsigned long long t_last;
    if (n < 0) {
        n = 0;
        const char* e = getenv("PS3_PEEK");
        while (e && *e && n < 16) {
            ea[n] = (u32)strtoul(e, (char**)&e, 16); wd[n] = 'w';
            if (*e == ':') { e++; wd[n] = *e ? *e++ : 'w'; }
            last[n] = 0xDEADBEEFu; n++;
            while (*e == ',' || *e == ' ') e++;
        }
    }
    /* PS3_SNAP_DIR=<dir>: debug -- when <dir>\req exists, write the guest data
     * committed guest memory below 0x40000000 to <dir>\snap<k>.bin and delete req. */
    static int snap = -1; static char snapdir[400]; static int snap_k;
    if (snap < 0) { const char* e = getenv("PS3_SNAP_DIR"); snap = e && *e;
                    if (snap) { strncpy(snapdir, e, sizeof snapdir - 1); } }
    if (!n && !snap) return;
    const unsigned long long now = (unsigned long long)GetTickCount64();
    if (now - t_last < 50) return;
    t_last = now;
    if (snap) {
        char rq[440]; snprintf(rq, sizeof rq, "%s\\req", snapdir);
        FILE* r = fopen(rq, "rb");
        if (r) { fclose(r); remove(rq);
            char fn[440]; snprintf(fn, sizeof fn, "%s\\snap%d.bin", snapdir, snap_k++);
            FILE* w = fopen(fn, "wb");
            /* every committed, readable guest range below 0x40000000, each as
             * [u32 guest EA][u32 size] + the bytes as they are (big-endian). */
            if (w) {
#ifdef _WIN32
                for (u32 ga = 0x10000; ga < 0x40000000u; ) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (!VirtualQuery(vm_base + ga, &mi, sizeof mi)) break;
                    uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
                    u32 gend = end - (uintptr_t)vm_base > 0x40000000u ? 0x40000000u : (u32)(end - (uintptr_t)vm_base);
                    if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && gend > ga) {
                        u32 hdr[2] = { ga, gend - ga };
                        fwrite(hdr, 4, 2, w); fwrite(vm_base + ga, 1, gend - ga, w);
                    }
                    ga = gend;
                }
#endif
                fclose(w); }
            printf("[snap] t=%llu wrote %s\n", now, fn); fflush(stdout); }
    }
    for (int i = 0; i < n; i++) {
        u32 v = wd[i] == 'b' ? vm_read8(ea[i]) : wd[i] == 'h' ? vm_read16(ea[i]) : vm_read32(ea[i]);
        if (v != last[i]) {
            printf("[peek] t=%llu 0x%08X = 0x%X (was 0x%X)\n", now, ea[i], v, last[i]);
            fflush(stdout);
            last[i] = v;
        }
    }
}

/* host\shadows (2026-11-22): "Disable dynamic shadows". God of War: Ascension reads the .data word 0x81B5B4 every frame
 * (value 1 on the disc, never written by the game): 0 = no cascade shadow maps are drawn and the shadow-mask pass is left
 * out, so every receiver reads "lit". PS3_NO_SHADOWS=1 starts with it 0; the in-game menu switches it live through
 * cellPad_set_no_shadows(). Applied from the pad poll (one aligned 4-byte word). While the player wants them off the
 * word is kept 0; switching back on writes the value read before the first change back ONCE and then leaves the word
 * to the game again (so a write of the game's own is never undone while shadows are on). A different program (the word
 * is not 0 or 1 there) is left alone. */
#define PAD_SHADOW_EA 0x0081B5B4u
static volatile long s_noshadow_want = -1;    /* -1: not read yet; 0 shadows on; 1 shadows off */
static int s_noshadow_have = 0, s_noshadow_bad = 0, s_noshadow_done = 0; static u32 s_noshadow_disc = 1u;
int cellPad_no_shadows_ok(void) { return !s_noshadow_bad; }   /* 0: the word is not this game's switch (menu greys the row) */
int cellPad_no_shadows(void)
{
    if (s_noshadow_want < 0) { const char* e = getenv("PS3_NO_SHADOWS"); InterlockedCompareExchange(&s_noshadow_want, (e && e[0] == '1') ? 1 : 0, -1); }
    return s_noshadow_want > 0;
}
void cellPad_set_no_shadows(int on) { (void)cellPad_no_shadows(); InterlockedExchange(&s_noshadow_want, on ? 1 : 0); }
static void pad_shadow_apply(void)
{
    const int want = cellPad_no_shadows();
    if (s_noshadow_bad || (!want && (!s_noshadow_have || s_noshadow_done))) return;   /* on: only the one write back */
    const u32 cur = vm_read32(PAD_SHADOW_EA);
    if (!s_noshadow_have) {
        if (cur > 1u) { s_noshadow_bad = 1; printf("[shadows] word 0x%08X = 0x%X (not 0/1): not this game, switch off\n", PAD_SHADOW_EA, cur); fflush(stdout); return; }
        s_noshadow_disc = cur ? cur : 1u; s_noshadow_have = 1;
    }
    const u32 to = want ? 0u : s_noshadow_disc;
    static int s_off_kept = 0, s_game_wrote = 0;   /* s_off_kept: our 0 is in place; a later non-zero = the game's own write */
    if (cur != to) {
        vm_write32(PAD_SHADOW_EA, to);
        if (want && s_off_kept) {
            if (++s_game_wrote <= 4) { printf("[shadows] the game set 0x%08X to %u while shadows are off: set back to 0 (%d)\n", PAD_SHADOW_EA, cur, s_game_wrote); fflush(stdout); }
        } else { printf("[shadows] dynamic shadows %s (0x%08X = %u, was %u)\n", want ? "OFF" : "ON", PAD_SHADOW_EA, to, cur); fflush(stdout); }
    }
    s_off_kept = want; s_noshadow_done = !want;
}

static void pad_poll_backend(void)
{
    pad_peek();    pad_shadow_apply();
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;
    { static int nohost = -1;   /* mpnet kit: a test window must not follow the player's controller */
      if (nohost < 0) { const char* e = getenv("PS3_PAD_NO_HOST"); nohost = (e && e[0] == '1') ? 1 : 0; }
      if (nohost) { memset(&s_host_state[0], 0, sizeof(s_host_state[0])); s_host_state[0].connected = 1;
                    pad_movie_skip(); return; } }

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#ifdef _WIN32
    { static int merge = -1;
      if (merge < 0) { const char* e = getenv("PS3_PAD_MERGE"); merge = !(e && e[0] == '0'); }
      PadHostState kb;
      const int kb_on = pad_read_keyboard(&kb);
      if (merge) pad_merge_port0(&kb, kb_on);
      else if (!s_host_state[0].connected) s_host_state[0] = kb;   /* connected=1 even unfocused, as before */
      if (!merge && !kb_on && !s_host_state[0].connected) s_host_state[0].connected = 1; }
#endif
#ifdef _WIN32
    { extern volatile long g_pad_menu_block;   /* in-game menu open: the movie skip's presses would move its cursor */
      if (g_pad_menu_block) return; }
#endif
    pad_movie_skip();
}

#ifdef _WIN32
#include "cellPad_menu.h"   /* in-game settings menu: neutral pad while its panel is open, SELECT hold */
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000,
     * SELECT 0x0001. PAD_SCRIPT_HOLD=<polls> (default 40): how long each press
     * is held; God of War: Ascension's pause menu auto-repeats a held DOWN
     * through every item at 40, one item at 6.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1, s_hold = 40;
      static struct { double t; unsigned mask; } ev[64]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 64) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              const char* hd = getenv("PAD_SCRIPT_HOLD");
              if (hd && atoi(hd) > 0) s_hold = atoi(hd);
              printf("[cellPad] PAD_SCRIPT: %d event(s), each held %d polls\n", n_ev, s_hold);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[64], held[64];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = s_hold; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[64];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0; static int held_st[4] = {-1, -1, -1, -1};
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, &rest, 0);
                      /* optional sticks (test drivers, 2026-11-13): left X Y [right X Y], 0-255, 128 = centre */
                      int st[4] = {-1, -1, -1, -1}, ns = 0;
                      while (ns < 4) { char* e2; long v = strtol(rest, &e2, 0); if (e2 == rest) break;
                                       st[ns++] = v < 0 ? 0 : (v > 255 ? 255 : (int)v); rest = e2; }
                      if (m || ns >= 2) { for (int k = 0; k < 4; k++) held_st[k] = st[k];
                               printf("[cellPad] PAD_FILE sticks %d %d %d %d\n", st[0], st[1], st[2], st[3]); }
                      if (m || ns >= 2) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF);
              if (held_st[0] >= 0) { hs->analog_lx = (u8)held_st[0]; hs->analog_ly = (u8)held_st[1]; }
              if (held_st[2] >= 0) { hs->analog_rx = (u8)held_st[2]; hs->analog_ry = (u8)held_st[3]; }
              if (held_n == 0 && held_st[0] >= 0) { hs->analog_lx = hs->analog_ly = 128; }
              if (held_n == 0 && held_st[2] >= 0) { hs->analog_rx = hs->analog_ry = 128; }
              if (held_n == 0) { held_st[0] = held_st[2] = -1; } }
      } }

    /* PS3_QUIT_HOLD_MS (default 2000; 0 off): SELECT and START held together
     * that long close the game the way its window's close button does, so the
     * pipelines compiled this session are saved first ("[boot] game window
     * closed"). God of War: Ascension's own Quit Game only goes back to its
     * menu and a full-screen window has no close button: players ended the
     * game from the Task Manager or by closing the launcher, the pipelines it
     * had compiled were lost, and the next session compiled them again with
     * parts of the area undrawn meanwhile (tester logs, 2026-11-04). Port 0,
     * after the scripted presses, so PAD_SCRIPT can test it. A controller is
     * read even when the game is not in front (XInput, SDL's background
     * events): its buttons count only with the game's window in front (the
     * keyboard is already gated so). The game stops polling on loading
     * screens: a gap of more than 250 ms between two looks starts the hold
     * again, so a brief press after a load cannot finish an old one. */
#ifdef _WIN32
    if (port_no == 0) {
        static long hold_ms = -1; static unsigned long long since = 0, seen = 0; static int done = 0;
        if (hold_ms < 0) { const char* e = getenv("PS3_QUIT_HOLD_MS"); hold_ms = e ? atol(e) : 2000; if (hold_ms < 0) hold_ms = 0; }
        const u16 both = CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_START;
        const int held = (data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & both) == both &&
                         ((hs->buttons & both) != both || pad_host_window_focused());
        const unsigned long long now = GetTickCount64();
        if (hold_ms && !done && held) {
            if (!since || now - seen > 250) since = now;
            seen = now;
            if (now - since >= (unsigned long long)hold_ms) {
                extern int rsx_window_request_close(void);
                if (rsx_window_request_close()) {
                    done = 1;
                    printf("[cellPad] SELECT+START held %.1f s: closing the game\n", (double)hold_ms / 1000.0);
                    fflush(stdout);
                } else since = now;   /* no window yet: hold again */
            }
        } else since = 0;
    }
#endif

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_PLAY=<start s>[,<end s>]: a crude player for performance tests,
       * so a run exercises gameplay instead of standing still: the left stick
       * walks a slow circle, SQUARE every 0.6 s, TRIANGLE every 2.5 s, R1 +
       * CIRCLE every 4 s, CROSS (jump) every 5 s. Only port 0. */
      { static int pl = -1; static double p_from = 0, p_until = 1e30;
        static unsigned long long p_t0 = 0;
        if (pl < 0) { const char* e = getenv("PAD_PLAY"); pl = e ? 1 : 0;
                      if (e) sscanf(e, "%lf,%lf", &p_from, &p_until); p_t0 = GetTickCount64(); }
        const double t = (double)(GetTickCount64() - p_t0) / 1000.0;
        if (pl && port_no == 0 && t >= p_from && t < p_until) {
            const double a = (t - p_from) * 0.8;
            hs->analog_lx = (u8)(128.0 + 120.0 * cos(a));
            hs->analog_ly = (u8)(128.0 + 120.0 * sin(a));
            hs->connected = 1;
            u16 d1 = 0, d2 = 0;
            if (fmod(t, 0.6) < 0.12) d2 |= 0x80;          /* SQUARE */
            if (fmod(t, 2.5) < 0.15) d2 |= 0x10;          /* TRIANGLE */
            if (fmod(t, 4.0) < 0.20) d2 |= 0x08 | 0x20;   /* R1 + CIRCLE */
            if (fmod(t, 5.0) < 0.10) d2 |= 0x40;          /* CROSS */
            data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= d1;
            data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= d2;
        } }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
#ifdef _WIN32
    if (port_no == 0 && g_pad_menu_enabled) pad_menu_gate(data);   /* in-game menu (cellPad_menu.h) */
#endif
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (_n++ < 2)
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

/* Vibration (2026-11-14): a wireless pad (tester: 8BitDo Ultimate 2 over XInput) stopped sending input while the
 * game asked for vibration continuously (the Ouroboros amulet): every call went to the pad. Now a new vibration is sent
 * only when its strength changes, or to keep it going (every 80 ms while on), so the radio is not flooded.
 * PS3_PAD_RUMBLE=0 turns vibration off; PS3_PAD_RUMBLE=2 sends every call as before. */
static int pad_rumble_mode(void)
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("PS3_PAD_RUMBLE"); m = (e && *e) ? atoi(e) : 1;
                 printf("[cellPad] vibration: %s (PS3_PAD_RUMBLE=%d)\n", m == 0 ? "off" : m == 2 ? "every call" : "on change, 80 ms keep-alive", m); }
    return m;
}
/* 1: send this one; 0: skip it (same strength as the last one sent, sent less than 80 ms ago). */
static int pad_rumble_should_send(u32 port, unsigned lo, unsigned hi)
{
    static unsigned last_lo[CELL_PAD_MAX_PORT_NUM], last_hi[CELL_PAD_MAX_PORT_NUM];
    static unsigned long long last_ms[CELL_PAD_MAX_PORT_NUM];
    const int m = pad_rumble_mode();
    if (m == 0) return 0;
    if (m == 2) return 1;
    const unsigned long long now = (unsigned long long)GetTickCount64();
    if (lo == last_lo[port] && hi == last_hi[port] && (lo == 0 && hi == 0 ? 1 : now - last_ms[port] < 80)) return 0;
    last_lo[port] = lo; last_hi[port] = hi; last_ms[port] = now;
    return 1;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);
    {   /* skip repeats (see pad_rumble_should_send) */
        const unsigned lo = (unsigned)param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], hi = (unsigned)param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL];
        if (!pad_rumble_should_send(port_no, lo, hi)) return CELL_OK;
    }

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            200 /* duration ms: longer than the 80 ms keep-alive, so a held vibration does not stutter */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
