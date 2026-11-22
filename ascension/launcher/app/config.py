"""Paths, the settings and the choices the pages offer."""

import json
import os
import sys
from pathlib import Path


VERSION = '0.2.0'   # the port release (BilalReloaded); was the launcher's own 0.2
APP_NAME = 'God of War: Ascension'
GAME_ID = 'BCES01741'      # the European disc the port is built from
US_ID = 'BCUS98232'        # the US disc: the same program 16 bytes on (app/us_eboot.py converts its EBOOT)
GAME_IDS = (GAME_ID, US_ID)
WINDOWS = sys.platform == 'win32'
MAC = sys.platform == 'darwin'
SHOT = '--shot' in sys.argv     # development: screenshot each page, then quit (no music)

HERE = Path(__file__).resolve().parent.parent          # launcher/
ROOT = HERE.parent                                   # ascension/
WORK = ROOT.parent / 'work'
# Artwork made from the player's own disc (app/assets.py) goes to the second.
ASSET_DIRS = [HERE / 'assets', WORK / 'data' / 'launcher']
LOG_DIR = ROOT / 'build' / 'logs'
if WINDOWS:
    CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'ascension-launcher'
elif MAC:
    CONFIG_DIR = Path.home() / 'Library' / 'Application Support' / 'ascension-launcher'
else:
    CONFIG_DIR = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'ascension-launcher'
SHARED_CONFIG_FILE = CONFIG_DIR / 'settings.json'
# A release folder (its own Python beside ascension-recomp\) keeps its settings
# in that folder: with several port folders on one PC (a newer full package
# next to an older one, a development checkout) each starts its own game; with
# one shared file the newer launcher kept starting the older folder's game.
# The first start copies the shared settings (load_settings).
INSTALL = ROOT.parent.parent
RELEASE = (INSTALL / 'runtime' / 'python').is_dir()
CONFIG_FILE = INSTALL / 'settings.json' if RELEASE else SHARED_CONFIG_FILE


# The PS3 console languages the disc has text and voices for (CELL_SYSUTIL_LANG_*).
# Greek is not a console language: the game itself offers it at first start.
# CELL_SYSUTIL_LANG: 2 = French, 3 = Spanish (the game's own LANGS_BCES01741.TXT: "2 fr FRENCH",
# "3 es SPANISH"; the labels of 2 and 3 were swapped before 2026-11-10).
LANGUAGES = [
    ('18', 'English (UK)'), ('1', 'English (US)'), ('7', 'Português (Portugal)'),
    ('17', 'Português (Brasil)'), ('3', 'Español'), ('2', 'Français'), ('4', 'Deutsch'),
    ('5', 'Italiano'), ('8', 'Русский'), ('16', 'Polski'), ('19', 'Türkçe'),
]
# The US disc's languages (its GAME.PSARC LANGS_BCUS98232.TXT: "1 en ENGLISH", "2 fr FRENCH",
# "17 pt-br BR_PORTUGUESE", "21 es-la LA_SPANISH"; Brazilian Portuguese is on the disc, no mod).
LANGUAGES_US = [
    ('1', 'English (US)'), ('2', 'Français'), ('17', 'Português (Brasil)'), ('21', 'Español (Latinoamérica)'),
]
# The voices, apart from the text (the system save keeps them in their own byte;
# the game's own Options -> Speech changes it too). 1 = the English voices the
# game itself picks with English text; 'game' = leave the byte as the game set it.
VOICE_LANGUAGES = [('same', 'Same as the game language'), ('1', 'English'),
                   ('game', "Keep the game's own choice (Options → Speech)")]
# Offline Multiplayer (the patch of that name): the mode its Quick Play starts
# and the map of the first match (PS3_MP_MODE / PS3_MP_MAP, read by the game
# when it starts: host\\ascension_patches.cpp). The maps of a mode are in the
# order of the game's own list (map 1, 2, ...); names from the game's texts.
MP_MODES = [('trial1', 'Trial of the Gods (1 player)'), ('trial2', 'Trial of the Gods (2 players, co-op)'),
            ('favor4', 'Favor of the Gods (4 players)'), ('favor8', 'Favor of the Gods (8 players)'),
            ('team4', 'Team Favor of the Gods (2 vs 2)'), ('team8', 'Team Favor of the Gods (4 vs 4)'),
            ('ctf', 'Capture the Flag (4 vs 4)')]
MP_MAPS = {
    'trial1': ('Rotunda of Olympus', 'Desert of Lost Souls', 'Forum of Hercules'),
    'trial2': ('Desert of Lost Souls', 'Forum of Hercules', 'Rotunda of Olympus'),
    'favor4': ('Forum of Hercules', "The Oracle's Chamber", 'Coliseum of Persia', 'Rotunda of Olympus'),
    'favor8': ('Bog of the Forgotten', 'Labyrinth of Daedalus', 'Walls of Troy', 'Desert of Lost Souls'),
    'team4': ('Rotunda of Olympus', 'Forum of Hercules', "The Oracle's Chamber", 'Coliseum of Persia'),
    'team8': ('Bog of the Forgotten', 'Desert of Lost Souls', 'Labyrinth of Daedalus', 'Walls of Troy'),
    'ctf': ('Bog of the Forgotten', 'Labyrinth of Daedalus', 'Walls of Troy', 'Desert of Lost Souls'),
}


def mp_map_choices(mode):
    """The map choices of a mode: 'cycle' (the game's own shuffled order) or
    the number of a map in the mode's list."""
    names = MP_MAPS.get(mode, MP_MAPS['trial1'])
    return [('cycle', 'All maps, in random order')] + [(str(i + 1), name) for i, name in enumerate(names)]


DISPLAY_MODES = [('window', 'Window'), ('fullscreen', 'Full screen (borderless)')]
WINDOW_SIZES = [(s, s.replace('x', ' × ')) for s in
                ('1280x720', '1600x900', '1920x1080', '2560x1440', '3840x2160')]
CPU_LIMITS = [('0', 'All cores'), ('8', '8 cores (test)'), ('6', '6 cores (test)'), ('4', '4 cores (test)')]
ENTER_BUTTONS = [('cross', '✕ confirms (West)'), ('circle', '○ confirms (Japan)')]
ON_OFF = [('1', 'On'), ('0', 'Off')]
# AMD FSR 3.1 (2026-11-11, host ar-fsr): one mode at a time (both together broke Kratos in 1 of 9 test shots).
FSR_MODES = [('0', 'Off'), ('aa', 'Anti-aliasing (instead of SMAA)'), ('fg', 'Frame generation (RX 5000 / RTX 20 or newer)')]
# Where the game draws: a window of its own (as scripts\windows\run.ps1 starts
# it), or inside the launcher's window (PS3_PARENT_HWND; Windows only).
GAME_WINDOWS = ([('own', 'Its own window'), ('embedded', "Inside the launcher's window")] if WINDOWS else
                [('own', 'Its own window')])

# The environment switches the launcher can set. Which of them the game reads
# is found in its executable (game.runtime_switches): a getenv() name is a
# string in the .exe. A setting whose switch the build does not read is not
# shown, and not passed. As of this launcher, ps3recomp/ascension read:
#   LD_PERF_OVERLAY, PS3_FPS_LIMIT, PS3_FULLSCREEN, PS3_WINDOW_SIZE,
#   PS3_WINDOW_ICON, PS3_ESC_QUITS, PS3_PARENT_HWND, PS3_ENTER_BUTTON,
#   PS3_SYS_LANG, PS3_MODS, PS3_VFS_ROOT, PS3_HDD0_ROOT
# and not (yet): PS3_KEYMAP, PS3_PADMAP, PS3_PAD_DEADZONE, PS3_RENDER_SCALE,
#   PS3_VSYNC, PS3_ASPECT, PS3_SCALE_FILTER, PS3_ANISO, PS3_RSX(_RENDERER).
# PS3_SKIP_MOVIES (skip the prerendered movies) is being added to the runtime.
SWITCHES = ('LD_PERF_OVERLAY', 'PS3_FPS_LIMIT', 'PS3_FULLSCREEN', 'PS3_WINDOW_SIZE', 'PS3_WINDOW_ICON',
            'PS3_ESC_QUITS', 'PS3_PARENT_HWND', 'PS3_ENTER_BUTTON', 'PS3_SYS_LANG', 'PS3_MODS',
            'PS3_VFS_ROOT', 'PS3_HDD0_ROOT', 'PS3_SKIP_MOVIES', 'PS3_KEYMAP', 'PS3_PADMAP',
            'PS3_PAD_DEADZONE', 'PS3_RENDER_SCALE', 'PS3_VSYNC', 'PS3_ASPECT', 'PS3_SCALE_FILTER',
            'PS3_ANISO', 'PS3_RSX', 'PS3_RSX_RENDERER', 'LD_SMAA', 'LD_SHARPEN', 'LD_VIBRANCE', 'PS3_HOST_PIN', 'PS3_CPU_LIMIT',
            'PS3_DISPLAY_MATCH', 'PS3_PATCHES', 'PS3_PATCH_LIST', 'LD_PSO_PRECOMPILE',
            'PS3_QUIT_HOLD_MS', 'OCCL', 'FSR3_NATIVE_AA', 'FSR3_FG', 'PS3_MP_MODE', 'LD_UBER', 'LD_AO', 'LD_FULLRES_FX', 'GP_PULL',
            'PS3_INGAME_SETTINGS', 'LD_MENU_LANG', 'PS3_NO_SHADOWS')
# What the source of this build reads, for when the executable cannot be looked at.
KNOWN_SWITCHES = frozenset(SWITCHES[:12])
# The game's clock steps 1/30 s a frame: a higher limit is faster gameplay, so
# 30 is the only limit offered until a patch unties the clock.
FPS_LIMIT = '30'
# With Unlock FPS the clock follows real time, so the limit is a choice: 30
# halves the work each second (the PS3 original runs at 30).
FPS_CAPS = [('60', '60 FPS'), ('45', '45 FPS (120/144 Hz, VRR)'), ('40', '40 FPS (120 Hz screens)'),
            ('30', '30 FPS (for lower-end PCs)'), ('120', '120 FPS (experimental)'),
            ('0', 'Unlocked (no limit)')]
# 120 (ovl3, 2026-11-21): the game's clock keeps real time at 120 (game clock 1.000, audio real time);
# experimental because few PCs hold it. '0' = no limit (PS3_FPS_LIMIT=0). Both also in the in-game panel.
# The Display page's presets: what changes, the rest stays as set.
PRESETS = {
    'low': {'fps_cap': '30', 'render_scale': '100', 'smaa': '1', 'fsr': '0', 'sharpen': '40', 'aniso': '0'},
    'balanced': {'fps_cap': '60', 'render_scale': '150', 'smaa': '1', 'fsr': '0', 'sharpen': '60', 'aniso': '8'},
    'high': {'fps_cap': '60', 'render_scale': '200', 'smaa': '1', 'fsr': '0', 'sharpen': '80', 'aniso': '16'},
    'ultra': {'fps_cap': '60', 'render_scale': '300', 'smaa': '1', 'fsr': '0', 'sharpen': '80', 'aniso': '16'},
}
# Home's quality slider and Graphics' preset cards (Simple view), in order.
PRESET_ORDER = ('low', 'balanced', 'high', 'ultra')
PRESET_NAMES = ('Low-end PC', 'Balanced', 'High', 'Ultra')
PRESET_NOTES = ('For 4-core laptops', 'Most PCs', 'For 1440p screens', 'For strong graphics cards')
# Short labels for choices shown side by side (Graphics); the long ones stay in the drop-downs.
SHORT_LABELS = {
    'render_scale': [('100', '720p'), ('150', '1080p'), ('200', '1440p'), ('300', '4K'), ('400', '5K')],
    'fps_cap': [('30', '30'), ('40', '40'), ('45', '45'), ('60', '60 FPS')],
    'vsync': [('auto', 'Auto'), ('0', 'Off'), ('1', 'On'), ('vrr', 'G-Sync / FreeSync')],
    'display_mode': [('window', 'Window'), ('fullscreen', 'Full screen')],
    'aspect': [('keep', 'Keep 16:9'), ('stretch', 'Stretch')],
    'vibrance': [('0', 'Off'), ('25', 'Low'), ('50', 'Medium'), ('75', 'High'), ('100', 'Max')],
    'sharpen': [('0', 'Off'), ('40', '40%'), ('60', '60%'), ('80', '80%'), ('100', '100%')],
    'aniso': [('0', "Game's"), ('2', '2x'), ('4', '4x'), ('8', '8x'), ('16', '16x')],
    'scale_filter': [('linear', 'Smooth'), ('nearest', 'Sharp'), ('fsr', 'FSR 1')],
}
# Home's "New in" line: three short items of this release.
NEWS = ('Big performance update', 'In-game settings menu: F1 or hold SELECT', '120 FPS (experimental)',
        'Trophies', 'Disable dynamic shadows', 'Long sessions no longer stop saving')

# Video. The renderer: RPCS3's RSX (ps3recomp gpu/, a build with
# -DASCENSION_RPCS3_RSX=ON) or live draw. On macOS, Vulkan is MoltenVK and
# live draw is Metal; the GL backend is WGL, so it is not offered there.
RENDERERS = ([('rpcs3-vulkan', 'RPCS3 · Vulkan (MoltenVK)'),
              ('metal', 'Live draw · Metal')] if MAC else
             [('rpcs3-vulkan', 'RPCS3 · Vulkan'), ('rpcs3-opengl', 'RPCS3 · OpenGL'),
              ('livedraw', 'Live draw · Direct3D 12')] if WINDOWS else
             [('rpcs3-vulkan', 'RPCS3 · Vulkan'), ('rpcs3-opengl', 'RPCS3 · OpenGL')])
RENDER_SCALES = [('100', 'HD · 1280 x 720 (original)'), ('150', 'Full HD · 1920 x 1080'),
                 ('200', '2K · 2560 x 1440'), ('300', '4K · 3840 x 2160'), ('400', '5K · 5120 x 2880')]
ASPECTS = [('keep', 'Keep 16:9 (black bars)'), ('stretch', 'Stretch to the window')]
VIBRANCE_LEVELS = [('0', 'Off'), ('25', 'Low'), ('50', 'Medium'), ('75', 'High'), ('100', 'Very high')]
SHARPEN_LEVELS = [('0', 'Off'), ('40', '40%'), ('60', '60%'), ('80', '80% (as RPCS3)'), ('100', '100%')]
SCALE_FILTERS = [('linear', 'Smooth (bilinear)'), ('nearest', 'Sharp (pixel)'),
                 ('fsr', 'AMD FSR 1 (RPCS3)')]
VSYNC_MODES = [('auto', "Automatic (the renderer's)"), ('1', 'On'), ('0', 'Off'),
               ('vrr', 'G-Sync / FreeSync (VRR)')]
ANISO_LEVELS = [('0', "The game's"), ('2', '2x'), ('4', '4x'), ('8', '8x'), ('16', '16x')]

# The game's own settings menu (F1 or holding SELECT while playing; ps3kit
# libs/video/rsx_ingame_menu.h) writes what the player changed there to this
# file, next to the launcher's settings; the game gets its path in
# PS3_INGAME_SETTINGS and never writes settings.json itself. The launcher takes
# the file in (merge_ingame) when it starts, before a game starts and when the
# game closes. The file holds the launcher's own keys and values:
#   {"format": 1, "settings": {"fsr": "aa", "sharpen": "60", "fps_cap": "30"}}
INGAME_FILE = CONFIG_FILE.parent / 'ingame-settings.json'
INGAME_CHOICES = {'fsr': FSR_MODES, 'smaa': ON_OFF, 'sharpen': SHARPEN_LEVELS, 'vibrance': VIBRANCE_LEVELS,
                  'fps_cap': FPS_CAPS, 'perf_overlay': ON_OFF, 'render_scale': RENDER_SCALES,
                  'aniso': ANISO_LEVELS, 'vsync': ON_OFF, 'no_shadows': ON_OFF}

# Controls: the PS3 pad's controls, grouped as the Controls page shows them,
# with what they do in the game where that is certain. The ids are
# ps3recomp's (PS3_KEYMAP / PS3_PADMAP, libs/input/pad_keymap.h).
CONTROL_GROUPS = [
    ('Left stick · walk', [('ls_up', 'Up'), ('ls_down', 'Down'), ('ls_left', 'Left'),
                                     ('ls_right', 'Right')]),
    ('Right stick · evade', [('rs_up', 'Up'), ('rs_down', 'Down'), ('rs_left', 'Left'),
                                      ('rs_right', 'Right')]),
    ('Buttons', [('cross', '✕  Jump'), ('square', '□  Light attack'), ('triangle', '△  Heavy attack'),
                ('circle', '○  Grab')]),
    ('Shoulders and triggers', [('l1', 'L1  Block'), ('r1', 'R1'), ('l2', 'L2'), ('r2', 'R2'),
                           ('l3', 'L3 (stick click)'), ('r3', 'R3 (stick click)')]),
    ('D-pad', [('up', '↑  Up'), ('down', '↓  Down'), ('left', '←  Left'), ('right', '→  Right')]),
    ('System', [('start', 'START  Pause'), ('select', 'SELECT')]),
]
PAD_CONTROLS = [c for _, items in CONTROL_GROUPS for c, _ in items if not c.startswith(('ls_', 'rs_'))]

KEY_PRESETS = {
    'pc': {   # WASD to walk, arrows to evade, the face buttons under the right hand and the mouse
        'ls_up': ['W'], 'ls_down': ['S'], 'ls_left': ['A'], 'ls_right': ['D'],
        'rs_up': ['Up'], 'rs_down': ['Down'], 'rs_left': ['Left'], 'rs_right': ['Right'],
        'cross': ['Space', 'K'], 'square': ['J', 'Mouse1'], 'triangle': ['I', 'Mouse2'], 'circle': ['L', 'C'],
        'l1': ['LShift', 'Q'], 'r1': ['E', 'Mouse4'], 'l2': ['Z'], 'r2': ['R', 'Mouse5'],
        'l3': ['LCtrl'], 'r3': ['V'],
        'up': ['T'], 'down': ['G'], 'left': ['F'], 'right': ['H'],
        'start': ['Enter', 'Escape'], 'select': ['Tab', 'Backspace'],
    },
    'classic': {   # the runtime's old fixed map: arrows, Z/X/A/S
        'up': ['Up'], 'down': ['Down'], 'left': ['Left'], 'right': ['Right'],
        'ls_up': ['Up'], 'ls_down': ['Down'], 'ls_left': ['Left'], 'ls_right': ['Right'],
        'cross': ['Z'], 'circle': ['X'], 'square': ['A'], 'triangle': ['S'],
        'l1': ['Q'], 'r1': ['W'], 'l2': ['1'], 'r2': ['2'], 'start': ['Enter'], 'select': ['Tab'],
    },
}
PAD_DEFAULTS = {'up': 'up', 'down': 'down', 'left': 'left', 'right': 'right', 'cross': 'a', 'circle': 'b',
                'square': 'x', 'triangle': 'y', 'l1': 'lb', 'r1': 'rb', 'l2': 'lt', 'r2': 'rt',
                'l3': 'ls', 'r3': 'rs', 'start': 'start', 'select': 'back'}
PAD_BUTTONS = [('a', 'A  ·  ✕'), ('b', 'B  ·  ○'), ('x', 'X  ·  □'), ('y', 'Y  ·  △'),
               ('lb', 'LB  ·  L1'), ('rb', 'RB  ·  R1'), ('lt', 'LT  ·  L2'), ('rt', 'RT  ·  R2'),
               ('ls', 'Left click  ·  L3'), ('rs', 'Right click  ·  R3'),
               ('back', 'View / Back  ·  Share'), ('start', 'Menu / Start  ·  Options'),
               ('guide', 'Xbox / PS'), ('up', 'D-pad ↑'), ('down', 'D-pad ↓'),
               ('left', 'D-pad ←'), ('right', 'D-pad →'), ('none', '(none)')]
DEADZONES = [(str(v), f'{v}%' + (' (default)' if v == 24 else '')) for v in (0, 5, 10, 15, 20, 24, 30, 40, 50)]

# Key names as ps3recomp reads them, from Tk's keysyms.
TK_KEYS = {
    'space': 'Space', 'Return': 'Enter', 'KP_Enter': 'Enter', 'Tab': 'Tab', 'ISO_Left_Tab': 'Tab',
    'BackSpace': 'Backspace', 'Escape': 'Escape', 'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right',
    'Shift_L': 'LShift', 'Shift_R': 'RShift', 'Control_L': 'LCtrl', 'Control_R': 'RCtrl',
    'Alt_L': 'LAlt', 'Alt_R': 'RAlt', 'Option_L': 'LAlt', 'Option_R': 'RAlt',
    'comma': 'Comma', 'period': 'Period', 'slash': 'Slash', 'semicolon': 'Semicolon',
    'apostrophe': 'Quote', 'quoteright': 'Quote', 'bracketleft': 'LBracket', 'bracketright': 'RBracket',
    'minus': 'Minus', 'equal': 'Equals', 'grave': 'Grave', 'quoteleft': 'Grave', 'backslash': 'Backslash',
    'Insert': 'Insert', 'Delete': 'Delete', 'Home': 'Home', 'End': 'End', 'Prior': 'PageUp',
    'Next': 'PageDown', 'Caps_Lock': 'CapsLock',
    **{f'KP_{i}': f'Num{i}' for i in range(10)}, **{f'F{i}': f'F{i}' for i in range(1, 13)},
}
KEY_LABELS = {
    'Space': 'Space', 'Escape': 'Esc', 'Up': '↑', 'Down': '↓', 'Left': '←', 'Right': '→',
    'LShift': 'Left Shift', 'RShift': 'Right Shift', 'LCtrl': 'Left Control', 'RCtrl': 'Right Control',
    'LAlt': 'Left Option' if MAC else 'Left Alt', 'RAlt': 'Right Option' if MAC else 'Right Alt',
    'Comma': ',', 'Period': '.', 'Slash': '/', 'Semicolon': ';', 'Quote': "'", 'LBracket': '[',
    'RBracket': ']', 'Minus': '-', 'Equals': '=', 'Grave': '`', 'Backslash': '\\',
    'PageUp': 'Page Up', 'PageDown': 'Page Down', 'CapsLock': 'Caps Lock',
    'Mouse1': '🖱 Left', 'Mouse2': '🖱 Right', 'Mouse3': '🖱 Middle', 'Mouse4': '🖱 Button 4',
    'Mouse5': '🖱 Button 5', **{f'Num{i}': f'Num {i}' for i in range(10)},
}


def key_label(name):
    return KEY_LABELS.get(name, name) if name else '—'


GAME_EXE = ROOT / 'build' / 'win' / 'AscensionRecomp.exe' if WINDOWS else ROOT / 'build' / 'mac' / 'AscensionRecomp'
# The game is started as `AscensionRecomp.exe <EBOOT.ELF>` from ascension/ (the
# saves are ./gamedata/...), as scripts\windows\run.ps1 does. On Windows the
# disc is extracted to work/disc and the EBOOT.ELF it loads is the patched one
# apply-patches.ps1 writes there (work/pristine/EBOOT.ELF is the unpatched one).
# On macOS the disc stays an .iso, mounted read-only at work/disc when the game
# starts; the decrypted EBOOT.ELF sits in ascension/input/.
EBOOT = (WORK / 'disc' / 'PS3_GAME' / 'USRDIR' / 'EBOOT.ELF') if WINDOWS else ROOT / 'input' / 'EBOOT.ELF'


def applied_patches():
    """The RPCS3 patches the current build was made with (apply-patches.ps1
    writes build/patches-applied.json), or None when it was never run."""
    try:
        data = json.loads((ROOT / 'build' / 'patches-applied.json').read_text(encoding='utf-8'))
        return [str(p['name']) for p in data.get('applied', []) if isinstance(p, dict) and p.get('name')]
    except (OSError, ValueError, AttributeError):
        return None


def fps_unlocked(names=None):
    """The game has the Unlock FPS patch (ported to 1.00): its clock follows
    real time, so it can run at 60 without running fast. names: the patches
    on when the game switches them as it starts (patches.patches_on); None:
    what the build was made with."""
    return 'Unlock FPS' in (names if names is not None else (applied_patches() or []))


def fps_limit(s=None, names=None):
    """The frame rate limit the game gets: the chosen cap (fps_cap) with Unlock
    FPS on, else the fixed 30."""
    if not fps_unlocked(names):
        return FPS_LIMIT
    cap = str((s or {}).get('fps_cap', '60'))
    return cap if cap in dict(FPS_CAPS) else '60'


DEFAULTS = {
    'display_mode': 'window', 'window_size': '1280x720', 'perf_overlay': '1', 'fps_cap': '60',
    'renderer': RENDERERS[0][0], 'render_scale': '200', 'smaa': '1', 'fsr': '0', 'sharpen': '80', 'vibrance': '50', 'aspect': 'keep',
    'scale_filter': 'linear',
    'vsync': 'auto', 'display_match': '1', 'aniso': '0', 'skip_movies': '1', 'settings_version': 2,
    'title_id': '',   # the prepared disc: BCES01741 (Europe) or BCUS98232 (USA); '' = not known yet (Europe)
    'esc_quits': '0', 'language': '18', 'voice_language': 'same', 'enter_button': 'cross', 'subtitles': '1',
    'keymap': KEY_PRESETS['pc'], 'padmap': PAD_DEFAULTS, 'pad_deadzone': '24',
    'mods_dir': str(ROOT / 'mods'), 'mods_enabled': ['@skip-intro'], 'mods_order': [],
    'rpcs3_patches': [],
    # '1' once the patches were ticked on a game that switches them as it
    # starts: until then nothing is passed and the game's own defaults apply.
    'patches_chosen': '0',
    'game_exe': str(GAME_EXE), 'eboot': str(EBOOT),
    'disc_dir': '' if WINDOWS else str(WORK / 'disc'), 'disc_iso': '', 'game_source': '', 'port_hdd': '0',
    'host_pin': '1',   # PS3_HOST_PIN: thread placement (10+ cores and uniform 6c/12t; the game turns it off elsewhere, Intel P+E CPUs included)
    'precompile': '1',  # LD_PSO_PRECOMPILE: build the recorded shaders before the game starts
    'mp_mode': 'trial1', 'mp_map': 'cycle',   # PS3_MP_MODE / PS3_MP_MAP: what Offline Multiplayer's Quick Play starts
    'occlusion': '1',   # OCCL: occlusion culling (the game's hidden-object tests answered; on by default, user 2026-11-05)
    'ambient_occlusion': '0',   # LD_AO: soft contact shadows (2026-11-15, off by default)
    'no_shadows': '0',   # PS3_NO_SHADOWS=1: the game draws no dynamic shadow maps (2026-11-22, flatter look, off by default)
    'gp_pull': '0',   # GP_PULL=1 + W_VDIRECT=3: GPU vertex decoding (2026-11-22, 0.2.0, experimental, off by default)
    'smooth_glow': '1',   # LD_FULLRES_FX: full-resolution glow above 1x (on in the game by default; '0' sets LD_FULLRES_FX=0)
    'uber_shaders': '0',   # LD_UBER + LD_UBER_BUDGET + LD_UBER_FAST: ubershader stand-ins while a shader is built (host uber5; NVIDIA only, off by default)
    'cpu_limit': '0',  # PS3_CPU_LIMIT: testing only, run on fewer cores (0 = all)
    'game_window': 'own', 'hide_while_playing': '0',
    'backup_saves': '1', 'music': '1', 'extra_env': '', 'ui_language': 'auto',
    'graphics_view': 'simple',   # Graphics page: the six main settings, or all of them
    'sound_mix_104': '0',   # the 1.04 sound mix (app/upd104.py), from the player's own update package
    'update_pkg': '',       # that package: the official 1.04 update .pkg (US or EU)
    'check_updates': '1',   # ask GitHub for a newer release at start (app/updater.py)
    'update_token': '',     # a GitHub token: the repository is private
    # Community online multiplayer (Online page; app/game.py online_environment)
    'online': '0', 'online_server': '', 'online_name': '', 'online_port': '3658', 'online_party': '',
    'online_exe': '',       # the online build while it is separate from game_exe; '' = game_exe
    'online_discord': '1', 'online_discord_exe': '',   # Discord presence helper; '' = <port>\\discord\\gowa_discord.exe
}


def asset(name):
    for folder in ASSET_DIRS:
        if (folder / name).is_file():
            return folder / name
    return None


def _other_port_folder(value):
    """A path inside some port folder (an ascension-recomp\\ tree) other than
    this release's own."""
    try:
        path = Path(str(value)).resolve()
    except (OSError, ValueError):
        return False
    if 'ascension-recomp' not in [p.lower() for p in path.parts]:
        return False
    try:
        path.relative_to(INSTALL.resolve())
        return False
    except ValueError:
        return True


def load_settings():
    data = None
    for f in (CONFIG_FILE, SHARED_CONFIG_FILE) if RELEASE else (CONFIG_FILE,):
        try:
            data = json.loads(f.read_text(encoding='utf-8'))
            break
        except (OSError, ValueError):
            continue
    data = data or {}
    if RELEASE:
        # copied from the shared settings or another folder: this folder's own
        for key in ('game_exe', 'eboot', 'mods_dir'):
            if key in data and _other_port_folder(data[key]):
                del data[key]
    s = {**DEFAULTS, **data}
    if 'rpcs3_patches' not in data:
        # apply-patches.ps1 -FromLauncher applies exactly this list: start
        # from what the game is built with, so a first Apply keeps it.
        s['rpcs3_patches'] = applied_patches() or []
    if not isinstance(s['rpcs3_patches'], list):
        s['rpcs3_patches'] = []
    if s['game_window'] not in dict(GAME_WINDOWS):
        s['game_window'] = 'own'
    if s['voice_language'] not in dict(VOICE_LANGUAGES):
        s['voice_language'] = 'same'
    if s['mp_mode'] not in dict(MP_MODES):
        s['mp_mode'] = 'trial1'
    if s['mp_map'] not in dict(mp_map_choices(s['mp_mode'])):
        s['mp_map'] = 'cycle'
    keymap = s['keymap'] if isinstance(s['keymap'], dict) else {}
    s['keymap'] = {c: [k for k in keymap.get(c, [])][:2] for _, items in CONTROL_GROUPS for c, _ in items}
    padmap = s['padmap'] if isinstance(s['padmap'], dict) else {}
    s['padmap'] = {c: padmap.get(c, PAD_DEFAULTS[c]) for c in PAD_CONTROLS}
    if s['renderer'] not in dict(RENDERERS):
        s['renderer'] = RENDERERS[0][0]
    if data.get('settings_version', 1) < 2:
        # Before version 2 the internal resolution was never offered for the
        # live renderer, so a stored one is the old default, not a choice.
        s['render_scale'] = DEFAULTS['render_scale']
        s['settings_version'] = 2
    return s


def merge_ingame(s, path=None):
    """Take the in-game menu's file into the settings `s` (changed in place).
    Only the keys of INGAME_CHOICES with a value from the launcher's own list
    are taken; anything else in the file is ignored. The file is deleted once
    it was read. Returns the keys that changed with their new values ({} when
    there was no file or nothing new): the caller saves `s` then."""
    path = Path(path) if path else INGAME_FILE
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except OSError:
        return {}
    except ValueError:
        data = None
    try:
        path.unlink()
    except OSError:
        pass
    settings = data.get('settings') if isinstance(data, dict) and data.get('format') == 1 else None
    if not isinstance(settings, dict):
        return {}
    changed = {}
    for key, value in settings.items():
        value = str(value)
        if key not in INGAME_CHOICES or value not in dict(INGAME_CHOICES[key]):
            continue
        if key == 'vsync':
            # The game only knows on and off. "Automatic" already means on and
            # "G-Sync / FreeSync" already means off: those choices stay.
            if (value == '1') == (s.get('vsync', 'auto') in ('auto', '1')):
                continue
        if str(s.get(key)) != value:
            s[key] = value
            changed[key] = value
    return changed


def save_settings(values):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG_FILE.with_suffix('.tmp')
    tmp.write_text(json.dumps(values, indent=2, ensure_ascii=False), encoding='utf-8')
    tmp.replace(CONFIG_FILE)

# The community servers the Online page always offers (2026-11-13); each one's own list (GET /api/servers,
# kept on its admin page) is added when the page loads. (name, address, region)
KNOWN_SERVERS = ()   # public source: no built-in server addresses (type one, use an invite code, or a LAN server)
