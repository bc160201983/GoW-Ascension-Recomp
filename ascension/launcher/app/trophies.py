"""The game's trophies for the launcher's Trophies page (host/trophies, 2026-11-22).

The list (names, descriptions, grades, hidden flags, icons) comes only from the player's own game files: the disc's
PS3_GAME/TROPDIR/<NPCOMMID>/TROPHY.TRP (or an installed update's copy; the newest trophy-set version wins). The game
copies those files into gamedata/trophies/<NPCOMMID>/ when it starts; this module reads that copy, or makes it from the
TRP itself when the game has not run yet. Which trophies are unlocked: gamedata/trophies/<commId>.json, written only
by the game ({"trophies": [{"id": N, "timestamp": unix seconds}]}). Nothing here writes that file.

TRP: big-endian u32 magic 0xDCA24D00, u32 version, u64 file size, u32 entry count, u32 entry size (64); entries at
0x40: char name[32], u64 offset, u64 size, u32 flags, padding.
"""
import json
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from .config import ROOT

NPCOMMID = 'NPWR00802_00'                 # God of War: Ascension (EU and US discs, all updates)
POINTS = {'B': 15, 'S': 30, 'G': 90, 'P': 180}   # what each grade is worth in the PS3's own progress figure
GRADES = {'P': 'Platinum', 'G': 'Gold', 'S': 'Silver', 'B': 'Bronze'}
# launcher language -> the set's text file (trophy language numbers: 03 Spanish, 17 Portuguese BR)
TEXT_FILE = {'es': 'TROP_03.SFM', 'pt-BR': 'TROP_17.SFM'}


def _read_trp(path):
    """{name: bytes} of a TROPHY.TRP, or None when it is not one."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if len(data) < 0x40:
        return None
    magic, _ver, size, count, esize = struct.unpack('>IIQII', data[:24])
    if magic != 0xDCA24D00 or size != len(data) or esize != 64 or not 0 < count <= 1024 or 0x40 + count * 64 > len(data):
        return None
    files = {}
    for i in range(count):
        e = data[0x40 + i * 64:0x40 + (i + 1) * 64]
        name = e[:32].split(b'\0')[0].decode('ascii', 'replace')
        off, sz = struct.unpack('>QQ', e[32:48])
        if off > len(data) or sz > len(data) - off or not name or re.search(r'[\\/:]|^\.', name):
            return None
        files[name] = data[off:off + sz]
    return files


def _xml(raw):
    try:
        return ET.fromstring(raw)
    except ET.ParseError:
        return None


def _set_version(raw):
    root = _xml(raw)
    return (root.findtext('trophyset-version') or '').strip() if root is not None else ''


def trp_candidates(settings=None, root=ROOT):
    """Every TROPHY.TRP of the game this install can see."""
    out = [root.parent / 'work' / 'disc' / 'PS3_GAME' / 'TROPDIR' / NPCOMMID / 'TROPHY.TRP']
    disc = (settings or {}).get('disc_dir', '')
    if disc:
        out.append(Path(disc) / 'PS3_GAME' / 'TROPDIR' / NPCOMMID / 'TROPHY.TRP')
    game = root / 'gamedata' / 'dev_hdd0' / 'game'
    if game.is_dir():
        out += sorted(game.glob(f'*/TROPDIR/{NPCOMMID}/TROPHY.TRP'))
    return [p for p in out if p.is_file()]


def ensure_cache(settings=None, root=ROOT):
    """The set's files in gamedata/trophies/<NPCOMMID>/ (made from the newest TRP found when they are missing or
    older). Returns that folder, or None when there are no trophy files anywhere."""
    cache = root / 'gamedata' / 'trophies' / NPCOMMID
    have = cache / 'TROPCONF.SFM'
    have_ver = _set_version(have.read_bytes()) if have.is_file() else ''
    best, best_ver = None, ''
    for p in trp_candidates(settings, root):
        files = _read_trp(p)
        if not files or 'TROPCONF.SFM' not in files:
            continue
        v = _set_version(files['TROPCONF.SFM'])
        if best is None or v > best_ver:
            best, best_ver = files, v
    if best is not None and (not have.is_file() or best_ver > have_ver):
        try:
            cache.mkdir(parents=True, exist_ok=True)
            for name, blob in best.items():
                dst = cache / name
                if not dst.is_file() or dst.stat().st_size != len(blob):
                    tmp = dst.with_name(dst.name + '.tmp')
                    tmp.write_bytes(blob)
                    tmp.replace(dst)
        except OSError:
            return None
    return cache if have.is_file() else None


def unlocked(root=ROOT):
    """{id: unix time} from the game's file (the same line-based form the game writes)."""
    path = root / 'gamedata' / 'trophies' / (NPCOMMID.split('_')[0] + '.json')
    out = {}
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        for t in data.get('trophies', []):
            out[int(t['id'])] = int(t.get('timestamp', 0))
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        pass
    return out


def merge_unlocks_from(old_root, root=ROOT):
    """Saves found in an older install folder are being taken over (game.recover_save): its unlocked trophies come
    along -- the ids missing here are added with their old time; ids already here are left as they are (a file with
    nothing to add is not touched; a file here that cannot be read is never replaced). Written to <file>.tmp, then put in place; the old
    file is only read. Returns how many ids were added (0 when nothing to do). Only while the game is not running."""
    old = Path(old_root) / 'gamedata' / 'trophies' / (NPCOMMID.split('_')[0] + '.json')
    if not old.is_file() or Path(old_root).resolve() == Path(root).resolve():
        return 0
    cur = root / 'gamedata' / 'trophies' / old.name
    try:   # a current file that exists but cannot be read: never replace it (its unlocks would be lost)
        if cur.is_file() and cur.stat().st_size and 'trophies' not in json.loads(cur.read_text(encoding='utf-8')):
            return 0
    except (OSError, ValueError, TypeError):
        return 0
    theirs, mine = unlocked(Path(old_root)), unlocked(root)
    added = [i for i in theirs if i not in mine]
    merged = dict(mine)   # the ids already here keep their own times: the file only changes when ids are added
    for i in added:
        merged[i] = theirs[i]
    if not added:
        return 0
    path = root / 'gamedata' / 'trophies' / old.name
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = ',\n'.join(f'    {{ "id": {i}, "timestamp": {merged[i]} }}' for i in sorted(merged))
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text('{\n  "trophies": [\n' + rows + '\n  ]\n}\n', encoding='utf-8')
    tmp.replace(path)
    return len(added)


def install_root_of(path):
    """The install folder (the one holding gamedata\\) a found save file lies in, or None."""
    for p in Path(path).parents:
        if p.name.lower() == 'gamedata':
            return p.parent
    return None


def load(language='en', settings=None, root=ROOT):
    """The whole page's data, or None when the game files have no trophies."""
    cache = ensure_cache(settings, root)
    if cache is None:
        return None
    conf = _xml((cache / 'TROPCONF.SFM').read_bytes())
    if conf is None:
        return None
    texts = None
    for name in (TEXT_FILE.get(language), 'TROP.SFM'):
        if name and (cache / name).is_file():
            texts = _xml((cache / name).read_bytes())
            if texts is not None:
                break
    words = {}
    if texts is not None:
        for t in texts.iter('trophy'):
            words[int(t.get('id', -1))] = (' '.join((t.findtext('name') or '').split()),
                                           ' '.join((t.findtext('detail') or '').split()))
    got = unlocked(root)
    rows = []
    for t in conf.iter('trophy'):
        tid, grade = int(t.get('id', -1)), (t.get('ttype') or 'B')[:1]
        if tid < 0 or grade not in POINTS:
            continue
        name, detail = words.get(tid, ('', ''))
        icon = cache / f'TROP{tid:03d}.PNG'
        rows.append({'id': tid, 'grade': grade, 'hidden': t.get('hidden') == 'yes', 'name': name, 'detail': detail,
                     'icon': icon if icon.is_file() else None, 'unlocked': tid in got, 'time': got.get(tid, 0)})
    if not rows:
        return None
    total = sum(POINTS[r['grade']] for r in rows)
    have = sum(POINTS[r['grade']] for r in rows if r['unlocked'])
    counts = {g: (sum(1 for r in rows if r['grade'] == g and r['unlocked']), sum(1 for r in rows if r['grade'] == g))
              for g in 'PGSB'}
    title = ''
    if texts is not None:
        title = ' '.join((texts.findtext('title-name') or '').split())
    icon0 = cache / 'ICON0.PNG'
    return {'title': title, 'version': _set_version((cache / 'TROPCONF.SFM').read_bytes()),
            'icon': icon0 if icon0.is_file() else None, 'trophies': rows,
            'unlocked': sum(1 for r in rows if r['unlocked']), 'total': len(rows),
            'percent': int(have * 100 // total) if total else 0, 'counts': counts}


def ordered(rows):
    """Unlocked first (newest first), then the rest in the game's order."""
    return sorted(rows, key=lambda r: (not r['unlocked'], -r['time'] if r['unlocked'] else 0, r['id']))
