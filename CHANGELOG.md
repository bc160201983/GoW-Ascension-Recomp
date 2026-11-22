# Changelog

## 0.2.0 (beta), 2026-11-22

### Big performance update
All numbers below were measured during development on our test PCs (a 16-core Ryzen 9 9950X3D desktop, limited to fewer cores for the
low-core numbers, against 0.1.35). Your results will be different.

- **Native job pool:** the game's background jobs (the PS3's SPU work) now run on a pool of normal PC threads. This
  helps PCs with few cores the most: with 2 cores we measured 2 to 2.6 times the frame rate of 0.1.35; with 4 cores
  +24% to +35%. (These 2- and 4-core numbers were measured with the new GPU vertex decoding switched on; see below.)
- **Faster recompiled game code:** busy fights run 17-19% faster on our test PCs.
- **Background jobs in a better order:** the jobs that the next frame waits for most now go first. Some opening areas
  run 17-22% faster on our test PCs.
- **SPURS task fix:** a part of the PS3 job system now runs on the same fast path as the rest.
- **GPU vertex decoding (experimental, off by default):** new checkbox in the launcher (Graphics, Shaders). The
  graphics card decodes the game's 3D model data itself instead of the processor. Faster in busy scenes, mostly on PCs
  with few CPU cores. Turn it off if something looks wrong.

### In-game settings menu
Press **F1** on the keyboard, or **hold SELECT** on the controller, while playing. Change graphics settings without
going back to the launcher; most changes apply at once, the rest after a restart. Your choices are saved for the
launcher. The menu is new: please tell us if a key or button does not work with your controller.

### Trophies
The game's trophies now unlock while you play, with a small popup when you earn one. The launcher has a new
**Trophies** page: it lists the game's trophies (read from your own game files) and shows which ones you earned.
Unlocks are kept in `ascension-recomp\ascension\gamedata\trophies`; extracting over your old folder keeps them.

### Disable dynamic shadows (option)
New checkbox in the launcher (Graphics, Lighting), **off by default**: "Disable dynamic shadows (flatter look)". The game
stops drawing its real-time shadows, so the picture looks brighter and flatter. It can also be switched while playing
(F1 menu, Display).

### Frame rate limits
- New **120 FPS (experimental)** limit, in the launcher and in the in-game menu. Only a strong CPU holds it, and even on our
  fastest test PC some busy areas stay below 120.
- New **Unlocked** option (no limit).

### Fixes
- **Saving in long sessions:** Long play sessions no longer stop saving after about 40-60 minutes (progress could be lost when returning to the menu).
- **Delete Profile** in the game's profile screen now really deletes the profile.

### Known issue
- After Continue in the first Megaera fight, Kratos may appear without his head and wrist chains; a new game is fine.

## 0.1.35 (beta), 2026-11-18

### Saves: the real date
The in-game save list showed dates in 1969 or 1970: the game's clock started at the moment Windows booted. The game
now gets the real date and time.

### Offline multiplayer: no freeze with a 1.04 update file
Copies of the game with the official 1.04 update merged in have the update's big TPPS.PSARC in place of the disc's
tiny one. The 1.00 game read half of it, and the first multiplayer match froze on its first frame. The launcher now
gives the game the disc's own small file (offline and online), so matches start normally.

### Online
- European-disc and US-disc players are now matched together (the two discs carried different multiplayer data;
  the game now uses the US value for both). Not fully tested yet: please tell us how it goes.
- A wrong or missing Online Pass no longer sends the game to the PlayStation Store, where it spun forever.

### Other
- New launcher checkbox "Smoother glow at high resolutions" (on by default; turn it off on a slow graphics card).
- A rare error loop can no longer fill the disk with a log file of many gigabytes.

## 0.1.34 (beta), 2026-11-16

### Saves: an older save no longer replaces your progress
Since 0.1.30 the launcher could copy a save from another install of the game (an older version folder) over
yours, without asking, whenever that save had a newer file date. One checkpoint in an old install was enough:
on the next launcher start your chapter 22 went back to chapter 19, and again after every play there.
Now the launcher only fills in a save when there is none here. It never replaces an existing save at start-up.
The Saves page's "Find lost saves" button still offers the other save, says that dates are not story progress,
and its default answer is No. Restored or imported saves are no longer left read-only.

**Lost progress?** It was not deleted. The replaced save is in `gamedata\replaced-saves\<date-time>\`:
close the game and copy that save folder back into `gamedata\dev_hdd0\home\00000001\savedata\`.
Older copies are in the launcher's Saves page (Restore picks one of the backups).

### Online
- The "Pick a server" list now follows the address in the Server box (it could show an old "Other:" address).
- European-disc players see a note that online they only meet other European-disc players (the two discs carry
  different multiplayer data).

### Saves: the right save date
The save date in the game (and in Explorer) stayed at the first save's date, or showed 1970: Windows does not move a
folder's date when the files in it are overwritten, and the game only got the folder's date. The game now gets the
newest save file's date, and each save also sets the folder's date.

- Game exe: 0.1.32 + the save-date fix.

## 0.1.33 (beta), 2026-11-15

### Online: cheats no longer block online play
With any cheat patch ticked, the game turned online off and only showed "signed out of PlayStation Network"
(80010001). Now an online run leaves cheats and other gameplay patches out by itself (only Unlock FPS and the
visual Disable ... patches stay on, the ones the game allows online), and the launcher says how many it left out.
Your ticks stay as they are: single player still uses all of them.

- Launcher-only update: the game executable is the same as 0.1.32.

## 0.1.32 (beta), 2026-11-15

### Online multiplayer is back in the port
Community online multiplayer is now part of the normal download (it was only in test packages before).
- New **Online** page in the launcher: play on a community server, on your own network (LAN), or host your own
  server (https://github.com/bc160201983/gowa-community-server).
- **No server is built in.** Type the address of a community server in the Server box, use LAN, or host one.
- Match codes for private games with friends, a server list with ping and players searching, leaderboards, and an
  optional Discord status with a Join button.
- You need your **own Online Pass** (INDEX.EDAT from your PS3) and the same game region as the other players.
  README-ONLINE.txt in the download explains everything.
- Offline Multiplayer patches are back too (Patches page, experimental).

### 5.1 surround sound
The game mixes 5.1 sound, but the port always folded it down to stereo, so surround speakers stayed silent.
- When Windows is set to 5.1 or 7.1 speakers, the port now sends real 5.1. Stereo setups are unchanged.
- Measured on a recording of the 6 channels: front, center, subwoofer and both surround channels all carry sound.
  We could not test it on real surround speakers: please tell us how it sounds.

### Ambient occlusion (new option, off by default)
- Graphics page -> Lighting -> **Ambient occlusion**: soft contact shadows in corners, under ledges and where
  characters stand. Not part of the PS3 game.
- No measurable frame-rate cost on our RTX 5080 test PC (the game is limited by the CPU there).

### For modders
- **Texture packs:** `LD_TEX_DUMP=<folder>` writes every game texture as a PNG; `LD_TEX_PACK=<folder>` loads your
  edited or upscaled PNGs (any size) in their place. About 1% slower with a full pack in our test.
- **Shader edits:** `LD_HLSL_DUMP=<folder>` writes the game's shaders as HLSL; put an edited `ps_<hash>.hlsl` /
  `vs_<hash>.hlsl` in the folder named by `LD_HLSL_REPLACE` and the game uses it.
- Both are off unless you set them. Packs are made from the game's own art: share them as the community does,
  they are not part of this project.

## 0.1.31 (beta), 2026-11-14 - final update

### The final update
0.1.31 is the **last update** of this port. Thank you, everyone, for playing, testing, sending logs and reporting bugs.
Your help made this port much better than we could have made it alone. Enjoy the game!

### Smoother glow at high resolutions
At render scales above 1x the glow (bloom) around fire, magic and bright light looked dotty and checkered.
- The glow is now made at full resolution and matches the PS3 look: in our test scene (2x) the final glow brightness
  is within 1% of the PS3 picture at 1x, and the dotty pattern is gone.
- Frame rate is the same: 60 fps at 2x and 3x in our tests (60 fps limit).
- It is on by default. Nothing to change in the launcher.

### Faster graphics thread for everyone
- The graphics thread now does less work per draw (lower time per draw in all 7 of our test runs). This helps most
  when you play with a high or unlocked frame rate.

### "Fewer shader stutters (NVIDIA, experimental)" is better
The option is still off by default (Graphics page, NVIDIA only). With it on:
- Fewer stutters right after a loading screen.
- It now also covers vertex shaders.
- In our heavy-fight test (one run each, empty shader cache, our own settings) the time spent waiting for shaders during
  play went from 873 ms (measured with the 0.1.29 build) to 419 ms.

### Other fixes
- **Wireless controllers** (for example 8BitDo): vibration is now sent less often, which should stop the short
  freezes some of you had while the pad vibrates. We could not test this with a real wireless pad.
- **PLAY with an .iso:** the launcher remembers where the image is mounted and uses it again, instead of asking
  Windows to mount it on every PLAY.
- Your saves from older port folders are still found and copied in, as in 0.1.30.

### How to update without losing your saves
- **Easiest:** extract `GodOfWarAscension-0.1.31-windows.zip` **over your old folder** and say yes to replace the files.
- **Or** extract it into a new folder and start the new launcher once: it copies your saves from the old folder.
- **Do not delete the old folder** until your progress shows in the new one.
- The `-update.zip` is only for extracting over an existing port folder.

### Tested for this release
- Heavy fight 180 s at 2x with the option on: 59.8 fps average, lowest 5 s 56.6, no crash.
- Rooftop 120 s at 2x: 59.6 fps average, no crash.
- Clean folder: the game starts, 60 fps, no crash. Update over 0.1.30, 0.1.29 and 0.1.28 gives the same files as the
  full zip. Save copy from a 0.1.30 folder works and the save loads.

### Known issues
- **Videos can show a black screen when Skip cutscenes is off** (sound plays). Keep **Skip cutscenes** on (default).
- **Kratos can look yellow in the chapter 29 cutscene** ("This is Sparta").
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **"Fewer shader stutters" is tested on NVIDIA only.** AMD and Intel are untested, so the option stays NVIDIA only.
- The new glow builds a few new shaders the first time: a short wait before the first loading screen (in our tests
  0.05-0.16 s at 2x, about 2 s at 3x).
- **Frame generation** runs the game at half your screen's refresh rate: below 120 Hz use **Off** or **Anti-aliasing**.
- **Older CPUs without AVX2/FMA** cannot run the game (the launcher says so).
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped patches** in some floor lighting (for example the Village of Kirra).
- Offline Multiplayer: leaving a match back to the main multiplayer lobby with the buttons does not always work
  (hold SELECT+START to quit; your profile is saved after every match). In Team Favor on the Labyrinth of Daedalus the
  altar stays inactive when you play alone.
- Online multiplayer stays hidden in this release.
- The save search looks only next to the port folder, one level up, in Downloads and on the Desktop. If your old
  folder is somewhere else, copy its `ascension-recomp\ascension\gamedata` folder into the new one by hand.

Thank you again for everything. Have fun with Kratos!

## 0.1.30 (beta), 2026-11-14

### Your saves now follow you to a new version
Players told us their progress was gone after updating. The reason: the saves live inside the game folder
(`ascension-recomp\ascension\gamedata`), and most people extract each new full zip into a **new** folder, so the new
version started with no saves. The progress was still there, in the old folder.

From 0.1.30 the launcher fixes this by itself:
- **When it starts, it looks for saves in your older port folders**: folders next to this one, one level up, in
  Downloads and on the Desktop (two folder levels deep), and from now on every folder a 0.1.30 or later launcher has
  run from. Only real port release folders are used.
- **If this folder has no save, or the one it finds is newer, it copies it in** and tells you once what it copied and
  from where. Your old folder is only read, never changed.
- **If you already started again in the new folder**, it asks first. Say yes and your old progress comes in; the new
  save is kept in `gamedata\replaced-saves`.
- **Checkpoints saved in the wrong place by 0.1.26 and older** (loose files in the `savedata` folder) are found and put
  into your profile the same way.
- **It does not delete your saves.** It makes a save backup first, a save it replaces is moved to
  `gamedata\replaced-saves\<time>`, and it does nothing while the game is running.
- **Game -> Saves -> Find lost saves** looks again at any time (also for finds you said no to before).
- Each find is copied or offered only once, so restoring a backup later is not undone at the next start.
- The new texts are also in Spanish and Brazilian Portuguese.

### How to update without losing your saves
- **Easiest:** extract `GodOfWarAscension-0.1.30-windows.zip` **over your old folder** and say yes to replace the files.
- **Or** extract it into a new folder and start the new launcher once: it copies your saves from the old folder.
- **Do not delete the old folder** until your progress shows in the new one.
- Already lost progress after an earlier update? If the old folder still exists, start 0.1.30 (or press
  **Find lost saves**) and it will bring the saves in.

### Correction to the 0.1.29 announcement
The 0.1.29 post said "the launcher offers the update by itself". That is not true for most players: the launcher's
update check needs access to the GitHub repository, which is private. Please download the **full zip**
(`GodOfWarAscension-0.1.30-windows.zip`) and install it as described above. The `-update.zip` is only for extracting
over an existing port folder.

### What did not change
- The game itself is exactly the 0.1.29 game (same `AscensionRecomp.exe`). The NVIDIA option "Fewer shader stutters"
  is still there, off by default.
- Online multiplayer stays hidden in this release.

### Known issues
- **Frame generation** runs the game at half your screen's refresh rate: below 120 Hz use **Off** or **Anti-aliasing**.
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** cannot run the game (the launcher says so).
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped patches** in some floor lighting (for example the Village of Kirra) and pixelated bloom: being looked at.
- Offline Multiplayer: leaving a match back to the main multiplayer lobby with the buttons does not always work yet
  (hold SELECT+START to quit; your profile is saved after every match). In Team Favor on the Labyrinth of Daedalus the
  altar stays inactive when you play alone.
- The save search looks only in the places listed above. If your old folder is somewhere else, copy its
  `ascension-recomp\ascension\gamedata` folder into the new one by hand (before the first PLAY), or extract the new zip
  over the old folder. It can also miss it when Downloads or the Desktop holds several hundred folders.

Everything else as in 0.1.29.

## 0.1.29 (beta), 2026-11-14

### Fewer shader stutters on NVIDIA graphics cards (new option, experimental, off by default)
- New checkbox: **Graphics -> All settings -> Shaders -> Fewer shader stutters (NVIDIA, experimental)**.
- What it does: when the game meets a shader it has not built yet, it normally pauses for a moment until that shader is
  ready (the short "shader stutter" the first time you see a new effect or area). With the option on, the game draws
  that object right away with a slower all-purpose shader, then switches to the real one as soon as it is ready.
- Measured on an RTX 5080 in a big fight with an empty shader cache:
  - with 4 CPU cores, 4 runs each: time spent waiting for shaders **3.5 s -> 1.4 s**, waits of 50 ms or longer
    **29.5 -> 5.8** per run, worst 5 seconds **30.2 -> 34.4 FPS** (average FPS the same, 51);
  - with 1440p, 2x internal resolution, SMAA ultra, 60 FPS cap: waiting **3.1 s -> 0.9 s**, worst 5 seconds
    **46.4 -> 56.4 FPS**;
  - this release's own check (same fight, same settings, 1 run each): waiting for shaders during play **2.7 s -> 0.9 s**, waits of 50 ms or longer **23 -> 4**, worst 5 seconds
    **42.4 -> 55.6 FPS**, average **58.9 -> 59.7 FPS**.
- NVIDIA cards only: on AMD and Intel graphics the game turns it off by itself. Tested on one card (RTX 5080) so far.
  It may use a little more GPU time while it is working. Not yet tested with FSR frame generation, in multiplayer or in
  long sessions.
- What it does not fix: the waits right after a loading screen stay as they are, and so does one longer wait of
  0.1-0.2 s in that fight (one very large shader).
- It is off by default. If something looks wrong with it on, turn it off and tell us.

### Clear messages when a PC cannot run the game
- **Processors without AVX2** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX, some Pentium/Celeron): the
  game used to stop with only an error code. The launcher now says the processor is not supported, why, and that
  settings or updates cannot fix it.
- **Old Intel integrated graphics** (HD 4000 / 4600 / 5000 and older): when the game stops because the graphics card
  cannot build the game's shaders, the launcher now names the card and explains it. It suggests updating the driver,
  or making the game use the NVIDIA or AMD card if the PC has one.
- Both messages are also in Spanish and Brazilian Portuguese.

### Launcher
- **PLAY** no longer reports the port's own files as missing when the saved game path points at a game file that is
  gone or has no port files beside it (for example an older, deleted install): it now uses this folder's game.
- The Home page's "New in" line shows this version's changes.

### Known issues
- **Frame generation** runs the game at half your screen's refresh rate: below 120 Hz use **Off** or **Anti-aliasing**.
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** still cannot run the game (now the launcher says so clearly).
- The first visit to a new area can still stutter briefly while its new shaders compile (the new option above makes
  this shorter on NVIDIA cards).
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped patches** in some floor lighting (for example the Village of Kirra) and pixelated bloom: being looked at.
- Offline Multiplayer: leaving a match back to the main multiplayer lobby with the buttons does not always work yet
  (hold SELECT+START to quit; your profile is saved after every match). In Team Favor on the Labyrinth of Daedalus the
  altar stays inactive when you play alone. The launcher's hint under **Offline multiplayer map** still describes the
  old same-map rematch.

Everything else as in 0.1.28.

## 0.1.28 (beta), 2026-11-13

### Rooftop shimmer fixed
- With the internal resolution above the original HD (Full HD, 2K, 4K, 5K), still surfaces flickered between sharp and
  smeared every frame: fine sparkling pixels, most visible on the prison rooftop. The cause was a repair step of the
  port, not the game. Fixed, and still surfaces now look sharper too (checked on the rooftop at Full HD, 2K and 4K).

### Faster on 16-core Ryzen X3D CPUs (Ryzen 9 7950X3D / 9950X3D)
- With **Thread placement** on (the default), the game's main thread now runs on the large-cache cores together with
  the graphics and SPU threads (before, it ran on the other core group).
- Measured on a 9950X3D in a big fight with the frame cap off (test settings: sound muted, occlusion culling off),
  three runs each: **97 -> 110 FPS**, and the slowest 1% of frames about 67 -> 78 FPS. The prison rooftop is unchanged
  (69.5 FPS both); other scenes were not measured. At the usual 60 FPS cap this CPU already held 60, so in normal play
  this is headroom.
- 12-core X3D CPUs (7900X3D / 9900X3D) and 8-core ones keep the old layout. Not tested on a 7950X3D yet.
  `PS3_PIN_X3D=0` in the extra variables (Advanced page) brings back the old layout.

### Intel CPUs with E-cores: Thread placement now turns itself off
- On Intel CPUs with E-cores (for example Core i7-13620H), Thread placement put the game's main thread and two SPU
  threads (one of them the sound) alone on the slower E-cores: freezes of several seconds and missing sound. Without
  placement the same laptop played without them. The game now turns Thread placement off by itself on these CPUs
  (the log says `[pin] hybrid: thread placement off`). `PS3_PIN_HYBRID_OLD=1` in the extra variables brings back the
  old behaviour.

### Controller: full stick range on the diagonals (the 0.1.27 fix now really works)
- 0.1.27 said diagonal pushes now reach about 90% of a full push. That fix never reached the released game: the
  release reads every controller (Xbox, PlayStation, Steam Deck and generic pads) through SDL2, and the fix was only
  in a controller path the release does not use. It is in now, for every controller: a full diagonal push goes from
  about 70% to about 88% of the range on each axis, as in RPCS3. This should help the right-stick evade and stick
  prompts such as the one on the Manticore's tail (not yet checked with a real controller). `PS3_PAD_SQUIRCLE=0` in the
  extra variables brings back the old behaviour.

### Offline Multiplayer: all seven modes (experimental)
- Tick **Offline Multiplayer (experimental)** on the Patches page. In the multiplayer lobby, **Game Mode** (one step up
  from Quick Play) now lists and starts all seven modes: Trial of the Gods (1 player or co-op), Favor of the Gods
  (4 or 8 players), Team Favor (2 vs 2 or 4 vs 4) and Capture the Flag.
- New on the launcher's **Game** page: **Offline multiplayer mode** and **Offline multiplayer map** choose what Quick
  Play starts (the map is the first match's; read when the game starts).
- New patches: **Offline Multiplayer: no instant win when alone** (the modes made for 2-8 players otherwise end about
  10 seconds after you appear when you are alone; with it on, a match runs to its 10-minute timer) and
  **Offline Multiplayer: skip the tutorial**.
- After a match alone, the next one starts on the next map of the list instead of a forced rematch on the same map.
  Leaving a match back to the main multiplayer lobby with the buttons does not always work yet. To stop playing,
  hold SELECT+START: it quits the game, and your multiplayer profile is already saved after every match.
- A multiplayer profile file that cannot be read (for example locked by another program) is no longer replaced by a
  new empty profile: the lobby stops at a profile error instead.
- Trial of the Gods has the game's own monster waves; the other modes are an empty arena. There are still **no other
  players or bots**: the game has no AI players, and playing with other people would need a server.

### Launcher
- The Home page's "New in" line shows this version's changes (it still showed 0.1.25's).

### Known issues
- **Frame generation** runs the game at half your screen's refresh rate: below 120 Hz use **Off** or **Anti-aliasing**.
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped patches** in some floor lighting (for example the Village of Kirra) and pixelated bloom: being looked at.
- The launcher's hint under **Offline multiplayer map** still describes the old same-map rematch.

Everything else as in 0.1.27.

## 0.1.27 (beta), 2026-11-11

### Autosave: Continue loads your latest checkpoint again
- On some PCs every checkpoint was saved into the wrong folder (the main save folder instead of your profile), so
  **Continue started from an older chapter**. Fixed. It only happened on PCs where Windows places the game's memory
  in a particular way, so most players never saw it.
- Checkpoints that were already saved to the wrong folder are not moved back automatically.

### "Not Responding" window fixed
- When the game ran below 60 FPS (for example with frame generation on a 60-100 Hz screen, or a heavy scene at 4K),
  Windows could mark the game window "Not Responding" while the game kept running. Fixed.

### Controller: right-stick evade
- Diagonal flicks of the sticks reached only about 60% of a full push; they now reach about 90%, as on a PS3
  controller (RPCS3 does the same). This should make the right-stick evade fire reliably in every direction.
  `PS3_PAD_SQUIRCLE=0` in the extra settings brings back the old behaviour.

### Launcher
- A clearer message when a copy of the game has a custom-firmware loader as its EBOOT.BIN: it now says to put the
  `ORIG` folder (with the disc's own EBOOT.BIN) back next to `PS3_GAME`.

### Known issues
- **Frame generation** runs the game at half your screen's refresh rate: below 120 Hz use **Off** or **Anti-aliasing**.
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped patches** in some floor lighting (for example the Village of Kirra) and pixelated bloom: being looked at.

Everything else as in 0.1.26.

## 0.1.26 (beta), 2026-11-11

### AMD FSR frame generation: no more big FPS drops on older PCs
- Frame generation now turns itself off on GPUs older than AMD's minimum (Radeon RX 5000 / GeForce RTX 20): Radeon
  RX 400/500, Vega and older, GeForce GTX 10 and older, Intel integrated graphics and AMD's built-in "Radeon Graphics".
  On those it only cost FPS (an RX 580: 60 FPS without it, 7-17 with it). `FSR3_FG_OLDGPU=1` in the extra settings
  keeps it on anyway.
- If the game falls below 25 FPS with frame generation, it pauses; the second time it stays off for the session
  instead of switching on and off.
- While it is paused, the extra drawing it needs for moving objects stops too (that work was what slowed weaker CPUs).
- The launcher now says who it is for: "Frame generation (RX 5000 / RTX 20 or newer)".
- Known: the FPS counter can look smeared while frame generation is on (a display issue only).

### New: optional "1.04 sound mix" (Mods page)
- The official 1.04 update's sound mix: louder music where it was too quiet, and the fixed Hecaton cutscene and
  boss-fight mix. Off by default.
- Choose your own 1.04 update file (.pkg, US or EU); the launcher takes only that one file from it. Nothing is
  downloaded and no game file comes with the port. Later updates (1.05-1.12) are refused: their mix doesn't fit.
- Works together with Skip intro and other mods.

### Skip intro now works on the US disc
- It only blanked the European disc's list of intro screens; it now blanks the US one too.

### Launcher
- The built-in update check finds new versions again (it used the repository's old name).
- Fixed a freeze when the mouse rested on the big PLAY button.

### Known issues
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller.
- **Box-shaped shadows** on some effects and pixelated bloom: being looked at.

Everything else as in 0.1.25.

## 0.1.25 (beta), 2026-11-11

### Freeze on the second snake (picture stops, sound keeps playing): fixed
- After about 12 minutes of play the game's own memory grew over the small area where the port keeps the graphics
  chip's progress signals. The game then wrote its data there, and the drawing side waited forever for a signal that
  never came: the picture froze while the sound went on. That area now sits far away from the game's memory.
- Confirmed with a test that puts the area back inside the game's memory on purpose: it freezes the same way within
  seconds. With the fix: no freeze signs in the heavy fight (4 cores) or on the rooftop.

### Controller stops working after some cutscenes: fixed
- A wireless controller that dropped its connection for a moment (for example during heavy rumble) was never found
  again until the game was restarted. The port now notices when a controller comes back and uses it again.
- The game's log now says when a controller connects or disconnects, which helps with reports.

### Less sound crackling on 4-core PCs
- On busy 4-core PCs the game's sound job could run late, and the missing bits were played as short silences
  (crackling). Two fixes, both on by default:
  - The thread that runs the sound job gets a higher priority while it does.
  - A late bit of sound is filled with the previous one, faded smoothly, instead of silence.
- Measured on a PC slowed down to a weak 4-core laptop, in normal runs: missing sound 4.3 % and 1.8 % before, 0 % and
  0 % after; long stutters (100 ms or more) about 85 and 23 per minute before, 6 and 9 after. No new clicks.

### AMD FSR 3.1 (new, Graphics page)
- **Anti-aliasing:** smoother edges and less shimmer than SMAA. Best when the internal resolution matches your screen.
- **Frame generation:** shows twice as many frames, for PCs that cannot hold 60 FPS. It holds the game at half your
  refresh rate and adds some input delay.
- One mode at a time. Both use the game's own motion blur for moving objects, so they turn it on, and they need
  "Unlock FPS" and "Disable SPU MLAA" (Patches page, on by default). Works on NVIDIA, AMD and Intel graphics cards;
  tested on NVIDIA only so far.
- Off by default. Uses AMD's FidelityFX SDK (MIT licence, included); the idea of using the game's motion-blur data comes
  from the Ascension-R project (MIT).

### Offline multiplayer: progress is saved
- With "Offline Multiplayer" ticked, your multiplayer XP, level and unlocks are now kept between sessions.
- Quick Play starts Trial of the Gods solo offline.

### New launcher look
- A cleaner, modern design. Home has the big PLAY button with your last save, a picture-quality slider
  (Low-end PC, Balanced, High, Ultra), the everyday switches and a "Ready to play" check.
- Graphics has a **Simple** view (four presets and the six settings that matter, FSR included) and **All settings**.
- Each option shows one short line; rest the mouse on it for the whole explanation.
- A search box on every page finds any setting.
- Settings save by themselves (no Save button); the first change keeps a backup of the old settings file.
- New **Support** page.

### Known issues
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part.
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile.
- Heavy fights can still drop below 60 on 8-core CPUs and smaller (the game's work on the PS3's SPUs is the limit).
- **Box-shaped shadows** on some effects (for example grabbing an enemy) and pixelated bloom: being looked at.
- More offline multiplayer modes and maps: coming in the next update.

Everything else as in 0.1.24.

## 0.1.24 (beta), 2026-11-10

### The US disc works now
- The US disc (**BCUS98232**, version 1.00) can be used like the European one: in the launcher choose its game folder or
  its decrypted .iso.
- Both discs carry the same program. The launcher turns the US EBOOT into the European one the port is built from
  (a 42 KB difference, checked byte for byte), so every fix and patch works the same.
- The US disc keeps its own languages: English, French, Brazilian Portuguese (no mod needed) and Latin-American Spanish.
  Its saves are kept apart from the European ones (`BCUS98232-*` folders).
- Tested: the heavy fight from a US disc image at a steady 60 fps, with its saves and English voices.

### Start-up hang with SPU MLAA on: fixed
- With the "Disable SPU MLAA" patch unticked, slower PCs (laptops) could freeze on a black screen at the first start:
  the port waited only 50 ms for the game's anti-aliasing jobs, a slower CPU needs longer, and the jobs then waited for
  each other forever. The port now waits as long as the jobs need.
- Measured on slowed-down cores: 0.1.23 froze every time (2 of 2); now 3 of 3 starts reach play and a 3.5-minute fight
  runs. With the release patches (SPU MLAA off) nothing changes: 62 fps before and after.
- "Disable SPU MLAA" stays ticked by default: it is faster.

### Faster on 6-core CPUs
- 6-core / 12-thread CPUs with one core cluster (for example Ryzen 5 5600 / 7600 / 9600, Core i5-12400F, i5-11400) now
  use thread placement. Measured on a fast 6-core against 0.1.23 in the same session: rooftop 55.5 -> 59.0 fps, and the
  graphics thread needs about 17 % less time per draw; the heavy fight holds 60.
- The freeze that placement caused on 6 cores before (an SPU worker left without CPU time for 4 seconds) is fixed.
- It stays off on two-cluster Ryzen 5 1600 / 2600 / 3600 and on 6-core CPUs without hyper-threading (not tested there
  yet): `PS3_PIN_SMALL=1` in the launcher's extra settings turns it on.

### Less CPU wasted waiting (first step)
- The game's EDGE watchdog asks the SPU scheduler to set the same priorities again about 2,500-2,900 times a second,
  and every call locked the whole scheduler and woke all SPU workers. Calls that change nothing are now skipped
  (66-95 % of them): the watchdog's CPU time about halves. `SPURS_PRIO_SKIP_SAME=0` brings back the old way.
- Bigger cuts were measured and are included **switched off**, because they cost fps or gave more long frames while
  other programs were busy: the main game thread sleeping instead of spinning while it waits for the graphics side
  (`SYS_USLEEP_REFWAIT=375A78`, about 300 ms of the main thread's time per second at the 60 cap) and the SPU scheduler
  parking instead of spinning (`SPURS_KERNEL_PARK=1`). They come on once they are tuned.

### Intel 12th-14th gen and Core Ultra (P-cores + E-cores): new thread placement to test
- By default these CPUs run exactly as in 0.1.23. A first test version that put all the heavy work on the P-cores made
  a tester's i5-12500H laptop slower (25 instead of 40 fps): on laptops Windows lets the game use only one thread per
  P-core.
- New placements are included for testing only (`PS3_HYBRID_PLAN`, `PS3_HOST_PIN_AB=30000`). **Not recommended for
  playing:** the first real laptop logs (i5-13420H, i7-12700H) showed more long frames with them than with 0.1.23's
  placement, and one 6-second freeze. Work on the sound crackling on these CPUs continues.

### Launcher
- **Voice language** option (Game page): for example Brazilian Portuguese text with English voices, or keep the voices
  you chose in the game's own Options -> Speech.
- **Español and Français were swapped** in the European language list: picking Español gave French. Fixed.
- The Brazilian Portuguese mod builder is now included in the package (it was missing) and finds the European disc's
  files inside a disc image too.
- A still-encrypted 1:1 disc image (a Redump .iso) now gets a clear explanation of what to do.
- An incomplete install (for example a test build or an update extracted without the full package) now says which
  files are missing, instead of a Windows error about SDL2.dll.

### Known issues
- **Kratos keeps dying in a cutscene near the end of the game:** turn on **Skip cutscenes** for that part. Being looked at.
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile. A permanent fix is in progress.
- **Box-shaped shadows:** a dark box on the ground (for example when grabbing an enemy with the chains) or a light box
  where a shadow is missing.

Everything else as in 0.1.23.

## 0.1.23 (beta), 2026-11-09

### Freezes every few seconds on 8-core CPUs: fixed
- On CPUs with exactly 8 cores (for example Ryzen 7 5800X3D / 7800X3D / 9800X3D, Ryzen 7 laptops, Core i9-9900K),
  the **Thread placement** option caused a short freeze of about 200 ms every few seconds, even when standing still.
- The game now turns thread placement off by itself on 8 cores. CPUs with 10 or more cores keep it; below 8 cores it was
  already off. The log says `[pin] 8 cores: thread placement off`.
- Measured on a tester's Ryzen 7 9800X3D: with thread placement on, 4.3-5.1 freezes of 100 ms or more per minute of
  play (three sessions); with it off, 1 in 38 minutes. Thanks Ati!
- Cost: on a simulated 8-core CPU the rooftop runs at about 56 fps instead of 60. On the tester's real 8-core the game
  stayed at 60 with it off.

### Even frame pacing with the built-in fps cap
- The game's own fps cap let each frame out 0-4 ms late (it only checked the time in whole milliseconds), so frame times
  wobbled: a wavy line in frame-time graphs such as MSI Afterburner's. It now waits precisely and lets each frame out on
  time, the way RTSS does. An external limiter is no longer needed for an even line.
- Measured with the 60 fps cap (opening, 3-minute runs alternated old/new): frames went out 1.8-3.1 ms late (median),
  now 0.005 ms; 95 % of frame-to-frame times were under 19.2 ms, now under 16.9 ms; frames of 20 ms or more: 145 -> 0.
- In the busy rooftop scene the spread of frame times dropped from 1.4-1.8 ms to 0.4-1.1 ms. On 4 cores with the 40 fps
  cap it dropped from 1.8 ms to 0.3 ms. Cost: about 1 % of one CPU thread.

### Fewer sound cuts on slow CPUs
- When the CPU is at 100 % (weaker 4-6 core CPUs in big fights), the game's own sound sometimes came a little late and
  the port played silence instead: short sound cuts. Now the port waits for late sound while it still has sound
  buffered (the idea RPCS3 uses), and when it really must fill a gap it fades out and back in instead of cutting hard.
- Measured with the game's sound made 60 ms late every 2 seconds on purpose: sound gaps 423 -> 0 (a few short soft dips
  remain). On a simulated 3-core CPU in a heavy fight: 78 gaps -> 0 outside whole-game freezes (those still go quiet,
  but now fade in and out).
- The sound buffer is a little bigger, so sound plays about 18 ms later than before. `AUDIO_ELASTIC=0` brings back the
  old behaviour.

### NVIDIA: glowing blobs fixed
- On NVIDIA cards some surfaces could show as bright glowing blobs (for example the cistern vines). The game now compiles
  the affected shaders so that the depth pass and the colour pass match exactly. The `setx LD_VS_STRICT 1` workaround
  is no longer needed (if you set it, it does no harm).
- The first start after the update builds the fixed shaders in the background while you play; from the next start the
  fix is active.

### Fewer first-time stutters in later chapters
- The recorded pipelines built behind the start-up progress bar now cover **13,425** pipelines (1,074 more, up to the
  end of the game), so fewer new shaders are compiled while you play the later chapters.

### Launcher
- Game copies that keep a small loader as `USRDIR\EBOOT.BIN` and the original in an `ORIG` folder: the launcher now
  finds the original EBOOT.BIN by itself instead of refusing the copy.

### Also in 0.1.22 (missing from its notes)
- Crackling sound in heavy fights is fixed: the sound thread now gets top priority on a busy CPU.

### Known issues
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
- The first visit to a new area can still stutter briefly while its new shaders compile. A permanent fix is in progress.
- **Box-shaped shadows:** a dark box on the ground (for example when grabbing an enemy with the chains) or a light box
  where a shadow is missing.
- **6-core CPUs at 100 % CPU:** short sound cuts in big fights. Being looked at.

Everything else as in 0.1.22.

## 0.1.22 (beta), 2026-11-08

### Much faster on 4-8 core CPUs: more of the game now runs as real PC code
- The game's geometry jobs (EDGE) and its render jobs used to run as translated PS3 SPU code. They now run as
  native PC code, inside the same worker threads, and give the same output: checked job by job, millions of jobs.
- The graphics command thread does less work per frame.
- Measured against the previous build at 4 CPU cores, in whole runs alternated old/new:
  - **Rooftop scene:** 33.3 → **47.0 fps (+41 %)**.
  - **Heaviest part of a big fight:** 40.7 → **46.2 fps (+13 %)**.
  - Long frames (over 34 ms) almost gone.
- On all cores of a 16-core PC, the rooftop now holds **60 fps** (was about 48), and heavy fights stay at 60.
- In a 44-minute play session (16 cores, 60 fps cap), big hitches of 100 ms or more were about **5x rarer** than before.

### Fewer first-time shader stutters
- When something appears for the first time, the game has to compile its shaders. When both of its shaders are new,
  they are now compiled at the same time instead of one after the other. In tests with an empty shader cache, the time
  spent waiting during play dropped by 17 %, and big stutters during play (100 ms or more) went from 5 to 1.
- The recorded pipelines that are built behind the start-up progress bar now cover 12,351 pipelines (241 more, from newer
  areas), so fewer new shaders are compiled while you play.

### Saves: backups while you play, export and import
- With **Back up before playing** on (*Game → Saves*; on by default), the launcher now also backs up your saves each time
  the game saves while you play. If the game crashes during a save, your earlier progress is still in the backups
  (*Restore*). The last 20 backups are kept.
- New buttons: **Back up now**, and **Export saves… / Import saves…** to carry your saves to another PC in one .zip.
  Importing backs up the current saves first.

### Known issues
- **Older CPUs without AVX2/FMA** (for example Intel Core i5/i7 2nd and 3rd generation, AMD FX) cannot run the game yet.
  It closes at once with code 3221225501. A version for these CPUs is being looked at.
- **NVIDIA:** some surfaces can show as bright glowing blobs, for example the cistern vines. Until the proper fix ships,
  run `setx LD_VS_STRICT 1` once in a Command Prompt and restart the launcher. Expect one longer start while shaders rebuild.
- The first visit to a new area can still stutter briefly while its new shaders compile. A permanent fix is in progress.

Everything else as in 0.1.21.

## 0.1.21 (beta), 2026-11-07

### Fewer stalls on 8-core CPUs and handhelds (occlusion culling)
- With **Occlusion culling** on, the game asks the GPU which objects are hidden and waits for the answers. On a CPU
  that is already fully busy (8-core CPUs such as the Ryzen 7840HS or 9800X3D, handhelds), 15-27 % of those answers
  arrived too late -- with NVIDIA and AMD graphics alike -- and every late one held the game for 8 ms. That caused
  the micro-freezes testers reported.
- The game now notices this within about 10 seconds of play and stops waiting for the rest of the session
  (everything is drawn). Measured on an 8-core laptop: 24.5-25.0 fps instead of 22.4-24.3, on both its NVIDIA and
  its AMD GPU. On bigger CPUs, where the answers come back in time, occlusion culling stays on and keeps its gain.
- Nothing to change: leave **Occlusion culling** ticked.

### Story movies are skipped
- **Skip cutscenes** now really skips the pre-rendered story movies: the game ignores a button press there, so the
  port uses the game's own pause menu (Skip Cinematic -> Yes). It checks the highlighted menu item before every press,
  so it can never pick "Quit Game". The opening movie of a New Game is skipped about 3.5 s in.
- The story scenes rendered live in the game engine cannot be skipped (the game has no skip for them).

Everything else as in 0.1.20.

## 0.1.20 (beta), 2026-11-07

### Falling through the floor on slower PCs: fixed
- Players reported Kratos falling out of the map in Chapter 1 (after breaking free from the chains, after the fall
  with the Fury, at the window). The cause was in the **Unlock FPS** patch: it switches the game's clock from screen
  refreshes to milliseconds, and that also cut the time the game's level loader gets each frame from 3.3 ms to
  0.2 ms. On a slower CPU the next area's floor (and its collision) was not loaded yet when Kratos landed.
- **Unlock FPS 2.1** gives the loader its original time again. Loading screens on a 4-core PC: about 15-20 s -> about
  5 s.
- It also limits one game step to 66.7 ms again, as on the PS3 (the RPCS3 patch allowed 100 ms): after a long frame
  the game slows down for a moment instead of jumping ahead.
- Nothing to tick: Unlock FPS is on by default and carries both fixes.

### Shorter shader freezes on first visits
- New shaders are now compiled with Microsoft's newer DXC compiler. With an empty shader cache, the time the game
  waited for new shaders fell 29-54 %, and the longest freeze from about 540 ms to about 200 ms. Shader waits of 50 ms or
  more in the opening: 25 -> 13 (other slow frames, e.g. in heavy fights, are not changed by this).
- The release now ships **12,110 recorded shaders** (was 10,176), including the later chapters: a player following
  that path builds them before the game starts instead of mid-fight.
- **First start after updating takes about 20-30 s longer, once**: the shaders are rebuilt with the new compiler.
- The two compiler files (`dxil.dll`, `dxcompiler.dll`) sit next to the game exe. Without them the game uses the old
  compiler, so deleting them is a way back if your graphics driver has trouble (please report it).

### Other programs no longer slow the game down as much
- The game now runs at **above-normal priority**: Windows serves it before background programs (a video render, a
  download, a compile), which get the CPU the game leaves. Background programs run a bit slower while you play.
- The graphics thread runs at high priority: about +5 % fps on 4-core PCs, no change on bigger CPUs.

### Launcher
- Unticking **Skip cutscenes** now really turns skipping off (it could not before). Note: it skips the pre-rendered
  video movies; the in-engine story scenes are not skipped yet (being worked on).

Graphics and everything else as in 0.1.19.

## 0.1.19 (beta), 2026-11-06

### Cheats (new): tick boxes on the launcher's Patches page
All off by default. Tick the ones you want, then press PLAY; untick them and the game is back to normal.
- **Cheat: God Mode**: Kratos takes no damage from hits. This is the developers' own God Mode switch, which the retail
  game never turns on. Some traps, falls or failed button prompts can still kill him.
- **Cheat: Infinite Magic**, **Cheat: Infinite Rage**, **Cheat: Health Regeneration**, **Cheat: Red Orbs x10**: the
  effects of the game's artifacts, without having to finish the game first.
- **Cheat: No Cooldown**: item and ability cooldowns are zero (the developers' switch; experimental).
- **Cheat: Unlock All**: Titan difficulty, New Game+ and its 8 costumes, the extras videos and the 10 artifacts with
  known cheats (switch them on in New Game+). The unlocks are not written to your save: untick it and they are locked
  again (games you start or save meanwhile are kept). Chapters you have not reached stay locked: the save has no data
  for them.
- Trophies still unlock with cheats on.

### Offline Multiplayer (new, experimental)
- Tick **Offline Multiplayer (experimental)** on the Patches page, then press **Circle** on the title screen. The
  multiplayer menu opens without PSN: the online checks pass, every match runs on this PC only, and the warrior profile
  is the game's built-in default.
- Tested: the multiplayer tutorial, including its fight against the AI. **There are no other players**: playing with other people would need Sony's servers or a replacement server, which
  this is not. Multiplayer progress is not saved.

### For testers
- `PS3_GAME_LOG=1` prints the game's own debug messages into the log (`[game] ...` lines), which the retail game threw
  away. Off by default.

Same game code as 0.1.17 otherwise: no change to fps or graphics.

## 0.1.18 (beta), 2026-11-06

### Game copies made on a FAT32 USB drive: PLAY joins the split file for you
- A USB drive formatted as FAT32 cannot hold a file over 4 GB, so PS3 copy tools (multiMAN, webMAN, IRISMAN) cut the
  game's 8.7 GB `EXTRASCIN2.PSARC` into parts: `EXTRASCIN2.PSARC.66600`, `.66601` and `.66602`. The game needs the
  file in one piece, and 0.1.17's launcher only said that the copy was incomplete. Thanks to Nirval33 for finding it.
- PLAY now recognises the parts and offers to join them in your game folder. That needs about 8.7 GB of free space;
  the parts are kept, so you can delete them afterwards. Before writing anything it checks that every part is there,
  that the game folder is not still on the FAT32 drive, and that there is room. A join stopped half way (or the
  launcher closed during it) never leaves a broken file behind.
- **Tested:** split copies joined byte for byte identical; a missing part, a full drive and a folder on a FAT32 drive
  are each reported before anything is written. Texts in English, Spanish and Portuguese.

### Wording
- The launcher's About page and the README now say **no game assets are included**: you need your own copy of the game.

Same game build as 0.1.17: no change to fps or graphics.

## 0.1.17 (beta), 2026-11-05

### More fps in heavy places: occlusion culling (new, on by default)
- The game itself tests, every frame, which objects are hidden behind walls (the PS3's "occlusion queries"). The port
  used to answer "visible" to every test, so everything was drawn. It now gives the game the real answers, so hidden
  objects are skipped exactly as on a PS3. Nothing visible is ever skipped: every "hidden" is the GPU's own count for
  that object.
- **Tested** on the satyr prison rooftop (one of the heaviest views): +7.8% fps at 4 cores, +7.8% at 6, +9.5% on all
  cores; heavy fight wall views +6.5% / +15.8% / +17.1%. No missing scenery in frame-by-frame checks.
- Launcher: *Advanced → Runtime → Occlusion culling* (on). Turn it off only if objects flicker or something is missing.

### More fps in heavy places: each draw is cheaper
- The graphics thread now skips work that gave the same result as last time: identical constant blocks are reused,
  pipeline choices and shader analyses are kept by content, render targets and bindings are not set again when they
  are already set, smaller index buffers. One buffer used to fill up once a frame and make the graphics thread wait for
  the GPU; that wait is gone.
- **Tested** on the satyr prison rooftop with occlusion on: +12.5% fps at 4 cores, +17.5% at 6, +15.5% on all cores;
  every result checked against the old way (0 differences in millions of draws).
- Together with occlusion, the rooftop went from ~39 to ~49 fps on all cores and from ~27 to ~32 at 4 cores.

### No more shader stutter at start-up and after loading screens
- The graphics thread no longer builds, loads or waits for shader pipelines itself; it only takes ready ones. At
  start-up it used to wait on a lock held by the background loader (up to ~1 s frames in the first 15 s): now ~0 ms.
- Shader compiles are about 1.3x faster; precompiling at first start is shorter.
- The release ships **10,176 recorded pipelines** (was 9,465).

### Missing floor / objects popping in: fixed by design
- A draw whose shader is not ready yet is no longer skipped: the frame waits for that shader and is shown complete.
  On a slow PC that is a short pause instead of a missing floor.
- **Tested** with every shader build slowed down like on an RX 580 PC, no shader cache, 4 and 6 cores: 0 missing draws
  in every shown frame.
- New in every log: counters of any draw that was missing, and a small check (one frame every 10 s) of the AMD depth
  precision cause, so a tester's log tells us at once whether their PC has the floor problem.

### Also
- An SPU safety check could stop a game job that was only waiting for the graphics thread, which froze the game for
  good; it no longer does.
- The launcher checks for an incomplete game copy (the USRDIR\ARCHIVES list and every archive it names) and says so,
  instead of the game closing after compiling shaders.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package
**GodOfWarAscension-0.1.17-windows.zip**. The first start after the update compiles the new shaders once.

## 0.1.16 (beta), 2026-11-04

### PLAY did nothing on Windows in other languages, fixed
- On a Windows that is not in English (Russian, German, French, Spanish, ...), pressing PLAY did nothing: no window, no message, no log. Before starting the game the launcher asks Windows whether the game is already running; Windows answers in its own language and in an old text encoding the launcher's Python could not read, and that error stopped PLAY without a word. The launcher now reads the answer in a way that works in every language. The other places where it reads a program's output (disc images, the desktop shortcut, the menu music) can no longer fail on such text either.
- **Tested** with the exact answers a Russian and a German Windows give: 0.1.15's launcher failed on both, 0.1.16 passes and starts the game.
- The game itself is the same as 0.1.15.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.16-windows.zip**. If your launcher can't start the game at all yet (non-English Windows on 0.1.15 or older), update first from *Advanced → Updates*, or extract the update zip over your folder.

## 0.1.15 (beta), 2026-11-04

### Compile shaders before playing (new, on by default)
- When the game starts, every recorded shader pipeline is built first, on all your CPU cores, with a progress screen ("COMPILING SHADERS 1234 / 9465"). Then the game starts and new areas have nothing left to compile, so the floor, objects and effects are there the first time you reach them instead of popping in late.
- It takes a few minutes once after an install or an update (all 9,465 pipelines: 33 s using every core of a fast 16-core CPU, 60 s limited to 4 of its cores; slower CPUs take longer), then about a second at every start. Launcher: *Advanced → Runtime → Compile shaders before playing*.
- **Tested** as a new player limited to 4 cores: draws left out while shaders compiled fell from 7,400-8,200 to 890 on the first start (the rest were shaders nobody had recorded yet) and to 0 on the next start.
- The release now ships **9,465 recorded pipelines** (was 5,738).
- After the first time, the game starts at once: pipelines your library already holds load in the background while you play (a 342 MB library used to hold the start for ~20 s).

### Garbled and blurry textures on 4-6 core PCs
- The game streams a texture's full detail in later, and marks it in a table when it gives the texture its memory. A game job on the SPUs reads that whole table, updates it and writes it all back; on a busy PC that job runs long enough for the game to mark new textures meanwhile, and the write-back erased those marks. Those textures never got their detail: blurry floors, and Kratos' skin or gates in a scrambled "camouflage" pattern. The write-back now keeps every change the game made meanwhile.
- **Tested** with the job slowed down the way a busy PC slows it: 620-660 textures lost their detail per run before, 0 after. Normal play: the textures the old code had to repair afterwards (4 in a heavy fight at 6 cores, 2 in the opening at 4 cores) are 0 now. Same fps.

### Less work after every loading screen
- After a load the game moves many textures around in memory; the port decoded and uploaded every moved copy again (94% of the "new" textures). It now reuses the copy it already has. Decompression of the game's files is ~4x faster.
- **Tested** at 4 and 6 cores (opening, heavy fight, Chapter 13): texture upload work in the 70 s after a load fell from ~800 ms to ~140 ms and decompression from ~1.9 s to ~0.45 s, every shared texture checked byte for byte (0 differences in 47,000+). +4% fps right after a heavy load at 4 cores; the same elsewhere.

### Shaders are kept even if the game crashes or is force-closed
- New shaders were saved only when the game window closed normally. Ended from the Task Manager, by a crash, or by closing the launcher, a session lost all of them and the next one compiled them again (a tester's PC rebuilt ~600 every session, with the floor after the opening cutscene missing meanwhile). They are now saved in the background while you play. **Tested:** a game killed mid-session kept all 6,209 pipelines it had built.

### A way to quit from inside the game
- Hold **SELECT + START for 2 seconds** (Xbox: View + Menu; PlayStation: Share/Create + Options; keyboard: Tab + Enter) or press **Alt+F4**: the game saves its shaders and closes. The game's own *Quit Game* only goes back to its menu.
- Closing the launcher while the game runs now asks the game to close itself (and save) instead of killing it.

### Missing floor on AMD and Intel graphics
- The game draws each surface twice (a depth pass, then the colour pass where the depth is exactly equal). On some GPUs the two passes came out a hair apart and the floor was skipped. Vertex shaders are now compiled in strict math mode on AMD and Intel so both passes agree to the bit (NVIDIA keeps its shaders). AMD/Intel players rebuild their shaders once after this update (the progress screen above).

### Smaller fixes
- Textures in memory the game reloads are checked again after each load.
- The launcher shows an error instead of doing nothing: if something goes wrong when you press PLAY (before the game starts), it now says what and writes the details to `ascension-recomp\ascension\build\logs\launcher_error.log`; PLAY works again afterwards.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.15-windows.zip**.

## 0.1.14 (beta), 2026-11-03

### Garbled scenery after a few deaths or reloads, fixed
- After dying, restarting a checkpoint, loading an area or quitting to the menu a few times in one session, objects (and sometimes the main menu) turned into flat one-colour triangles, stretched stripes and rainbow patches, until the game was restarted. It happened on every PC.
- The cause: the PS3's graphics chip reaches main memory through a 256 MB address window that the game maps and unmaps at every load. The port handed out new window addresses at every map and never reused the ones an unmap gave back, so after three or four reloads the addresses ran past what the game can store, its unmaps failed, and models and shaders were read from the wrong memory.
- Freed addresses are now reused, as on a PS3, and the window is kept at 256 MB.
- **Tested** with the same build, save and button presses (three quit-to-menu-and-continue cycles): with the old behaviour the whole room turned grey and faceted, with the fix it stayed clean (11,021 address errors and 12 failed unmaps before, none after).

### No sound on some PCs, fixed
- On some PCs the game ran silent from the start (`WASAPI Initialize failed: 0x88890008` in the log): the port asked Windows for one fixed audio format, and a sound device set to another rate or speaker layout (44.1 kHz, 5.1, 7.1, ...) refused it. Windows now converts to whatever your device uses. The log shows your device's format.

### Missing floor (and objects) on a first play, fixed
- The first time a new area appears, its shaders have to be compiled. On a CPU the game already keeps busy (6 cores and fewer), those compiles ran so rarely that parts of an area, such as the floor after the opening cutscene, stayed undrawn for seconds; a tester's first session skipped 69,000 draws that way.
- Compiles now run at normal priority, whatever is on screen is compiled first, and nothing waits longer than a quarter of a second: past that the game waits (at most 0.1 s a frame) for it instead of leaving it out.
- **Tested** as a new player on 6 CPU cores with an empty shader cache (the heaviest fight, two runs each): draws left out while shaders compiled fell from 33,000-36,000 to 7,400-8,200, and the same shaders were ready in 38-42 s of compile work instead of 62-73 s. The price is a few short pauses (at most 0.1 s) while a needed shader finishes, instead of invisible scenery.
- **The release ships 5,738 recorded pipelines** (the opening first): a new install builds them in the background from the first start, so most areas are ready before you reach them.

### Kratos no longer jumps by himself when a cutscene ends
- With *Skip cutscenes* on, the port presses the skip button (CROSS, which is also jump) while a movie plays. The last press could still be held when the game came back, so Kratos jumped as the cutscene ended. Presses are now shorter and stop as soon as the movie stops.

### Smaller fixes
- File reads from several game threads at once can no longer move each other's file position.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.14-windows.zip**.

## 0.1.13 (beta), 2026-11-03

### Rare freeze fixed (the picture stops, the sound keeps playing)
- The PS3's SPU co-processors write the game's drawing commands in blocks, and each block starts with a "wait here" word that holds the graphics thread back until the block is ready. On the PS3 a block arrives in one piece. Here an SPU is a CPU thread, and Windows can pause it in the middle of copying a block. The graphics thread then went on into the half that still held the previous pass's commands, followed an old jump and waited forever.
- The copy now writes a block's "wait here" word last, so the graphics thread only moves on once the whole block is there.
- **Tested** in the setting where the freeze showed up most (4 CPU cores, the heaviest fight, 4 minutes a run): 3 freezes in 17 runs before, **0 in 41 runs** after. With the copy paused on purpose at the dangerous moment: 3 of 3 runs froze without the fix, 0 of 3 with it (2,000-4,000 forced pauses a run). No frame-rate cost measured.
- `GCM_PUT_ORDER=0` in the launcher's extra variables turns the fix off (for comparisons only).

### Kratos's face no longer turns into a white patch at higher render scales
- The game's geometry jobs (EDGE) throw away triangles too small to cover a pixel at the PS3's 720p. At 200% render scale and up those triangles do cover pixels, and the game's depth pass had already drawn them, so faces built from hundreds of tiny triangles (Kratos's above all) showed white gaps. EDGE now judges "too small" at the real render resolution.
- **Tested** in play at 200% render scale. Cost: about 1-2% in the heaviest fight.
- `EDGE_CULL_SCALE=0` in the extra variables brings back the old behavior.

### Pick your game folder or .iso, and play: no more decrypting with RPCS3
- The launcher now decrypts the disc's `EBOOT.BIN` itself when you pick your game folder or disc image. Before, you had to decrypt it first with RPCS3 (*Utilities → Decrypt PS3 Binaries*).
- Your game folder is never changed: the decrypted, patched copy goes into the port's own folder as before. The result is checked byte for byte against the known EBOOT of disc version 1.00, so anything else is refused, never used.
- Clearer messages when it can't: an EBOOT from a game update, a disc copy that is itself still encrypted, or a missing EBOOT.BIN. An `EBOOT.elf` you already decrypted with RPCS3 still works.
- **Tested** from a freshly extracted package with a disc folder that held only the encrypted `EBOOT.BIN`: ready in 0.1 s, and the game started from it.

### 40 and 45 FPS caps, and a G-Sync / FreeSync mode
- *Graphics → Frame rate* adds **45 FPS** (120/144 Hz or variable-refresh screens) and **40 FPS** (120 Hz screens): steadier than a 60 the CPU cannot hold. On a 60 Hz screen choose 30 or 60.
- *VSync* adds **G-Sync / FreeSync (VRR)**: each frame is shown as soon as it is ready, so 40-59 FPS look smooth on a variable-refresh screen (turned on in the graphics driver; best in full screen). On other screens it tears like *Off*.
- **Tested** in the heaviest fight: the 40 cap held 40.0 FPS, the 45 cap 45.0; the game runs at normal speed at both.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.13-windows.zip**.

## 0.1.12 (beta), 2026-11-02

### Much faster on 4-core CPUs, a little faster on 6-core CPUs
- The PS3's six SPU co-processors run as six busy threads on your CPU. With thread placement on, the game gave its graphics thread and main thread a CPU thread each and squeezed the six SPU threads, plus a scheduler that never sleeps, onto what was left. On an 8-core or larger CPU that is the fastest layout. On smaller CPUs it starved the SPU threads while some CPU threads sat nearly idle.
- With fewer than 8 cores the game now leaves thread placement to Windows and lets the scheduler sleep between checks. 8-core and larger CPUs are unchanged.
- **Measured** in the heaviest fight (the egg fight), the game limited to fewer cores on a 16-core PC: 4 cores 23.8 -> about 34 FPS (two runs, no freezes); 6 cores 43.7 -> 44.7 FPS on average, with 5% less SPU time per frame. 8 cores and up: same as 0.1.11.
- The launcher's *Thread placement* setting is still there; below 8 cores the game now turns it off by itself (`PS3_PIN_SMALL=1` in Extra variables keeps it on, for testing).

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.12-windows.zip**.

## 0.1.11 (beta), 2026-11-02

### Garbled textures after loading screens fixed (e.g. a magic effect drawn as striped squares)
- The game streams some textures in at low resolution first. Sometimes, while an area loads, one of these textures ends up without its "low resolution" mark. The game then describes it to the graphics chip at full size although only the small versions are in memory, and the chip reads on into whatever lies behind: other textures. Seen as big see-through squares full of stripes on the Furies' purple burst, and as garbled surfaces until the next loading screen.
- 0.1.6 repaired such a texture only when the game later tried to shrink it again, which can be much later or never. The repair now runs at the moment the game describes the texture, before anything is drawn with it.
- **Tested:** in the heaviest fight's area load it caught the problem in 6 of 8 runs (19 textures) before it could show; on the save where the broken burst was seen, the same textures were caught and the burst drew correctly. No crashes, frame rate unchanged.
- It was not the new texture memory from 0.1.10 (that was suspected at first): 38,000+ texture placements with heavy reuse showed no garbage. A freed texture's memory is now reused only after the graphics chip is certainly done with it anyway.
- If you added `LD_TEX_HEAP=0` to the launcher's extra variables to work around this, remove it: it brings back the stutter after loading screens.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.11-windows.zip**.

## 0.1.10 (beta), 2026-11-02

### Boot crash fixed
- Since 0.1.7 the game rebuilds the pipelines it recorded in earlier sessions on background threads at start. Those threads share compiled shaders with the graphics thread, and the shader compiler's objects do not count their references safely across threads: a reference could be lost and a shader still in use freed, crashing the game in its first second. It got likelier the more pipelines were recorded (the longer you have played).
- Every reference change on the shared shaders now happens under the lock that already guards them. **Tested:** 10 of 10 starts clean with 754 recorded pipelines (before: 2 crashes in 6 starts).

### No more texture stutter right after a loading screen
- After an area loads, the game creates 100-280 new textures a frame for about 3 seconds. Each one was its own Windows memory allocation, about 0.23 ms apiece, which made 30-70 ms hitches. New textures now go into large GPU memory blocks reserved once (about 0.05 ms each). The picture is unchanged.
- **Measured** after the heaviest fight's area load: hitches 10 (767 ms lost) -> 2 (174 ms), and neither of the two left is texture work; the first frame after the load screen 149 -> 90 ms.
- `last_run.log`'s `[hitch]` lines now split texture time into its parts (creation, copying, upload waits).

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.10-windows.zip**.

## 0.1.9 (beta), 2026-11-01

### Each port folder starts its own game
- All launchers on a PC shared one settings file, including the path of the game to start. A newer full package extracted into a new folder kept starting the **older folder's** game, so the new Patches page fell back to the old one ("rpcs3_patch.py or apply-patches.ps1 is missing") and the new game was never used.
- A port folder now keeps its settings in its own `settings.json` (next to the launcher). On the first start it copies the shared settings, so your choices carry over, and points the game, EBOOT and mods at its own folder. A new folder asks once for the game folder (*Choose game folder*), as on a first install.
- **Tested** with a fresh 0.1.9 folder and settings that pointed at another folder's game: the launcher started its own game, the Patches page read the switches from it, and the shared settings were left as they were.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package **GodOfWarAscension-0.1.9-windows.zip**.

## 0.1.8 (beta), 2026-11-01

### Turn patches on and off yourself
- The **Patches** page now works in the release: it lists the game's patches (Disable Depth of Field, Disable MLAA, Disable Motion Blur, Unlock FPS, Disable SPU MLAA) with their authors and notes, read from the game itself.
- Tick or untick one: it is saved at once and used the next time the game starts. Nothing is rebuilt or downloaded.
- Until you change something, the game uses its own defaults, marked **DEFAULT** (Disable Motion Blur, Unlock FPS, Disable SPU MLAA: the same as before). *Use the game's defaults* goes back to them.
- Without Unlock FPS the game runs at 30 FPS (its clock moves 1/30 s per frame); with it, the frame-rate limit on the Graphics page applies.
- **Tested** from a freshly extracted package: the page reads the list from the game in under 0.1 s, an untick is saved and passed to the game, the game starts with exactly those patches (with SPU MLAA on again, the game's original SPU MLAA program runs), and *Use the game's defaults* goes back to passing nothing.

### Full package in every release
- Each release now has **GodOfWarAscension-&lt;version&gt;-windows.zip**, the complete port (launcher, its Python, the game and its DLLs): extract it and start *God of War Ascension Launcher.exe*. The `-update.zip` is only for updating an existing folder; started on its own, the game cannot find `SDL2.dll`.

**Updating:** 0.1.4 and later offer this update in the launcher (*Advanced → Updates*). New install: the full package.

## 0.1.7 (beta), 2026-11-01

### Smooth frame pacing in full screen
- With VSync, a frame can only change on a refresh of the monitor. At 60 FPS on a 75 Hz monitor, three frames stay up for one refresh (13.3 ms) and the fourth for two (26.7 ms): a judder 15 times a second while the FPS counter says a steady 60. 144 and 165 Hz monitors do the same.
- New option *Display → Match monitor refresh* (on by default): going full screen switches that monitor to the same resolution at a rate the frame limit divides (60 or 120 Hz for a 60 FPS limit), and back when you leave full screen or close the game. The screen goes black for a second when it switches.
- **Measured** in the heaviest fight on a 75 Hz monitor: frames held for two refreshes **25% before, 0% after** in calm scenes; in the heaviest fight the 1-6% left are the frames where the game itself took longer than 16.7 ms.
- `last_run.log` says the monitor's refresh, the frame limit and whether frames can be shown evenly (`[display]`), and in full screen how many refreshes each frame stayed on screen (`[pacing]`, every 10 s).

### Less stutter
- The saved pipeline library (over 200 MB) now opens in the background at start, and its loads no longer block the graphics thread on cards where they are slow (some AMD drivers): a stall at the first frames is gone.
- Every pipeline the game builds while you play is recorded, and at the next start they are rebuilt in the background on low-priority threads (about 0.1 s from the library). **Measured** with an empty shader cache, the heaviest fight started twice: on the second start, draws skipped while waiting for pipelines went from 15,331 to 0, and stutters from 21 (1.1 s lost) to 7 (0.4 s).

### Patches switched as the game starts (under the hood)
- RPCS3's patches used to be written into the game's code before it was translated and compiled, so changing one meant rebuilding the game. Now the code each patch touches is translated both ways and the game writes the chosen patches into its memory as it starts, as RPCS3 does: no rebuild. The translated code is identical to before except at the 9 patched instructions.
- This release keeps the same three patches on (Disable Motion Blur, Unlock FPS, Disable SPU MLAA). Ticking patches in the launcher needs the patch list, which a release does not have yet; a future release will offer the switches to everyone.

**Updating:** 0.1.4 to 0.1.6 offer this update in the launcher (*Advanced → Updates*). From 0.1.2 or 0.1.3, extract **GodOfWarAscension-0.1.7-update.zip** over the port folder once.

## 0.1.6 (beta), 2026-11-01

### Blown-out lighting on AMD graphics cards is fixed
- Some of the game's glow and bloom passes blend with two different constants at once (one for the new pixel, one for what is already there). Graphics cards without DirectX 12's alpha blend factor, such as AMD's RX 6000 series, have room for only one, and the port used the same value for both: a pass that should *replace* the light *added* to it every frame, and lights blew out to white.
- Now constants that are exactly 0 or 1 become plain zero/one factors, and where two different constants remain, one of them is applied inside the pixel shader. Both are exact.
- **Measured** by making an RTX 5080 act like a card without the feature, heaviest-fight scene, average difference from the normal picture (0-255): **8.39 before, 0.25 after**. Cards that have the feature are unchanged.

### Garbled textures after some area loads are repaired
- The game streams the top detail levels of big textures in and out. Sometimes a texture's memory block lost its "low-detail only" mark; the graphics chip was then told the texture was full size and read past its block into other data, showing garbage until the next area load. The port now restores the mark and rebuilds the texture's description, so it shows its lower detail levels and streams back in normally.
- **Tested:** 10 runs of the heaviest fight. The problem hit once (about as often as before); it was repaired at once and never came back in that run (before, the game kept re-picking the broken texture every few frames). The 9 other runs were unchanged; 60 FPS throughout, no freeze.

### Sky colour (Tower of Delphi)
- A 3-component dot product in the PS3's pixel shaders rounds slightly differently from a PC GPU's `dot()`, and the sky shader there is sensitive to it (orange instead of pale blue). The port now computes it the way the PS3 does, as RPCS3 found on real hardware (RPCS3 PR #19293). RPCS3 reports this fixed the sky on NVIDIA but not on AMD RX 6000 cards; not yet checked at Delphi in this port. The first start rebuilds part of the shader cache once.

### Better logs for reporting problems
`last_run.log` now says which graphics card, driver and Windows build the game ran on, whether the card has the alpha blend factor, which blends were rewritten, and the picture's average colour every 10 seconds (top band and middle), so "the sky is not blue" or "too bright" shows up as numbers. A garbled-texture repair is logged too.

**Updating:** 0.1.4 and 0.1.5 offer this update in the launcher (*Advanced → Updates*). From 0.1.2 or 0.1.3, extract **GodOfWarAscension-0.1.6-update.zip** over the port folder once.

## 0.1.5 (beta), 2026-11-01

### The "picture freezes, sound keeps playing" freeze is fixed
- At the end of each frame the game's SPU jobs patch a "wait here" command in the graphics command stream with the way to the next frame. When that patch came more than 2 seconds late (a busy or smaller CPU), the port gave up and skipped ahead, and the skip threw away the signal the game's main thread waits for: the picture stopped for good while the sound went on. It now waits up to 60 seconds, so a late patch is a short pause instead.
- **Tested:** all six SPU threads held for 3 seconds in the heaviest fight. 0.1.4 froze for good, the same way as in a tester's logs. 0.1.5 paused for 3 seconds, then ran on at 60 FPS.
- The rare black screen at boot has the same signature, so it may be gone too; not yet confirmed.

### Smoother on 6-core CPUs
- Thread placement had a poor plan for CPUs with fewer than 8 cores. The SPU kernel sat beside the main game thread, and every other thread (game helper threads, audio, shader building, the graphics driver) had to share **one** logical processor.
- The new plan keeps the main game thread and the graphics thread on their own hardware threads, puts the SPU work on the cores between, and lets the other threads use everything else.
- **Measured** with the game limited to 6 cores in the heaviest fight, 2.5 minutes each:

| 6-core plan | Stalls over 100 ms | Worst stall | Time lost to stalls |
|---|---|---|---|
| 0.1.4 | 28 | 759 ms | 10.1 s |
| 0.1.5 | 0 | 59 ms | 0.3 s |

  Frame rate is unchanged (about 47 FPS in that fight on 6 cores of a Ryzen 9 9950X3D).
- *Advanced → Runtime → CPU cores (testing)* now keeps thread placement on and plans it for the chosen core count, as on a real CPU of that size.

### Xbox controllers work
- SDL read Xbox controllers through Windows' raw input, which only delivers input to the thread that started SDL. The game reads the pad from other threads, so an Xbox controller was found but never moved. Xbox pads now go through XInput. PlayStation pads are unchanged. `PS3_PAD_RAWINPUT=1` restores the old way.

**Updating:** the launcher offers 0.1.5 by itself on 0.1.4 (*Advanced → Updates*). From 0.1.2 or 0.1.3, extract **GodOfWarAscension-0.1.5-update.zip** over the port folder once.

## 0.1.4 (beta), 2026-11-01

### Controls fixed
- **Every controller and the keyboard now play together as player one.** Before, only the first controller SDL found drove the game: with a DualSense or a second pad paired, an Xbox controller could be ignored, and the keyboard only worked with no controller connected at all. Buttons from all of them combine; each stick and trigger follows whichever is moved most. `PS3_PAD_MERGE=0` brings back the old behaviour.
- **The Controls page now reaches the game.** The key map, the controller button map and the stick dead zone (`PS3_KEYMAP`, `PS3_PADMAP`, `PS3_PAD_DEADZONE`) were never read by the game before, so it always used its old fixed keys (arrows, Z/X/A/S). Now the default keys are the page's: WASD to walk, the arrows to evade, Space / J / I / L for the face buttons, the mouse buttons, Shift to block, Enter or Esc for START.

### The launcher updates itself
- At start the launcher asks GitHub for the latest release. When it is newer and has an update zip, the launcher offers it, downloads it (size and checksum checked) and installs it over the port folder, then restarts. Saves, settings, the prepared game files and the bundled Python are never touched; replaced files are kept in `updates\backup-<old version>`.
- **Advanced → Updates:** switch the start-up check off, *Check now*, and the *Release page*.
- The repository is private, so GitHub needs your access: paste a read-only token there (github.com → Settings → Developer settings → Fine-grained tokens, *Contents: Read-only* for this repository), or sign in once with the GitHub CLI (`gh auth login`). Without one, the launcher says so and keeps working.

### Stutter reports
- Frames that take 40 ms or more now leave a `[hitch]` line in `last_run.log` with where the time went: building a shader pipeline, decoding a texture, waiting for the game or the SPUs, presenting. Please send your log when you see stutter. `LD_HITCH_MS` sets the threshold (0 = off).

Same speed and the same built-in patches (Disable Motion Blur, Unlock FPS, Disable SPU MLAA) as 0.1.3.

**Updating:** from 0.1.3 or 0.1.2, extract **GodOfWarAscension-0.1.4-update.zip** over the port folder once. Later updates come through the launcher.

## 0.1.3 (beta), 2026-11-01

### Faster: the heaviest fight reaches 60 FPS on a desktop CPU
The SPU geometry work (EDGE), which set the frame rate in heavy fights, now runs mostly as native code and on all six SPUs:
- **Native EDGE job front end:** the control code of every EDGE geometry job runs as hand-written host code instead of recompiled SPU code. Checked bit-exact against the recompiled code on 2,840 recorded jobs (`EDGE_HLE=1`).
- **Faster EDGE math kernels:** transform and lighting passes rewritten as exact native kernels, with AVX-512 where the CPU has it (`SPU_NXF`, `EDGE_KERN_LIGHT`, `K512`).
- **Geometry on all six SPUs:** the game only builds geometry rings for four SPUs; the port now gives SPUs 5 and 6 their own rings, so geometry work spreads over all six (`EDGE6=1`). In 0.1.2 this froze the game at boot.
- **Scheduler interrupt as host code:** the job scheduler's interrupt handler runs natively (`EDGE_PM=1`).
- **Lock-line lock split into 64 stripes:** SPU atomic operations on different memory lines no longer wait for each other (`SPU_LL_STRIPE=1`, stress-tested).

Every piece can be switched off on its own; `runtime-20261031/README.md` lists the switches.

**Measured** in the heaviest egg fight (5-second windows with at least 3,400 draws per frame), Ryzen 9 9950X3D + RTX 5080:

| Build | Heavy-fight FPS |
|---|---|
| 0.1.2 | 50.3 |
| 0.1.3 | 59.5-60 |
| 0.1.3, game limited to 6 cores | 46.4 |

Not yet measured on a laptop with this build. On an 8-core laptop such as the Ryzen 7 7840HS, expect a smaller gain (estimate about 30-34 FPS in the heaviest fights): there the main game thread and the graphics command thread set the limit, not the SPU work.

### Other changes
- **Launcher:** Advanced → Runtime → *CPU cores (testing)*: All / 8 / 6 / 4, to try how the port runs on smaller CPUs. Thread placement is off while a limit is set. Keep *All* for normal play.
- **FPS overlay:** the GPU figure now shows the busiest GPU engine, as Task Manager does. On RTX 50 cards it read 0% before.
- **Build tools:** applying RPCS3 patches from the launcher no longer drops two hooks from the translated game code (the VRAM copy-plan bound and the Z pre-pass hook); `apply-patches.ps1` now puts them back after every re-translation.

### Known issues
- **Textures can look wrong after some area loads** (the video-memory manager moves a texture block twice). The crash this used to cause is guarded; quit and Continue fixes the picture. Root cause still open.
- Rare boot hang (the launcher restarts the game), rare SPU freeze in long sessions, and stutter the first time an area is entered, as in 0.1.2.

## 0.1.2 (beta), 2026-10-31

### Small fix, same speed class
- **Blits:** a 1:1 image copy that the GPU already made between two of its own surfaces no longer also copies stale bytes through guest memory on the CPU. Graphics thread about 3% faster per draw; heavy fights 28.9 vs 28.2 FPS in an in-run A/B. Screenshots unchanged. Runtime: `runtime-patches/blit-guest-skip.diff` (`LD_BLIT_GUEST_SKIP`, on by default).
- **A diagnostic switched off** (`LD_SURF_GUEST_NEWER=0`): it only counted, and read the write watch over whole surfaces every frame.
- **Measured in the final build** (heavy egg fight, Ryzen 7 7840HS laptop): 28.0 FPS mean over 40 heavy windows, no crash in the stability runs. Same range as 0.1.1.

### Measured: what does not get this laptop to 60
Tested in-game with on/off windows in the same run (details in PERFORMANCE.md):
- Moving EDGE geometry math to the GPU: slower (the work around each job costs more than the math).
- Faster SPU code paths (verified exact on ~39,000 replayed SPU runs): no clear gain in game; left off.
- Letting EDGE use all six SPUs: the game freezes at boot.
- No triangle culling on the SPUs: slower.
- Lower scenery detail: even with almost the whole level hidden, heavy fights stay well below 60. The cost is enemies, effects and the game's own logic.

## 0.1.1 (beta), 2026-10-30

### Faster: +10-19% in the heaviest fights
- **Thread placement, on by default** (launcher → Advanced → Runtime → *Thread placement*):
  - The game's busiest threads each get a CPU core: the graphics command thread, the main game thread and the six SPU workers.
  - The always-spinning SPU scheduler thread now sits beside an SPU worker instead of beside the main game thread, which needs about 20 ms of real work per heavy frame.
- **Measured:** heavy egg fight, Ryzen 7 7840HS laptop, placement on vs off within the same run:

  | Run | On | Off | Frames slower than 34 ms (on / off) |
  |---|---|---|---|
  | 1 | 30.8 FPS | 27.8 FPS | 25% / 76% |
  | 2 | 33.2 FPS | 27.8 FPS | 7% / 34% |
  | 3 | 30.7 FPS | 27.3 FPS | 32% / 79% |

- **Fewer slow frames:** frames slower than 34 ms dropped by half or more in every run.
- **Other CPUs:** the placement adapts to any CPU with 8 or more cores. 6-core CPUs keep the earlier plan, and Intel CPUs with P- and E-cores use the P-cores first. Only the 7840HS was measured.
- **Runtime:** `host_pin.c`, see `runtime-patches/thread-placement.diff`. `PS3_PIN_KERNEL_PPU=1` restores the old placement.

### Measured: why a locked 60 is out of reach on this laptop
- **The main game thread:** it does about 20 ms of real work per heavy frame. That is the game's own logic, spread over hundreds of functions, so moving geometry to the GPU cannot fix it. See [PERFORMANCE.md](PERFORMANCE.md).

## 0.1.0 (beta), 2026-10-30

First release by BilalReloaded.

### New
- **Launcher `.exe`** (`God of War Ascension Launcher.exe`). Its Python runtime is bundled, so players install nothing.
- **Choose game folder / Choose disc image (.iso)** on the launcher's Home page and in Advanced → Game files:
  - The launcher finds `PS3_GAME` in the folder you pick, in a parent of it, or one level down. It checks that the game is BCES01741.
  - It finds the decrypted `EBOOT.elf` (from RPCS3: Utilities → Decrypt PS3 Binaries) and writes the copy this build needs into the port folder.
  - Your game folder or `.iso` is never changed. An `.iso` is mounted read-only by Windows when you press PLAY.
- **Game data in the port folder:** game data, saves and the system cache go to `ascension-recomp\ascension\gamedata`.
  - Runtime: `/dev_hdd1` now honours `PS3_HDD1_ROOT` (`ppu_fs.cpp`, `sys_fs.c`).
  - `cellGame` no longer creates folders inside the disc folder when `PS3_HDD0_ROOT` is set.
- **Port by BilalReloaded** in the launcher's title, its sidebar and its About page, with YouTube, GitHub and Patreon link icons.
- **Menu music** in the launcher (`ffmpeg.exe` bundled, from the same LGPL build as the game's DLLs).

### Faster
Heavy combat: the egg fight, 3,300-4,000 draws per frame. Ryzen 7 7840HS laptop with an RTX 4060, Windows "Best performance". All runs compared the change on and off within the same run.

| Change | FPS |
|---|---|
| AVX2 vertex fetch, faster SPU runtime paths, native EDGE fragments 310/330 | 23.5 → 25.9 |
| Hand-written AVX2 EDGE vertex fragment 301 (1.85x faster than before) | 25.6 → 26.3 |

- In total, heavy combat went from 18.5 to 26.3 FPS on this laptop, including about 10% from the Windows power mode.
- Every SPU change replays bit-exact on 12,342 recorded geometry jobs.

### Fixed in earlier internal builds, included here
- **Shadows and sunlight:** sun shadows were missing and sunlight flickered (a full render-target table dropped the shadow passes).
- **White glowing blobs:** the cloth banners rendered as white glowing blobs (the GPU "front face" register was never read).
- **Black or magenta frames:** single black or magenta frames appeared in combat (NaN values spreading through the exposure).
- **Main-menu flicker:** caused by per-frame upload rings being reused while the GPU still read them.
- **Ignored GPU states:** constant blend colour, depth bias, texture border colour, anisotropic filtering, LOD bias, the depth bounds test and the render-target channel order are now applied.
- **Crashes:**
  - A crash in Windows' text-input service during level loads.
  - The game closing at a stage change (an SPU host stack overflow).
- **Textures:** scrambled textures and a freeze after some area loads (a texture block shrunk twice).
- **White specks:** white specks and lines at 2x internal resolution.
- **Log flood:** the log grew by about 1 GB per minute after about 21 minutes of play.
- **Faster loading:** "Continue" loading went from 22-40 s to about 7 s.
