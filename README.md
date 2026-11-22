# God of War: Ascension PC port: source code

<p align="left">
  <a href="https://www.youtube.com/@BilalReloaded"><img alt="YouTube: BilalReloaded" src="https://img.shields.io/badge/YouTube-BilalReloaded-FF0000?style=for-the-badge&logo=youtube&logoColor=white"></a>
  <a href="https://www.patreon.com/cw/bilalreloaded"><img alt="Support on Patreon" src="https://img.shields.io/badge/Patreon-Support-F96854?style=for-the-badge&logo=patreon&logoColor=white"></a>
</p>

> **A message from the author**
>
> Don't expect many updates from me from now on. I won't be able to continue the project because of my finances. The project is open source, so you're welcome to build on it. Thank you for all the support.
>
> BilalReloaded

This is the source code of the PC port of the PS3 game **God of War: Ascension** (BCES01741, Europe, version 1.00).

It is a **static recompilation**, not an emulator. Tools in this repository translate the game's PowerPC (PPU) and SPU machine code into C and C++ ahead of time. That code is then compiled into a normal Windows program. A runtime supplies the PS3 system libraries, and a Direct3D 12 renderer draws the RSX graphics work.

> [!IMPORTANT]
> **No game code or game data is in this repository.** Nothing here was taken from the game: no translated code, no executables, no assets, no textures, no audio, no shader caches. You need **your own legally dumped copy** of the game (the European disc BCES01741, version 1.00) and you translate it yourself on your own PC with the steps below. The translated code you make is made from the game, so do not share it.
>
> This is a fan project. It is not affiliated with Sony Interactive Entertainment or Santa Monica Studio. God of War is a trademark of Sony Interactive Entertainment.

**Single player and community online multiplayer.** The game's multiplayer works through **community servers** that
anyone can run (no Sony servers): see [Online multiplayer](#online-multiplayer). No server address is built in.

## Download (ready to play)

The [Releases](../../releases/latest) page has **GodOfWarAscension-0.2.0-windows.zip**: extract it anywhere and run the launcher. You still need your own copy of the game. Already playing an older version? Extract it **over your old folder**, or into a new folder and start the launcher once: your saves are copied over.

The ready-made build includes the hand-written native SPU code, so it is faster in heavy fights than a build made from this source alone.

## What has been fixed so far

Highlights from 0.1.0 (2026-10-30) to 0.2.0. The full list is in [CHANGELOG.md](CHANGELOG.md).

**Speed**
- More of the game runs as real PC code (SPU jobs rewritten natively): much faster on 4-8 core CPUs (0.1.12, 0.1.22).
- Native job pool, faster recompiled code and better job order: big gains on 2- and 4-core CPUs (0.2.0, see CHANGELOG.md).
- Occlusion culling: objects hidden behind walls are not drawn (0.1.17, 0.1.21).
- Cheaper draws in the graphics thread, and since 0.1.31 those speed-ups are on for everyone, not only with the FPS counter shown.
- Thread placement tuned for 6-core CPUs, 16-core Ryzen X3D CPUs, and switched off where it hurt (8-core CPUs, Intel CPUs with E-cores).
- Fewer CPU cycles wasted waiting; other programs slow the game down less.

**Stutter**
- Shaders compiled before playing, with a recorded set of 13,425 pipelines shipped in the package.
- Shaders built in the background; no more stutter at start-up and after loading screens; no texture stutter after loads.
- Optional "Fewer shader stutters" setting (NVIDIA): a stand-in shader is drawn while the real one is built, also right after loading screens.

**Crashes and freezes**
- Freezes where the picture stopped but the sound kept playing (0.1.13, 0.1.25).
- Freezes every few seconds on 8-core CPUs (0.1.23), start-up hang with SPU MLAA on (0.1.24), boot crash (0.1.10), "Not Responding" window (0.1.27).
- Falling through the floor on slower PCs (0.1.20).

**Graphics**
- Missing floors and objects, garbled scenery, striped textures, white patches on Kratos's face at high resolutions, glowing blobs on NVIDIA, blown-out lighting on AMD, rooftop shimmer.
- Native 1080p, 1440p, 4K and 5K rendering; SMAA, AMD FSR 3.1, sharpening, anisotropic filtering.
- Glow (bloom) made at full resolution at 1440p/4K: smooth instead of dotted, brightness within 1% of the PS3 (0.1.31).

**Controls, saves and launcher**
- Controllers: full stick range on diagonals, right-stick evade, controllers that stopped after cutscenes, wireless controllers that froze during heavy vibration (0.1.31).
- Saves: backups while you play, export and import, the autosave that went to the wrong folder (0.1.27), and saves carried over automatically when you install a new version in a new folder (0.1.30).
- In-game settings menu (F1 or hold SELECT), trophies with a Trophies page in the launcher, 120 FPS and unlocked frame rate limits, saving in long sessions fixed (0.2.0).
- US disc support, game folder or .iso picked directly (no RPCS3 decrypting), FAT32 split files joined, cheats and patches with tick boxes, PT-BR translation mod, 1.04 sound mix, skip intro and story movies.

## Performance

All numbers are measured (each one says on which PC and how). The game is CPU-heavy: in big fights the PS3's SPU work and the game's main thread, not the graphics card, set the frame rate.

**Test PC: Ryzen 9 9950X3D + RTX 5080**

| Scene | Setup | FPS |
|---|---|---|
| Heaviest fight | 1440p window, 2x render scale, 60 cap, all cores | 59-60 (capped) |
| Rooftop (very busy view) | all cores, FPS cap 240, 0.1.31 build | 106 |
| Heaviest fight | limited to 6 cores, FPS cap 240 | ~77 |
| Rooftop | limited to 6 cores, FPS cap 240 | ~51-53 |
| Heaviest fight | limited to 4 cores, FPS cap 240 | ~48-54 |
| Rooftop | limited to 4 cores, FPS cap 240, 0.1.31 speed-ups | ~41-45 (was ~38-40) |

The 4- and 6-core rows are this PC limited to fewer cores, which is faster than a real 4- or 6-core CPU.

**Shader stutter (optional "Fewer shader stutters" setting, RTX 5080, empty shader cache)**

| Heaviest fight | Setting off | Setting on |
|---|---|---|
| Time frozen waiting for shaders (1440p, 2x) | 3.5 s | 0.5-0.9 s |
| Freezes of 50 ms or longer | 35 | 4 |
| Worst 5 seconds | 43 fps | 56 fps |
| Waiting right after a loading screen (4 cores) | 1.9-2.6 s | ~0.4 s |

**Players' PCs (from their logs)**

| PC | Version | Result |
|---|---|---|
| 6-core Ryzen + RX 6750 XT, 4K | 0.1.28 | ~56 fps in busy scenes with Thread placement on |
| Core i5-14400F + RTX 5070 | 0.1.28 | ~50 fps in lighter areas, 21-25 fps in one heavy area |
| Laptop, 4P+4E Intel + RTX 4050 Laptop | 0.1.28 | 30 fps (locked by the "Low" preset's 30 fps cap) |
| 4-core / 8-thread CPU + RX 580 | 0.1.22 | 15-30 fps, 30 fps cap |

## Known issues

- **Videos can show black (sound plays) when "Skip cutscenes" is off.** Keep Skip cutscenes on.
- **Kratos keeps dying in a cutscene near the end of the game.** Turn on Skip cutscenes for that part.
- **Kratos's skin turns yellow in one Chapter 29 cutscene** (the "This is Sparta" kick).
- **Box-shaped patches in some floor lighting and shadows.**
- **Heavy fights drop below 60 fps on CPUs with 8 cores or fewer**, more on laptops (use Best performance power mode, stay plugged in).
- **Some PCs with slow per-draw cost** (seen on one i5-14400F): one heavy area runs at 21-25 fps; the cause was not found.
- **The first visit to a new area can stutter briefly** while new shaders compile (much less with the NVIDIA setting).
- **"Fewer shader stutters" is tested on NVIDIA only;** the game turns it off on AMD and Intel.
- **AMD FSR frame generation runs the game at half your refresh rate:** below 120 Hz use Off or Anti-aliasing.
- **Needs a CPU with AVX2** (no Intel Core 2nd/3rd gen, no AMD FX) and a DirectX 12 card with Shader Model 6 (old Intel HD 4000/4600 graphics usually cannot run it). The launcher says so when this is the reason.
- **Offline Multiplayer (experimental):** going back to the main MP lobby with the buttons does not always work; hold SELECT + START to quit.
- **Online multiplayer:** players far apart (another continent) get high lag; players behind carrier-grade NAT always
  go through the server's relay. The host leaving a 3+ player match ends it (the game never finished host migration).

## Online multiplayer

The game's own multiplayer (Quick Match, the Game Mode list, Favor of the Gods, Team Favor, Capture the Flag, Trial of
the Gods solo and co-op, 2 to 8 players) works again through **community servers**. Nothing here contacts Sony.


### What you need to play online

1. **Your own copy of the game**, set up in the launcher as usual (the launcher shows *America disc* or *Europe disc*).
   Players in one match need the **same region**: USA (BCUS98232) with USA, Europe (BCES01741) with Europe, as on
   the PS3.
2. **Your own Online Pass** (the game calls it a *Network Pass*). It is the licence file **`INDEX.EDAT`** that your
   PS3 saved when you redeemed the Online Pass code from your game box (or bought it on the PlayStation Store).
   The game checks it itself, exactly like on the console: no pass, no online play. Nothing here skips that check,
   and nobody can give you theirs: use only your own.
   - **Where it is on your PS3:** `/dev_hdd0/game/BCUS98232/USRDIR/DLC/DLC0/INDEX.EDAT` (USA) or
     `/dev_hdd0/game/BCES01741/USRDIR/DLC/DLC0/INDEX.EDAT` (Europe). Copy it to your PC with a tool you already use
     for your console (for example an FTP server on your own PS3, or a backup of its hard drive).
   - **Where it goes on your PC** (inside the port folder; make the folders if they are missing):

     | Your game | Put `INDEX.EDAT` here |
     |---|---|
     | USA (BCUS98232) | `ascension-recomp\ascension\gamedata\dev_hdd0\game\BCUS98232\USRDIR\DLC\DLC0\INDEX.EDAT` |
     | Europe (BCES01741) | `ascension-recomp\ascension\gamedata\dev_hdd0\game\BCES01741\USRDIR\DLC\DLC0\INDEX.EDAT` |

   - The name must be exactly `INDEX.EDAT`. In File Explorer turn on *View > Show > File name extensions* so it is
     not secretly `INDEX.EDAT.txt`. The file is small (a few hundred bytes).
   - With *Play online* on, the launcher's **Play** tells you when the pass is not where the game reads it, with the
     exact path. If the game itself says **"A Network Pass is required"**, the pass is missing, in the wrong folder,
     or from the other region.
3. **A server**: type its address on the launcher's **Online** page (*Server*), paste a friend's invite code
   (`CODE@server`) into *Match code*, or pick a LAN server under *Pick a server*.
4. **A player name** (up to 16 letters or digits). The first sign-in keeps it for your PC: the key file
   `ascension-recomp\ascension\gamedata\mpnet-token.txt` proves it is you. Keep that file (copy it with your saves
   when you move to a new folder) and never share it.
5. **Windows Firewall:** when it asks about `AscensionRecomp.online.exe`, allow it (at least *Private networks*).
   The game talks to other players on **UDP 3658**.

### Playing

- **Launcher, Online page:** turn on *Play online*, type a server address (or pick one under *Pick a server*: LAN
  servers on your network appear by themselves), choose a player name, press Play.
- **Private matches:** *New code* then *Copy code* gives `CODE@server`; friends paste it into *Match code* and land on
  the same server and match.
- **LAN play:** one player presses *Host a LAN server* (the launcher runs the server with its own Python); the others
  press *Refresh* and pick it. The launcher looks for the server code in `ascension\server\`: copy the `server\`
  folder of the server repository there.
- **Run a server:** [gowa-community-server](https://github.com/bc160201983/gowa-community-server) (Python, one-command
  Linux installer, Windows starter, admin page, leaderboards, LAN discovery). Its
  [hosting guide](https://github.com/bc160201983/gowa-community-server/blob/main/docs/HOSTING-GUIDE.md) also explains
  how to get a server into the launcher's list.
- **Discord:** *Show on Discord* starts `ascension\discord\gowa_discord.exe` (Rich Presence and a Join button); build
  it from `ascension\discord-helper\` (needs the Discord Social SDK).
- **How it works:** `ps3kit/libs/network/mpnet.c` (sign-in, ticket, peer addresses, relay, HTTPS), the NP/network HLE
  next to it, and `ascension/tools/mksrc.py`, which `prepare.ps1` runs after the lift (it switches the matchmaker's SSL
  wrapper off and reads the match settings as plain text while online play is on; a community server has no Sony
  certificate). Details: `ps3kit/docs/ONLINE.md`.

## What is in here

```
ps3kit/      the runtime and the translators (a fork of ps3recomp, MIT)
ascension/   the port: CMake project, game-specific host code, scripts, tools, launcher, patches
licenses/    licence texts of third-party code and libraries
work/        (you make it) your decrypted EBOOT.ELF and what is made from it; never committed
```

`MANIFEST.md` lists what this release contains and what was left out, and why.

## Building it (Windows 10/11, x64)

A build takes about 10-40 minutes on a fast PC. You need about 30 GB of free disk space and at least 16 GB of RAM.

### 1. Install the tools

- **Visual Studio 2022** (the free Build Tools are enough) with the workload *Desktop development with C++* and the component *C++ Clang Compiler for Windows* (clang-cl). CMake and Ninja come with it.
- **Python 3.10 or newer** (python.org), on PATH. Then, from this folder:
  ```powershell
  python -m pip install -r ps3kit\requirements.txt
  python -m pip install pillow      # for the launcher
  ```
- **FFmpeg**, a shared LGPL build for Windows x64, for example `ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases). Extract it anywhere.
- **SDL2** (optional, for PlayStation controllers): `SDL2-devel-2.32.x-VC.zip` from [libsdl-org/SDL](https://github.com/libsdl-org/SDL/releases). Extract it anywhere.

### 2. Decrypt your own EBOOT

From your own disc, take `PS3_GAME\USRDIR\EBOOT.BIN` and decrypt it to `EBOOT.ELF`, with either:

- RPCS3: *Utilities > Decrypt PS3 Binaries*, or
- `python ascension\tools\decrypt_self.py` with your own scetool-format key file (no keys are included).

Put the result here (make the folders):

```
work\pristine\EBOOT.ELF
```

It must be the disc EBOOT of BCES01741 version 1.00. The scripts check this and stop if it is not.

### 3. Local settings

```powershell
cd ascension
copy .env.example .env
notepad .env
```

Set `VCVARS`, `FFMPEG_INCLUDE_DIR`, `FFMPEG_BIN_DIR` and `SDL2_DIR` to the folders on your PC. Leave `PS3KIT_DIR=../ps3kit` and `WORK_DIR=../work` as they are. Set `JOBS` to about your RAM in GB divided by 4.

### 4. Translate your EBOOT

Still in `ascension\`:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\prepare.ps1
```

This takes a few minutes. It reads `work\pristine\EBOOT.ELF` and writes `src\recomp\` (the PPU code as C++), `src\spu\` (the SPU programs as C), `meta\spu\`, `meta\patch_sites.json`, `host\ascension_patch_table.inc` and `work\jobmods\`. All of these are made from the game: they are in `.gitignore` and must not be shared.

### 5. Build

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\build.ps1
```

The first run configures CMake. The game is `ascension\build\win\AscensionRecomp.exe`, with `SDL2.dll` and the FFmpeg DLLs copied beside it. The build log is `ascension\build\logs\build.log`.

### 6. Play

The game also needs your disc's files. Start the launcher and use it to pick your game folder (the folder that holds `PS3_GAME`) or your `.iso`, then press PLAY:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\launcher.ps1
```

Or run the game directly with `.\scripts\windows\run.ps1` (it expects the extracted disc in `work\disc`, with the decrypted `EBOOT.ELF` at `work\disc\PS3_GAME\USRDIR\EBOOT.ELF`).

If you want the launcher to decrypt `EBOOT.BIN` by itself, put a scetool-format `keys.txt` next to `ascension\launcher\` (or set `PS3_KEYS` to its path). No console keys are included in this source.

## Game patches

`ascension\patches-rpcs3\ascension-1.00.yml` has RPCS3-format patches for version 1.00: Unlock FPS (60 fps), Disable Depth of Field, Disable Motion Blur, Disable MLAA (ported from the RPCS3 community patches for 1.04 by ZEROx and illusion) and a few cheats. They are switched on and off when the game starts (no rebuild), from the launcher's Patches page.

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and contributors: the recompiler, the runtime and the system libraries this port is built on (MIT). See `ps3kit/CONTRIBUTORS.md`.
- `ascension/tools/decrypt_self.py` comes from the [Twisted Metal](https://github.com/sp00nznet/twistedmetal) port (MIT).
- `ascension/launcher/app/unself.py` is a port of part of [RPCS3](https://github.com/RPCS3/rpcs3)'s `Crypto/unself.cpp` (GPL-2.0).
- RPCS3 community patches for God of War: Ascension 1.04 by ZEROx and illusion, ported to 1.00.
- AMD FidelityFX (MIT), SMAA (MIT), the ATRAC3+ decoder from PPSSPP / FFmpeg (LGPL-2.1), stb_image (public domain), SDL2 (zlib), FFmpeg (LGPL), the Cinzel font (SIL Open Font License).
- The launcher follows the look of FelipePrado0's [gow3_pc](https://github.com/FelipePrado0/gow3_pc) launcher.

## Licence

The port's own code (everything in `ascension/` unless a file says otherwise) is MIT, see `LICENSE`. `ps3kit/` is MIT (ps3recomp), see `ps3kit/LICENSE`. Third-party code keeps its own licence; the texts are in `licenses/`. `ascension/launcher/app/unself.py` is GPL-2.0 because it is derived from RPCS3.
