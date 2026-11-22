/* In-game settings menu: the pad's side (ovl, 2026-11-11). Included once by cellPad.c (Windows only).
 *
 * The menu is drawn and driven by the renderer (libs/video/rsx_ingame_menu.h). cellPadGetData is the one place
 * where every input reaches the game (real pads, the keyboard map, PAD_SCRIPT / PAD_FILE / PAD_PLAY / PAD_STICK), so:
 *   - while the panel is open (g_pad_menu_block) port 0 hands the game a neutral pad: no buttons, sticks centred,
 *     no pressure;
 *   - after it closes the pad stays neutral until every button is up (at most 3 s), so the press that closed the
 *     panel never reaches the game;
 *   - with the panel closed, SELECT held ALONE for LD_MENU_HOLD_MS (default 500) asks the menu to open
 *     (g_pad_menu_hold_req). SELECT is this game's own pause / back button, so a SELECT press that starts alone is
 *     HELD BACK from the game until it is clear what it is (step 4, after the review's fault 1: the press of a hold
 *     reached the game and un-paused it when it was already in its pause menu):
 *       let go before the hold time  = a tap: the game gets a short SELECT press right after (its pause works as
 *                                      ever, at most half a second later than the finger);
 *       held long enough             = the panel: the game never sees that press. When the game is NOT in its pause
 *                                      menu, one short SELECT press is handed to it as the panel opens, so the game
 *                                      pauses itself (LD_MENU_AUTOPAUSE=0: no such press);
 *       another button joins         = not for the menu: the pad is handed on as it is (SELECT+START quit is read
 *                                      before this gate and is not touched).
 * Nothing here runs unless the menu enabled itself (g_pad_menu_enabled): with LD_INGAME_MENU=0 the cost is one read
 * of a flag per pad read. The counters (cellPad_menu_counters) are the proof that the game got nothing. */
#ifndef CELLPAD_MENU_H
#define CELLPAD_MENU_H

volatile long g_pad_menu_enabled;     /* the renderer's menu exists (set once by it) */
volatile long g_pad_menu_block;       /* 1 while the panel is open */
volatile long g_pad_menu_hold_req;    /* SELECT was held alone long enough; the menu takes it */
static volatile long s_pm_wait;       /* panel closed, waiting for every button to be released */
static volatile long s_pm_last_btn, s_pm_last_lx = 128, s_pm_last_ly = 128;
static volatile unsigned long long s_pm_last_ms, s_pm_close_ms;
static volatile long long s_pm_neutral, s_pm_neutral_input, s_pm_wait_reads, s_pm_passed, s_pm_passed_input;
static volatile long s_pm_back_ms = -1;
static long s_pm_hold_ms = -1;
/* SELECT held back (see the top): 0 nothing, 1 down alone and held back, 2 the hold fired (held back until it is let
 * go), 3 handed on as it is until it is let go. The tap handed to the game: reads left / until / given up at. */
static int s_pm_sel, s_pm_autopause = -1;
static unsigned s_pm_prev_btn;
static unsigned long long s_pm_sel_since, s_pm_sel_seen, s_pm_tap_until, s_pm_tap_expire;
static volatile long s_pm_tap_reads;
static volatile long long s_pm_sel_withheld, s_pm_sel_taps, s_pm_sel_pause, s_pm_sel_holds, s_pm_pause_reads;
static volatile long s_pm_last_pause_sent = -1;   /* the last hold: 1 a pause press was handed to the game, 0 not */

static void pad_poll_backend(void);

/* The menu opened (on = 1) or closed (on = 0: the release wait starts). */
void cellPad_menu_block(int on)
{
    if (on) { s_pm_wait = 0; s_pm_back_ms = -1; g_pad_menu_block = 1; }
    else if (g_pad_menu_block) { s_pm_close_ms = GetTickCount64(); s_pm_wait = 1; g_pad_menu_block = 0; }
}

/* What the pad holds now, for the menu's own navigation: the merged host state (controllers + keyboard map) and,
 * when the game read the pad in the last 150 ms, what that read held before it was made neutral (test injections
 * are only there). When the game is not reading the pad (a loading screen) the backend is polled from here. */
void cellPad_menu_read(unsigned* buttons, int* lx, int* ly)
{
    const unsigned long long now = GetTickCount64();
    const int fresh = now - s_pm_last_ms < 150;
    if (!fresh && s_pad_initialized) pad_poll_backend();
    unsigned b = s_host_state[0].buttons;
    int x = s_host_state[0].analog_lx, y = s_host_state[0].analog_ly;
    if (!s_host_state[0].connected) { b = 0; x = y = 128; }
    if (fresh) {
        b |= (unsigned)s_pm_last_btn;
        if (abs((int)s_pm_last_lx - 128) > abs(x - 128)) x = (int)s_pm_last_lx;
        if (abs((int)s_pm_last_ly - 128) > abs(y - 128)) y = (int)s_pm_last_ly;
    }
    *buttons = b; *lx = x; *ly = y;
}

/* neutral: pad reads handed to the game neutral; with_input: how many of them held a button or a pushed stick;
 * wait_reads: the neutral reads after closing, until release; passed / passed_input: reads handed on unchanged
 * (and those of them with input) since the menu exists; back_ms: from closing to the first unchanged read (-1: not
 * yet). */
void cellPad_menu_counters(long long* neutral, long long* with_input, long long* wait_reads, long long* passed,
                           long long* passed_input, long* back_ms)
{
    *neutral = s_pm_neutral; *with_input = s_pm_neutral_input; *wait_reads = s_pm_wait_reads;
    *passed = s_pm_passed; *passed_input = s_pm_passed_input; *back_ms = s_pm_back_ms;
}

/* The SELECT gate's totals: reads with SELECT held back, taps handed on, pause presses made for a hold, holds that
 * asked for the panel; last_pause: what the last hold did (1 pause press sent, 0 none, -1 no hold yet); pause_reads:
 * the pad reads that carried such a pause press while the panel was open (SELECT alone; not counted as neutral). */
void cellPad_menu_select_counters(long long* withheld, long long* taps, long long* pauses, long long* holds, long* last_pause,
                                  long long* pause_reads)
{
    *withheld = s_pm_sel_withheld; *taps = s_pm_sel_taps; *pauses = s_pm_sel_pause; *holds = s_pm_sel_holds;
    *last_pause = s_pm_last_pause_sent; *pause_reads = s_pm_pause_reads;
}

/* 1: the game's own pause menu is up, 0: it is not, -1: not this game / unknown (the flag the movie skip reads:
 * byte 0x83D1B5, after the same check of the code that sets it). */
int cellPad_menu_game_paused(void)
{
    static int gow = -1;
    if (!vm_base) return -1;
    if (gow < 0) gow = vm_read32(0x15958) == 0x3CC00084u && vm_read32(0x15970) == 0x9883008Cu;
    if (!gow) return -1;
    return vm_read8(0x83D1B5) != 0;
}

/* A short SELECT press for the game (a tap that was held back, or the pause press of a hold): 3 reads that carry
 * data and at least 80 ms; given up after 1.5 s without reads (a loading screen). */
static void pad_menu_tap_start(unsigned long long now)
{
    s_pm_tap_reads = 3; s_pm_tap_until = now + 80; s_pm_tap_expire = now + 1500;
}
static int pad_menu_tap_due(unsigned long long now)
{
    if (s_pm_tap_reads <= 0 && now >= s_pm_tap_until) return 0;
    if (now > s_pm_tap_expire) { s_pm_tap_reads = 0; s_pm_tap_until = 0; return 0; }
    return 1;
}

static void pad_menu_gate(CellPadData* d)
{
    const unsigned btn = (unsigned)(d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xFF) |
                         ((unsigned)(d->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xFF) << 8);
    const unsigned long long now = GetTickCount64();
    /* a read with len 0 (nothing new since the last one) carries no sticks: keep the last ones */
    const int has = d->button[1] != 0;
    const int lx = has ? (int)d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] : 128;
    const int ly = has ? (int)d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] : 128;
    const int rx = has ? (int)d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] : 128;
    const int ry = has ? (int)d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] : 128;
    const int input = btn || abs(lx - 128) > 48 || abs(ly - 128) > 48 || abs(rx - 128) > 48 || abs(ry - 128) > 48;
    s_pm_last_btn = (long)btn; s_pm_last_lx = lx; s_pm_last_ly = ly; s_pm_last_ms = now;
    int neutral = 0;
    if (g_pad_menu_block) neutral = 1;
    else if (s_pm_wait) {
        if (!btn || now - s_pm_close_ms > 3000) {
            if (btn) { printf("[menu] buttons still held 3 s after closing (0x%04X): control handed back anyway\n", btn); fflush(stdout); }
            s_pm_wait = 0;
            s_pm_back_ms = (long)(now - s_pm_close_ms);
        } else { neutral = 1; s_pm_wait_reads++; }
    }
    if (neutral) {
        s_pm_neutral++;
        if (input) s_pm_neutral_input++;
        if (has) {
            d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = 0;
            d->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = 0;
            d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = 128; d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 128;
            d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = 128;  d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] = 128;
            for (int k = CELL_PAD_BTN_OFFSET_PRESS_RIGHT; k <= CELL_PAD_BTN_OFFSET_PRESS_R2; k++) d->button[k] = 0;
            /* the pause press of the hold that opened the panel: SELECT alone, nothing else */
            if (pad_menu_tap_due(now)) { d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = CELL_PAD_CTRL_SELECT & 0xFF; if (s_pm_tap_reads > 0) s_pm_tap_reads--;
                                         s_pm_neutral--; if (input) s_pm_neutral_input--; s_pm_pause_reads++; }
        }
        if (!(btn & CELL_PAD_CTRL_SELECT)) s_pm_sel = 0;
        s_pm_prev_btn = btn;
        return;
    }
    /* SELECT that starts alone is held back until it is a tap or a hold (see the top). A gap of more than 250 ms
     * between two reads (a loading screen) starts the hold again, as for the quit hold. */
    const unsigned sel = CELL_PAD_CTRL_SELECT;
    int withhold = 0;
    if (s_pm_sel == 0 && (btn & sel) && !(s_pm_prev_btn & sel)) {
        if (btn == sel) { s_pm_sel = 1; s_pm_sel_since = now; s_pm_sel_seen = now; } else s_pm_sel = 3;
    }
    if (s_pm_sel == 1) {
        if (s_pm_hold_ms < 0) { const char* e = getenv("LD_MENU_HOLD_MS"); s_pm_hold_ms = e ? atol(e) : 500; if (s_pm_hold_ms < 100) s_pm_hold_ms = 100; }
        if (btn == sel) {
            if (now - s_pm_sel_seen > 250) s_pm_sel_since = now;
            withhold = 1;
            if (now - s_pm_sel_since >= (unsigned long long)s_pm_hold_ms) {
                s_pm_sel = 2;
                /* a real controller counts only with the game's window in front (as the quit hold) */
                if (!(s_host_state[0].buttons & sel) || pad_host_window_focused()) {
                    if (s_pm_autopause < 0) { const char* e = getenv("LD_MENU_AUTOPAUSE"); s_pm_autopause = !(e && e[0] == '0'); }
                    const int paused = cellPad_menu_game_paused();
                    const int send = s_pm_autopause && paused == 0;
                    if (send) { pad_menu_tap_start(now); s_pm_sel_pause++; }
                    s_pm_last_pause_sent = send;
                    s_pm_sel_holds++;
                    printf("[menu] SELECT held %ld ms: the game did not get that press; the game's pause menu is %s -> %s\n",
                           (long)(now - s_pm_sel_since), paused == 1 ? "up" : paused == 0 ? "not up" : "unknown",
                           send ? "one short SELECT press is handed to the game so it pauses" : "no press is handed to the game");
                    fflush(stdout);
                    g_pad_menu_hold_req = 1;
                }
            }
        } else if (btn & sel) s_pm_sel = 3;               /* another button joined: not for the menu, handed on as it is */
        else {                                            /* let go early: a tap, handed on now */
            s_pm_sel = 0;
            pad_menu_tap_start(now);
            if (s_pm_sel_taps++ < 40) { printf("[menu] SELECT tap (%ld ms) handed on to the game\n", (long)(now - s_pm_sel_since)); fflush(stdout); }
        }
        s_pm_sel_seen = now;
    } else if (s_pm_sel == 2) { if (btn & sel) withhold = 1; else s_pm_sel = 0; }
    else if (s_pm_sel == 3) { if (!(btn & sel)) s_pm_sel = 0; }
    s_pm_prev_btn = btn;
    if (withhold) {
        s_pm_sel_withheld++;
        if (has) d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= (u16)~(sel & 0xFF);
        return;
    }
    if (pad_menu_tap_due(now)) {
        if (has) { d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(sel & 0xFF); if (s_pm_tap_reads > 0) s_pm_tap_reads--; }
        return;
    }
    s_pm_passed++;
    if (input) s_pm_passed_input++;
}
#endif
