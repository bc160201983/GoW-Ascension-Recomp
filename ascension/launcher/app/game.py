"""The disc, mods, saves and environment the game starts with."""

import hashlib
import json
import re
import os
import shutil
import struct
import stat
import subprocess
import threading
import time
from pathlib import Path
from .config import (CONFIG_DIR, FPS_LIMIT, GAME_EXE, GAME_ID, GAME_IDS, HERE, INGAME_FILE, KNOWN_SWITCHES, LANGUAGES, LANGUAGES_US, MAC, MP_MODES,
                     ROOT, SWITCHES, US_ID, fps_limit, mp_map_choices, WINDOWS, asset,
                     INSTALL, SHARED_CONFIG_FILE, VERSION)
from .i18n import current, tr

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def save_dir():
    return ROOT / 'gamedata' / 'dev_hdd0' / 'home' / '00000001' / 'savedata'


def mp_profile_dir():
    """The multiplayer profile (Offline Multiplayer patch): the game's online
    storage (TUS), kept in files by the runtime beside the saves."""
    return ROOT / 'gamedata' / 'dev_hdd0' / 'home' / '00000001' / 'tus'


_switch_cache = {}


def runtime_switches(exe):
    """The environment switches (config.SWITCHES) this build of the game
    reads: each getenv() name is a NUL-terminated string in the executable.
    Kept in CONFIG_DIR/runtime-switches.json while the executable is the same
    (reading the ~150 MB file takes a moment). Without the executable: what
    the source read when this launcher was written (KNOWN_SWITCHES)."""
    path = Path(exe)
    try:
        st = path.stat()
    except OSError:
        return set(KNOWN_SWITCHES)
    key = f'{path.resolve()}|{st.st_size}|{int(st.st_mtime)}'
    if key in _switch_cache:
        return _switch_cache[key]
    cache = CONFIG_DIR / 'runtime-switches.json'
    try:
        data = json.loads(cache.read_text(encoding='utf-8'))
        if data.get('key') == key and data.get('names') == list(SWITCHES):
            _switch_cache[key] = set(data['found'])
            return _switch_cache[key]
    except (OSError, ValueError, AttributeError, KeyError):
        pass
    try:
        blob = path.read_bytes()
    except OSError:
        return set(KNOWN_SWITCHES)
    found = {n for n in SWITCHES if n.encode('ascii') + b'\0' in blob}
    del blob
    _switch_cache[key] = found
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps({'key': key, 'names': list(SWITCHES), 'found': sorted(found)}),
                         encoding='utf-8')
    except OSError:
        pass
    return found


def game_running(exe):
    """Whether a copy of the game is already running (started by this launcher
    or elsewhere, e.g. scripts\\windows\\run.ps1): only one may run at a time."""
    # Development PCs: while a save-protected test run has the player's save slot swapped out
    # (build/SAVE_RESTORE_PENDING, removed when the slot is put back and verified), PLAY must not
    # start a second game on the test save. The file never exists in a player's install.
    try:
        if (Path(exe).resolve().parent.parent / 'SAVE_RESTORE_PENDING').exists():
            return True
    except OSError:
        pass
    if not WINDOWS:
        return False
    try:
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {Path(exe).name}', '/NH', '/FO', 'CSV'],
                             capture_output=True, timeout=5, creationflags=NO_WINDOW).stdout or b''
    except (OSError, subprocess.SubprocessError):
        return False
    # Bytes, not text: tasklist answers in the console's code page (a Russian Windows says
    # "ИНФОРМАЦИЯ: ..." in cp866), which the launcher's Python (UTF-8 mode) cannot decode;
    # that error stopped PLAY before the game started, with no message and no log. The
    # image name in the CSV is ASCII in every language.
    return f'"{Path(exe).name.lower()}"'.encode('ascii', 'replace') in out.lower()


def _post_close(pid):
    """WM_CLOSE to every window of process `pid`: its own top-level windows and,
    embedded, the child window it renders into inside the launcher. Returns how
    many it asked."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (WNDENUMPROC, wintypes.LPARAM)
    user32.EnumChildWindows.argtypes = (wintypes.HWND, WNDENUMPROC, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    found = []

    def mine(hwnd):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        return owner.value == pid

    def child(hwnd, _):
        if mine(hwnd):
            found.append(hwnd)
        return True

    def top(hwnd, _):
        if mine(hwnd):
            found.append(hwnd)
        user32.EnumChildWindows(hwnd, WNDENUMPROC(child), 0)
        return True

    user32.EnumWindows(WNDENUMPROC(top), 0)
    for hwnd in found:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    return len(found)


def close_gently(proc, timeout=20, pump=None):
    """End the game the way its window's close button does, so it saves the
    pipelines it compiled this session ("[boot] game window closed"); a
    terminate skipped that and the next session compiled them all again, with
    parts of an area undrawn meanwhile (tester logs, 2026-11-04). Terminate,
    then kill, only if it is still running after `timeout` seconds.
    `pump` (the launcher's window update) runs while waiting: a game rendering
    inside the launcher's window needs that window to answer it while it
    closes, and a blocked wait left both waiting until the timeout (then the
    game was killed without its save)."""
    if proc is None:
        return
    if hasattr(proc, 'stop'):
        proc.stop()                  # WatchedGame: no boot-hang restart now
    if proc.poll() is not None:
        return                       # already ended
    if WINDOWS:
        try:
            if _post_close(proc.pid):
                deadline = time.monotonic() + timeout
                while proc.poll() is None and time.monotonic() < deadline:
                    if pump:
                        pump()
                    time.sleep(0.03)
                if proc.poll() is not None:
                    return
        except (OSError, AttributeError, ValueError):
            pass
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()


def icon_file():
    """icon-2.png as an .ico (window, taskbar and the game's window); on
    macOS the .png itself (the game's Dock icon)."""
    src = asset('icon-2.png')
    if not src or not WINDOWS:
        return src
    ico = CONFIG_DIR / 'icon.ico'
    try:
        if not ico.is_file() or ico.stat().st_mtime < src.stat().st_mtime:
            from PIL import Image
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            with Image.open(src) as im:
                im.convert('RGBA').save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
        return ico
    except OSError:
        return None


BUILTIN_MODS = ROOT / 'mods-builtin'
GENERATED_DIR = CONFIG_DIR / 'generated'


def read_mod_json(folder):
    try:
        data = json.loads((Path(folder) / 'mod.json').read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def mod_text(meta, field):
    """A mod.json field in the launcher's language: "translations": {"en":
    {"name": ..., "description": ...}} beside the field itself."""
    lang = current()
    table = meta.get('translations') if isinstance(meta.get('translations'), dict) else {}
    for code in (lang, lang.split('-')[0]):
        value = (table.get(code) or {}).get(field) if isinstance(table.get(code), dict) else None
        if value:
            return str(value)
    return str(meta.get(field, ''))


def all_mods(s):
    """Every mod the launcher knows, in priority order (first wins): the ones
    that come with Ascension (mods-builtin, ids '@<folder>') and the user's
    (the mods folder, ids = folder names). Each is a dict: id, path, name,
    description, builtin, meta (its mod.json)."""
    found = {}
    if BUILTIN_MODS.is_dir():
        for d in sorted(BUILTIN_MODS.iterdir()):
            if d.is_dir():
                found['@' + d.name] = (d, True)
    user = Path(s['mods_dir'])
    if user.is_dir():
        for d in sorted(user.iterdir()):
            if d.is_dir() and d.name not in found:
                found[d.name] = (d, False)
    order = [m for m in s.get('mods_order', []) if m in found]
    order += [m for m in found if m not in order]
    mods = []
    for mid in order:
        path, builtin = found[mid]
        meta = read_mod_json(path)
        mods.append({'id': mid, 'path': path, 'builtin': builtin, 'meta': meta,
                     'name': mod_text(meta, 'name') or path.name,
                     'description': mod_text(meta, 'description')})
    return mods


def enabled_mod_list(s):
    on = set(s.get('mods_enabled', []))
    return [m for m in all_mods(s) if m['id'] in on]


def ptbr_mod(s):
    """An enabled mod that brings Brazilian Portuguese (tools/make_ptbr_mod.py
    builds one from the US disc): BR_PORTUGUESE.PSARC, the US voices, and the
    language-table entry "17 pt-br BR_PORTUGUESE" (the text, R_LANG_PT-BR, is
    already on the European disc)."""
    for m in enabled_mod_list(s):
        if (m['path'] / 'BR_PORTUGUESE.PSARC').is_file() or str(m['meta'].get('language', '')).lower() == 'pt-br':
            return m
    return None


def title_id(s):
    """The title ID of the disc the game plays from: BCES01741 (Europe) or BCUS98232 (USA). The one
    gamefiles.prepare stored, else the disc's PARAM.SFO, else the European one."""
    t = str(s.get('title_id') or '')
    if t in GAME_IDS:
        return t
    try:
        info = disc_info(s)
    except Exception:
        info = None
    return info[0] if info and info[0] in GAME_IDS else GAME_ID


def language_options(s):
    """The disc's own languages. On the European disc Brazilian Portuguese is offered only with a
    mod that brings it; the US disc has it."""
    if title_id(s) == US_ID:
        return LANGUAGES_US
    if ptbr_mod(s):
        return LANGUAGES
    return [o for o in LANGUAGES if o[0] != '17']


def language_fallback(s):
    """The language a setting falls back to when the disc does not have it."""
    if title_id(s) == US_ID:
        return '1'
    return '7' if s.get('language') == '17' else '18'


def effective_language(s):
    lang = str(s['language'])
    if lang not in dict(language_options(s)):
        return language_fallback(s)
    return lang


def apply_text_ops(data, ops):
    """One archive file changed by a mod: {"text": "..."} replaces it,
    {"insert_after": "<line>", "line": "<new line>"} adds a line (once)."""
    for op in ops:
        if 'data_file' in op:       # a whole file the launcher prepared (upd104: the 1.04 sound mix)
            data = Path(op['data_file']).read_bytes()
        elif 'text' in op:
            data = op['text'].encode('utf-8')
        elif 'insert_after' in op and 'line' in op:
            eol = b'\r\n' if b'\r\n' in data else b'\n'
            anchor, line = op['insert_after'].encode('utf-8'), op['line'].encode('utf-8')
            lines = data.split(eol)
            if line not in lines and anchor in lines:
                i = lines.index(anchor)
                lines.insert(i + 1, line)
                data = eol.join(lines)
    return data


def disc_root(s):
    """The disc's folder (the one holding PS3_GAME) when one is set, else None:
    on Windows the EBOOT.ELF's own disc decides."""
    d = s.get('disc_dir', '')
    return Path(d) if d else None


def disc_usrdir(s):
    root = disc_root(s)
    return root / 'PS3_GAME' / 'USRDIR' if root else Path(s['eboot']).parent


def eu_usrdir(s):
    """The European disc's PS3_GAME\\USRDIR, for tools that need its archives (the PT-BR mod builder needs its
    GAME.PSARC and LOCALIZED_ARCHIVES): the disc folder or the mounted .iso the game plays from, else the
    folder of the EBOOT.ELF when that is an extracted disc. (The EBOOT.ELF the launcher decrypts itself lives
    in its own work folder, without the disc's archives: a tester, 2026-11-09.)"""
    if WINDOWS and s.get('disc_iso'):
        from . import gamefiles
        root = gamefiles.ensure_mounted(s)     # NotReady with the reason when it cannot
        if root:
            return root / 'PS3_GAME' / 'USRDIR'
    usr = disc_usrdir(s)
    if (usr / 'GAME.PSARC').is_file():
        return usr
    alt = Path(s.get('eboot') or '.').parent
    return alt if (alt / 'GAME.PSARC').is_file() else usr


def disc_ps3_game(s):
    """The disc's PS3_GAME folder (PARAM.SFO, PIC1.PNG, ICON0.PNG, SND0.AT3)."""
    return disc_usrdir(s).parent


def read_sfo(path):
    """A PARAM.SFO's entries: {key: text or number}."""
    data = Path(path).read_bytes()
    if data[:4] != b'\0PSF':
        raise ValueError('not a PARAM.SFO')
    keys, values, count = struct.unpack_from('<III', data, 8)
    out = {}
    for i in range(count):
        koff, fmt, length, _cap, voff = struct.unpack_from('<HHIII', data, 20 + 16 * i)
        key = data[keys + koff:data.index(b'\0', keys + koff)].decode('ascii', 'replace')
        raw = data[values + voff:values + voff + length]
        out[key] = int.from_bytes(raw[:4], 'little') if fmt == 0x0404 else raw.rstrip(b'\0').decode('utf-8', 'replace')
    return out


def disc_problem(s):
    """Why the disc found is not the one the game was built from, or None
    (also when there is no disc to look at yet)."""
    root = disc_root(s)
    sfo = (root / 'PS3_GAME' if root else Path(s['eboot']).parent.parent) / 'PARAM.SFO'
    if not sfo.is_file():
        return None
    try:
        info = read_sfo(sfo)
    except (OSError, ValueError, struct.error):
        return tr("the disc's PARAM.SFO could not be read ({path})", path=sfo)
    title = str(info.get('TITLE_ID', ''))
    if title not in GAME_IDS:
        name = str(info.get('TITLE', '')).replace('\n', ' ')
        title = f'{title or "?"} ({name})' if name else (title or '?')
        return tr('the disc is {title}, not the European ({id}) or US ({us}) release',
                  title=title, id=GAME_ID, us=US_ID)
    return None


def disc_info(s):
    """(title id, version, region) of the disc found, or None."""
    sfo = disc_ps3_game(s) / 'PARAM.SFO'
    try:
        info = read_sfo(sfo)
    except (OSError, ValueError, struct.error):
        return None
    title = str(info.get('TITLE_ID', '')) or '?'
    region = {'E': 'Europe', 'U': 'America', 'J': 'Japan', 'A': 'Asia', 'K': 'Korea'}.get(title[2:3], '')
    return title, str(info.get('APP_VER') or info.get('VERSION') or '?').lstrip('0') or '?', region


def ensure_disc(s):
    """macOS: mount the .iso read-only at the disc folder if it is not there yet.
    Returns the folder when this call mounted it (release_disc undoes that)."""
    root = disc_root(s)
    if not root or (root / 'PS3_GAME').is_dir():
        return None
    iso = s.get('disc_iso', '')
    if MAC and iso and Path(iso).is_file():
        root.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', str(root), iso],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise OSError('hdiutil: ' + (r.stderr or r.stdout).strip())
        return root
    return None


def release_disc(root):
    """Unmount a disc ensure_disc mounted; a busy one stays (nothing is lost)."""
    if root and MAC:
        subprocess.run(['hdiutil', 'detach', str(root)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


MODS_LOCK = threading.Lock()     # PLAY and the build after Save write the same archives


def compose_mods(s, log=lambda m: None):
    with MODS_LOCK:
        return _compose_mods(s, log)


GENERATED_ONLINE_DIR = CONFIG_DIR / 'generated-online'
# Mods that may stay on online: MEASURED 2026-11-13 not to change the game's data hash (the matchmaking key):
# skip-intro (empties /INTRO/BOOT_*.TXT; run n2-us-lobby-20261113-233752, US disc: F8C0B979 = the disc's).
ONLINE_SAFE_MODS = {'@skip-intro'}


def compose_online_mods(s, log=lambda m: None):
    """Online play loads no mods (every player must have the same data: it is in the matchmaking key), except the
    1.04 sound mix and the ONLINE_SAFE_MODS (2026-11-13, MEASURED): with only MASTER_MIXER.GMX or the intro list
    changed the game's data hash stays the disc's (US F8C0B979), so players with and without them still meet.
    Built in its own folder, so switching online on and off does not rebuild the offline mods' archive."""
    safe = [m for m in enabled_mod_list(s) if m['id'] in ONLINE_SAFE_MODS]
    with MODS_LOCK:
        return _compose_mods(s, log, mods=safe, out_dir=GENERATED_ONLINE_DIR)


def prebuild_mods(s):
    """The mods' archives built ahead of PLAY, when the disc is there."""
    try:
        if disc_usrdir(s).is_dir():
            compose_mods(s)
    except Exception:       # PLAY builds them again and reports what fails
        pass


def _compose_mods(s, log, mods=None, out_dir=None):
    """The folders for PS3_MODS: first the archives the launcher builds from
    the enabled mods' recipes ("psarc_patches" in mod.json, applied to the
    copy of that archive the highest-priority mod ships, or to the disc's),
    then every enabled mod's own files. Rebuilt only when an input changes."""
    mods = enabled_mod_list(s) if mods is None else mods
    out_dir = out_dir or GENERATED_DIR
    usrdir = disc_usrdir(s)
    from . import upd104
    extra = upd104.psarc_ops(s)      # the 1.04 sound mix (Mods page): below every mod, so a mod has the last word
    wanted = {archive: [] for archive in extra}
    for m in mods:
        for archive, files in (m['meta'].get('psarc_patches') or {}).items():
            wanted.setdefault(archive, [])
    dirs = []
    if wanted:
        out_dir.mkdir(parents=True, exist_ok=True)
        import importlib.util
        spec = importlib.util.spec_from_file_location('psarc_replace', ROOT / 'tools' / 'psarc_replace.py')
        pr = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pr)
        for archive in wanted:
            base = next((m['path'] / archive for m in mods if (m['path'] / archive).is_file()), usrdir / archive)
            # lowest priority first, so the first mod in the list has the last word
            ops = {}
            for inner, op in (extra.get(archive) or {}).items():
                ops.setdefault(inner, []).append(op)
            for m in reversed(mods):
                for inner, op in ((m['meta'].get('psarc_patches') or {}).get(archive) or {}).items():
                    op = op if isinstance(op, dict) else {'text': str(op)}
                    if 'data_file' not in op:       # the launcher's own; not from a mod.json
                        ops.setdefault(inner, []).append(op)
            st = base.stat()
            key = json.dumps([str(base), st.st_size, int(st.st_mtime), ops], sort_keys=True)
            out = out_dir / archive
            keyf = out_dir / (archive + '.key')
            if not (out.is_file() and keyf.is_file() and keyf.read_text(encoding='utf-8') == key):
                log(f'montando {archive}')
                src = pr.Psarc(str(base))
                replace = {inner: apply_text_ops(src.read(src.index_of(inner)), oplist)
                           for inner, oplist in ops.items() if inner in src.names}
                pr.write(src, str(out), replace)
                keyf.write_text(key, encoding='utf-8')
        dirs.append(str(out_dir))
    dirs += [str(m['path']) for m in mods]
    stub = tpps_stub_dir(usrdir, log)
    if stub:
        dirs.insert(0, stub)          # first = wins over every mod and the disc
    return dirs


TPPS_STUB_DIR = CONFIG_DIR / 'tpps-stub'
TPPS_STUB_NAME = b'/WAD/PS3_BE/TPPS-STUB.TXT'


def tpps_stub_bytes():
    """The disc's TPPS.PSARC (121 bytes, the same on BCES01741 and BCUS98232): a PSARC whose only file is the empty
    /WAD/PS3_BE/TPPS-STUB.TXT. Built here, byte for byte equal to the discs' (checked 2026-11-16)."""
    name = TPPS_STUB_NAME
    hdr = struct.pack('>4sI4sIIIII', b'PSAR', 0x00010004, b'zlib', 0x60, 30, 2, 0x10000, 2)
    e0 = bytes(16) + struct.pack('>I', 0) + len(name).to_bytes(5, 'big') + (0x60).to_bytes(5, 'big')
    e1 = (hashlib.md5(name).digest() + struct.pack('>I', 1) + (0).to_bytes(5, 'big')
          + (0x60 + len(name)).to_bytes(5, 'big'))
    return hdr + e0 + e1 + struct.pack('>HH', len(name), 0) + name


def tpps_stub_dir(usrdir, log=lambda m: None):
    """A folder for PS3_MODS with the disc's TPPS.PSARC stub, when the game folder has another TPPS.PSARC.
    Copies of the game with an official update merged in (1.04 and later) carry the update's 205 MB TPPS.PSARC
    in place of the disc's stub. The 1.00 program mounts it (it is the last line of ARCHIVES), so its new DCB files
    and DEPENDENCYGRAPH.IFF replace the disc's, but it cannot apply the update's .WAD.PATCH files: the multiplayer
    player spawn then builds an object from a type the 1.00 data does not have, calls a NULL function pointer 400
    times and the match freezes on its first frame (tester log 2026-11-16, reproduced on the US disc with the
    1.04 TPPS.PSARC). The stub keeps the 1.00 data as the disc has it, offline and online (same data hash)."""
    try:
        f = usrdir / 'TPPS.PSARC'
        if not f.is_file() or f.stat().st_size == 121:
            return None
        TPPS_STUB_DIR.mkdir(parents=True, exist_ok=True)
        out = TPPS_STUB_DIR / 'TPPS.PSARC'
        data = tpps_stub_bytes()
        if not (out.is_file() and out.read_bytes() == data):
            out.write_bytes(data)
        log(f'TPPS.PSARC from an update ({f.stat().st_size} bytes): the disc stub is used')
        return str(TPPS_STUB_DIR)
    except OSError:
        return None


def keymap_env(keymap):
    """PS3_KEYMAP: "cross=Space,K;square=J,Mouse1;..." (libs/input/pad_keymap.h)."""
    return ';'.join(f'{c}=' + ','.join(keys) for c, keys in keymap.items() if keys)


def padmap_env(padmap):
    return ';'.join(f'{c}={b}' for c, b in padmap.items())


def online_on(s):
    """Community online multiplayer is switched on (Online page)."""
    return s.get('online') == '1'


def play_exe(s):
    """The executable to start: the online build while online is on and that file exists, else game_exe."""
    exe = s.get('online_exe', '')
    if online_on(s) and exe and Path(exe).is_file() and not port_dlls_missing(exe):
        return exe
    base = installed_exe(s['game_exe'])
    beside = Path(base).with_name('AscensionRecomp.online.exe')   # the tester package puts it there
    if online_on(s) and beside.is_file():
        return str(beside)
    return base


def installed_exe(exe):
    """game_exe, or this install's own build\\win game when that setting points at a file that is gone or has no
    port DLLs beside it (2026-11-13: the settings live in %APPDATA%, so a tester's fresh install still used the old
    install's path and PLAY reported every DLL missing)."""
    if Path(exe).is_file() and not port_dlls_missing(exe):
        return exe
    own = Path(GAME_EXE)
    if own.is_file() and not port_dlls_missing(own):
        return str(own)
    return exe



# ---- LAN play (2026-11-15) ----------------------------------------------------------------------------------
# A player can host the community server on their own PC for the players on the same network: the launcher starts
# it with its own Python (nothing to install), and every launcher on the network finds it by a UDP broadcast
# ("GOWA?1" to port 36104; the server answers with its name and address, server gowa_server.LanBeacon).
LAN_DISCOVERY_PORT = 36104
_lan_server = {'proc': None, 'address': ''}


def server_dir():
    """The community server's code: ascension\\server in a player package, host\\mpnet\\server in the source tree."""
    for d in (ROOT / 'server', ROOT / 'host' / 'mpnet' / 'server'):
        if (d / 'gowa_server.py').is_file():
            return d
    return None


def lan_ip():
    """This PC's address on its home network (the interface a LAN packet leaves from; a UDP connect sends nothing)."""
    import socket
    for probe in ('192.168.255.255', '10.255.255.255', '172.31.255.255'):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((probe, 9))
            ip = s.getsockname()[0]
            if ip.startswith(('192.168.', '10.')) or any(ip.startswith('172.%d.' % n) for n in range(16, 32)):
                return ip
        except OSError:
            pass
        finally:
            s.close()
    return ''


def lan_discover(timeout=1.0):
    """The LAN servers that answer a broadcast within `timeout` seconds: [{'name', 'address', 'players'}]."""
    import socket
    found = {}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.2)
        for dest in ('255.255.255.255', '127.0.0.1'):
            try:
                s.sendto(b'GOWA?1', (dest, LAN_DISCOVERY_PORT))
            except OSError:
                pass
        end = time.time() + timeout
        while time.time() < end:
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                r = json.loads(data.decode('utf-8', 'replace'))
            except ValueError:
                continue
            if r.get('gowa') == 1 and r.get('address'):
                found[str(r['address'])[:64]] = {'name': str(r.get('name', 'LAN server'))[:40],
                                                 'address': str(r['address'])[:64], 'players': int(r.get('players') or 0)}
    finally:
        s.close()
    return list(found.values())


def lan_server_running():
    p = _lan_server['proc']
    return p is not None and p.poll() is None


def start_lan_server(name):
    """Start the community server on this PC's LAN address. Returns (address, None) or (None, reason)."""
    if lan_server_running():
        return _lan_server['address'], None
    d = server_dir()
    if d is None:
        return None, tr('the server files are not in this install (ascension\\server)')
    ip = lan_ip()
    if not ip:
        return None, tr('no home-network address found on this PC')
    import sys
    py = Path(sys.executable)
    if py.name.lower() == 'pythonw.exe' and (py.parent / 'python.exe').is_file():
        py = py.parent / 'python.exe'
    data = ROOT / 'gamedata' / 'lan-server'
    data.mkdir(parents=True, exist_ok=True)
    log = ROOT / 'build' / 'logs' / 'lan-server.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(py), '-I', str(d / 'gowa_server.py'), '--host', ip, '--lan', '--name', (name or 'LAN server')[:40],
           '--data', str(data), '--log', str(log)]
    try:
        _lan_server['proc'] = subprocess.Popen(cmd, cwd=str(d), creationflags=NO_WINDOW,
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as e:
        return None, str(e)
    time.sleep(1.5)
    if not lan_server_running():
        return None, tr('the server stopped at once (see build\\logs\\lan-server.log; another program may use port 36101)')
    _lan_server['address'] = ip
    return ip, None


def stop_lan_server():
    p = _lan_server['proc']
    if p is not None and p.poll() is None:
        p.terminate()
        try:
            p.wait(5)
        except Exception:
            p.kill()
    _lan_server['proc'] = None
    _lan_server['address'] = ''


import atexit
atexit.register(lambda: stop_lan_server())   # the LAN server ends with the launcher


def split_invite(text):
    """A match code as typed or pasted: 'ABC123' -> ('ABC123', '') ; 'ABC123@203.0.113.25' -> ('ABC123', '203.0.113.25')
    (2026-11-15: Copy code copies the server with the code, so a friend lands on the same server)."""
    raw = (text or '').strip()
    code, _, host = raw.partition('@')
    code = ''.join(c for c in code.upper() if c.isascii() and c.isalnum())[:12]
    host = host.strip()
    if host and not re.match(r'^[A-Za-z0-9.-]{1,253}(:\d{1,5})?$', host):
        host = ''
    return code, host


def online_environment(s, env):
    """The online settings as the online runtime reads them (kit mpnet.c / sysNet.c). Online, every player must
    load the same game data (it is part of the matchmaking key): no mods, so a skip-intro archive built by one
    launcher version never splits players from another."""
    env['PS3_MPNET'] = '1'
    env['PS3_NET_ONLINE'] = '1'
    server = (s.get('online_server') or '').strip()
    _code, invite_host = split_invite(s.get('online_party'))
    if invite_host:        # a code copied with its server ("ABC123@host") plays on that server
        server = invite_host
    if server:
        env['PS3_MPNET_SERVER'] = server
    # HTTPS sign-in (2026-11-14): the known community servers answer the game's requests over HTTPS on their
    # <ip-with-dashes>.sslip.io certificate (Caddy); other servers (a PC at home) stay on plain HTTP.
    from .config import KNOWN_SERVERS
    host = server.split(':')[0]
    if host and host in {a for _n, a, _r in KNOWN_SERVERS}:
        env['PS3_MPNET_HTTPS'] = host.replace('.', '-') + '.sslip.io'
    else:
        env.pop('PS3_MPNET_HTTPS', None)
    name = (s.get('online_name') or '').strip()
    if name:
        env['PS3_NP_ONLINE_ID'] = name[:16]
    port = (s.get('online_port') or '3658').strip()
    env['PS3_NET_P2P_PORT'] = port if port.isdigit() else '3658'
    party = split_invite(s.get('online_party'))[0]
    if party:
        env['PS3_MPNET_PARTY'] = party
    else:
        env.pop('PS3_MPNET_PARTY', None)
    env.pop('PS3_MODS', None)
    env['PS3_MPNET_TOKEN_FILE'] = str(token_file())
    # Fair play (2026-11-15): the game turns online off when any non-visual patch is on and then only shows the
    # stock "signed out of PlayStation Network" screen. So an online run leaves cheats and other gameplay patches
    # out (they stay ticked for single player) and the launcher says so.
    env.pop(ONLINE_DROPPED_KEY, None)
    if env.get('PS3_PATCHES') and env['PS3_PATCHES'] != 'none':
        names = [n.strip() for n in env['PS3_PATCHES'].split('|') if n.strip()]
        keep = [n for n in names if n in ONLINE_FAIR_PATCHES]
        dropped = [n for n in names if n not in ONLINE_FAIR_PATCHES]
        if dropped:
            env['PS3_PATCHES'] = '|'.join(keep) or 'none'
            env[ONLINE_DROPPED_KEY] = '|'.join(dropped)


# The patches the game allows online (kit libs/network/mpnet.c, list `fair`): keep the two lists the same.
ONLINE_FAIR_PATCHES = ('Unlock FPS', 'Disable Depth of Field', 'Disable Motion Blur', 'Disable MLAA',
                       'Disable SPU MLAA - God of War: Ascension (01)')
ONLINE_DROPPED_KEY = 'PS3_LAUNCHER_ONLINE_DROPPED'   # read by the launcher only (the status line)


def online_dropped_patches(env):
    """The patches an online run left out (online_environment), for the launcher's message."""
    v = env.get(ONLINE_DROPPED_KEY, '')
    return [n for n in v.split('|') if n]


def token_file():
    """The player's sign-in key for the community server, always in one place (2026-11-13): gamedata\\mpnet-token.txt.
    Without this the game kept it beside wherever /dev_hdd0 pointed, so changing a game-data setting made a new key
    and the server then refused the player's own name. A key found in an older place is moved here first."""
    dst = ROOT / 'gamedata' / 'mpnet-token.txt'
    if not dst.exists():
        for old in (ROOT / 'mpnet-token.txt', ROOT.parent / 'work' / 'mpnet-token.txt'):
            try:
                if old.is_file() and old.stat().st_size >= 32:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(old, dst)
                    break
            except OSError:
                pass
    return dst


def game_environment(s, switches=None):
    """The game's environment from the settings. `switches` is the set the
    executable reads (runtime_switches); a setting whose switch it does not
    read is left out, so nothing is passed that would silently do nothing."""
    if switches is None:
        switches = runtime_switches(play_exe(s))
    env = dict(os.environ)
    env['PS3_SYS_LANG'] = effective_language(s)
    env['PS3_ENTER_BUTTON'] = s['enter_button']
    env['PS3_WINDOW_SIZE'] = s['window_size']
    env['PS3_FULLSCREEN'] = '1' if s['display_mode'] == 'fullscreen' else '0'
    env['PS3_ESC_QUITS'] = s['esc_quits']
    env['LD_PERF_OVERLAY'] = s['perf_overlay']
    # The game patches, when the game lists and switches them as it starts
    # (PS3_PATCH_LIST / PS3_PATCHES; older builds have them built in). Passed
    # only once the player ticked them (patches_chosen): until then the game's
    # own defaults apply, so a list saved by an older launcher can never turn
    # patches off by accident.
    names = None
    if 'PS3_PATCH_LIST' in switches:
        from . import patches as rpcs3
        try:
            names = rpcs3.patches_on(s, rpcs3.exe_patches(s['game_exe']))
        except OSError:
            names = None
        if names is not None and str(s.get('patches_chosen', '0')) == '1':
            env['PS3_PATCHES'] = '|'.join(names) or 'none'
    # AMD FSR 3.1 (host ar-fsr): object motion comes from the game's own motion-blur draws, so motion blur is
    # turned on for it (every test run had it on). Frame generation needs VSync; FSR itself refuses without
    # Unlock FPS / Disable SPU MLAA and logs why.
    fsr = s.get('fsr', '0')
    if fsr in ('aa', 'fg') and ('FSR3_NATIVE_AA' if fsr == 'aa' else 'FSR3_FG') in switches:
        env['FSR3_NATIVE_AA' if fsr == 'aa' else 'FSR3_FG'] = '1'
        env['FSR3_OBJECTS'] = '1'
        if names is not None:
            env['PS3_PATCHES'] = '|'.join(n for n in names if n != 'Disable Motion Blur') or 'none'
        if fsr == 'fg' and 'PS3_VSYNC' in switches:
            s = dict(s, vsync='1')
    env.setdefault('PS3_FPS_LIMIT', fps_limit(s, names))         # the cap (60 or 30) with Unlock FPS on, else 30
    env.setdefault('LD_PERF_OVERLAY_FPS', fps_limit(s, names))   # what the overlay's SPD compares with
    # Prerendered cutscenes and intro movies (a runtime switch being added;
    # passed either way, so a newer build picks it up).
    # Always explicit: the game's own default is 1 (host\ascension_defaults.cpp), so leaving it unset
    # could never turn skipping off (2026-11-07).
    env['PS3_SKIP_MOVIES'] = '1' if s.get('skip_movies') == '1' else '0'
    env['PS3_OVERLAY_FONT'] = str(HERE / 'Cinzel-Black.ttf')   # RPCS3's overlay in the launcher's font
    # The game's own settings menu (F1 / hold SELECT): where it writes what the
    # player changed, for config.merge_ingame. Only to a game that has the menu.
    if 'PS3_INGAME_SETTINGS' in switches:
        env['PS3_INGAME_SETTINGS'] = str(INGAME_FILE)
    if 'LD_MENU_LANG' in switches:
        env['LD_MENU_LANG'] = current()     # that menu in the launcher's language (en, es, pt-BR)
    # Thread placement: the walker, PPU main thread, SPU workers and SPURS
    # kernel each on their own core (EXPERIMENTS rows 26-27: +12% in heavy fights).
    if 'PS3_HOST_PIN' in switches:
        env.setdefault('PS3_HOST_PIN', '1' if s.get('host_pin', '1') == '1' else '0')
    # The recorded shaders built before the game starts (on: no compiling in new areas).
    if 'LD_PSO_PRECOMPILE' in switches:
        env.setdefault('LD_PSO_PRECOMPILE', '1' if s.get('precompile', '1') == '1' else '0')
    # Occlusion culling: objects the game finds hidden behind walls are not drawn (2 = exact answers).
    if 'OCCL' in switches:
        env.setdefault('OCCL', '2' if s.get('occlusion', '1') == '1' else '0')
    # Fewer shader stutters (host uber5, opt-in): a draw whose shader is still being built is drawn with the
    # ubershader meanwhile, within a GPU budget, with its fast form. The game refuses it on GPUs other than NVIDIA
    # (LD_UBER_ANYGPU, testers only, is never set here. LD_UBER_VS, the vertex ubershader, is: proven 2026-11-14, host/w2i.)
    if 'LD_UBER' in switches and s.get('uber_shaders', '0') == '1':
        for key in ('LD_UBER', 'LD_UBER_BUDGET', 'LD_UBER_FAST', 'LD_UBER_AFTERLOAD', 'LD_UBER_KAUTO', 'LD_UBER_VS'):   # +uber6: after-load stand-ins, vertex ubershader (host/w2i check 2)
            env.setdefault(key, '1')
    # Ambient occlusion (2026-11-15, opt-in): soft contact shadows added at the end of the scene's 3D pass.
    if 'LD_AO' in switches and s.get('ambient_occlusion', '0') == '1':
        env.setdefault('LD_AO', '1')
    # Smoother glow at high resolutions (LD_FULLRES_FX, on in the game by default since 2026-11-14): the switch only
    # turns it off, for slow graphics cards.
    if 'LD_FULLRES_FX' in switches and s.get('smooth_glow', '1') == '0':
        env.setdefault('LD_FULLRES_FX', '0')
    # Disable dynamic shadows (2026-11-22, off by default): the game draws no shadow maps (flatter look).
    if 'PS3_NO_SHADOWS' in switches and s.get('no_shadows', '0') == '1':
        env.setdefault('PS3_NO_SHADOWS', '1')
    # GPU vertex decoding (0.2.0, experimental, off by default): the GPU reads the game's vertex data itself
    # (GP_PULL=1) and the walker hands it over directly (W_VDIRECT=3).
    if 'GP_PULL' in switches and s.get('gp_pull', '0') == '1':
        env.setdefault('GP_PULL', '1')
        env.setdefault('W_VDIRECT', '3')
    # Offline Multiplayer: the mode Quick Play starts and the map of its first match
    # (the game reads them only with that patch on).
    if 'PS3_MP_MODE' in switches:
        mode = s.get('mp_mode', 'trial1')
        mode = mode if mode in dict(MP_MODES) else 'trial1'
        mp_map = s.get('mp_map', 'cycle')
        env.setdefault('PS3_MP_MODE', mode)
        env.setdefault('PS3_MP_MAP', mp_map if mp_map in dict(mp_map_choices(mode)) else 'cycle')
    # Testing only: the game limits itself to that many cores (placement plans on them).
    if 'PS3_CPU_LIMIT' in switches and s.get('cpu_limit', '0') not in ('', '0'):
        env.setdefault('PS3_CPU_LIMIT', s['cpu_limit'])
    if MAC:
        env['PS3RECOMP_METAL_FPS'] = s['perf_overlay']    # in the window's title
    # Video
    env.pop('PS3_RSX', None)
    env.pop('PS3_RSX_RENDERER', None)
    if s['renderer'].startswith('rpcs3') and 'PS3_RSX_RENDERER' in switches:
        env['PS3_RSX'] = 'rpcs3'
        env['PS3_RSX_RENDERER'] = 'opengl' if s['renderer'] == 'rpcs3-opengl' else 'vulkan'
        if MAC and s['renderer'] == 'rpcs3-vulkan':
            icd = next((p for p in (
                '/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json',
                '/usr/local/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json') if Path(p).is_file()), None)
            if icd and not env.get('VK_ICD_FILENAMES'):
                env['VK_ICD_FILENAMES'] = icd
            lib = '/opt/homebrew/lib' if Path('/opt/homebrew/lib').is_dir() else '/usr/local/lib'
            env['DYLD_FALLBACK_LIBRARY_PATH'] = lib + os.pathsep + env.get('DYLD_FALLBACK_LIBRARY_PATH', '')
    for key, value in (('PS3_RENDER_SCALE', s['render_scale']), ('PS3_ASPECT', s['aspect']),
                       ('PS3_SCALE_FILTER', s['scale_filter']), ('PS3_ANISO', s['aniso'])):
        if key in switches:
            env[key] = value
    if s['vsync'] in ('0', '1') and 'PS3_VSYNC' in switches:
        env['PS3_VSYNC'] = s['vsync']
    elif s['vsync'] == 'vrr' and 'PS3_VSYNC' in switches:
        # Variable refresh: each frame presented as soon as it is ready (the
        # frame cap still paces them); the game then leaves the monitor at its
        # own refresh, which a G-Sync / FreeSync screen fits to the frames.
        env['PS3_VSYNC'] = '0'
    if 'PS3_DISPLAY_MATCH' in switches:
        env['PS3_DISPLAY_MATCH'] = s['display_match']
    if 'LD_SMAA' in switches:
        env['LD_SMAA'] = s['smaa']
    if 'LD_SHARPEN' in switches:
        env['LD_SHARPEN'] = s['sharpen']
    if 'LD_VIBRANCE' in switches:
        env['LD_VIBRANCE'] = s['vibrance']
    # Controls (the runtime's own fixed layout until it reads these)
    if 'PS3_KEYMAP' in switches:
        env['PS3_KEYMAP'] = keymap_env(s['keymap']) or 'start=Enter'
    if 'PS3_PADMAP' in switches:
        env['PS3_PADMAP'] = padmap_env(s['padmap'])
    if 'PS3_PAD_DEADZONE' in switches:
        env['PS3_PAD_DEADZONE'] = s['pad_deadzone']
    root = disc_root(s)
    if root:
        env['PS3_VFS_ROOT'] = str(root)
    if MAC or s.get('port_hdd') == '1':
        # Game files prepared by the launcher (Windows) keep the player's folder
        # untouched: the game's own data goes to ./gamedata, as on macOS.
        # The disc image is mounted read-only, so it reports no free space and
        # the game exits with NOSPACE. Saves and the cache live beside the disc.
        # On Windows /dev_hdd0 stays where run.ps1 has it: the game's own data
        # (BCES01741-DATA) under the extracted disc's game/ folder; the saves
        # are ./gamedata/dev_hdd0/home/... either way (cellSaveData).
        for key, folder in (('PS3_HDD0_ROOT', ROOT / 'gamedata' / 'dev_hdd0'),
                            ('PS3_HDD1_ROOT', ROOT / 'gamedata' / 'dev_hdd1')):
            folder.mkdir(parents=True, exist_ok=True)
            env[key] = str(folder)
    mods = compose_mods(s)
    if mods:
        env['PS3_MODS'] = ';'.join(mods)
    else:
        env.pop('PS3_MODS', None)
    ico = icon_file()
    if ico:
        env['PS3_WINDOW_ICON'] = str(ico)
    for item in s.get('extra_env', '').replace('\n', ';').split(';'):
        if '=' in item:
            k, v = item.split('=', 1)
            if k.strip():
                env[k.strip()] = v.strip()
    if online_on(s):
        online_environment(s, env)   # after the extra variables: the Online page wins over old typed lines
        online = compose_online_mods(s)   # online: no mods, except the 1.04 sound mix (same data hash)
        if online:
            env['PS3_MODS'] = ';'.join(online)
    return env


def apply_save_settings(s):
    """Language and subtitles into the game's system save (BCES01741-SYSDATA).

    The game asks the console language only when it makes that save, at the
    first start; after that it reads its own copy. SAVEDATA (4 KB, no checksum)
    is a 16-byte header and the options block the boot hands out
    (func_0001A680): +0x0C text language (to func_003963A8), +0x0D voice
    language (to the audio side, func_00396348), +0x18 subtitles (to
    func_00032C74, which -subtitlesOn also drives). With English the game writes
    18 (English UK, the European text) and 1. The voices follow the text unless
    the 'voice_language' setting says English (1) or 'game' (the byte is left as
    the game's own Options -> Speech set it). The US disc (BCUS98232) keeps its own
    BCUS98232-SYSDATA, and its English is 1 for text and voices (its LANGS table has
    no 18). Returns what it changed."""
    title = title_id(s)
    path = save_dir() / f'{title}-SYSDATA' / 'SAVEDATA'
    try:
        data = bytearray(path.read_bytes())
    except OSError:
        return None                                  # no save yet: the console language decides
    if len(data) < 0x29 or data[0:4] != bytes((0, 0, 0, 1)):
        return None
    lang = int(effective_language(s))                # 17: the US disc, or the PT-BR mod on the European one
    if lang in (1, 18):
        text, voice = (1, 1) if title == US_ID else (18, 1)
    else:
        text = voice = lang
    pick = s.get('voice_language', 'same')
    if pick == '1':
        voice = 1                                    # English voices with any text
    new = bytearray(data)
    new[0x1C] = text
    if pick != 'game':
        new[0x1D] = voice
    new[0x28] = 1 if s.get('subtitles') == '1' else 0
    if new != data:
        path.write_bytes(bytes(new))
        return True
    return False


def tree_signature(folder):
    """Relative path, size and mtime of every file (copytree keeps mtimes)."""
    out = []
    for dirpath, _dirs, files in os.walk(folder):
        for name in files:
            st = (Path(dirpath) / name).stat()
            out.append((os.path.relpath(Path(dirpath) / name, folder), st.st_size, int(st.st_mtime)))
    return sorted(out)


def backup_mp_profile():
    """A copy of the multiplayer profile folder in gamedata/mp-profile-backups,
    as backup_saves does for the saves (kept apart: a save backup is restored
    into the savedata folder as it is)."""
    src = mp_profile_dir()
    if not src.is_dir() or not any(src.iterdir()):
        return None
    folder = ROOT / 'gamedata' / 'mp-profile-backups'
    backups = sorted(folder.iterdir()) if folder.is_dir() else []
    if backups and tree_signature(backups[-1]) == tree_signature(src):
        return backups[-1]
    dst = folder / time.strftime('%Y%m%d-%H%M%S')
    if dst.exists():
        return dst
    shutil.copytree(src, dst)
    for old in sorted(folder.iterdir())[:-20]:      # keep the last twenty
        shutil.rmtree(old, ignore_errors=True)
    return dst


def backup_saves():
    """A copy of the saves, unless the newest backup already has them as they
    are: starting the game ten times without playing keeps the older ones.
    The multiplayer profile is copied too (backup_mp_profile)."""
    try:
        backup_mp_profile()
    except OSError:
        pass                                        # the saves still get their copy
    src = save_dir()
    if not src.is_dir() or not any(src.iterdir()):
        return None
    folder = ROOT / 'gamedata' / 'save-backups'
    backups = sorted(folder.iterdir()) if folder.is_dir() else []
    if backups and tree_signature(backups[-1]) == tree_signature(src):
        return backups[-1]
    dst = folder / time.strftime('%Y%m%d-%H%M%S')
    if dst.exists():                                # two backups in the same second
        return dst
    shutil.copytree(src, dst)
    backups = sorted(folder.iterdir())
    for old in backups[:-20]:                       # keep the last twenty
        shutil.rmtree(old, ignore_errors=True)
    return dst


def saves_signature():
    """The saves and the multiplayer profile, for watch_saves."""
    sig = [tree_signature(d) if d.is_dir() else None for d in (save_dir(), mp_profile_dir())]
    return None if sig == [None, None] else sig


def watch_saves(process, stable_s=10, step_s=5):
    """While the game runs, back up the saves each time the game has saved: when
    the save files changed since the last copy and then stayed the same for
    `stable_s` seconds (a save is written in one short burst). A crash can then
    only cost the save being written at that moment, not the earlier ones. No copy
    after the game ends: a crash in the middle of a save must not become a backup."""
    try:
        last = saves_signature()
    except OSError:
        last = None
    seen, since = last, time.monotonic()
    while process.poll() is None:
        time.sleep(step_s)
        if process.poll() is not None:
            break
        try:
            now = saves_signature()
        except OSError:
            continue                                # a file vanished mid-scan: try again
        if now != seen:
            seen, since = now, time.monotonic()
            continue
        if now and now != last and time.monotonic() - since >= stable_s:
            try:
                backup_saves()
                last = now
            except OSError:
                pass


SAVE_PREFIX = tuple(f'{t}-' for t in GAME_IDS)   # the save folders of both discs (str.startswith takes a tuple)


def export_saves(zip_path):
    """All the saves (the game's BCES01741-* folders) in one .zip, to carry to
    another PC. Returns the number of files written."""
    import zipfile
    src = save_dir()
    folders = sorted(d for d in src.iterdir() if d.is_dir() and d.name.startswith(SAVE_PREFIX)) if src.is_dir() else []
    if not folders:
        raise OSError(tr('There are no saves yet.'))
    n = 0
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for d in folders:
            for dirpath, _dirs, files in os.walk(d):
                for name in files:
                    p = Path(dirpath) / name
                    z.write(p, p.relative_to(src).as_posix())
                    n += 1
    return n


def import_saves(zip_path):
    """The saves from a .zip made by export_saves (or any .zip whose folders are the
    game's BCES01741-* save folders). The current saves are backed up first."""
    import zipfile
    with zipfile.ZipFile(zip_path) as z:
        names = [i.filename for i in z.infolist() if not i.is_dir()]
        if not names:
            raise OSError(tr('The file has no saves in it.'))
        for name in names:
            parts = Path(name).parts
            if (not parts or not parts[0].startswith(SAVE_PREFIX) or '..' in parts or name.startswith('/')
                    or ':' in name or len(parts) < 2):
                raise OSError(tr('This is not a save file of this game: {name}', name=name))
        dst = save_dir()
        tmp = dst.with_name('savedata.importing')
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        z.extractall(tmp)
    make_writable(tmp)
    if dst.is_dir():
        backup_saves()
        for d in dst.iterdir():                     # keep anything that is not a game save folder
            if not d.name.startswith(SAVE_PREFIX):
                shutil.move(str(d), str(tmp / d.name))
        shutil.rmtree(dst, ignore_errors=True)
    tmp.rename(dst)
    return len(names)


def backup_list():
    """The save backups, newest first."""
    folder = ROOT / 'gamedata' / 'save-backups'
    return sorted((d for d in folder.iterdir() if d.is_dir()), reverse=True) if folder.is_dir() else []


def make_writable(folder):
    """Clear the read-only flag on every file under `folder`: copy2/copytree carry it over, and the game
    cannot write a read-only save file (its save then fails without a word)."""
    for dirpath, _dirs, files in os.walk(folder):
        for name in files:
            p = os.path.join(dirpath, name)
            try:
                if not os.access(p, os.W_OK):
                    os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
            except OSError:
                pass


def restore_saves(backup):
    """The saves as they were in `backup`. The current ones are backed up
    first; the copy is taken before that, since it can drop the oldest."""
    dst = save_dir()
    tmp = dst.with_name('savedata.restoring')
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.copytree(backup, tmp)
    make_writable(tmp)
    backup_saves()
    shutil.rmtree(dst, ignore_errors=True)
    tmp.rename(dst)


# ---- saves left behind (2026-11-14) ---------------------------------------------------------------
# Players said "my saves are getting lost after the update". Two causes:
#  1. Each full zip is extracted into a NEW folder, and the saves live in the install folder
#     (gamedata\, the game writes ./gamedata/... from the launcher's folder): the new version starts
#     with no saves; the progress is still in the old folder.
#  2. Builds up to 0.1.26 on PCs where the game's memory sat below 4 GB saved every checkpoint
#     loosely into savedata\ itself (cellSaveDataAutoSave2 converted the name twice) while loading
#     read BCES01741-PROFDATA-0; from 0.1.27 nothing reads those files.
# At launcher start the release launcher looks for such saves and copies them into place: only when the
# save it finds is newer than the one here (or there is none here), never deleting anything (what it
# replaces is moved to gamedata\replaced-saves\<time>\, and backup_saves() runs first). A found save
# that is OLDER than the one here (a player who started again in the new folder) is only offered.
# Each find is acted on or offered once (installs.json 'handled'), so a Restore is not undone later.
INSTALLS_FILE = CONFIG_DIR / 'installs.json'
SAVE_REL = Path('gamedata') / 'dev_hdd0' / 'home' / '00000001' / 'savedata'
NEWER_S = 60                     # a found save must be this much newer to replace one


def _story(name):
    return name.startswith('DAT-') or name == 'DATFEST'


def _load_installs():
    try:
        d = json.loads(INSTALLS_FILE.read_text(encoding='utf-8'))
        if isinstance(d, dict):
            d.setdefault('installs', {})
            d.setdefault('handled', {})
            return d
    except (OSError, ValueError):
        pass
    return {'installs': {}, 'handled': {}}


def _write_installs(d):
    INSTALLS_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = INSTALLS_FILE.with_name(INSTALLS_FILE.name + '.tmp')
    tmp.write_text(json.dumps(d, indent=1), encoding='utf-8')
    os.replace(tmp, INSTALLS_FILE)


def _same(a, b):
    try:
        return Path(a).resolve() == Path(b).resolve()
    except OSError:
        return False


def _key_files(folder):
    """The files that hold progress: DAT-*/DATFEST of a profile, SAVEDATA of a system save, else all."""
    files = [f for f in folder.iterdir() if f.is_file()] if folder.is_dir() else []
    if '-PROFDATA-' in folder.name:
        return [f for f in files if _story(f.name)]
    if folder.name.endswith('-SYSDATA'):
        return [f for f in files if f.name == 'SAVEDATA']
    return files


def _newest(files):
    return max((f.stat().st_mtime for f in files), default=0)


def _has_save(folder):
    return (folder / 'PARAM.SFO').is_file() and bool(_key_files(folder))


def _content(files):
    return sorted((f.name, f.stat().st_size, f.read_bytes()) for f in files)


def record_install():
    """This install in the shared installs.json (any release folder of the port on this PC)."""
    d = _load_installs()
    sd = save_dir()
    newest = 0
    if sd.is_dir():
        for f in sd.iterdir():
            if f.is_dir() and f.name.startswith(SAVE_PREFIX):
                newest = max(newest, _newest(_key_files(f)))
    d['installs'][str(ROOT.resolve())] = {'version': VERSION, 'seen': int(time.time()), 'newest_save': int(newest)}
    _write_installs(d)


def _root_of(path, levels=6):
    """The ascension\\ folder (the one with gamedata\\...\\savedata) that `path` lies in, if any."""
    try:
        p = Path(path)
    except TypeError:
        return None
    for q in [p, *p.parents][:levels]:
        if (q / SAVE_REL).is_dir():
            return q
    return None


def other_install_roots(s=None):
    """ascension\\ folders of other copies of the port: the ones in installs.json, the paths in the old
    shared settings (game_exe etc. of an earlier install), and the release folders beside this one, one
    level up, in Downloads and on the Desktop (a full zip extracted next to the old one, or in a new
    wetransfer_* folder)."""
    seen, out = set(), []

    def add(root):
        if root is None or _same(root, ROOT):
            return
        try:
            k = str(Path(root).resolve()).lower()
        except OSError:
            return
        # only release folders (every full zip since 0.1.7 has runtime\python): never a development checkout
        if k not in seen and (Path(root) / SAVE_REL).is_dir() and (Path(root).parent.parent / 'runtime' / 'python').is_dir():
            seen.add(k)
            out.append(Path(root))

    for path in _load_installs()['installs']:
        add(Path(path))
    settings = [s or {}]
    try:
        settings.append(json.loads(SHARED_CONFIG_FILE.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    for d in settings:
        for key in ('game_exe', 'online_exe', 'eboot', 'mods_dir'):
            v = d.get(key) if isinstance(d, dict) else None
            if isinstance(v, str) and v:
                add(_root_of(v))
    home = Path.home()
    bases = [INSTALL.parent, INSTALL.parent.parent, home / 'Downloads', home / 'Desktop']
    for base in bases:
        try:
            children = [c for c in base.iterdir() if c.is_dir()][:400]
        except OSError:
            continue
        for c in children:
            add(c / 'ascension-recomp' / 'ascension' if (c / 'ascension-recomp').is_dir() else None)
            try:
                grand = [g for g in c.iterdir() if g.is_dir()][:40]
            except OSError:
                continue
            for g in grand:
                if (g / 'ascension-recomp').is_dir():
                    add(g / 'ascension-recomp' / 'ascension')
    return out


def _sources(sd, prefix, where, here=False):
    """{save folder name: (files, newest, label)} of one savedata folder; loose files of the AutoSave2 bug
    count as <prefix>-PROFDATA-0 / -SYSDATA when they are newer than those folders."""
    out = {}
    if not sd.is_dir():
        return out
    for f in sd.iterdir():
        if f.is_dir() and f.name.startswith(SAVE_PREFIX) and _has_save(f):
            out[f.name] = ([x for x in f.iterdir() if x.is_file()], _newest(_key_files(f)), where, False)
    loose = [f for f in sd.iterdir() if f.is_file()]
    prof = [f for f in loose if _story(f.name)]
    if prof:
        name = f'{prefix}-PROFDATA-0'
        t = _newest(prof)
        if here or name not in out or t > out[name][1] + NEWER_S:   # here: an older one is still offered
            extra = [f for f in loose if f.name in ('ICON0.PNG', 'PARAM.SFO')]
            out[name] = (prof + extra, t, where, True)
    sysf = [f for f in loose if f.name == 'SAVEDATA']
    if sysf:
        name = f'{prefix}-SYSDATA'
        if name not in out:          # the launcher rewrites SYSDATA's language bytes at each PLAY: times say little
            out[name] = (sysf + [f for f in loose if f.name == 'PARAM.SFO'], _newest(sysf), where, True)
    return out


def lost_save_plan(s, ignore_handled=False):
    """What to recover: [{'name', 'files', 'newest', 'where', 'loose', 'here_newest', 'auto', 'key'}].
    auto: copied without asking (nothing here, or the found save is newer); else offered."""
    prefix = title_id(s)
    here_dir = save_dir()
    handled = {} if ignore_handled else _load_installs()['handled']
    best = {}
    # cause 2 in this folder
    for name, src in _sources(here_dir, prefix, here_dir, here=True).items():
        if src[3]:
            best[name] = src
    # cause 1: other installs (their loose bug files too)
    for root in other_install_roots(s):
        for name, src in _sources(root / SAVE_REL, prefix, root / SAVE_REL).items():
            if name not in best or src[1] > best[name][1]:
                best[name] = src
    plan = []
    for name, (files, newest, where, loose) in sorted(best.items()):
        here = here_dir / name
        here_has = _has_save(here)
        here_newest = _newest(_key_files(here)) if here_has else 0
        try:
            if here_has and _content([f for f in files if _story(f.name) or f.name == 'SAVEDATA']) == \
                    _content([f for f in here.iterdir() if f.is_file() and (_story(f.name) or f.name == 'SAVEDATA')]):
                continue                                    # already here
        except OSError:
            continue
        key = f'{where}|{name}|{int(newest)}'
        if key in handled:
            continue
        # auto (copied without asking) ONLY when there is no save here. A file time says when a save was
        # written, not how far the story is: one checkpoint in an old install (ch19) made its files newer
        # than this install's ch22, and every next launcher start copied ch19 over ch22 without asking
        # (players, 2026-11-15: "I always return to chapter 19"). A save here is only replaced when the
        # player picks the other one on the Saves page (check_lost_saves(manual=True)).
        if not here_has:
            auto = True
        elif name.endswith('-SYSDATA'):
            continue                                        # a system save here is never replaced
        else:
            auto = False
        install = Path(where).parents[6] if len(Path(where).parents) > 6 else Path(where)   # the release folder
        plan.append({'name': name, 'files': files, 'newest': newest, 'where': str(install), 'loose': loose,
                     'here_newest': here_newest, 'auto': auto, 'key': key})
    return plan


def any_game_running():
    """Any copy of the port's game (any install, any build name): saves are not touched while one runs."""
    if not WINDOWS:
        return False
    try:
        out = subprocess.run(['tasklist', '/NH', '/FO', 'CSV'], capture_output=True, timeout=10,
                             creationflags=NO_WINDOW).stdout or b''
    except (OSError, subprocess.SubprocessError):
        return True                                         # cannot tell: do nothing
    return b'"ascensionrecomp' in out.lower()


def recover_save(item, stamp=None):
    """Put one plan item in place. The save folder here is first copied whole (backup_saves) and then
    moved to gamedata\\replaced-saves\\<time>\\ (kept for good); the found files are copied, never moved.
    Returns the replaced-saves folder (or None when nothing was replaced)."""
    stamp = stamp or time.strftime('%Y%m%d-%H%M%S')
    sd = save_dir()
    sd.mkdir(parents=True, exist_ok=True)
    backup_saves()
    dst = sd / item['name']
    tmp = sd.with_name(f'savedata.recovering-{item["name"]}')
    shutil.rmtree(tmp, ignore_errors=True)
    if item['loose'] and dst.is_dir():
        shutil.copytree(dst, tmp)                           # the loose files go over the profile's own
    else:
        tmp.mkdir(parents=True)
    for f in item['files']:
        if f.name == 'PARAM.SFO' and item['loose'] and (tmp / 'PARAM.SFO').is_file():
            continue                                        # keep the folder's own
        shutil.copy2(f, tmp / f.name)
    make_writable(tmp)
    if item['loose']:
        # PARAM.PFD: the runtime's list of the files written as secure files (cellSaveData.c pfd_add)
        pfd = tmp / 'PARAM.PFD'
        names = pfd.read_text(errors='replace').split() if pfd.is_file() else []
        for f in item['files']:
            if (_story(f.name) or f.name == 'SAVEDATA') and f.name not in names:
                names.append(f.name)
        pfd.write_text(''.join(n + '\n' for n in names))
    replaced = None
    if dst.exists():
        replaced = ROOT / 'gamedata' / 'replaced-saves' / stamp
        replaced.mkdir(parents=True, exist_ok=True)
        shutil.move(str(dst), str(replaced / item['name']))
    tmp.rename(dst)
    try:   # host/trophies: the old folder's unlocked trophies come along with its saves (union, never removed)
        from .trophies import install_root_of, merge_unlocks_from
        old = install_root_of(item['files'][0]) if item['files'] else None
        if old is not None:
            merge_unlocks_from(old)
    except (OSError, ValueError):
        pass
    return replaced


def mark_handled(items):
    d = _load_installs()
    for it in items:
        d['handled'][it['key']] = int(time.time())
    _write_installs(d)


def missing_game_files(s):
    """Files of the disc the game cannot start without: USRDIR\\ARCHIVES (its list of
    data archives) and every archive that list names. A copy of the game without
    them started, compiled its shaders, waited 5 s for its data and closed with no
    message (a player's log, 2026-11-04: ARCHIVES missing). Only for a disc folder
    that is there; a .iso is checked once it is mounted."""
    usr = disc_usrdir(s)
    if not (usr.parent / 'PARAM.SFO').is_file():
        return []
    listing = usr / 'ARCHIVES'
    if not listing.is_file():
        return [listing]
    try:
        names = [n.strip() for n in listing.read_text(encoding='ascii', errors='replace').splitlines()[1:]]
    except OSError:
        return [listing]
    return [usr / n for n in names if n and not (usr / n).is_file()]


# A PS3 dump copied to a FAT32 drive (multiMAN, webMAN, IRISMAN) has each file over
# 4 GB cut into NAME.66600, NAME.66601, ...; the game needs NAME in one piece. A
# player's copy, 2026-11-05: EXTRASCIN2.PSARC (8.7 GB) in three parts. The sizes of
# this disc's archives that can be split, to check the parts add up (BCES01741 1.00).
SPLIT_SIZES = {'EXTRASCIN2.PSARC': 9360605068}


def split_parts(path):
    """The parts `path` was cut into, in order (NAME.66600, NAME.66601, ...), or []
    when there are none or the numbers have a gap."""
    try:
        names = os.listdir(path.parent)
    except OSError:
        return []
    head = path.name.lower() + '.666'
    found = {}
    for n in names:
        low = n.lower()
        if low.startswith(head) and len(low) == len(head) + 2 and low[-2:].isdigit():
            found[int(low[-2:])] = path.parent / n
    if not found or sorted(found) != list(range(len(found))):
        return []
    return [found[i] for i in range(len(found))]


def split_game_files(s):
    """The game files missing in one piece whose parts are there: [(file, parts, bytes)].
    A file whose parts cannot be read is left out (reported as missing instead)."""
    out = []
    for f in missing_game_files(s):
        parts = split_parts(f)
        if parts:
            try:
                out.append((f, parts, sum(p.stat().st_size for p in parts)))
            except OSError:
                pass
    return out


def local_number(text):
    """A number written the English way ('9,360,605,068', '8.7') in the launcher's
    language: Spanish and Portuguese swap the two separators."""
    if current().split('-')[0].lower() in ('es', 'pt'):
        return text.translate(str.maketrans(',.', '.,'))
    return text


def gigabytes(n):
    return local_number(f'{n / 2**30:.1f}')


FAT_LIMIT = 1 << 32    # FAT32 holds no file of 4 GiB or more


def on_fat(path):
    """True when `path` is on a FAT or FAT32 drive (Windows; elsewhere the
    preallocation in join_split fails at once instead)."""
    if not WINDOWS:
        return False
    try:
        import ctypes
        k = ctypes.windll.kernel32
        root = ctypes.create_unicode_buffer(300)
        fs = ctypes.create_unicode_buffer(64)
        if not k.GetVolumePathNameW(str(path), root, 300):
            return False
        if not k.GetVolumeInformationW(root.value, None, 0, None, None, None, fs, 64):
            return False
        return fs.value.upper() in ('FAT', 'FAT32')
    except Exception:
        return False


def fat_error():
    return OSError(tr("your game folder is on a FAT32 drive, which cannot hold files over 4 GB: copy the whole "
                      "game folder to your PC's drive (or an NTFS or exFAT drive), then pick it again"))


def join_check(split):
    """Raises OSError with the reason when the parts cannot be joined (a part
    missing, the folder on a FAT32 drive, too little space); writes nothing."""
    for f, _parts, size in split:
        want = SPLIT_SIZES.get(f.name.upper())
        if want and size != want:
            raise OSError(tr('the parts of {name} add up to {got} bytes instead of {want}: a part is missing '
                             'or incomplete, copy the parts again', name=f.name,
                             got=local_number(f'{size:,}'), want=local_number(f'{want:,}')))
    folder = split[0][0].parent
    if any(size >= FAT_LIMIT for _f, _p, size in split) and on_fat(folder):
        raise fat_error()
    need = sum(size for _f, _p, size in split)
    stale = 0                  # a join stopped half way left these; join_split deletes them first
    for f, _p, _n in split:
        try:
            stale += f.with_name(f.name + '.joining').stat().st_size
        except OSError:
            pass
    free = shutil.disk_usage(folder).free + stale
    if free < need + (64 << 20):
        raise OSError(tr('not enough free space on that drive: {need} GB needed, {free} GB free',
                         need=gigabytes(need), free=gigabytes(free)))


def join_split(split, status=lambda m: None, stop=None):
    """Joins each file's parts into the file, next to them (the parts are kept).
    Written as NAME.joining and renamed once complete and on the disk, so a join that
    stops half way never leaves a NAME the game would read. `stop`: a threading.Event
    that ends the join (the launcher closing)."""
    join_check(split)
    for f, _p, _n in split:
        try:
            f.with_name(f.name + '.joining').unlink()
        except OSError:
            pass
    for f, parts, size in split:
        tmp = f.with_name(f.name + '.joining')
        done, shown = 0, -1
        try:
            with open(tmp, 'wb') as out:
                if not WINDOWS:            # sparse and instant there, and fails at once on FAT32 (EFBIG);
                    try:                   # Windows writes the whole size in zeros instead (on_fat covers it)
                        out.truncate(size)
                    except OSError:
                        if size >= FAT_LIMIT:
                            raise fat_error()
                        raise
                    out.seek(0)
                for p in parts:
                    with open(p, 'rb') as src:
                        while True:
                            if stop is not None and stop.is_set():
                                raise OSError(tr('the join was stopped'))
                            block = src.read(8 << 20)
                            if not block:
                                break
                            try:
                                out.write(block)
                            except OSError:
                                if size >= FAT_LIMIT and done + len(block) >= FAT_LIMIT:
                                    raise fat_error()    # a FAT32 drive on_fat could not see (a network share)
                                raise
                            done += len(block)
                            pct = done * 100 // size if size else 100
                            if pct != shown:
                                shown = pct
                                status(tr('Joining {name}: {percent}%…', name=f.name, percent=pct))
                status(tr('Saving {name} to disk…', name=f.name))
                out.flush()
                os.fsync(out.fileno())
            if done != size or tmp.stat().st_size != size:
                raise OSError(tr('{name} came out {got} bytes instead of {want}', name=f.name,
                                 got=local_number(f'{done:,}'), want=local_number(f'{size:,}')))
            os.replace(tmp, f)
        except BaseException:
            try:
                tmp.unlink()
            except OSError:
                pass
            raise


# The DLLs the full package puts next to AscensionRecomp.exe. An update zip or a test build holds only the exe:
# extracted on its own, Windows found another program's older SDL2.dll and stopped with "entry point
# SDL_GameControllerPath not found" (a tester, 2026-11-10).
PORT_DLLS = ('SDL2.dll', 'dxcompiler.dll', 'dxil.dll', 'avcodec-*.dll', 'avformat-*.dll', 'avutil-*.dll',
             'swresample-*.dll', 'swscale-*.dll')


def port_dlls_missing(game_exe):
    """The PORT_DLLS not found next to the game's executable (Windows only)."""
    if not WINDOWS:
        return []
    folder = Path(game_exe).parent
    out = []
    for name in PORT_DLLS:
        found = any(folder.glob(name)) if '*' in name else (folder / name).is_file()
        if not found:
            out.append(name.replace('-*', ''))
    return out


def missing(s, split_ok=False):
    """What PLAY cannot start without, as text. `split_ok`: leave out the game files
    whose split parts are there (the window's PLAY offers to join them)."""
    out = []
    if not Path(play_exe(s)).is_file():
        out.append(tr("the game's executable ({path})", path=play_exe(s)))
    else:
        dlls = port_dlls_missing(play_exe(s))
        if dlls:
            out.append(tr("the port's own files next to the game ({files}): this install is incomplete. Install "
                          'the full package (the -windows.zip), then extract updates and test builds over it',
                          files=', '.join(dlls)))
    if not Path(s['eboot']).is_file():
        out.append(tr("the disc's EBOOT.ELF ({path})", path=s['eboot']))
    root = disc_root(s)
    if root and not (root / 'PS3_GAME').is_dir() and not Path(s.get('disc_iso') or '-').is_file():
        out.append(tr('the disc ({path}; or the .iso image)' if MAC else 'the disc ({path})', path=root))
    gone = missing_game_files(s)
    split = {f for f, _p, _n in split_game_files(s)} if gone else set()
    if split and not split_ok:
        out.append(tr('game files split into parts by a FAT32 drive ({files}): press PLAY in the launcher '
                      'window to join them', files=', '.join(f.name for f in sorted(split))))
    gone = [p for p in gone if p not in split]
    if gone:
        names = ', '.join(str(p) for p in gone[:3]) + (' …' if len(gone) > 3 else '')
        out.append(tr('game files ({files}): your copy of the game is incomplete, copy or extract it again', files=names))
    if online_on(s):
        edat = online_pass_file(s)
        if edat is not None and not edat.is_file():
            out.append(tr('your Online Pass for online play ({path}): the game checks it itself and, without it, '
                          'waits on a black screen for the PlayStation Store. Put your own INDEX.EDAT there (make the '
                          'folders if needed), or switch online off on the Online page to play offline', path=edat))
    return out


def online_pass_file(s):
    """Where the game reads the Online Pass (2026-11-13: a tester's pass in another folder left the game on a black
    store screen with no message). Only for installs whose /dev_hdd0 is ./gamedata (the launcher's own game setup,
    macOS); None elsewhere (development setups keep /dev_hdd0 where run.ps1 has it)."""
    if not (MAC or s.get('port_hdd') == '1'):
        return None
    return (ROOT / 'gamedata' / 'dev_hdd0' / 'game' / title_id(s) / 'USRDIR' / 'DLC' / 'DLC0' / 'INDEX.EDAT')


def prepare_game(s, status=lambda m: None):
    """Everything before the game starts (it can take a while: the disc,
    the saves, the mods' archives). Returns the environment and the disc
    folder it mounted, if any."""
    status(tr('Preparing the game…'))
    if WINDOWS and s.get('disc_iso'):
        from . import gamefiles     # Windows mounts the .iso; its drive letter can change
        try:
            root = gamefiles.ensure_mounted(s)
        except gamefiles.NotReady as e:
            raise OSError(str(e))
        s = {**s, 'disc_dir': str(root)}
    mounted = ensure_disc(s)
    problem = disc_problem(s)
    if problem:
        release_disc(mounted)
        raise OSError(problem[0].upper() + problem[1:] + '.')
    if s.get('backup_saves') == '1':
        status(tr('Copying the saves…'))
        backup_saves()
    apply_save_settings(s)
    status(tr('Preparing the mods…'))
    return game_environment(s), mounted


def _spawn(s, env, log):
    flags = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
    return subprocess.Popen([play_exe(s), s['eboot']], cwd=str(ROOT), env=env,
                            stdout=log, stderr=subprocess.STDOUT, creationflags=flags)


def boot_hung(text):
    """The rare boot hang (4 of ~120 test boots). The main thread waits for
    an SPU job list whose jobs never finish, right after the game sets up its
    graphics a second time. The game's hang watchdog samples its threads at
    8 s and 15 s; a stuck boot has guest thread 1 at lr=0x00467B1C in both,
    and either the event the game sends once that first wait is over
    ("port_send(port=1)", about 2 s into a normal boot) never came, or it did
    and the first menu frames then stopped: the command stream sat on a
    placeholder jump the SPU jobs never patched, and the runtime gave up on
    it ("FIFO resync (jump-park)")."""
    return (text.count('guest-tid 1   lr=0x00467B1C') >= 2 and
            ('port_send(port=1)' not in text or 'FIFO resync (jump-park)' in text))


class WatchedGame:
    """The game process, started again (at most twice) when its boot hangs
    (boot_hung). Stands in for the Popen: poll, wait, terminate, kill, pid."""

    def __init__(self, s, env, log_path, log):
        self.s, self.env, self.log_path = s, env, log_path
        self.lock = threading.Lock()
        self.stopped = False
        self.restarts = 0
        self.extra_logs = []
        self.proc = _spawn(s, env, log)
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        start = 0                     # where this copy's part of the log begins
        while True:
            proc, t0 = self.proc, time.time()
            hung = False
            while proc.poll() is None and time.time() - t0 < 40:
                time.sleep(1)
                try:
                    with open(self.log_path, encoding='utf-8', errors='replace') as f:
                        f.seek(start)
                        hung = boot_hung(f.read())
                except OSError:
                    pass
                if hung:
                    break
            if not hung or self.restarts >= 2:
                return
            with self.lock:
                if self.stopped or proc.poll() is not None:
                    return
                self.restarting = True
                proc.kill()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass
                log = open(self.log_path, 'a', encoding='utf-8', errors='replace')
                log.write('\n[launcher] the boot hung (main thread stuck at 0x00467B1C): starting the game again\n')
                log.flush()
                start = log.tell()
                self.extra_logs.append(log)
                self.restarts += 1
                try:
                    self.proc = _spawn(self.s, self.env, log)
                finally:
                    self.restarting = False

    restarting = False

    @property
    def pid(self):
        return self.proc.pid

    @property
    def returncode(self):
        return self.proc.returncode

    def poll(self):
        with self.lock:
            return None if self.restarting else self.proc.poll()

    def wait(self, timeout=None):
        while True:
            proc = self.proc
            code = proc.wait(timeout)
            with self.lock:
                if self.proc is proc and not self.restarting:
                    for log in self.extra_logs:
                        log.close()
                    self.extra_logs = []
                    return code

    def stop(self):
        """No more boot-hang restarts (set under the lock the watcher holds
        while it restarts, so a restart already under way finishes first)."""
        with self.lock:
            self.stopped = True

    def terminate(self):
        with self.lock:
            self.stopped = True
            self.proc.terminate()

    def kill(self):
        with self.lock:
            self.stopped = True
            self.proc.kill()


def discord_helper(s):
    """The Discord presence helper (host\\mpnet\\discord: gowa_discord.exe + discord_partner_sdk.dll), or None."""
    exe = Path(s.get('online_discord_exe') or (ROOT / 'discord' / 'gowa_discord.exe'))
    return exe if exe.is_file() else None


def start_discord_helper(s, game_pid):
    """Online + "Show on Discord": shows what the player does on their Discord profile while the game runs (it
    ends with the game). Rich Presence goes through the Discord app on this PC; no sign-in, nothing if it is closed."""
    exe = discord_helper(s)
    name = (s.get('online_name') or '').strip()
    server = (s.get('online_server') or '').strip()
    if not (online_on(s) and s.get('online_discord') == '1' and exe and name and server):
        return None
    try:
        return subprocess.Popen([str(exe), '--server', server, '--name', name[:16], '--pid', str(game_pid)],
                                cwd=str(exe.parent), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except OSError:
        return None


def take_discord_join(s):
    """A friend's Join from Discord (the helper wrote "server|code"): returns (server, code) once, else None."""
    exe = discord_helper(s)
    f = (exe.parent / 'discord-join.txt') if exe else None
    if not f or not f.is_file():
        return None
    try:
        text = f.read_text(encoding='utf-8', errors='replace').strip()
        f.unlink()
    except OSError:
        return None
    server, _, code = text.partition('|')
    code = ''.join(c for c in code.upper() if c.isascii() and c.isalnum())[:12]
    server = server.strip()[:100]
    if not server or not code or any(c.isspace() for c in server):
        return None
    return server, code


def start_game(s, env, log_path, parent=None):
    """Start the game; with `parent` (a window handle) it renders inside that
    window. The previous run's log is kept as last_run.prev.log. A boot that
    hangs is started again (WatchedGame)."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.is_file():
        log_path.replace(log_path.with_suffix('.prev.log'))
    log = open(log_path, 'w', encoding='utf-8', errors='replace')
    if parent:
        env = {**env, 'PS3_PARENT_HWND': str(parent)}
    try:
        game = WatchedGame(s, env, log_path, log)
        start_discord_helper(s, game.pid)
        return game, log
    except BaseException:
        log.close()
        raise
