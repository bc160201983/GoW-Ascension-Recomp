"""The launcher's window: what each page does. The look (frame, sidebar, Home,
Graphics, cards and rows, search, automatic saving) is in look.py."""

import ctypes
import os
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path
from . import assets as artwork
from . import patches as rpcs3
from .config import (ANISO_LEVELS, APP_NAME, ASPECTS, CONFIG_FILE, CONTROL_GROUPS, CPU_LIMITS, DEADZONES,
    DEFAULTS, DISPLAY_MODES, ENTER_BUTTONS, FPS_CAPS, FPS_LIMIT, GAME_ID, GAME_WINDOWS, HERE, KEY_PRESETS, LOG_DIR, MAC,
    MP_MODES, mp_map_choices,
    ON_OFF, PAD_BUTTONS, PAD_CONTROLS, PAD_DEFAULTS, PRESETS, RENDERERS, RENDER_SCALES, ROOT, SCALE_FILTERS, SHARPEN_LEVELS, FSR_MODES, SHOT, VIBRANCE_LEVELS,
    TK_KEYS, VERSION, VOICE_LANGUAGES, VSYNC_MODES, WINDOWS, WINDOW_SIZES, asset, fps_limit, fps_unlocked, key_label,
    load_settings, merge_ingame,
    save_settings)
from .game import (online_on, title_id as game_title_id, online_dropped_patches, play_exe, installed_exe, lan_discover, start_lan_server, stop_lan_server, lan_server_running, take_discord_join, all_mods, backup_list, close_gently, disc_info, disc_problem, disc_ps3_game, game_running, icon_file,
    gigabytes, join_check, join_split, language_fallback, language_options, missing, prebuild_mods, prepare_game, release_disc, restore_saves, runtime_switches,
    save_dir, split_game_files, start_game, backup_saves, watch_saves, export_saves, import_saves,
    record_install, lost_save_plan, recover_save, mark_handled, any_game_running)
from .config import RELEASE
from .i18n import available, current, set_language, tr
from .music import Music
from .native import mac_dock, register_font
from . import social
from . import updater
from . import widgets as W
from .look import (ACCENT, ACCENT_HI, BG, CARD, GOLD, LINE, LOG_BG, LOG_FG, MUTED, OFF, PAGES, PANEL, TEXT, Look,
    translated)


BACKGROUNDS = ['background.jpg', 'background.png', 'background-2.jpg', 'background-3.jpg', 'background-4.png']
MAX_LOG_LINES = 4000

# The runtime's fixed layout (ps3kit libs/input/cellPad.c): SDL2 game
# controllers, and the keyboard on the first port when no controller is on.
PAD_LAYOUT = [
    ('✕  Cross', 'A', 'Jump'), ('○  Circle', 'B', 'Grab'), ('□  Square', 'X', 'Light attack'),
    ('△  Triangle', 'Y', 'Heavy attack'), ('L1', 'LB', 'Block'), ('R1', 'RB', ''), ('L2', 'LT', ''),
    ('R2', 'RT', ''), ('L3 / R3', 'Stick clicks', ''), ('Left stick', 'Left stick', 'Walk'),
    ('Right stick', 'Right stick', 'Evade'), ('D-pad', 'D-pad', ''),
    ('START', 'Menu · Options', 'Pause'), ('SELECT', 'View · Share / Touchpad', ''),
]
KEY_LAYOUT = [
    ('Arrows', 'D-pad and left stick'), ('Z', '✕  Cross'), ('X', '○  Circle'), ('A', '□  Square'),
    ('S', '△  Triangle'), ('Q / W', 'L1 / R1'), ('1 / 2', 'L2 / R2'), ('Enter', 'START'), ('Tab', 'SELECT'),
]


def tr_key(name):
    return tr(key_label(name))



AVX2_MESSAGE = ('The game could not run on this processor (code {code}).\n\n'
                'It needs a CPU with AVX2: Intel 4th generation (Haswell, 2013) or newer, or AMD Ryzen. '
                'Older CPUs such as the i7-2600, i7-3770, AMD FX and some Pentium/Celeron chips do not have it. '
                'Settings and updates cannot fix this.\n\n'
                "Open this run's log?")
GPU_MESSAGE = ('The game stopped (code {code}): your graphics card ({gpu}) could not build the game\'s shaders.\n\n'
               'The game needs a DirectX 12 card with Shader Model 6. Older integrated Intel graphics '
               '(HD 4000 / 4600 / 5000 and older) usually cannot run it. Try updating the graphics driver; '
               'if you also have an NVIDIA or AMD card, make sure the game uses that one.\n\n'
               "Open this run's log?")


def unsupported_gpu(log):
    """The GPU name when the run's log shows pipelines failing to build (hr 0x80004005)
    on an Intel integrated GPU before the game stopped, else None."""
    try:
        text = Path(log).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return None
    gpu = None
    for line in text.splitlines():
        if line.startswith('[rsx] live-draw GPU:'):
            gpu = line.split(':', 1)[1].rsplit('(', 1)[0].strip()
            break
    if not gpu or 'intel' not in gpu.lower() or ' arc' in gpu.lower():
        return None
    return gpu if text.count('[pso-fail] create hr=0x80004005') >= 5 else None


class Tail:
    """New text at the end of a log file another process writes."""
    def __init__(self, path):
        self.path, self.pos = Path(path), 0

    def rewind(self, to_end=False):
        try:
            self.pos = self.path.stat().st_size if to_end else 0
        except OSError:
            self.pos = 0

    def read(self, limit=1 << 20):
        try:
            with open(self.path, 'rb') as f:
                f.seek(0, 2)
                size = f.tell()
                if size < self.pos:          # a new file (the previous run's was renamed)
                    self.pos = 0
                if size - self.pos > 4 * limit:
                    self.pos = size - limit
                f.seek(self.pos)
                data = f.read(size - self.pos)
        except OSError:
            return ''
        self.pos += len(data)
        try:
            return data.decode('utf-8')
        except UnicodeDecodeError:
            return data.decode('mbcs' if WINDOWS else 'latin-1', 'replace')


class Launcher(Look):
    FADE_STEPS, FADE_MS, HOLD_MS = 30, 40, 7000

    def __init__(self, root):
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        root.report_callback_exception = self.callback_error   # an error in a button's code: shown and logged
        self.first_start = not CONFIG_FILE.is_file()
        self.s = load_settings()
        if merge_ingame(self.s):     # changed in the game's own menu (F1) since the launcher last looked
            save_settings(self.s)
        self.s['game_exe'] = installed_exe(self.s['game_exe'])   # a fresh install with an old install's settings
        self.ui_language = self.s['ui_language']       # the one this window was built in
        set_language(self.ui_language)
        self.vars = {}
        self.dynamic = {}
        self.mod_vars = {}
        self.mod_order = list(self.s['mods_order'])
        self.selected_mod = None
        self.keymap = {c: list(k) for c, k in self.s['keymap'].items()}
        self.padmap = dict(self.s['padmap'])
        self.capturing = None
        self.process = None
        self.preparing = False
        self.hidden_for_game = False
        self.embedded = False
        self.mounted = None        # the disc folder the launcher mounted (macOS)
        self.patch_list, self.patch_vars, self.patch_error = None, {}, None
        self.patch_proc = self.patch_job = None
        self.game_tail = Tail(LOG_DIR / 'last_run.log')
        self.patch_tail = Tail(rpcs3.LOG)
        # The switches this build of the game reads: the pages offer only those.
        self.switches = runtime_switches(self.s['game_exe'])
        if MAC:   # Tk on macOS counts 72 points to the inch: the sizes below assume Windows' 96
            root.tk.call('tk', 'scaling', 1.33)
        try:
            self.scale = max(1.0, float(root.winfo_fpixels('1i')) / 96.0) if not MAC else 1.0
        except (ValueError, self.tk.TclError):
            self.scale = 1.0
        self.heading = 'Georgia'
        if register_font(HERE / 'Cinzel-Black.ttf'):
            from tkinter import font as tkfont
            families = set(tkfont.families(root))
            self.heading = next((f for f in ('Cinzel Black', 'Cinzel') if f in families), 'Georgia')
        self.ui = 'Segoe UI' if WINDOWS else 'Helvetica Neue' if MAC else 'DejaVu Sans'
        self.mono = 'Consolas' if WINDOWS else 'Menlo' if MAC else 'DejaVu Sans Mono'
        root.title(f'{APP_NAME} (PC) · {social.POSTED_BY}')
        root.configure(bg=BG)
        w = min(self.px(1240), int(root.winfo_screenwidth() * 0.92))
        h = min(self.px(800), int(root.winfo_screenheight() * 0.88))
        root.geometry(f'{w}x{h}+{(root.winfo_screenwidth() - w) // 2}+{(root.winfo_screenheight() - h) // 3}')
        root.minsize(min(self.px(1080), w), min(self.px(700), h))
        self.set_icon()
        self.style()
        self.bg_sources = [p for p in (asset(n) for n in BACKGROUNDS) if p]
        self.bg_index, self.bg_cache, self.bg_size, self.frames = 0, {}, None, {}
        self.logo_src = asset('logo.png')
        self.music = Music() if not SHOT else Music.__new__(Music)
        if SHOT:
            self.music.ok = False
            self.music.error = ''
        self.build()
        self.show('home')
        self.saved = self.gather()
        if self.s.get('music') == '1':
            self.music.play()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(400, self.watch_changes)
        root.after(self.HOLD_MS, self.next_background)
        root.after(300, self.tail_logs)
        if not SHOT:
            root.after(600, self.first_run_artwork)
        if self.first_start and not missing(self.s, split_ok=True):
            self.refresh_status(tr('Welcome! Check the settings on each page, then press PLAY.'))
        if not SHOT:
            threading.Thread(target=self.check_running_elsewhere, daemon=True).start()
        self.update_busy = False
        if not SHOT and self.s.get('check_updates') == '1':
            root.after(3000, self.check_updates)
        if not SHOT and (RELEASE or os.environ.get('ASCENSION_SAVE_RECOVERY') == '1'):
            root.after(1500, self.check_lost_saves)

    def check_lost_saves(self, manual=False):
        """Saves an update left behind (game.lost_save_plan, 2026-11-14): story saves in another copy of the port
        (a full zip extracted into a new folder) or loose in savedata\\ (the AutoSave2 bug of 0.1.26 and older).
        Newer ones, or any where there is none here, are copied in; an older, different one is offered. Each
        find once; the Saves page button looks again, finds already answered included."""
        try:
            record_install()
        except OSError:
            pass
        if self.process or self.preparing:
            if manual:
                self.messagebox.showinfo(APP_NAME, tr('Close the game first.'))
            return
        try:
            if any_game_running():
                if manual:
                    self.messagebox.showinfo(APP_NAME, tr('Close the game first.'))
                return
            plan = lost_save_plan(self.s, ignore_handled=manual)
        except OSError as e:
            if manual:
                self.messagebox.showerror(APP_NAME, tr('Could not look for saves:\n{error}', error=e))
            return

        def when(t):
            return time.strftime('%d/%m/%Y %H:%M', time.localtime(t)) if t else '-'

        def where(it):
            if it['loose']:
                return tr('{folder} (checkpoints that version 0.1.26 or older saved in the wrong folder)',
                          folder=it['where'])
            return it['where']

        stamp = time.strftime('%Y%m%d-%H%M%S')
        done, kept, answered, errors = [], set(), [], []
        # At launcher start only saves missing here are filled in (auto). A save here is never replaced unless
        # the player asks on the Saves page (2026-11-15: the start-up check put chapter 19 back over chapter 22).
        offers = [p for p in plan if not p['auto']] if manual else []
        for it in [p for p in plan if p['auto']] + offers:
            if not it['auto'] and not self.messagebox.askyesno(APP_NAME, tr(
                    'Another story save of this game was found:\n\n{where}\nlast saved {then}\n\nThe save here ({name}) '
                    'was last saved {now}.\n\nThe dates only say when each was saved, not how far each one is in the '
                    'story. Answer Yes only if you know the other one is further.\n\nUse the other save? The save here '
                    'is moved to gamedata\\replaced-saves (nothing is deleted).', where=where(it), then=when(it['newest']),
                    name=it['name'], now=when(it['here_newest'])), default='no'):
                answered.append(it)
                continue
            try:
                r = recover_save(it, stamp)
            except OSError as e:
                errors.append(f"{it['name']}: {e}")
                continue
            answered.append(it)
            done.append(it)
            if r:
                kept.add(str(r))
        try:
            mark_handled(answered)
        except OSError:
            pass
        if done:
            lines = '\n'.join(tr('• {name} from {where} (last saved {then})', name=it['name'], where=where(it),
                                 then=when(it['newest'])) for it in done)
            text = tr('Your saves were recovered:\n\n{lines}', lines=lines)
            if kept:
                text += '\n\n' + tr('The saves they replaced are kept in {folder}.', folder=', '.join(sorted(kept)))
            text += '\n\n' + tr('A copy of the saves from before is also under Game → Saves → Save backups.')
            self.messagebox.showinfo(APP_NAME, text)
            try:
                self.refresh_backups()
            except (AttributeError, self.tk.TclError):
                pass
            self.refresh_status(tr('Saves recovered.'))
        if errors:
            self.messagebox.showerror(APP_NAME, tr('Could not recover:\n{error}', error='\n'.join(errors)))
        if manual and not done and not errors and not answered:
            self.messagebox.showinfo(APP_NAME, tr('No other saves of this game were found.'))

    def check_running_elsewhere(self):
        """A copy of the game started elsewhere (run.ps1, --play): say so."""
        if game_running(play_exe(self.s)):
            self.root.after(0, lambda: not self.process and self.refresh_status(
                tr('The game is already running (started elsewhere); PLAY works once it is closed.')))

    def px(self, n):
        return round(n * self.scale)

    def set_icon(self):
        ico = icon_file()
        if not ico:
            return
        try:
            if WINDOWS:
                self.root.iconbitmap(default=str(ico))
            else:
                self.icon_photo = self.tk.PhotoImage(file=str(ico))
                self.root.iconphoto(True, self.icon_photo)
        except self.tk.TclError:
            pass

    # ---- look -------------------------------------------------------------------------------
    # ---- settings widgets -------------------------------------------------------------------
    def var(self, key):
        if key not in self.vars:
            self.vars[key] = self.tk.StringVar(value=str(self.s.get(key, DEFAULTS.get(key, ''))))
        return self.vars[key]

    def has(self, *switches):
        return all(n in self.switches for n in switches)

    def runtime_patches(self):
        """The game lists its patches and switches them as it starts
        (PS3_PATCH_LIST / PS3_PATCHES): ticking one needs no rebuild, in a
        release too."""
        return self.has('PS3_PATCH_LIST')

    def patches_now(self, s):
        """The patches on at the next start when the game switches them, else
        None (what the build was made with)."""
        if not self.runtime_patches():
            return None
        try:
            listed = self.patch_list if self.patch_list is not None else rpcs3.exe_patches(s['game_exe'])
        except OSError:
            return None
        return rpcs3.patches_on(s, listed)

    def refresh_choices(self, key, fallback):
        for box, shown, source in self.dynamic.get(key, []):
            options = translated(source())
            box.maps = (dict(options), {label: value for value, label in options})
            box.configure(values=[label for _, label in options])
            if self.var(key).get() not in box.maps[0]:
                self.var(key).set(fallback)
            shown.set(box.maps[0].get(self.var(key).get(), options[0][1]))

    def title_cell(self, parent, title, badge=None, badge_style='Badge.TLabel'):
        cell = self.ttk.Frame(parent)
        self.ttk.Label(cell, text=title).pack(side='left')
        if badge:
            self.ttk.Label(cell, text=badge, style=badge_style).pack(side='left', padx=(8, 0))
        return cell

    @staticmethod
    def log_append(widget, text):
        if not text or not widget.winfo_exists():
            return
        at_end = widget.yview()[1] >= 0.999
        widget.configure(state='normal')
        widget.insert('end', text)
        lines = int(widget.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            widget.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        if at_end:
            widget.see('end')
        widget.configure(state='disabled')

    @staticmethod
    def log_set(widget, text):
        widget.configure(state='normal')
        widget.delete('1.0', 'end')
        widget.insert('end', text)
        widget.see('end')
        widget.configure(state='disabled')

    # ---- layout -----------------------------------------------------------------------------
    def fullscreen_keys(self):
        return 'F11 ' + tr('or') + (' Ctrl+Cmd+F' if MAC else ' Alt+Enter')

    WHEEL_STEP = 40     # pixels per wheel notch

    def page_scroll(self, event):
        page = self.pages.get(self.current)
        widget = str(event.widget).lower()
        if hasattr(page, 'scroll') and 'combobox' not in widget and 'popdown' not in widget \
                and not isinstance(event.widget, self.tk.Text):
            return page.scroll
        return None

    def wheel(self, event, notches=None):
        """A wheel notch is 120 from Tk 8.7 on, and on Windows; Tk 8.6 on
        macOS counts 1 per notch."""
        scroll = self.page_scroll(event)
        if not scroll:
            return
        if notches is None:
            notches = -event.delta / 120 if WINDOWS or self.tk.TkVersion >= 8.7 else -event.delta
        scroll(round(notches * self.WHEEL_STEP))

    def touchpad(self, event):
        scroll = self.page_scroll(event)
        if scroll:
            _dx, dy = (int(v) for v in self.root.tk.splitlist(self.root.tk.call('tk::PreciseScrollDeltas', event.delta)))
            if dy:
                scroll(-dy)

    # ---- home: the cover art, the logo, quick settings ----------------------------------------
    def scaled_logo(self, width):
        if not self.logo_src:
            return None
        try:
            from PIL import Image, ImageTk
            with Image.open(self.logo_src) as im:
                im = im.convert('RGBA')
                return ImageTk.PhotoImage(im.resize((width, round(im.height * width / im.width)), Image.LANCZOS))
        except (OSError, ImportError):
            return None

    def next_background(self):
        if len(self.bg_sources) < 2 or self.current != 'home' or self.process:
            self.root.after(self.HOLD_MS, self.next_background)
            return
        nxt = (self.bg_index + 1) % len(self.bg_sources)

        def step(i):
            if i > self.FADE_STEPS:
                self.bg_index = nxt
                self.draw_banner()
                self.root.after(self.HOLD_MS, self.next_background)
                return
            t = i / self.FADE_STEPS
            self.draw_banner((nxt, t * t * (3 - 2 * t)))    # smoothstep
            self.root.after(self.FADE_MS, step, i + 1)
        step(1)

    def choose_game_files(self, kind):
        """Pick the game folder, the .iso or a decrypted EBOOT.elf, then make the game ready."""
        from . import gamefiles
        if self.process or self.preparing:
            return
        eboot = None
        if kind == 'iso':
            source = self.filedialog.askopenfilename(title=tr('Your disc image'),
                                                     filetypes=[(tr('Disc image'), '*.iso'), (tr('All files'), '*.*')])
        elif kind == 'eboot':
            eboot = self.filedialog.askopenfilename(title=tr('The decrypted EBOOT.elf'),
                                                    filetypes=[('EBOOT', '*.elf *.ELF *.bin *.BIN'), (tr('All files'), '*.*')])
            if not eboot:
                return
            source = self.s.get('game_source') or self.filedialog.askdirectory(
                title=tr('Your game folder (with PS3_GAME)'))
        else:
            source = self.filedialog.askdirectory(title=tr('Your game folder (with PS3_GAME)'))
        if not source:
            return
        self.preparing = True
        self.play_button.configure(state='disabled')

        def status(text):
            self.root.after(0, lambda: self.refresh_status(tr(text)))

        def work():
            try:
                result, error = gamefiles.prepare(source, eboot, status), None
            except gamefiles.NotReady as e:
                result, error = None, str(e)
            except OSError as e:
                result, error = None, str(e)
            except Exception as e:                   # never leave the launcher stuck in "preparing"
                result, error = None, f'Preparing the game files failed: {type(e).__name__}: {e}'
            self.root.after(0, self.game_files_ready, source, result, error)
        threading.Thread(target=work, daemon=True).start()

    def game_files_ready(self, source, result, error):
        self.preparing = False
        self.play_button.configure(state='normal')
        if error:
            self.s['game_source'] = str(source)      # "Choose decrypted EBOOT.elf" continues from here
            save_settings(self.s)
            self.refresh_status(tr('Game files not ready yet.'))
            self.messagebox.showwarning(APP_NAME, error)
            return
        for key, value in result.items():
            self.s[key] = value
            if key in self.vars:
                self.var(key).set(value)
        save_settings(self.s)
        # The US disc has its own languages (Brazilian Portuguese without a mod, Latin-American Spanish).
        self.refresh_choices('language', language_fallback(self.current_mod_settings()))
        self.saved = self.gather()
        self.refresh_status(tr('Game files ready · {summary}', summary=self.summary(self.s)))
        self.first_run_artwork()
        self.show('home')
        self.draw_banner()

    def first_run_artwork(self):
        """No artwork yet: make it from the disc (app/assets.py), in the background."""
        want = artwork.missing()
        ps3_game = disc_ps3_game(self.s)
        if not want or not (ps3_game / 'PIC1.PNG').is_file():
            return

        def work():
            notes = []
            done = artwork.make_assets(ps3_game, log=notes.append)
            self.root.after(0, self.artwork_ready, done, notes)
        threading.Thread(target=work, daemon=True).start()

    def artwork_ready(self, done, notes):
        if not done:
            return
        self.bg_sources = [p for p in (asset(n) for n in BACKGROUNDS) if p]
        self.logo_src = asset('logo.png')
        self.bg_cache, self.frames, self.bg_size = {}, {}, None
        self.logo_banner_size = None
        self.draw_side_logo()
        self.draw_banner()
        self.set_icon()
        if 'music.mp3' in done and not SHOT:
            self.music.close()
            self.music = Music()
            if self.music_on() and not self.process:
                self.music.play()
            self.refresh_music_label()

    # ---- graphics -----------------------------------------------------------------------------
    # ---- game: gameplay, language, saves ------------------------------------------------------
    def build_game(self):
        f = self.scrolled_page('game', tr('Game'), tr('Cutscenes, language, subtitles and saves.'))
        self.section(f, tr('Gameplay'), top=4)
        self.check(f, 'skip_movies', tr('Skip cutscenes'),
                   tr("The prerendered cutscenes and intro movies are skipped: the game goes on as if each one "
                      "had been skipped with the controller. The in-engine scenes still play. The Skip intro mod "
                      "(Mods) also removes the notice and logo screens at start."),
                   badge=None if self.has('PS3_SKIP_MOVIES') else tr('NEEDS A NEWER BUILD'))
        if self.has('PS3_MP_MODE'):
            # Offline Multiplayer (Patches): what its Quick Play starts. The map list follows the mode.
            self.section(f, tr('Offline multiplayer'))
            self.row(f, tr('Offline multiplayer mode'), self.choice(f, 'mp_mode', MP_MODES, width=36),
                     tr('The mode that Quick Play starts in the multiplayer lobby (Game Mode, one step up from Quick '
                        'Play there, lists every mode too). Needs the patch "Offline Multiplayer (experimental)" '
                        '(Patches). The modes for 2 to 8 players are played alone here: the game then declares you '
                        'the winner after about 10 seconds, unless the patch "Offline Multiplayer: no instant win '
                        'when alone" is on too.'))
            self.row(f, tr('Offline multiplayer map'),
                     self.choice(f, 'mp_map', lambda: mp_map_choices(self.var('mp_mode').get()), width=36),
                     tr('The map of the first match. After a match the lobby starts the next one by itself: Trial '
                        "of the Gods on the next map of the mode's list, the other modes on a map again (the same one "
                        "when a map is chosen here). A mode picked in the game's own Game Mode menu starts at "
                        'the same place of its own list. Both are read when the game starts: to change them, quit '
                        'the game (hold SELECT+START) and press PLAY again.'))
            self.var('mp_mode').trace_add('write', lambda *_: self.refresh_choices('mp_map', 'cycle'))
        self.section(f, tr('Language'))
        self.row(f, tr('Game language'), self.choice(f, 'language', self.languages),
                 tr("Text, subtitles and voices follow the PS3's system language. Greek voices: the game itself "
                    'offers them at first start.'))
        self.row(f, tr('Voice language'), self.choice(f, 'voice_language', VOICE_LANGUAGES),
                 tr('English voices with the text in another language, or keep the Speech option chosen in the '
                    "game's own Options menu. The very first start uses the game language for the voices too."))
        self.check(f, 'subtitles', tr('Subtitles'), tr('Subtitles for dialogue and cinematics.'))
        self.note(f, tr('The game keeps the language, voices and subtitles in its system save; the launcher sets '
                        'them there before each start, so they hold after the first start too.'))
        self.section(f, tr('Saves'))
        self.check(f, 'backup_saves', tr('Back up before playing'),
                   tr('Copies the saves to gamedata/save-backups before each start, and again each time the game '
                      'saves while you play, so a crash cannot take your progress with it (the last 20 are kept).'))
        holder = self.ttk.Frame(f)
        self.backup_box = self.ttk.Combobox(holder, state='readonly', width=30)
        self.backup_box.bind('<MouseWheel>', lambda e: 'break')
        self.backup_box.pack(side='left')
        self.restore_button = self.ttk.Button(holder, text=tr('Restore'), command=self.restore_backup)
        self.restore_button.pack(side='left', padx=(6, 0))
        self.ttk.Button(holder, text=tr('Refresh'), command=self.refresh_backups).pack(side='left', padx=(6, 0))
        self.row(f, tr('Save backups'), holder, tr('Newest first. Restoring backs up the current saves first, so '
                                                   'nothing is lost.'))
        self.buttons(f, ((tr('Back up now'), self.backup_now),
                         (tr('Export saves…'), self.export_saves),
                         (tr('Import saves…'), self.import_saves)), top=12)
        self.note(f, tr('Export makes one .zip of all your saves; on another PC, Import puts them in place '
                        '(its current saves are backed up first).'), top=4)
        self.buttons(f, ((tr('Open saves folder'), lambda: self.open_path(save_dir())),
                         (tr('Open backups'), lambda: self.open_path(ROOT / 'gamedata' / 'save-backups'))), top=8)
        self.buttons(f, ((tr('Find lost saves'), lambda: self.check_lost_saves(manual=True)),), top=8)
        self.note(f, tr('Looks for your saves in other folders of the port (an older version you extracted '
                        'somewhere else, Downloads, the Desktop) and copies them here. Nothing is deleted.'), top=4)
        self.refresh_backups()

    def backup_now(self):
        try:
            dst = backup_saves()
        except OSError as e:
            self.messagebox.showerror(APP_NAME, tr('Could not back up:\n{error}', error=e))
            return
        self.refresh_backups()
        self.refresh_status(tr('Saves backed up.') if dst else tr('There are no saves yet.'))

    def export_saves(self):
        path = self.filedialog.asksaveasfilename(
            title=tr('Export saves'), defaultextension='.zip', filetypes=[('Zip', '*.zip')],
            initialfile=time.strftime('GoW-Ascension-saves-%Y%m%d-%H%M.zip'))
        if not path:
            return
        try:
            n = export_saves(path)
        except (OSError, ValueError) as e:
            self.messagebox.showerror(APP_NAME, tr('Could not export:\n{error}', error=e))
            return
        self.refresh_status(tr('{n} save files exported to {path}', n=n, path=path))

    def import_saves(self):
        if self.process or self.preparing:
            self.messagebox.showinfo(APP_NAME, tr('Close the game first.'))
            return
        path = self.filedialog.askopenfilename(title=tr('Import saves'), filetypes=[('Zip', '*.zip')])
        if not path:
            return
        if not self.messagebox.askyesno(APP_NAME, tr('Replace the saves with the ones in {path}? The current saves '
                                                     'are backed up first.', path=Path(path).name)):
            return
        try:
            n = import_saves(path)
        except Exception as e:
            self.messagebox.showerror(APP_NAME, tr('Could not import:\n{error}', error=e))
            return
        self.refresh_backups()
        self.refresh_status(tr('{n} save files imported.', n=n))

    def refresh_backups(self):
        self.backups = backup_list()
        labels = []
        for d in self.backups:
            try:
                labels.append(time.strftime('%d/%m/%Y  %H:%M:%S', time.strptime(d.name, '%Y%m%d-%H%M%S')))
            except ValueError:
                labels.append(d.name)
        self.backup_box.configure(values=labels)
        self.backup_box.set(labels[0] if labels else tr('No backups'))
        busy = self.process or self.preparing or not labels
        self.restore_button.configure(state='disabled' if busy else 'normal')

    def restore_backup(self):
        i = self.backup_box.current()
        if i < 0 or self.process or self.preparing:
            return
        label = self.backup_box.get()
        if not self.messagebox.askyesno(APP_NAME, tr('Put the saves back to {label}?', label=label)):
            return
        try:
            restore_saves(self.backups[i])
        except OSError as e:
            self.messagebox.showerror(APP_NAME, tr('Could not restore:\n{error}', error=e))
            return
        self.refresh_backups()
        self.refresh_status(tr('Saves restored from {label}.', label=label))

    # ---- controls -------------------------------------------------------------------------------
    def build_controls(self):
        tk = self.tk
        f = self.scrolled_page('controls', tr('Controls'),
                               tr('Controllers, the keyboard, and the buttons of the system menus.'))
        self.section(f, tr('General'), top=4)
        self.row(f, tr('Confirm button'), self.choice(f, 'enter_button', ENTER_BUTTONS),
                 tr('Which button accepts in the system menus.'))
        self.check(f, 'esc_quits', tr('Esc closes the game'),
                   tr('Off: Esc does nothing in the game; close it with its window.'))
        self.key_cells, self.pad_vars = {}, {}
        remap_keys, remap_pad = self.has('PS3_KEYMAP'), self.has('PS3_PADMAP')
        self.section(f, tr('Controller'))
        self.note(f, tr('Xbox, DualShock 4, DualSense and other controllers SDL2 knows work as they are plugged '
                        'in, with the PS3 layout:'), top=4)
        if remap_pad:
            self.build_pad_tab(self.table_frame(f))
        else:
            t = self.table_frame(f)
            for col, head in enumerate((tr('PS3'), tr('Xbox · PlayStation'), tr('In the game'))):
                tk.Label(t, text=head, bg=PANEL, fg=MUTED, font=(self.ui, 8, 'bold')).grid(row=0, column=col, sticky='w',
                                                                                         padx=(0, self.px(30)))
            for r, (ps3, pad, action) in enumerate(PAD_LAYOUT, start=1):
                for col, text in enumerate((tr(ps3), tr(pad), tr(action) if action else '')):
                    tk.Label(t, text=text, bg=PANEL, fg=TEXT if col < 2 else MUTED, font=(self.ui, 10)).grid(
                        row=r, column=col, sticky='w', padx=(0, self.px(30)), pady=1)
        self.section(f, tr('Keyboard'))
        if remap_keys:
            self.build_keyboard_tab(self.table_frame(f))
        else:
            self.note(f, tr('With no controller connected, the keyboard plays as the controller:'), top=4)
            t = self.table_frame(f)
            for r, (key, ps3) in enumerate(KEY_LAYOUT):
                tk.Label(t, text=tr(key), bg=CARD, fg=TEXT, font=(self.ui, 10, 'bold'), width=12, pady=3).grid(
                    row=r, column=0, sticky='w', pady=2)
                tk.Label(t, text=tr(ps3), bg=PANEL, fg=TEXT, font=(self.ui, 10)).grid(row=r, column=1, sticky='w',
                                                                                      padx=(self.px(14), 0))
            self.note(f, tr('The keyboard has no right stick (evade) or L3/R3; a controller is the way to play.'))
        if not (remap_keys and remap_pad):
            self.section(f, tr('Coming later'))
            self.unavailable(f, [(tr('Change the buttons'), tr('not available yet')),
                                 (tr('Stick dead zone'), tr('not available yet'))],
                             tr("This build's runtime has the fixed layout above (it does not read PS3_KEYMAP, "
                                'PS3_PADMAP or PS3_PAD_DEADZONE yet); remapping shows up here once it does.'))

    def table_frame(self, f):
        t = self.tk.Frame(f, bg=PANEL)
        self.put(f, t, sticky='w', pady=(8, 0))
        return t

    def build_keyboard_tab(self, f):
        tk, ttk = self.tk, self.ttk
        top = tk.Frame(f, bg=PANEL)
        top.grid(row=0, column=0, columnspan=4, sticky='we', pady=(0, 6))
        tk.Label(top, text=tr('Click a key and press the new key or mouse button. Esc cancels; × clears.'),
                 bg=PANEL, fg=MUTED, font=(self.ui, 9)).pack(side='left')
        ttk.Button(top, text=tr('Classic (arrows)'), command=lambda: self.key_preset('classic')).pack(side='right')
        ttk.Button(top, text=tr('Default (WASD)'), command=lambda: self.key_preset('pc')).pack(side='right', padx=(0, 8))
        r = 1
        for group, items in CONTROL_GROUPS:
            tk.Label(f, text=tr(group).upper(), bg=PANEL, fg=GOLD, font=(self.heading, 10)).grid(
                row=r, column=0, columnspan=4, sticky='w', pady=(14, 4))
            r += 1
            for control, title in items:
                tk.Label(f, text=tr(title), bg=PANEL, fg=TEXT, font=(self.ui, 10), anchor='w', width=26).grid(
                    row=r, column=0, sticky='w', pady=2)
                for slot in (0, 1):
                    self.key_cells[(control, slot)] = self.key_cell(f, control, slot, r, 1 + slot)
                r += 1
        self.refresh_keys()

    def key_cell(self, parent, control, slot, row, column):
        tk = self.tk
        cell = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=LINE)
        cell.grid(row=row, column=column, sticky='w', padx=(0, 10), pady=2)
        label = tk.Label(cell, text='', bg=CARD, fg=TEXT, font=(self.ui, 10), width=16, pady=4, cursor='hand2')
        label.pack(side='left')
        clear = tk.Label(cell, text='×', bg=CARD, fg=MUTED, font=(self.ui, 11), padx=6, cursor='hand2')
        clear.pack(side='left')
        label.bind('<Button-1>', lambda _e: self.start_capture(control, slot))
        clear.bind('<Button-1>', lambda _e: self.set_key(control, slot, None))
        return cell, label

    def refresh_keys(self):
        for (control, slot), (cell, label) in self.key_cells.items():
            keys = self.keymap.get(control, [])
            name = keys[slot] if slot < len(keys) else None
            capturing = self.capturing == (control, slot)
            label.configure(text=tr('Press…') if capturing else tr_key(name),
                            fg=GOLD if capturing else (TEXT if name else MUTED))
            cell.configure(highlightbackground=GOLD if capturing else LINE)

    def set_key(self, control, slot, name):
        """Put `name` in a control's slot; a key used elsewhere leaves that place."""
        self.cancel_capture(refresh=False)
        if name:
            for other, keys in self.keymap.items():
                if name in keys and (other, keys.index(name)) != (control, slot):
                    keys.remove(name)
        keys = [k for k in self.keymap.get(control, []) if k]
        while len(keys) <= slot:
            keys.append(None)
        keys[slot] = name
        self.keymap[control] = [k for k in keys if k]
        self.refresh_keys()

    def key_preset(self, name):
        self.cancel_capture(refresh=False)
        self.keymap = {c: list(KEY_PRESETS[name].get(c, [])) for _, items in CONTROL_GROUPS for c, _ in items}
        self.refresh_keys()

    def start_capture(self, control, slot):
        self.cancel_capture(refresh=False)
        self.capturing = (control, slot)
        self.refresh_keys()
        # Armed a moment later, so the click that started it is not taken as Mouse1.
        self.root.after(250, self.arm_capture, (control, slot))

    def arm_capture(self, which):
        if self.capturing != which:
            return
        self.root.bind_all('<KeyPress>', self.capture_key)
        self.root.bind_all('<ButtonPress>', self.capture_mouse)

    def cancel_capture(self, refresh=True):
        if not getattr(self, 'capturing', None):
            return
        self.capturing = None
        self.root.unbind_all('<KeyPress>')
        self.root.unbind_all('<ButtonPress>')
        if refresh:
            self.refresh_keys()

    def capture_key(self, event):
        if not self.capturing:
            return None
        if event.keysym == 'Escape':
            self.cancel_capture()
            return 'break'
        sym = event.keysym
        name = TK_KEYS.get(sym)
        if not name and len(sym) == 1 and sym.isascii() and sym.isalnum():
            name = sym.upper()
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def capture_mouse(self, event):
        if not self.capturing:
            return None
        old_aqua = MAC and self.tk.TkVersion < 8.7          # Tk 8.6 on macOS: 2 is the right button
        names = {1: 'Mouse1', 2: 'Mouse2' if old_aqua else 'Mouse3', 3: 'Mouse3' if old_aqua else 'Mouse2',
                 8: 'Mouse4', 9: 'Mouse5'}
        if WINDOWS and self.tk.TkVersion < 8.7:
            names.update({4: 'Mouse4', 5: 'Mouse5'})
        name = names.get(event.num)
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def build_pad_tab(self, f):
        tk, ttk = self.tk, self.ttk
        top = tk.Frame(f, bg=PANEL)
        top.grid(row=0, column=0, columnspan=4, sticky='we', pady=(0, 6))
        ttk.Button(top, text=tr('Restore defaults'), command=self.pad_defaults).pack(side='left')
        r = 1
        for group, items in CONTROL_GROUPS:
            items = [(c, t) for c, t in items if c in PAD_CONTROLS]
            if not items:
                continue
            tk.Label(f, text=tr(group).upper(), bg=PANEL, fg=GOLD, font=(self.heading, 10)).grid(
                row=r, column=0, columnspan=4, sticky='w', pady=(14, 4))
            r += 1
            for control, title in items:
                tk.Label(f, text=tr(title), bg=PANEL, fg=TEXT, font=(self.ui, 10), anchor='w', width=26).grid(
                    row=r, column=0, sticky='w', pady=2)
                v = self.tk.StringVar(value=self.padmap.get(control, PAD_DEFAULTS[control]))
                v.trace_add('write', lambda *_a, c=control, var=v: self.padmap.__setitem__(c, var.get()))
                self.pad_vars[control] = v
                self.choice_var(f, v, PAD_BUTTONS, width=26).grid(row=r, column=1, sticky='w', pady=2)
                r += 1
        if self.has('PS3_PAD_DEADZONE'):
            tk.Label(f, text=tr('Dead zone'), bg=PANEL, fg=TEXT, font=(self.ui, 10), anchor='w', width=26).grid(
                row=r, column=0, sticky='w', pady=(14, 2))
            self.choice(f, 'pad_deadzone', DEADZONES, width=26).grid(row=r, column=1, sticky='w', pady=(14, 2))

    def pad_defaults(self):
        for control, v in self.pad_vars.items():
            v.set(PAD_DEFAULTS[control])
        self.var('pad_deadzone').set('24')

    def choice_var(self, parent, var, options, width=28):
        """A read-only combobox over a StringVar of its own (not a setting key)."""
        options = translated(options)
        shown = self.tk.StringVar()
        box = self.ttk.Combobox(parent, textvariable=shown, state='readonly', width=width,
                                values=[label for _, label in options])
        to_label, to_value = dict(options), {label: value for value, label in options}
        shown.set(to_label.get(var.get(), options[0][1]))
        shown.trace_add('write', lambda *_: shown.get() in to_value and var.set(to_value[shown.get()]))
        var.trace_add('write', lambda *_: shown.get() != to_label.get(var.get()) and shown.set(to_label.get(var.get(), '')))
        box.bind('<MouseWheel>', lambda e: 'break')
        return box

    # ---- patches: RPCS3's game patches, built into the game ----------------------------------
    def build_patches(self):
        tk, ttk = self.tk, self.ttk
        runtime = self.runtime_patches()
        f = self.scrolled_page('patches', tr('Patches'), tr("RPCS3's game patches, switched on or off when the game "
                                                            'starts.') if runtime else
                               tr("RPCS3's game patches, built into the game."))
        self.section(f, tr('How it works'), top=4)
        if runtime:
            self.note(f, tr('Tick or untick a patch: it is saved at once and the game uses it the next time it '
                            'starts. Nothing is rebuilt: the game carries each patched piece of code both ways and '
                            'writes the ticked patches into its memory as it starts, as RPCS3 does. Until you '
                            'change one, the game uses its own defaults (marked DEFAULT).'), top=4)
        else:
            self.note(f, tr('RPCS3 patches a game in memory while it runs. This port translates the game\'s code '
                            'ahead of time, so a patch is written into the game\'s executable and the code it '
                            'touches is translated and compiled again. Applying takes a few seconds for SPU and '
                            'data patches, and a few minutes for patches to the main (PPU) code.'), top=4)
            self.note(f, tr('This port is built from version 1.00 of the European disc (BCES01741); the US disc '
                            '(BCUS98232) works too, the launcher converts it. RPCS3\'s patch '
                            'database only has the SPU MLAA patch for 1.00 so far; its other patches are for the '
                            '1.04 and 1.12 updates, and are being ported.'), top=6)
        self.patch_state = self.note(f, '', top=10, style='Value.TLabel')
        self.section(f, tr('Available patches'))
        self.patch_frame = ttk.Frame(f)
        self.put(f, self.patch_frame, sticky='we')
        self.patch_frame.columnconfigure(0, weight=1)
        bar = ttk.Frame(f)
        self.put(f, bar, sticky='we', pady=(14, 0))
        self.apply_button = ttk.Button(bar, text=tr('Apply'), style='Accent.TButton', command=self.apply_patches)
        self.update_button = ttk.Button(bar, text=tr('Update patch list'), command=self.update_patches)
        self.default_button = ttk.Button(bar, text=tr("Use the game's defaults"), command=self.reset_patches)
        if runtime:         # saved as they are ticked; the list comes from the game itself
            self.default_button.pack(side='left')
        else:
            self.apply_button.pack(side='left')
            self.update_button.pack(side='left', padx=(6, 0))
            ttk.Button(bar, text=tr('Open patch log'), command=lambda: self.open_path(rpcs3.LOG)).pack(side='left',
                                                                                                    padx=(6, 0))
        progress = ttk.Frame(f)
        self.put(f, progress, sticky='we', pady=(10, 0))
        self.patch_bar = ttk.Progressbar(progress, mode='indeterminate', length=self.px(160))
        self.patch_status = ttk.Label(progress, text='', style='Muted.TLabel', wraplength=self.px(560), justify='left')
        self.patch_status.pack(side='left')
        if not runtime:
            self.section(f, tr('Output'))
            self.patch_log = self.log_box(f, height=12)
            self.patch_tail.rewind()          # the last apply's output, if the launcher ran one
            self.log_append(self.patch_log, self.patch_tail.read(256 << 10))
        self.refresh_patch_rows()

    def load_patches(self, after=None):
        """The patches for this executable, from tools/rpcs3_patch.py (a second or two)."""
        self.patch_error = None
        if not self.patch_status.cget('text'):
            self.patch_status.configure(text=tr('Reading the patch list…'))

        runtime, exe = self.runtime_patches(), self.s['game_exe']

        def work():
            try:
                items, error = (rpcs3.exe_patches(exe) if runtime else rpcs3.list_patches()), None
            except (OSError, subprocess.SubprocessError) as e:
                items, error = None, str(e)
            self.root.after(0, self.patches_loaded, items, error, after)
        threading.Thread(target=work, daemon=True).start()

    def patches_loaded(self, items, error, after=None):
        self.patch_list, self.patch_error = items, error
        if not self.patch_proc and self.patch_status.cget('text') == tr('Reading the patch list…'):
            self.patch_status.configure(text='')
        self.refresh_patch_rows()
        if after:
            after()

    def selected_patches(self):
        if self.patch_list is None:
            return sorted(self.s.get('rpcs3_patches', []))
        if self.runtime_patches():
            return sorted(n for n, v in self.patch_vars.items() if v.get())
        known = {p['name'] for p in self.patch_list}
        chosen = [n for n, v in self.patch_vars.items() if v.get()]
        # names not offered for this executable stay as they were
        return sorted(chosen + [n for n in self.s.get('rpcs3_patches', []) if n not in known and n not in chosen])

    def refresh_patch_rows(self):
        tk, ttk, f = self.tk, self.ttk, self.patch_frame
        for w in f.winfo_children():
            w.destroy()
        runtime = self.runtime_patches()
        built, finished = rpcs3.built_state(self.s['game_exe'])
        chosen = set(self.s.get('rpcs3_patches', []))     # as saved, for rows not shown before
        if runtime and self.patch_list is not None:
            chosen = set(rpcs3.patches_on(self.s, self.patch_list))
        why = None if runtime else rpcs3.problem()
        if why or self.patch_error:
            ttk.Label(f, text=tr(why) if why else
                      tr('Could not read the patch list:\n{error}', error=self.patch_error),
                      style='Warning.TLabel', wraplength=self.px(700), justify='left').grid(row=0, column=0, sticky='w')
        elif self.patch_list is None:
            ttk.Label(f, text=tr('Reading the patch list…'), style='Muted.TLabel').grid(row=0, column=0, sticky='w')
        elif not self.patch_list:
            ttk.Label(f, text=tr('No patches for this executable in the patch files yet.'),
                      style='Muted.TLabel').grid(row=0, column=0, sticky='w')
        old = {n: v.get() for n, v in self.patch_vars.items()}
        self.patch_vars = {}
        for i, p in enumerate(self.patch_list or []):
            v = tk.BooleanVar(value=old.get(p['name'], p['name'] in chosen))
            v.trace_add('write', lambda *_: self.patch_toggled())
            self.patch_vars[p['name']] = v
            card = tk.Frame(f, bg=CARD, highlightthickness=1, highlightbackground=LINE, padx=14, pady=10)
            card.grid(row=i, column=0, columnspan=2, sticky='we', pady=4)
            card.columnconfigure(1, weight=1)
            W.Toggle(self, card, v, True, False, bg=CARD).grid(row=0, column=0, rowspan=3, sticky='n',
                                                                              padx=(0, 10), pady=(2, 0))
            head = tk.Frame(card, bg=CARD)
            head.grid(row=0, column=1, sticky='w')
            tk.Label(head, text=p['name'], bg=CARD, fg=TEXT, font=(self.ui, 10, 'bold')).pack(side='left')
            tk.Label(head, text=f" {p.get('kind', '')} ", bg='#3a1a1a', fg=ACCENT_HI,
                     font=(self.ui, 7, 'bold')).pack(side='left', padx=(10, 0))
            if runtime and p.get('default'):
                tk.Label(head, text=tr(' DEFAULT '), bg='#3a2a14', fg=GOLD,
                         font=(self.ui, 7, 'bold')).pack(side='left', padx=(6, 0))
            elif built is not None and p['name'] in built and not runtime:
                tk.Label(head, text=tr(' BUILT IN ') if finished else tr(' WRITTEN, NOT BUILT '), bg='#3a2a14', fg=GOLD,
                         font=(self.ui, 7, 'bold')).pack(side='left', padx=(6, 0))
            meta = ' · '.join(x for x in (tr('by {author}', author=p['author']) if p.get('author') else '',
                                           p.get('file', '')) if x)
            tk.Label(card, text=meta, bg=CARD, fg=MUTED, font=(self.ui, 9)).grid(row=1, column=1, sticky='w')
            if p.get('notes'):
                tk.Label(card, text=p['notes'], bg=CARD, fg=MUTED, font=(self.ui, 9), wraplength=self.px(640),
                         justify='left').grid(row=2, column=1, sticky='w', pady=(4, 0))
        self.refresh_patch_state()

    def patch_toggled(self):
        """Runtime patches: a tick is saved at once (the game reads the list as
        it starts) and from then on the ticks, not the game's defaults, are
        passed; otherwise it waits for Apply."""
        if (self.runtime_patches() and self.patch_list is not None
                and not getattr(self, 'patch_resetting', False)):
            names = self.selected_patches()
            if names != sorted(self.s.get('rpcs3_patches') or []) or self.s.get('patches_chosen') != '1':
                self.s['rpcs3_patches'] = names
                self.s['patches_chosen'] = '1'
                save_settings(self.s)
                if isinstance(getattr(self, 'saved', None), dict):
                    self.saved['rpcs3_patches'] = names
                self.refresh_status(tr('Patches saved: the game uses them the next time it starts.'))
        self.refresh_patch_state()

    def reset_patches(self):
        """Back to the game's own defaults (nothing passed at start)."""
        if self.patch_list is None:
            return
        defaults = [p['name'] for p in self.patch_list if p.get('default')]
        self.patch_resetting = True
        try:
            for name, v in self.patch_vars.items():
                v.set(name in defaults)
        finally:
            self.patch_resetting = False
        self.s['rpcs3_patches'] = sorted(defaults)
        self.s['patches_chosen'] = '0'
        save_settings(self.s)
        if isinstance(getattr(self, 'saved', None), dict):
            self.saved['rpcs3_patches'] = sorted(defaults)
        self.refresh_status(tr("Patches: back to the game's defaults."))
        self.refresh_patch_state()

    def refresh_patch_state(self):
        if not hasattr(self, 'patch_state'):
            return
        chosen = self.selected_patches()
        when = rpcs3.database_date()
        if self.runtime_patches():
            mine = self.s.get('patches_chosen') == '1'
            if self.patch_list is None:
                parts = [tr('Reading the patch list…')]
            else:
                parts = [tr('On the next time the game starts: {names}.', names=', '.join(chosen)) if chosen else
                         tr('No patch is on: the game starts unpatched.'),
                         tr('(your choice)') if mine else tr("(the game's defaults)")]
            self.patch_state.configure(text='  '.join(parts))
            busy = self.process is not None or self.preparing
            self.default_button.configure(state='disabled' if busy or not mine or self.patch_list is None
                                          else 'normal')
            return
        built, finished = rpcs3.built_state(self.s['game_exe'])
        parts = []
        if built is None:
            parts.append(tr('The game was built without patches.'))
        elif not built:
            parts.append(tr('The game is built without patches.') if finished else
                         tr('The last apply did not finish building.'))
        else:
            parts.append((tr('Built into the game: {names}.') if finished else
                          tr('Written but not built yet: {names}.')).format(names=', '.join(built)))
        pending = sorted(chosen) != sorted(built or []) or not finished
        if self.patch_proc:
            parts.append(tr('Applying…'))
        elif pending and self.patch_list is not None:
            parts.append(tr('Your choice differs: press Apply to rebuild the game with it.'))
        if when:
            parts.append(tr("RPCS3's patch file: downloaded {date}.", date=time.strftime('%Y-%m-%d', when)))
        self.patch_state.configure(text='  '.join(parts))
        busy = bool(self.patch_proc) or bool(rpcs3.problem()) or self.process is not None or self.preparing
        self.apply_button.configure(state='disabled' if busy or self.patch_list is None else 'normal')
        self.update_button.configure(state='disabled' if busy else 'normal')

    def patches_pending(self):
        if self.runtime_patches():
            return False
        if rpcs3.problem() or self.patch_list is None and not self.s.get('rpcs3_patches'):
            return False
        built, finished = rpcs3.built_state(self.s['game_exe'])
        return sorted(self.selected_patches()) != sorted(built or []) or not finished

    def run_patch_script(self, args, title, done):
        if self.patch_proc:
            return
        if self.process or self.preparing or game_running(play_exe(self.s)):
            self.messagebox.showwarning(APP_NAME, tr('Close the game first: it is rebuilt while patches are applied.'))
            return
        try:
            self.patch_proc, self.patch_job = rpcs3.start_script(*args)
        except OSError as e:
            self.messagebox.showerror(APP_NAME, tr('Could not start apply-patches.ps1:\n{error}', error=e))
            return
        self.patch_started = time.monotonic()
        self.patch_title, self.patch_done, self.patch_line = title, done, ''
        self.log_set(self.patch_log, '')
        self.patch_tail.rewind()
        self.patch_bar.pack(side='left', padx=(0, 14), before=self.patch_status)
        self.patch_bar.start(12)
        self.play_button.configure(state='disabled')
        self.refresh_patch_state()
        self.refresh_status(title)
        self.root.after(500, self.watch_patch_script)

    def watch_patch_script(self):
        text = self.patch_tail.read()
        if text:
            self.log_append(self.patch_log, text)
            last = [ln.strip() for ln in text.splitlines() if ln.strip()]
            if last:
                self.patch_line = last[-1][:110]
        elapsed = int(time.monotonic() - self.patch_started)
        self.patch_status.configure(text=f'{self.patch_title}  {elapsed // 60}:{elapsed % 60:02d}\n'
                                         f'{getattr(self, "patch_line", "")}')
        code = self.patch_proc.poll()
        if code is None:
            self.root.after(500, self.watch_patch_script)
            return
        self.log_append(self.patch_log, self.patch_tail.read())
        try:
            self.patch_job.close()
        except (OSError, AttributeError):
            pass
        self.patch_proc = self.patch_job = None
        self.patch_bar.stop()
        self.patch_bar.pack_forget()
        self.play_button.configure(state='normal')
        done, self.patch_done = self.patch_done, None
        done(code)

    def update_patches(self):
        def done(code):
            if code == 0:
                self.patch_status.configure(text=tr("RPCS3's patch file is up to date."))
                self.patch_list = None
                self.load_patches()
            else:
                self.patch_status.configure(text=tr('The update failed (code {code}); see the output.', code=code))
                self.refresh_patch_state()
            self.refresh_status()
        self.run_patch_script(['-Update', '-List'], tr("Downloading RPCS3's patch file…"), done)

    def apply_patches(self):
        if self.patch_list is None:
            return
        names = self.selected_patches()
        listed = '\n'.join('  • ' + n for n in names) or '  ' + tr('(none: the unpatched game)')
        if not self.messagebox.askyesno(APP_NAME, tr('Rebuild the game with these patches?\n\n{names}\n\nThis takes '
                                                     'from a few seconds to a few minutes; PLAY waits until it is '
                                                     'done.', names=listed)):
            return
        self.save()        # apply-patches.ps1 -FromLauncher reads rpcs3_patches from settings.json

        def done(code):
            self.refresh_patch_rows()
            if code == 0:
                self.patch_status.configure(text=tr('Done: the game was rebuilt with the chosen patches.'))
                self.refresh_status(tr('Patches applied; the game was rebuilt.'))
                self.switches = runtime_switches(self.s['game_exe'])
            else:
                self.patch_status.configure(text=tr('Applying failed (code {code}); see the output.', code=code))
                self.refresh_status(tr('Applying the patches failed; see Patches → Output.'))
                self.messagebox.showerror(APP_NAME, tr('Applying the patches failed (code {code}).\nThe output is on the '
                                                       'Patches page and in build\\logs\\apply-patches.log.', code=code))
        self.run_patch_script(['-FromLauncher'], tr('Applying patches and rebuilding the game…'), done)

    # ---- mods -----------------------------------------------------------------------------------
    def build_mods(self):
        tk, ttk = self.tk, self.ttk
        f = self.scrolled_page('mods', tr('Mods'), tr('Change the game\'s files without touching your disc.'))
        self.section(f, tr('Load order'))
        b = self.body(f)
        head = ttk.Frame(b)
        self.put(f, head, sticky='we', pady=(0, 8))
        self.mods_count = ttk.Label(head, text='', style='Muted.TLabel')
        self.mods_count.pack(side='left')
        ttk.Button(head, text=tr('Open folder'),
                   command=lambda: self.open_path(Path(self.var('mods_dir').get()), make=True)).pack(side='right')
        ttk.Button(head, text=tr('Refresh'), style='Link.TButton', command=self.refresh_mods).pack(side='right',
                                                                                                    padx=(0, 10))
        ttk.Label(head, text=tr('First in the list wins'), style='Muted.TLabel').pack(side='right', padx=(0, 10))
        body = ttk.Frame(b)
        self.put(f, body, sticky='we')
        body.columnconfigure(0, weight=1)
        self.mods_frame = tk.Frame(body, bg=CARD)
        self.mods_frame.grid(row=0, column=0, sticky='new')
        self.mod_details = tk.Frame(body, bg=CARD, padx=16, pady=16, width=self.px(290))
        self.mod_details.grid(row=0, column=1, sticky='n', padx=(self.px(14), 0))
        self.mod_details.grid_propagate(True)
        self.section(f, tr('Brazilian Portuguese'), top=28)
        self.note(f, tr('The European disc already has the Brazilian Portuguese text; the voices come from '
                        'the US disc. Choose the image (.iso) of your US disc, or its extracted folder, and '
                        'the launcher builds the PT-BR mod in your mods folder. With it on, Português '
                        '(Brasil) shows up under Language.'), top=4)
        self.buttons(f, ((tr('Create PT-BR mod from the US disc (.iso)…'), lambda: self.make_ptbr(folder=False)),
                         (tr('…or from the extracted folder'), lambda: self.make_ptbr(folder=True))))
        self.section(f, tr('Update 1.04 sound mix'), top=28)
        self.check(f, 'sound_mix_104', tr('1.04 sound mix'),
                   tr('The sound mix of the official update 1.04 (its "music too quiet" and Hecaton fixes: 17 mix '
                      'levels, 15 of them louder). The port runs the 1.00 game, which cannot load updates; this '
                      "one file fits it. It is taken from your own 1.04 update package and put into the launcher's "
                      'copy of GAME.PSARC; nothing is downloaded. It does not change crackling.'))
        self.path_row(f, 'update_pkg', tr('Update 1.04 package'),
                      hint=tr('The .pkg of the official update 1.04, US or European (its name has A0104). '
                              'Later updates do not fit the port.'))
        self.update_pkg_note = self.note(f, '', top=4)
        self.var('update_pkg').trace_add('write', lambda *_: self.check_update_pkg())
        self.check_update_pkg()
        self.section(f, tr('Folders'), top=28)
        self.path_row(f, 'mods_dir', tr('Mods folder'), folder=True)
        self.note(f, tr("Each mod is a subfolder that mirrors the game's USRDIR (a mod that replaces "
                        'GAME.PSARC carries <mod>/GAME.PSARC), or a mod.json with a recipe the launcher '
                        "applies to your disc's files when playing. The included mods come with Ascension "
                        'and carry nothing from the game.'))
        self.var('mods_dir').trace_add('write', lambda *_: self.refresh_mods())
        self.refresh_mods()

    def check_update_pkg(self):
        """What the chosen update package gives, under its row (read in a thread: the first look
        decrypts a little of the package)."""
        path = self.var('update_pkg').get()
        self.update_pkg_note.configure(text=tr('Checking the package…') if path.strip() else
                                       tr('No update package chosen.'))

        def work():
            from .upd104 import describe
            text = describe(path)
            self.root.after(0, lambda: self.var('update_pkg').get() == path and
                            self.update_pkg_note.winfo_exists() and self.update_pkg_note.configure(text=text))
        if path.strip():
            threading.Thread(target=work, daemon=True).start()

    def current_mod_settings(self):
        enabled = [m for m, v in self.mod_vars.items() if v.get()] if self.mod_vars else list(self.s['mods_enabled'])
        return {**self.s, 'mods_dir': self.var('mods_dir').get(), 'mods_enabled': enabled,
                'mods_order': list(getattr(self, 'mod_order', self.s['mods_order']))}

    # ---- trophies (host/trophies, 2026-11-22) ----------------------------------------------------------------------
    def build_trophies(self):
        """Trophies page: the game's trophy list from the player's own game files, which ones the game unlocked
        (gamedata/trophies, written by the game), the PS3's progress figure."""
        f = self.scrolled_page('trophies', tr('Trophies'), tr('The trophies you earned in the game.'))
        self.section(f, '', top=4)
        self.trophy_head = self.tk.Frame(self.body(f), bg=PANEL)
        self.put(f, self.trophy_head, sticky='we')
        self.section(f, tr('All trophies'))
        self.trophy_list = self.tk.Frame(self.body(f), bg=PANEL)
        self.put(f, self.trophy_list, sticky='we')
        self.trophy_images = []

    def trophy_image(self, path, side, grey=False):
        """A trophy icon side x side px (Pillow when present: smooth and, for locked ones, grey), else Tk's PNG."""
        if not path:
            return None
        try:
            from PIL import Image, ImageOps, ImageTk
            with Image.open(path) as im:
                im = im.convert('RGBA').resize((side, side), Image.LANCZOS)
                if grey:
                    a = im.getchannel('A')
                    im = ImageOps.grayscale(im).convert('RGBA')
                    im.putalpha(a.point(lambda v: v * 6 // 10))
                return ImageTk.PhotoImage(im)
        except (OSError, ImportError, ValueError):
            pass
        try:
            img = self.tk.PhotoImage(file=str(path))
            step = max(1, round(img.width() / side))
            return img.subsample(step, step) if step > 1 else img
        except Exception:   # noqa: BLE001 (a broken file: no icon)
            return None

    def refresh_trophies(self):
        if not hasattr(self, 'trophy_list'):
            return
        import time as _time
        from . import trophies as T
        tk, px = self.tk, self.px
        for box in (self.trophy_head, self.trophy_list):
            for w in box.winfo_children():
                w.destroy()
        self.trophy_images = []
        try:
            data = T.load(current(), self.s)
        except Exception:   # noqa: BLE001 (unreadable files: the page says so)
            data = None
        if not data:
            tk.Label(self.trophy_head, text=tr('No trophy list yet: it is read from your game files (the disc\'s '
                                               'PS3_GAME\\TROPDIR folder). Prepare the game on the Home page.'),
                     bg=PANEL, fg=MUTED, font=(self.ui, 10), wraplength=px(640), justify='left').pack(anchor='w')
            return
        head = self.trophy_head
        icon = self.trophy_image(data['icon'], px(64)) if data['icon'] else None
        if icon:
            self.trophy_images.append(icon)
            tk.Label(head, image=icon, bg=PANEL).pack(side='left', padx=(0, px(16)))
        text = tk.Frame(head, bg=PANEL)
        text.pack(side='left', fill='x', expand=True)
        tk.Label(text, text=tr('{n} of {total} trophies unlocked', n=data['unlocked'], total=data['total']),
                 bg=PANEL, fg=TEXT, font=(self.semi, 12), anchor='w').pack(anchor='w')
        bar = tk.Canvas(text, height=px(8), bg=PANEL, bd=0, highlightthickness=0, width=px(360))
        bar.pack(anchor='w', pady=(px(6), px(4)))
        w = px(360)
        bar.create_rectangle(0, 0, w, px(8), fill=LINE, outline='')
        bar.create_rectangle(0, 0, max(0, round(w * data['percent'] / 100)), px(8), fill=GOLD, outline='')
        c = data['counts']
        tk.Label(text, text=tr('{pct}% complete', pct=data['percent']) + '   ·   ' + '   '.join(
            f"{tr(T.GRADES[g])} {c[g][0]}/{c[g][1]}" for g in 'PGSB'),
            bg=PANEL, fg=MUTED, font=(self.ui, 9), anchor='w').pack(anchor='w')
        colours = {'P': '#b9d3ee', 'G': '#e8c25a', 'S': '#c9ced6', 'B': '#c8834e'}
        side = px(56)
        for i, r in enumerate(T.ordered(data['trophies'])):
            if i:
                tk.Frame(self.trophy_list, bg=LINE, height=1).pack(fill='x')
            line = tk.Frame(self.trophy_list, bg=PANEL, pady=px(7))
            line.pack(fill='x')
            secret = r['hidden'] and not r['unlocked']
            img = None if secret else self.trophy_image(r['icon'], side, grey=not r['unlocked'])
            if img:
                self.trophy_images.append(img)
                tk.Label(line, image=img, bg=PANEL, width=side, height=side).pack(side='left', padx=(0, px(14)))
            else:
                blank = tk.PhotoImage(width=side, height=side)   # the same size as an icon
                self.trophy_images.append(blank)
                tk.Label(line, image=blank, text='?', compound='center', bg='#1c1c20', fg=MUTED,
                         font=(self.semi, 18), bd=0).pack(side='left', padx=(0, px(14)))
            mid = tk.Frame(line, bg=PANEL)
            mid.pack(side='left', fill='x', expand=True)
            name = tr('Hidden trophy') if secret else r['name']
            detail = tr('Keep playing to find out.') if secret else r['detail']
            tk.Label(mid, text=name, bg=PANEL, fg=TEXT if r['unlocked'] else '#b8b0a8', font=(self.semi, 11),
                     anchor='w').pack(anchor='w')
            tk.Label(mid, text=detail, bg=PANEL, fg=MUTED, font=(self.ui, 9), anchor='w', justify='left',
                     wraplength=px(470)).pack(anchor='w')
            end = tk.Frame(line, bg=PANEL)
            end.pack(side='right', padx=(px(10), 0))
            tk.Label(end, text='\u25cf ' + tr(T.GRADES[r['grade']]), bg=PANEL, fg=colours[r['grade']],
                     font=(self.semi, 9), anchor='e').pack(anchor='e')
            when = (tr('Unlocked {date}', date=_time.strftime('%Y-%m-%d %H:%M', _time.localtime(r['time'])))
                    if r['unlocked'] and r['time'] else tr('Unlocked') if r['unlocked'] else tr('Locked'))
            tk.Label(end, text=when, bg=PANEL, fg='#62cf7a' if r['unlocked'] else MUTED, font=(self.ui, 9),
                     anchor='e').pack(anchor='e')
        self.root.after_idle(lambda: self.root.event_generate('<Configure>'))

    def refresh_mods(self):
        if not hasattr(self, 'mods_frame'):
            return
        tk, ttk = self.tk, self.ttk
        for w in self.mods_frame.winfo_children():
            w.destroy()
        s = self.current_mod_settings()
        mods = all_mods({**s, 'mods_order': self.mod_order})
        self.mods = {m['id']: m for m in mods}
        self.mod_order = [m['id'] for m in mods]
        enabled = set(s['mods_enabled'])
        if self.selected_mod not in self.mods:
            self.selected_mod = mods[0]['id'] if mods else None
        if not mods:
            tk.Label(self.mods_frame, text=tr('No mods yet. Put a mod folder in the mods folder, then Refresh.'),
                     bg=CARD, fg=MUTED, font=(self.ui, 10), pady=24, padx=16).pack(fill='x')
        for i, m in enumerate(mods):
            if i:
                tk.Frame(self.mods_frame, bg=LINE, height=1).pack(fill='x')
            picked = m['id'] == self.selected_mod
            bg = LINE if picked else CARD
            line = tk.Frame(self.mods_frame, bg=bg, padx=12, pady=6)
            line.pack(fill='x')
            v = self.mod_vars.get(m['id']) or tk.BooleanVar(value=m['id'] in enabled)
            self.mod_vars[m['id']] = v
            W.Toggle(self, line, v, True, False, command=self.mods_changed, bg=bg).pack(side='left')
            tk.Label(line, text=str(i + 1), bg=bg, fg=MUTED, font=(self.ui, 9), width=2, anchor='e').pack(
                side='left', padx=(4, 10))
            name = tk.Label(line, text=m['name'], bg=bg, fg=ACCENT_HI if picked else TEXT, anchor='w', cursor='hand2',
                            font=(self.ui, 10, 'bold') if picked else (self.ui, 10))
            name.pack(side='left')
            if m['builtin']:
                tk.Label(line, text=tr(' INCLUDED '), bg='#3a2a14', fg=GOLD, font=(self.ui, 7, 'bold')).pack(
                    side='left', padx=(10, 0))
            filler = tk.Frame(line, bg=bg)
            filler.pack(side='left', fill='both', expand=True)
            for widget in (line, name, filler):
                widget.bind('<Button-1>', lambda _e, mid=m['id']: self.select_mod(mid))
            ttk.Button(line, text='▼', width=2, style='Small.TButton',
                       command=lambda mid=m['id']: self.move_mod(mid, 1)).pack(side='right')
            ttk.Button(line, text='▲', width=2, style='Small.TButton',
                       command=lambda mid=m['id']: self.move_mod(mid, -1)).pack(side='right', padx=(0, 4))
        self.show_mod_details()
        self.mods_changed()

    def select_mod(self, mid):
        self.selected_mod = mid
        self.refresh_mods()

    def show_mod_details(self):
        tk = self.tk
        box = self.mod_details
        for w in box.winfo_children():
            w.destroy()
        m = self.mods.get(self.selected_mod) if self.selected_mod else None
        if not m:
            box.grid_remove()
            return
        box.grid()
        tk.Label(box, text=m['name'], bg=CARD, fg=TEXT, font=(self.heading, 13), wraplength=self.px(260),
                 justify='left').pack(anchor='w')
        tk.Label(box, text=tr('Comes with this port') if m['builtin'] else str(m['path']), bg=CARD, fg=MUTED,
                 font=(self.ui, 9), wraplength=self.px(260), justify='left').pack(anchor='w', pady=(4, 0))
        tk.Label(box, text=m['description'] or tr('No description.'), bg=CARD, fg=TEXT, font=(self.ui, 10),
                 wraplength=self.px(260), justify='left').pack(anchor='w', pady=(12, 0))
        archives = list((m['meta'].get('psarc_patches') or {}).keys())
        if archives:
            tk.Label(box, text=tr('Changes, inside the archive: {names}', names=', '.join(archives)), bg=CARD, fg=MUTED,
                     font=(self.ui, 9), wraplength=self.px(260), justify='left').pack(anchor='w', pady=(10, 0))
        self.ttk.Button(box, text=tr('Open mod folder'), command=lambda: self.open_path(m['path'])).pack(
            anchor='w', pady=(14, 0))

    def languages(self):
        return language_options(self.current_mod_settings())

    def mods_changed(self):
        on = sum(1 for v in self.mod_vars.values() if v.get())
        if hasattr(self, 'mods_count'):
            self.mods_count.configure(text=tr('{on} of {total} active', on=on, total=len(self.mod_vars)))
        self.refresh_choices('language', language_fallback(self.current_mod_settings()))

    def make_ptbr(self, folder):
        if folder:
            src = self.filedialog.askdirectory(title=tr('US disc folder (with PS3_GAME)'))
        else:
            src = self.filedialog.askopenfilename(title=tr('US disc image'),
                                                  filetypes=[(tr('Disc image'), '*.iso'), (tr('All'), '*.*')])
        if not src:
            return
        out = Path(self.var('mods_dir').get()) / 'PT-BR'
        s = {**self.s, **self.gather()}
        self.refresh_status(tr('Creating the PT-BR mod… (copies ~140 MB)'))

        def work():
            import importlib.util
            from .game import eu_usrdir
            try:
                tool = ROOT / 'tools' / 'make_ptbr_mod.py'
                if not tool.is_file():
                    raise FileNotFoundError(f'{tool} is missing: update the port to the latest version.')
                eu = eu_usrdir(s)      # the European disc's USRDIR (GAME.PSARC, LOCALIZED_ARCHIVES)
                spec = importlib.util.spec_from_file_location('make_ptbr_mod', tool)
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                us, unmount = Path(src), None
                if us.suffix.lower() == '.iso':
                    us, unmount = mod.mount_iso(us)
                elif (us / 'PS3_GAME' / 'USRDIR').is_dir():
                    us = us / 'PS3_GAME' / 'USRDIR'
                try:
                    mod.build(us, eu, out, log=lambda m: None)
                finally:
                    if unmount:
                        unmount()
                self.root.after(0, self.ptbr_done, None)
            except BaseException as e:   # SystemExit carries the reason
                self.root.after(0, self.ptbr_done, str(e) or type(e).__name__)
        threading.Thread(target=work, daemon=True).start()

    def ptbr_done(self, error):
        if error:
            self.refresh_status(tr('The PT-BR mod was not created.'))
            self.messagebox.showerror(APP_NAME, tr('Could not create the PT-BR mod:\n{error}', error=error))
            return
        if 'PT-BR' not in self.mod_order:
            self.mod_order.insert(0, 'PT-BR')
        self.mod_vars['PT-BR'] = self.tk.BooleanVar(value=True)
        self.refresh_mods()
        self.var('language').set('17')
        self.refresh_choices('language', language_fallback(self.current_mod_settings()))
        self.refresh_status(tr('PT-BR mod created and turned on; language: Português (Brasil). Click Save.'))

    def move_mod(self, name, step):
        i = self.mod_order.index(name)
        j = i + step
        if 0 <= j < len(self.mod_order):
            self.mod_order[i], self.mod_order[j] = self.mod_order[j], self.mod_order[i]
            self.refresh_mods()

    # ---- advanced -------------------------------------------------------------------------------
    def build_online(self):
        f = self.scrolled_page('online', tr('Online'),
                               tr('Community multiplayer: Quick Match, the Game Mode list and private matches with friends.'))
        self.section(f, tr('Online multiplayer'), top=4)
        self.check(f, 'online', tr('Play online (community server)'),
                   tr('Multi-Player then finds other players through the community server below. Your own Online '
                      'Pass from your PS3 is needed, as on the PS3: the game checks it itself. Only visual patches '
                      '(Unlock FPS, motion blur, depth of field, MLAA) may be on; any other patch switches online off. '
                      'Mods are left out while online so everyone loads the same game data, except Skip intro and the 1.04 sound '
                      'mix, which do not change it.'))
        self.row(f, tr('Server'), self.ttk.Entry(f, textvariable=self.var('online_server'), width=30),
                 tr("The community server's address (for example 192.168.1.20 at home, or a name). Ask the server's "
                    'host. It never contacts Sony.'))
        self.server_pick = self.tk.StringVar()
        self.server_box = self.ttk.Combobox(f, textvariable=self.server_pick, state='readonly', width=46)
        self.server_box.bind('<<ComboboxSelected>>', lambda e: self.online_pick_server())
        self.server_box.bind('<MouseWheel>', lambda e: 'break')
        self.server_choices = getattr(self, 'server_choices', {})
        # The picker follows a typed or pasted Server box (2026-11-15: it kept showing an old "Other:" address).
        self.var('online_server').trace_add('write', lambda *_: self.online_sync_pick())
        self.row(f, tr('Pick a server'), self.server_box,
                 tr('The community servers with your ping to each, nearest first, and the LAN servers on your own '
                    'network. Everyone you play with must use the same server. Refresh measures again.'))
        self.buttons(f, ((tr('Refresh'), self.online_load_servers),), top=4)
        self.section(f, tr('LAN play'))
        self.note(f, tr('Play with people on your own network (same router), no internet server needed: one player '
                        'presses Host a LAN server, the others press Refresh and pick it above. Allow Python in the '
                        'Windows Firewall when it asks (private networks). The server stops when you press Stop or '
                        'close the launcher. Everyone still needs their own Online Pass.'), top=4)
        self.buttons(f, ((tr('Host a LAN server'), self.online_host_lan), (tr('Stop LAN server'), self.online_stop_lan)),
                     top=6)
        self.lan_status = self.note(f, '', top=6)
        self.server_choices = {}
        self.root.after(300, self.online_load_servers)
        self.row(f, tr('Player name'), self.ttk.Entry(f, textvariable=self.var('online_name'), width=20),
                 tr('Up to 16 letters or digits. The first time you sign in, the server keeps this name for this PC '
                    '(a key file next to your saves proves it is you).'))
        self.row(f, tr('Network port'), self.ttk.Entry(f, textvariable=self.var('online_port'), width=8),
                 tr("UDP port for the game's own player-to-player traffic (3658). Allow it in your firewall on "
                    'private networks.'))
        self.path_row(f, 'online_exe', tr('Online executable'),
                      hint=tr('Empty: AscensionRecomp.online.exe next to the game, if it is there (the online test package has it).'))
        self.check(f, 'online_discord', tr('Show on Discord'),
                   tr('Your Discord profile shows what you play (lobby, searching, the mode of the match), through '
                      'the Discord app on this PC. With a match code, friends get a Join button that fills in '
                      'your server and code for them. No Discord sign-in, nothing is sent to Discord while it is off.'))
        self.section(f, tr('Private match'))
        self.row(f, tr('Match code'), self.ttk.Entry(f, textvariable=self.var('online_party'), width=14),
                 tr('Host: press New code, then Copy code and send it to your friends: it carries your server too '
                    '(CODE@server), so pasting it here puts them on your server. Everyone with the same code is matched '
                    'only with each other (Quick Match or a Game Mode pick). Empty: public matches.'))
        self.buttons(f, ((tr('New code'), self.online_new_code), (tr('Copy code'), self.online_copy_code),
                         (tr('Clear'), lambda: self.var('online_party').set(''))), top=6)
        self.online_status = self.note(f, '', top=6)
        joined = take_discord_join(self.s)
        if joined:
            self.var('online_server').set(joined[0])
            self.var('online_party').set(joined[1])
            self.var('online').set('1')
            self.online_status.configure(text=tr('Discord invite: server {server}, code {code}. Press Play.',
                                                 server=joined[0], code=joined[1]))
        self.section(f, tr('Leaderboards'))
        self.note(f, tr("The best players on the server above, from the stats each game saves after a match (XP, "
                        "favor, kills, wins, matches, trial waves). Reported by players' games (not verified)."), top=4)
        self.buttons(f, ((tr('Show leaderboards'), self.online_show_leaderboards),
                         (tr('Open in browser'), self.online_open_leaderboards)), top=6)
        b = self.body(f)
        self.online_boards = self.ttk.Frame(b)
        self.online_boards.grid(row=b.grid_size()[1], column=0, columnspan=2, sticky='w')
        self.section(f, tr('Community servers'))
        self.note(f, tr('Servers other players run. Show servers asks the server above for its list; Use puts an '
                        'address above.'), top=4)
        self.buttons(f, ((tr('Show servers'), self.online_list_servers),), top=6)
        b = self.body(f)
        self.online_servers = self.ttk.Frame(b)
        self.online_servers.grid(row=b.grid_size()[1], column=0, columnspan=2, sticky='w')

    @staticmethod
    def server_host(address):
        """An address as the Server box keeps it: the default HTTP port 36101 left out."""
        a = (address or '').strip()
        return a[:-len(':36101')] if a.endswith(':36101') else a

    def online_load_servers(self):
        """Fill the server picker (2026-11-13): the known servers plus each one's own list, with a ping to each
        (the time of one small HTTP request, best of two), nearest first. Runs in a thread; only these servers
        are contacted."""
        import json
        import threading
        import time
        import urllib.request
        from .config import KNOWN_SERVERS
        if not getattr(self, 'server_box', None):
            return
        self.server_box.configure(values=[tr('Measuring…')])
        self.server_pick.set(tr('Measuring…'))

        def get(host, path):
            with urllib.request.urlopen('http://%s:36101%s' % (host, path), timeout=3) as r:
                return r.read(65536)

        def work():
            found = {}
            try:   # LAN play (2026-11-15): servers on this network answer a broadcast
                for sv in lan_discover(1.0):
                    found.setdefault(sv['address'], (sv['name'], 'LAN'))
            except Exception:
                pass
            for name, addr, region in KNOWN_SERVERS:
                found.setdefault(self.server_host(addr), (name, region))
            for name, addr, region in KNOWN_SERVERS:
                try:
                    for sv in json.loads(get(self.server_host(addr), '/api/servers'))['servers'][:30]:
                        h = self.server_host(str(sv.get('address', '')))
                        if h and h not in found:
                            found[h] = (str(sv.get('name', h))[:40], str(sv.get('region', ''))[:24])
                except Exception:      # an unreachable server only loses its list
                    pass
            rows = []
            for h, (name, region) in found.items():
                best, st = None, {}
                for _ in range(2):
                    t0 = time.perf_counter()
                    try:
                        st = json.loads(get(h, '/api/status')) or st
                        ms = (time.perf_counter() - t0) * 500   # one request = connect + answer = 2 round trips
                        best = ms if best is None else min(best, ms)
                    except Exception:
                        pass
                rows.append((best if best is not None else 1e9, h, name, region,
                             int(st.get('players_online') or 0), int(st.get('searching') or 0)))
            rows.sort()
            self.root.after(0, lambda: self.online_fill_servers(rows))
        threading.Thread(target=work, daemon=True).start()

    AUTO_LABEL = None

    def online_fill_servers(self, rows):
        """Server browser (2026-11-15): ping, players online and searching per server; first choice Auto = the server
        where people are searching right now (then where people are online), else the nearest."""
        self.server_choices = {}
        labels = []
        live = [r for r in rows if r[0] < 1e9]
        if live:
            pick = sorted(live, key=lambda r: (-(r[5] > 0), -(r[4] > 0), r[0]))[0]
            why = (tr('players searching') if pick[5] else tr('players online') if pick[4] else tr('nearest'))
            auto = tr('Auto: {name} ({why})', name=pick[2], why=why)
            self.server_choices[auto] = pick[1]
            labels.append(auto)
        for ms, h, name, region, online, searching in rows:
            ping = tr('offline') if ms >= 1e9 else '%d ms' % ms
            who = tr('{n} online', n=online) + (tr(', {n} searching', n=searching) if searching else '')
            label = '%s  ·  %s  ·  %s  ·  %s' % (name, region or '?', ping, who)
            self.server_choices[label] = h
            labels.append(label)
        self.server_box.configure(values=labels)
        self.online_sync_pick()

    def online_sync_pick(self):
        """Shows in 'Pick a server' the server the Server box names (a listed one, else "Other: <address>")."""
        if not hasattr(self, 'server_pick'):
            return
        current = self.server_host(self.var('online_server').get())
        if self.server_choices.get(self.server_pick.get()) == current and current:
            return
        self.server_pick.set(next((l for l, h in getattr(self, 'server_choices', {}).items() if h == current),
                                  tr('Other: {address}', address=current) if current else ''))

    def online_host_lan(self):
        name = (self.var('online_name').get() or '').strip()
        addr, err = start_lan_server((name + "'s LAN") if name else 'LAN server')
        if err:
            self.lan_status.configure(text=tr('Could not start the LAN server: {error}', error=err))
            return
        self.var('online_server').set(addr)
        self.var('online').set('1')
        self.lan_status.configure(text=tr('LAN server running on {address}. Others on your network: Refresh, pick it, '
                                          'Play.', address=addr))
        self.online_load_servers()

    def online_stop_lan(self):
        was = lan_server_running()
        stop_lan_server()
        self.lan_status.configure(text=tr('LAN server stopped.') if was else tr('No LAN server is running.'))

    def online_pick_server(self):
        h = self.server_choices.get(self.server_pick.get())
        if h:
            self.var('online_server').set(h)

    def online_url(self, path):
        server = (self.var('online_server').get() or '').strip()
        if not server:
            raise ValueError(tr('Enter the server address first.'))
        host = server if ':' in server else server + ':36101'
        return 'http://' + host + path

    def online_fetch(self, path, done):
        """GET a small answer from the community server in a thread; done(text or None, error) on the UI thread."""
        import threading
        import urllib.request
        try:
            url = self.online_url(path)
        except ValueError as e:
            done(None, str(e))
            return

        def work():
            try:
                with urllib.request.urlopen(url, timeout=4) as r:
                    text, err = r.read(65536).decode('utf-8', 'replace'), None
            except Exception as e:     # any network error ends up in the status line
                text, err = None, str(e)
            self.root.after(0, lambda: done(text, err))
        threading.Thread(target=work, daemon=True).start()

    def online_leaderboard_page(self):
        """The server's public leaderboard page: HTTPS for the known servers (their sslip.io certificate), else HTTP."""
        from .config import KNOWN_SERVERS
        server = self.server_host(self.var('online_server').get())
        if not server:
            raise ValueError(tr('Enter the server address first.'))
        host = server.split(':')[0]
        if host in {a for _n, a, _r in KNOWN_SERVERS}:
            return 'https://%s.sslip.io/leaderboard' % host.replace('.', '-')
        return self.online_url('/leaderboard')

    def online_open_leaderboards(self):
        try:
            webbrowser.open(self.online_leaderboard_page())
        except ValueError as e:
            self.online_status.configure(text=str(e))

    def online_show_leaderboards(self):
        """Top 5 of each board from the server above (GET /api/leaderboard, 2026-11-14), shown on this page."""
        import json

        def done(text, err):
            for w in self.online_boards.winfo_children():
                w.destroy()
            try:
                d = json.loads(text) if text else None
                boards = d['boards'] if d else None
            except (ValueError, KeyError, TypeError):
                boards = None
            if boards is None:
                self.ttk.Label(self.online_boards, text=tr('Could not read the leaderboards: {error}', error=err or '?'),
                               style='Muted.TLabel').grid(row=0, column=0, sticky='w')
                return
            row = 0
            for i, bd in enumerate(boards[:8]):
                col = (i % 2) * 2
                if i % 2 == 0 and i:
                    row += 7
                self.ttk.Label(self.online_boards, text=tr(str(bd.get('title', '?'))),
                               style='Value.TLabel').grid(
                    row=row, column=col, sticky='w', pady=(8, 0), padx=(0, 6))
                rows = bd.get('rows') or []
                if not rows:
                    self.ttk.Label(self.online_boards, text=tr('No reports yet.'), style='Muted.TLabel').grid(
                        row=row + 1, column=col, sticky='w', padx=(0, 30))
                for j, r in enumerate(rows[:5]):
                    self.ttk.Label(self.online_boards, text='%s. %s  -  %s' % (
                        r.get('rank', '?'), str(r.get('name', '?'))[:16], '{:,}'.format(int(r.get('value', 0))))).grid(
                        row=row + 1 + j, column=col, sticky='w', padx=(0, 30))
            self.ttk.Label(self.online_boards, text=tr("Reported by players' games (not verified). This server only."),
                           style='Muted.TLabel').grid(row=row + 7, column=0, columnspan=4, sticky='w', pady=(8, 0))
        self.online_fetch('/api/leaderboard?n=5', done)

    def online_new_code(self):
        def done(text, err):
            code = (text or '').strip()
            if code.isalnum():
                self.var('online_party').set(code)
                self.online_status.configure(text=tr('New code {code}: send it to your friends.', code=code))
            else:
                self.online_status.configure(text=tr('Could not get a code: {error}', error=err or code))
        self.online_fetch('/api/party/new', done)

    def online_copy_code(self):
        from .game import split_invite
        code, host = split_invite(self.var('online_party').get())
        if code:
            host = host or self.server_host(self.var('online_server').get())
            text = code + ('@' + host if host else '')
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self.online_status.configure(text=tr('Copied {text}: your friends paste it into Match code and land on your '
                                                 'server with your code.', text=text))

    def online_list_servers(self):
        import json

        def done(text, err):
            for w in self.online_servers.winfo_children():
                w.destroy()
            try:
                servers = json.loads(text)['servers'] if text else None
            except (ValueError, KeyError, TypeError):
                servers = None
            if servers is None:
                self.ttk.Label(self.online_servers, text=tr('Could not read the list: {error}', error=err or '?'),
                               style='Muted.TLabel').grid(row=0, column=0, sticky='w')
                return
            if not servers:
                self.ttk.Label(self.online_servers, text=tr('No community servers listed yet.'),
                               style='Muted.TLabel').grid(row=0, column=0, sticky='w')
            for i, sv in enumerate(servers[:30]):
                text = '%s  -  %s  %s' % (sv.get('name', '?'), sv.get('address', '?'),
                                          ('(' + sv['region'] + ')') if sv.get('region') else '')
                self.ttk.Label(self.online_servers, text=text).grid(row=i, column=0, sticky='w', pady=(4, 0))
                self.ttk.Button(self.online_servers, text=tr('Use'),
                                command=lambda a=sv.get('address', ''): self.var('online_server').set(a)
                                ).grid(row=i, column=1, sticky='w', padx=(12, 0), pady=(4, 0))
        self.online_fetch('/api/servers', done)

    def build_advanced(self):
        f = self.scrolled_page('advanced', tr('Advanced'), tr('Game files, the launcher, runtime variables and the log.'))
        self.section(f, tr('Game files'), top=4)
        if WINDOWS:
            self.buttons(f, [(tr('Choose game folder…'), lambda: self.choose_game_files('folder')),
                             (tr('Choose disc image (.iso)…'), lambda: self.choose_game_files('iso')),
                             (tr('Choose decrypted EBOOT.elf…'), lambda: self.choose_game_files('eboot'))], top=4)
            self.note(f, tr('Your game folder or .iso is used where it is and never changed. The launcher checks '
                            'it is BCES01741 (Europe) or BCUS98232 (USA), decrypts its EBOOT.BIN itself (or uses an EBOOT.elf you decrypted '
                            'with RPCS3) and writes the copy this build needs into the port folder.'), top=6)
        self.path_row(f, 'game_exe', tr('Executable'), hint=tr('build\\win\\AscensionRecomp.exe, the recompiled '
                                                              'game.') if WINDOWS else None)
        self.path_row(f, 'eboot', 'EBOOT.ELF', hint=tr("The prepared EBOOT the game loads (set by the buttons "
                                                       'above: work\\eboot\\EBOOT.ELF).') if WINDOWS else None)
        self.path_row(f, 'disc_dir', tr('Disc folder'), folder=True,
                      hint=tr('The folder with PS3_GAME.') + (tr(' With the image below, the launcher mounts it here.') if MAC else
                                                       tr(" Empty: the EBOOT.ELF's own.")))
        if MAC:
            self.path_row(f, 'disc_iso', tr('Disc image (.iso)'),
                          hint=tr('Mounted read-only at the disc folder when playing.'))
        self.section(f, tr('Launcher'))
        self.row(f, tr('Launcher language'), self.choice(f, 'ui_language', [('auto', "Automatic (the system's)")] + available()),
                 tr("The launcher's own text only; the game's language is under Game."))
        self.var('ui_language').trace_add('write', lambda *_: self.root.after_idle(self.language_changed))
        self.check(f, 'music', tr('Background music'), tr("The disc's menu theme (SND0.AT3), made into music.mp3."))
        buttons = [(tr('Game folder'), lambda: self.open_path(ROOT)),
                   (tr('Make artwork from the disc'), self.remake_artwork)]
        if WINDOWS:
            buttons.insert(0, (tr('Desktop shortcut'), self.desktop_shortcut))
        self.buttons(f, buttons, top=12)
        self.section(f, tr('Updates'))
        self.check(f, 'check_updates', tr('Check for updates at start'),
                   tr('Asks GitHub for a newer release when the launcher opens. Nothing is downloaded '
                      'without asking; saves and settings are kept.'))
        self.row(f, tr('GitHub token'), self.ttk.Entry(f, textvariable=self.var('update_token'), width=50, show='•'),
                 tr('The repository is private: GitHub serves its releases only to an account with access. '
                    'Paste a token with read access to it (github.com → Settings → Developer settings → '
                    'Fine-grained tokens, Contents: Read-only), or sign in once with the GitHub CLI '
                    '(gh auth login). The token is kept in settings.json on this PC.'))
        self.buttons(f, ((tr('Check now'), lambda: self.check_updates(manual=True)),
                         (tr('Release page'), lambda: webbrowser.open(updater.RELEASES_PAGE))), top=8)
        self.update_note = self.note(f, tr('This is version {version}.', version=VERSION), top=6)
        self.section(f, tr('Runtime'))
        if 'LD_PSO_PRECOMPILE' in self.switches:
            self.check(f, 'precompile', tr('Compile shaders before playing (recommended)'),
                       tr('Builds all recorded shaders when the game starts, with a progress screen, so new areas '
                          'have nothing to compile. A few minutes once after an install or update, then about a '
                          'second. Off: they are built in the background while you play.'))
        if 'OCCL' in self.switches:
            self.check(f, 'occlusion', tr('Occlusion culling (recommended)'),
                       tr('Skips drawing objects the game itself finds hidden behind walls. About +8 to +17% in '
                          'heavy places, nothing hidden is ever skipped wrongly. Turn it off only if objects '
                          'flicker or something is missing.'))
        if 'PS3_HOST_PIN' in self.switches:
            self.check(f, 'host_pin', tr('Thread placement (recommended)'),
                       tr('Gives the game\'s busiest threads a CPU core each. Used with 10 or more cores and on '
                          '6-core CPUs with 12 threads (about +7 to +20% there); with 8 or 7 cores, fewer than 6, '
                          'a 6-core CPU of another kind, or an Intel CPU with E-cores, the game turns it off by itself (it runs faster without '
                          'it there). Turn it off only if the game stutters.'))
        if 'PS3_CPU_LIMIT' in self.switches:
            self.row(f, tr('CPU cores (testing)'), self.choice(f, 'cpu_limit', CPU_LIMITS),
                     hint=tr('Testing only: run the game on fewer CPU cores to see how a smaller CPU would '
                             'do. Thread placement then plans on those cores, as on a CPU of that size. Keep '
                             '"All cores" for play.'))
        entry = self.ttk.Entry(f, textvariable=self.var('extra_env'), width=70)
        self.row(f, tr('Extra variables'), entry,
                 tr('Separated by ";", for example  PS3_VERBOSE=1;VDEC_TIMELOG=1.  For debugging.'))
        read = sorted(self.switches)
        self.note(f, tr('This build reads: {names}.', names=', '.join(read)) if read else
                  tr('The executable was not found, so the switches it reads are not known.'))
        self.section(f, tr('Log'))
        self.buttons(f, ((tr('Copy'), self.copy_log), (tr('Open log file'), lambda: self.open_path(self.log_path())),
                         (tr('Logs folder'), lambda: self.open_path(LOG_DIR, make=True))), top=4)
        self.game_log = self.log_box(f, height=16)
        self.game_tail.rewind(to_end=False)
        self.log_append(self.game_log, self.game_tail.read(256 << 10)[-200_000:])

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.game_log.get('1.0', 'end'))
        self.refresh_status(tr('Log copied.'))

    def remake_artwork(self):
        ps3_game = disc_ps3_game({**self.s, **self.gather()})
        if not (ps3_game / 'PIC1.PNG').is_file():
            self.messagebox.showerror(APP_NAME, tr('The disc was not found ({path}).', path=ps3_game))
            return
        if not SHOT:     # the player holds music.mp3 open
            self.music.close()
            self.music = Music.__new__(Music)
            self.music.ok, self.music.error = False, ''
        for name in artwork.FILES:
            try:
                (artwork.OUT_DIR / name).unlink()
            except OSError:
                pass
        self.refresh_status(tr('Making the artwork from the disc…'))

        def work():
            notes = []
            done = artwork.make_assets(ps3_game, log=notes.append)
            self.root.after(0, self.artwork_remade, done, notes)
        threading.Thread(target=work, daemon=True).start()

    def artwork_remade(self, done, notes):
        self.artwork_ready(done, notes)
        text = tr('Artwork made: {names}.', names=', '.join(done)) if done else tr('No artwork was made.')
        if notes:
            text += ' ' + '; '.join(notes)
        self.refresh_status(text[:200])

    def desktop_shortcut(self):
        """A shortcut on the desktop that opens this launcher (pythonw, no console)."""
        pyw = Path(sys.executable).with_name('pythonw.exe')
        target = pyw if pyw.is_file() else Path(sys.executable)
        ico = icon_file()
        env = {**os.environ, 'ASC_TARGET': str(target), 'ASC_ARGS': f'"{HERE / "ascension_launcher.py"}"',
               'ASC_DIR': str(ROOT), 'ASC_ICON': str(ico) if ico else str(target), 'ASC_NAME': 'God of War Ascension'}
        script = ("$d=[Environment]::GetFolderPath('Desktop'); $l=Join-Path $d ($env:ASC_NAME + '.lnk'); "
                  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($l); $s.TargetPath=$env:ASC_TARGET; "
                  "$s.Arguments=$env:ASC_ARGS; $s.WorkingDirectory=$env:ASC_DIR; $s.IconLocation=$env:ASC_ICON; "
                  "$s.Description='God of War: Ascension (PC) launcher'; $s.Save(); Write-Output $l")
        try:
            r = subprocess.run(['powershell', '-NoProfile', '-Command', script], env=env, capture_output=True,
                               text=True, errors='replace', timeout=30,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except (OSError, subprocess.SubprocessError) as e:
            self.messagebox.showerror(APP_NAME, tr('Could not make the shortcut:\n{error}', error=e))
            return
        if r.returncode == 0:
            self.refresh_status(tr('Shortcut made: {path}', path=r.stdout.strip()))
        else:
            self.messagebox.showerror(APP_NAME, tr('Could not make the shortcut:\n{error}', error=r.stderr.strip()[-400:]))

    # ---- updates ----------------------------------------------------------------------------
    def update_said(self, text, status=False):
        """The Updates section's line (Advanced), and the status bar when asked."""
        if getattr(self, 'update_note', None) and self.update_note.winfo_exists():
            self.update_note.configure(text=text)
        if status:
            self.refresh_status(text)

    def check_updates(self, manual=False):
        """The latest release from GitHub (in a thread); offered when it is newer."""
        if self.update_busy:
            return
        self.update_busy = True
        self.update_said(tr('Checking for updates…'), status=manual)
        tok = self.var('update_token').get()

        def work():
            try:
                info, error = updater.latest(tok), None
            except updater.UpdateError as e:
                info, error = None, str(e)
            self.root.after(0, self.update_checked, info, error, manual)
        threading.Thread(target=work, daemon=True).start()

    def update_checked(self, info, error, manual):
        self.update_busy = False
        if error:
            self.update_said(tr('Update check: {error}', error=tr(error)), status=manual)
            if manual:
                self.messagebox.showerror(APP_NAME, tr(error))
            return
        if not updater.newer(info):
            self.update_said(tr('This is the latest version ({version}).', version=VERSION), status=manual)
            return
        new = tr('Version {new} is available (this is {old}).', new=info['version'], old=VERSION)
        self.update_said(new, status=True)
        if not info['asset'] or not updater.can_install():
            why = (tr('This release has no update zip: download it from the release page.') if not info['asset'] else
                   tr('This copy is a source checkout, not a release folder: update it from the source.'))
            if self.messagebox.askyesno(APP_NAME, f'{new}\n\n{why}\n\n' + tr('Open the release page?')):
                webbrowser.open(info['url'])
            return
        notes = info['notes'].strip().replace('\r', '')
        if len(notes) > 700:
            notes = notes[:700].rsplit('\n', 1)[0] + '\n…'
        if self.messagebox.askyesno(APP_NAME, f'{new}\n\n{notes}\n\n' + tr(
                'Download it ({size} MB) and install it now? Your saves and settings are kept.',
                size=max(1, round(info['asset']['size'] / 1e6)))):
            self.download_update(info)
        else:
            self.update_said(new + ' ' + tr('Advanced → Updates → Check now installs it.'), status=True)

    def download_update(self, info):
        self.update_busy = True
        tok = self.var('update_token').get()
        shown = [0]

        def progress(done, total):
            mb = done >> 20
            if mb != shown[0]:
                shown[0] = mb
                self.root.after(0, self.update_said, tr('Downloading {version}… {done} of {total} MB', version=info['version'],
                                                        done=mb, total=max(1, total >> 20)), True)

        def work():
            try:
                path = updater.download(info['asset'], tok, progress)
                self.root.after(0, self.install_update, info, path)
            except updater.UpdateError as e:
                self.root.after(0, self.update_failed, str(e))
        threading.Thread(target=work, daemon=True).start()

    def install_update(self, info, path):
        if self.process or self.preparing or self.patch_proc or game_running(play_exe(self.s)):
            self.update_busy = False
            self.update_said(tr('Version {version} is downloaded. Close the game, then press Check now '
                                '(Advanced → Updates) to install it.', version=info['version']), status=True)
            return
        self.update_said(tr('Installing {version}…', version=info['version']), status=True)
        self.root.update_idletasks()
        try:
            count = updater.install(path)
        except updater.UpdateError as e:
            self.update_failed(str(e))
            return
        path.unlink(missing_ok=True)
        self.update_busy = False
        done = tr('Updated to {version} ({count} files). The launcher restarts to use it.',
                  version=info['version'], count=count)
        self.update_said(done, status=True)
        if self.messagebox.askyesno(APP_NAME, done + '\n\n' + tr('Restart the launcher now?')):
            self.restart_launcher()

    def update_failed(self, error):
        self.update_busy = False
        self.update_said(tr('The update failed: {error}', error=tr(error)), status=True)
        self.messagebox.showerror(APP_NAME, tr('The update failed:\n{error}', error=tr(error)))

    def restart_launcher(self):
        if self.gather() != self.saved and self.messagebox.askyesno(APP_NAME, tr('Save the changes before leaving?')):
            self.save()
        self.music.close()
        release_disc(self.mounted)
        subprocess.Popen([sys.executable] + sys.argv, cwd=os.getcwd())
        self.root.destroy()

    def language_changed(self):
        """A new launcher language takes a new window: restart now, or on the next start."""
        code = self.var('ui_language').get()
        if code == self.ui_language or getattr(self, 'asking_language', False):
            return
        if self.process or self.preparing or self.patch_proc:
            self.var('ui_language').set(self.ui_language)
            self.refresh_status(tr("Close the game before changing the launcher's language."))
            return
        self.asking_language = True
        restart = self.messagebox.askyesno(APP_NAME, tr('Restart the launcher now to change the language?'))
        self.asking_language = False
        if not restart:
            self.refresh_status(tr('The language changes the next time the launcher opens (click Save).'))
            return
        self.save()
        self.music.close()
        release_disc(self.mounted)
        subprocess.Popen([sys.executable] + sys.argv, cwd=os.getcwd())
        self.root.destroy()

    def build_support(self):
        """Support page: what the project is, the Patreon button, the perks and the supporters' names
        (one per line in supporters.txt next to the launcher; lines starting with # are skipped)."""
        tk, ttk = self.tk, self.ttk
        f = self.scrolled_page('support', tr('Support'), tr('Help keep the port getting better.'))
        self.section(f, '')
        self.note(f, tr('Every release of this port is free for everyone, and it always will be. '
                        'If you enjoy it and want to see it keep improving (more speed, fewer stutters, '
                        'new features), you can support the project on Patreon.'), top=4, style='TLabel')
        holder = ttk.Frame(self.body(f))
        self.put(f, holder, sticky='w', pady=(self.px(16), 0))
        ttk.Button(holder, text='♥  ' + tr('Support on Patreon'), style='Accent.TButton',
                   command=lambda: webbrowser.open(social.SUPPORT_URL)).pack(side='left')
        link = tk.Label(holder, text=social.SUPPORT_URL, bg=PANEL, fg=GOLD, cursor='hand2',
                        font=(self.ui, 10, 'underline'))
        link.bind('<Button-1>', lambda _e: webbrowser.open(social.SUPPORT_URL))
        link.pack(side='left', padx=(self.px(14), 0))
        self.section(f, tr('What supporters get'))
        for line in (tr('Your name in the launcher, on this page'),
                     tr('Behind-the-scenes updates: screenshots, comparisons and progress before anyone else'),
                     tr('A vote on what gets built next')):
            self.note(f, '•  ' + line, top=4, style='TLabel')
        self.section(f, tr('Other ways to help'))
        for line in (tr('Share the port with friends and on social media'),
                     tr('Report bugs with your log file, so they can be fixed'),
                     tr('Follow the YouTube channel for progress videos')):
            self.note(f, '•  ' + line, top=4, style='TLabel')
        self.section(f, tr('Thank you, supporters'))
        names = []
        try:
            for raw in (HERE / 'supporters.txt').read_text(encoding='utf-8').splitlines():
                name = raw.strip()
                if name and not name.startswith('#'):
                    names.append(name)
        except OSError:
            pass
        if names:
            self.note(f, '   ·   '.join(names), top=6, style='Value.TLabel')
        else:
            self.note(f, tr('Be the first name on this list.'), top=6)
        self.note(f, tr('God of War is a trademark of Sony Interactive Entertainment. A non-profit fan '
                        'project, not affiliated with Sony or Santa Monica Studio. Support goes to the '
                        "port's development; it does not buy the game or any game content."), top=self.px(22))

    def build_about(self):
        f = self.scrolled_page('about', tr('About'), f'{APP_NAME} (PC) · launcher v{VERSION}')
        self.section(f, tr('The project'), top=4)
        self.note(f, tr("God of War: Ascension (PS3) recompiled to run natively on a PC. The game's PowerPC "
                        'code is translated to C and compiled for x86-64; the SPU code is recompiled too, '
                        "and what the PS3's system did (RSX video, audio, files, SPURS) is reimplemented by "
                        'the ps3recomp runtime. It is not an emulator and includes no game assets: '
                        'you need your own copy of the game.'), top=4)
        self.note(f, tr('Status: a work in progress. The game runs from boot into the levels, below full speed '
                        'in places; see the progress in the repository.'))
        self.section(f, social.POSTED_BY)
        self.put(f, social.link_bar(self.tk, self.body(f), PANEL, MUTED, (self.ui, 9), self.px(34)), sticky='w',
                 pady=(self.px(6), 0))
        for _kind, title, url in social.LINKS:
            link = self.tk.Label(f, text=url, bg=PANEL, fg=GOLD, cursor='hand2', font=(self.ui, 10, 'underline'))
            link.bind('<Button-1>', lambda _e, u=url: webbrowser.open(u))
            self.row(f, title, link)
        self.section(f, tr('Links'))
        for title, url in ((tr('This project'), 'https://github.com/arnaldolima1/gow-ascension-pc'),
                           ('ps3recomp (sp00nznet)', 'https://github.com/sp00nznet/ps3recomp'),
                           ('SpartanPort (FelipePrado0)', 'https://github.com/FelipePrado0/SpartanPort'),
                           (tr("RPCS3's game patches"), 'https://wiki.rpcs3.net/index.php?title=Help:Game_Patches')):
            link = self.tk.Label(f, text=url, bg=PANEL, fg=GOLD, cursor='hand2', font=(self.ui, 10, 'underline'))
            link.bind('<Button-1>', lambda _e, u=url: webbrowser.open(u))
            self.row(f, title, link)
        self.section(f, tr('Credits'))
        self.note(f, tr('By Arnaldo Lima, with the ps3recomp runtime by sp00nznet and contributors. '
                        "Launcher in the style of SpartanPort's (FelipePrado0). RPCS3 game patches by their "
                        'authors (RPCS3 patch database). Cinzel typeface (SIL Open Font License).'), top=4)
        self.note(f, tr('God of War is a trademark of Sony Interactive Entertainment. A non-profit fan '
                        'project, not affiliated with Sony or Santa Monica Studio.'))

    # ---- state ------------------------------------------------------------------------------
    def gather(self):
        values = {k: v.get() for k, v in self.vars.items()}
        values['keymap'] = {c: list(k) for c, k in self.keymap.items()}
        values['padmap'] = dict(self.padmap)
        values['mods_order'] = list(getattr(self, 'mod_order', self.s['mods_order']))
        if getattr(self, 'mod_vars', None):
            values['mods_enabled'] = [m for m, v in self.mod_vars.items() if v.get()]
        else:
            values['mods_enabled'] = list(self.s['mods_enabled'])
        values['rpcs3_patches'] = self.selected_patches() if hasattr(self, 'patch_vars') else list(self.s['rpcs3_patches'])
        return values

    def summary(self, s):
        """What the game starts with, in a line (the bottom bar and Home's strip)."""
        parts = [tr('Full screen') if s['display_mode'] == 'fullscreen' else s['window_size'].replace('x', ' × '),
                 tr('{fps} FPS', fps=fps_limit(s, self.patches_now(s)))]
        n = len(s.get('rpcs3_patches') or [])
        parts.append(tr('1 patch') if n == 1 else tr('{n} patches', n=n))
        m = len(s.get('mods_enabled') or [])
        parts.append(tr('1 mod') if m == 1 else tr('{n} mods', n=m))
        if s.get('skip_movies') == '1':
            parts.append(tr('cutscenes skipped'))
        return ' · '.join(parts)

    def refresh_status(self, text=None):
        if text is None:
            s = {**self.s, **self.gather()}
            gone, problem = missing(s, split_ok=True), disc_problem(s)
            split = [] if gone or problem else split_game_files(s)
            text = (tr('Missing {items}', items=tr(' and ').join(gone)) if gone else
                    tr('Warning: {problem}.', problem=problem) if problem else
                    tr('{files} split into parts by a FAT32 drive: PLAY joins them first.',
                       files=', '.join(f.name for f, _p, _n in split)) if split else
                    tr('Ready to play · {summary}', summary=self.summary(s)))
        self.status.configure(text=text)

    # ---- music ------------------------------------------------------------------------------
    def music_on(self):
        return getattr(self, '_music_on', self.s.get('music') == '1')

    def set_music(self, on):
        self._music_on = on
        if on and not self.process:
            self.music.play()
        else:
            self.music.pause()
        self.var('music').set('1' if on else '0')
        self.refresh_music_label()

    def toggle_music(self):
        self.set_music(not self.music_on())

    def refresh_music_label(self):
        if not hasattr(self, 'music_label'):
            return
        err = getattr(getattr(self, 'music', None), 'error', '')
        if err and self.music_on():
            text = tr('♪ Music: ') + err
        else:
            text = tr('♪ Music: on') if self.music_on() else tr('♪ Music: off')
        self.music_label.configure(text=text)

    # ---- actions ----------------------------------------------------------------------------
    def log_path(self):
        return LOG_DIR / 'last_run.log'

    def tail_logs(self):
        """The game's log into Advanced → Log while it runs (the game writes the file)."""
        if self.process and hasattr(self, 'game_log'):
            self.log_append(self.game_log, self.game_tail.read())
        self.root.after(500, self.tail_logs)

    def open_path(self, path, make=False):
        path = Path(path)
        if make:
            path.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            self.messagebox.showinfo(APP_NAME, tr('Does not exist yet:\n{path}', path=path))
            return
        if WINDOWS:
            os.startfile(str(path))
        else:
            subprocess.Popen(['open' if MAC else 'xdg-open', str(path)])

    def play(self):
        """The slow part (disc, saves, mods) runs in a thread with its steps in
        the status bar; the game starts once it is done."""
        if self.process or self.preparing:
            return
        if self.patch_proc:
            self.messagebox.showinfo(APP_NAME, tr('The game is being rebuilt with the patches; PLAY works again '
                                                  'when that is done (Patches page).'))
            return
        if self.gather() != self.saved:
            self.save()
        self.take_ingame()           # a file left by a game the launcher did not see close
        gone = missing(self.s, split_ok=True)
        if gone:
            self.messagebox.showerror(APP_NAME, tr('Could not find {items}.\nSet it in Advanced.', items=tr(' and ').join(gone)))
            self.show('advanced')
            return
        problem = disc_problem(self.s)
        if problem:
            self.messagebox.showerror(APP_NAME, tr('This game was built from the European release, but {problem}.', problem=problem))
            return
        split = split_game_files(self.s)
        if split:
            self.offer_join(split)
            return
        if game_running(play_exe(self.s)):
            self.messagebox.showerror(APP_NAME, tr('The game is already running (started elsewhere). Only one copy '
                                                   'can run at a time: close it first.'))
            return
        if self.patches_pending() and not self.messagebox.askyesno(
                APP_NAME, tr('The patches ticked on the Patches page are not built into the game yet (press Apply '
                             'there).\n\nPlay with the game as it is built?')):
            self.show('patches')
            return
        self.preparing = True
        self.play_button.configure(state='disabled')
        s = dict(self.s)

        def work():
            try:
                env, mounted = prepare_game(s, status=lambda m: self.root.after(0, self.refresh_status, m))
                self.root.after(0, self.launch, env, mounted)
            except Exception as e:
                self.root.after(0, self.launch_failed, str(e) or type(e).__name__)
        threading.Thread(target=work, daemon=True).start()

    def launch(self, env, mounted):
        self.mounted = self.mounted or mounted
        self.embedded = WINDOWS and self.s.get('game_window') == 'embedded' and 'PS3_PARENT_HWND' in self.switches
        host = self.enter_game_view() if self.embedded or not WINDOWS else None
        self.game_tail.rewind()
        try:
            self.process, self.log = start_game(self.s, env, self.log_path(),
                                                parent=host.winfo_id() if host else None)
        except Exception as e:
            if self.embedded or not WINDOWS:
                self.leave_game_view()
            self.launch_failed(str(e) or type(e).__name__)
            return
        self.preparing = False
        self.music.pause()
        self.refresh_backups()
        self.refresh_patch_state()
        self.log_set(self.game_log, '')
        self.play_button.configure(text=tr('RUNNING'), state='disabled')
        dropped = online_dropped_patches(env)
        if dropped:
            msg = tr('Game running online. Cheats are off online ({n} patches left out; they stay '
                     'on for single player).', n=len(dropped))
        else:
            msg = tr('Game running… the log is in Advanced → Log.')
        # 2026-11-18: the old EU-disc-only online note was removed: the game exe's
        # switch 3 (mpnet.c, func_0032760C) gives EU and US discs the same multiplayer data value since 0.1.35.
        self.refresh_status(msg)
        if WINDOWS and not self.embedded and self.s.get('hide_while_playing') == '1':
            self.root.after(4000, self.hide_for_game, self.process)
        threading.Thread(target=self.wait_game, daemon=True).start()
        if self.s.get('backup_saves') == '1':        # a copy each time the game saves (game.watch_saves)
            threading.Thread(target=watch_saves, args=(self.process,), daemon=True).start()

    def hide_for_game(self, process):
        if self.process is process and process.poll() is None:
            self.hidden_for_game = True
            self.root.withdraw()

    def launch_failed(self, error):
        self.preparing = False
        self.play_button.configure(text=tr('PLAY'), state='normal')
        self.refresh_status()
        self.messagebox.showerror(APP_NAME, tr('The game did not start:\n{error}', error=error))

    def offer_join(self, split):
        """A copy of the game made on a FAT32 drive has its files over 4 GB cut into
        parts (game.split_parts); PLAY offers to put them back together, in a thread
        with the progress in the status bar."""
        names = ', '.join(tr('{name} ({n} parts)', name=f.name, n=len(parts)) for f, parts, _n in split)
        size = sum(n for _f, _p, n in split)
        try:
            join_check(split)
        except OSError as e:
            self.messagebox.showerror(APP_NAME, tr('Could not join the parts:\n{error}', error=str(e) or type(e).__name__))
            return
        if not self.messagebox.askyesno(APP_NAME, tr(
                'Some files of your copy of the game are split into parts: {names}.\n\n'
                'A USB drive formatted as FAT32 cannot hold files over 4 GB, so the tool that copied the game '
                'cut them up. The game needs each file in one piece.\n\n'
                'Join them now? This writes {size} GB into your game folder:\n{folder}\n\n'
                'The parts are kept; you can delete them afterwards.',
                names=names, size=gigabytes(size), folder=split[0][0].parent)):
            return
        self.preparing = True
        self.play_button.configure(state='disabled')
        stop = self.join_stop = threading.Event()

        def status(text):
            if not stop.is_set():          # close() pumps the window while it waits, but say nothing more
                self.root.after(0, self.refresh_status, text)

        def work():
            try:
                join_split(split, status=status, stop=stop)
                error = None
            except Exception as e:
                error = str(e) or type(e).__name__
            if not stop.is_set():
                self.root.after(0, self.join_done, error)
        self.join_thread = threading.Thread(target=work, daemon=True)
        self.join_thread.start()

    def join_done(self, error):
        self.join_thread = None
        self.preparing = False
        self.play_button.configure(text=tr('PLAY'), state='normal')
        self.refresh_status()
        self.draw_banner()
        if error:
            self.messagebox.showerror(APP_NAME, tr('Could not join the parts:\n{error}', error=error))
        else:
            self.messagebox.showinfo(APP_NAME, tr('Done: the files are in one piece now. Press PLAY to start the game.'))

    def callback_error(self, exc, value, tb):
        """An error in the window's own code (a button, a timer). The launcher
        runs on pythonw, which has nowhere to print it: PLAY used to do nothing
        at all, with no log. Written to build\\logs\\launcher_error.log and shown;
        a PLAY stopped half way can be pressed again."""
        import traceback
        path = LOG_DIR / 'launcher_error.log'
        try:
            LOG_DIR.mkdir(parents=True, exist_ok=True)
            with open(path, 'a', encoding='utf-8') as f:
                f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + VERSION + '\n' + ''.join(traceback.format_exception(exc, value, tb)) + '\n')
        except OSError:
            pass
        if self.preparing and not self.process:
            self.preparing = False
            try:
                self.play_button.configure(text=tr('PLAY'), state='normal')
            except self.tk.TclError:
                pass
        try:
            self.messagebox.showerror(APP_NAME, tr('The launcher hit an error:\n{error}\n\nThe details are in {path}.',
                                                   error=f'{exc.__name__}: {value}', path=path))
        except self.tk.TclError:
            pass

    def enter_game_view(self):
        """Embedded: the launcher's window becomes the game's (the pages give
        way to a black area the game renders into, at the chosen size or full
        screen). On macOS the game opens a window of its own and the launcher hides."""
        root = self.root
        if not WINDOWS:
            root.withdraw()
            root.update_idletasks()
            root.update()
            mac_dock(False)
            return None
        self.prev_geometry, self.prev_minsize = root.geometry(), root.minsize()
        self.side.pack_forget()
        self.right.pack_forget()
        self.game_host = self.tk.Frame(root, bg='black', highlightthickness=0, bd=0)
        self.game_host.pack(fill='both', expand=True)
        root.minsize(320, 180)
        if self.s['display_mode'] == 'fullscreen':
            root.attributes('-fullscreen', True)
        else:
            w, h = self.s['window_size'].split('x')
            root.geometry(f'{w}x{h}+{max(0, root.winfo_x())}+{max(0, root.winfo_y())}')
        root.update_idletasks()
        root.update()
        return self.game_host

    def leave_game_view(self):
        root = self.root
        if not WINDOWS:
            mac_dock(True)
            root.deiconify()
            root.lift()
            return
        root.attributes('-fullscreen', False)
        try:   # the game's F11 may have left the frame borderless
            frame = int(root.wm_frame(), 16)
            u = ctypes.windll.user32
            u.SetWindowLongW(frame, -16, 0x00CF0000 | 0x10000000)   # GWL_STYLE: WS_OVERLAPPEDWINDOW | WS_VISIBLE
            u.SetWindowPos(frame, 0, 0, 0, 0, 0, 0x0027)           # NOSIZE|NOMOVE|NOZORDER|FRAMECHANGED
        except (AttributeError, OSError, ValueError):
            pass
        if getattr(self, 'game_host', None):
            self.game_host.destroy()
            self.game_host = None
        root.minsize(*self.prev_minsize)
        root.geometry(self.prev_geometry)
        self.side.pack(side='left', fill='y')
        self.right.pack(side='left', fill='both', expand=True)

    def wait_game(self):
        code = self.process.wait()
        try:
            self.root.after(0, self.game_ended, code)
        except RuntimeError:     # the launcher closed first
            pass

    def take_ingame(self, show=True):
        """What the player changed in the game's own menu (F1 / hold SELECT):
        into the settings, the settings file and, with `show`, the pages. Edits
        on the pages that are not saved yet stay as they are."""
        changed = merge_ingame(self.s)
        if not changed:
            return
        save_settings(self.s)
        if not show:
            return
        for key, value in changed.items():
            if key in self.vars:
                self.vars[key].set(value)
                self.saved[key] = value
        self.refresh_status(tr('Settings changed in the game were saved.'))

    def game_ended(self, code):
        self.log.close()
        self.process = None
        if getattr(self, 'closing', False):
            self.take_ingame(show=False)
            return                   # the launcher is closing it (close): nothing to show
        self.log_append(self.game_log, self.game_tail.read())
        self.play_button.configure(text=tr('PLAY'), state='normal')
        if self.current == 'trophies':
            self.refresh_trophies()   # host/trophies: what the game just unlocked
        if self.embedded or not WINDOWS:
            self.leave_game_view()
        if self.hidden_for_game:
            self.hidden_for_game = False
            self.root.deiconify()
            self.root.lift()
        self.refresh_backups()
        self.refresh_patch_state()
        if self.music_on():
            self.music.play()
        if code == 0:
            self.refresh_status(tr('Game closed.'))
            self.take_ingame()
            return
        self.take_ingame()           # also when the game stopped with an error (the messages below follow)
        if (code & 0xFFFFFFFF) == 0xC000001D:   # STATUS_ILLEGAL_INSTRUCTION: the CPU has no AVX2/FMA (the exe needs them)
            self.refresh_status(tr('Your processor is not supported (no AVX2).'))
            if self.messagebox.askyesno(APP_NAME, tr(AVX2_MESSAGE, code=code)):
                self.open_path(self.log_path())
            return
        gpu = unsupported_gpu(self.log_path())
        if gpu:
            self.refresh_status(tr('Your graphics card may not be able to run the game.'))
            if self.messagebox.askyesno(APP_NAME, tr(GPU_MESSAGE, code=code, gpu=gpu)):
                self.open_path(self.log_path())
            return
        self.refresh_status(tr('The game ended with code {code}.', code=code))
        if self.messagebox.askyesno(APP_NAME, tr("The game ended with code {code}.\nOpen this run's log?", code=code)):
            self.open_path(self.log_path())

    def close(self):
        if getattr(self, 'closing', False):
            return                   # already waiting for the game to close (the window is pumped meanwhile)
        joining = getattr(self, 'join_thread', None)
        if joining and joining.is_alive():
            if not self.messagebox.askyesno(APP_NAME, tr('The game files are still being joined. Close anyway? '
                                                         'The join stops and starts over next time.')):
                return
            self.join_stop.set()             # the join deletes its unfinished file; the window is pumped
            end = time.time() + 15           # meanwhile, since the join's thread may be waiting on it
            while joining.is_alive() and time.time() < end:
                self.root.update()
                time.sleep(0.05)
        if self.patch_proc:
            if not self.messagebox.askyesno(APP_NAME, tr('Patches are still being applied. Close the launcher anyway? '
                                                         'The build goes on in the background (its log: '
                                                         'build\\logs\\apply-patches.log).')):
                return
        try:
            if self.process:
                if self.embedded or not WINDOWS:
                    if not self.messagebox.askyesno(APP_NAME, tr('Close the game?')):
                        return
                    stop = True
                else:
                    stop = self.messagebox.askyesnocancel(APP_NAME, tr('The game is running. Close it too?\n\n'
                                                                       'No leaves it running.'))
                    if stop is None:
                        return
                if stop and self.process:    # it may have ended while the question was open
                    self.closing = True
                    self.refresh_status(tr('Closing the game (it saves its compiled shaders first)…'))
                    self.root.update_idletasks()
                    close_gently(self.process, pump=self.root.update)
                self.log.close()
            if self.gather() != self.saved and self.messagebox.askyesno(APP_NAME, tr('Save the changes before leaving?')):
                self.save()
            self.music.close()
            release_disc(self.mounted)
        except Exception:
            self.closing = False             # an error on the way out: the close button works again
            raise
        self.root.destroy()
