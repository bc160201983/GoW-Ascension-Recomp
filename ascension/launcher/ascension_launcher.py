"""God of War: Ascension (PC) launcher.

Settings for the recompiled game (window, video, controls, language, mods,
...), kept in %APPDATA%\\ascension-launcher\\settings.json (macOS:
~/Library/Application Support/ascension-launcher) and handed to the game as the
environment switches ps3recomp reads. PLAY starts build/win/AscensionRecomp.exe
(macOS: build/mac/AscensionRecomp) with the disc's EBOOT.ELF; `--play` does the
same without the window.

Artwork (backgrounds, logo, icon, music) is not part of the repository: it is
read from launcher/assets/ or, failing that, ../work/data/launcher/, where the
launcher makes it from the player's own disc on the first start (app/assets.py;
`--make-assets` does it from the command line).

The code is in app/: config (paths, settings, the pages' choices), game
(disc, mods, saves, the game's environment), patches (RPCS3 game patches,
built into the game by scripts/windows/apply-patches.ps1), assets (artwork
from the disc), music, native (macOS Dock, fonts) and ui (the window).
"""

import ctypes
import sys
from pathlib import Path
from app.config import LOG_DIR, SHOT, WINDOWS, load_settings, merge_ingame, save_settings
from app.game import disc_ps3_game, game_running, missing, prepare_game, release_disc, start_game
from app.i18n import set_language, tr
from app.ui import Launcher


def main():
    if '--make-assets' in sys.argv:
        from app.assets import OUT_DIR, make_assets, missing as missing_art
        s = load_settings()
        done = make_assets(disc_ps3_game(s), log=print)
        print(f'made {", ".join(done) or "nothing"} in {OUT_DIR}' + (f'; still missing: {", ".join(missing_art())}'
                                                                      if missing_art() else ''))
        return 0
    if '--play' in sys.argv:
        s = load_settings()
        if merge_ingame(s):          # changed in the game's own menu (F1) last time
            save_settings(s)
        set_language(s['ui_language'])
        gone = missing(s)
        if gone:
            print(tr('Missing {items}', items='; '.join(gone)))
            return 1
        if game_running(s['game_exe']):
            print(tr('The game is already running (started elsewhere). Only one copy can run at a time: close it first.'))
            return 1
        env, mounted = prepare_game(s, status=print)
        try:
            process, log = start_game(s, env, LOG_DIR / 'last_run.log')
            code = process.wait()
            log.close()
            if merge_ingame(s):
                save_settings(s)
        finally:
            release_disc(mounted)
        return code
    if WINDOWS:
        try:  # the taskbar shows this window's icon, not Python's; sharp text on scaled displays
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('ascensionpc.launcher')
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    import tkinter as tk
    root = tk.Tk()
    app = Launcher(root)
    if SHOT:
        out = Path(sys.argv[sys.argv.index('--shot') + 1])
        from app.ui import PAGES as pages

        def grab(i):
            from PIL import ImageGrab
            root.update()
            try:     # the window itself, whatever is on top of it (Windows)
                if not WINDOWS:
                    raise OSError
                img = ImageGrab.grab(window=int(root.wm_frame(), 16))
            except (TypeError, ValueError, OSError):
                x, y = root.winfo_rootx(), root.winfo_rooty()
                img = ImageGrab.grab((x, y, x + root.winfo_width(), y + root.winfo_height()))
            img.save(out / f'{pages[i]}.png')
            if i + 1 < len(pages):
                app.show(pages[i + 1])
                root.after(1800 if pages[i + 1] == "patches" else 700, grab, i + 1)
            else:
                root.destroy()
        root.after(2500, grab, 0)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
