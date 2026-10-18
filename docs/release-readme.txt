JEWEL CASE v1.0 - by SkaRift
A home-screen ROM launcher for jailbroken PS5 consoles
=====================================================================

Jewel Case puts all your games, from every system, on one screen and
starts each one in the right emulator. Games show up as cartridges and
discs with their cover art, which it downloads for you.


WHAT YOU NEED
-------------
- A jailbroken PS5 running etaHEN (tested on firmware 13.42).
  Jewel Case sends small helper programs to etaHEN's ELF loader
  (port 9021), so etaHEN must be running.
- ShadowMountPlus (or another way to install a homebrew app folder).
- At least one emulator. Standalone emulator apps are used first, and
  RetroArch (PPSA99169) is used for any system that doesn't have one.
  Supported apps:
    PSXS5 (PS1)              PS5SX2 (PS2)          Mupen64Plus PS5 (N64)
    PSFlycast (Dreamcast)    Porpoise (GC / Wii)   PS5CEMU-HAR (DS/3DS/Wii U)
    ProsperoEden (Switch)    XPSemu (Xbox)         PS5X360 (Xbox 360)
    RetroArch with its cores (everything else, and a fallback)
- Internet on the console, for the automatic cover downloads.


INSTALL
-------
1. Copy the PPSA75837 folder to your PS5:
   /data/homebrew/PPSA75837
2. Install it with ShadowMountPlus, like any other homebrew app.
3. Start Jewel Case from the home screen.

To update later: overwrite the files in the same folder.


ADDING GAMES
------------
Put each system's games in its own folder under /data/roms
(or on a USB drive under /mnt/usb0/roms). For example:

    /data/roms/snes/Super Mario World (USA).sfc
    /data/roms/psx/Final Fantasy VII (USA).m3u
    /data/roms/ps2/Kingdom Hearts (USA).iso

Settings > "Create system folders" makes every folder for you and
writes "_Jewel Case folder guide.txt" in /data/roms, listing each folder
and the file types it takes.

Folder names: nes snes n64 gb gbc gba nds 3ds genesis sms gg pce psx psp
ps2 ps3 gc wii wiiu dc saturn segacd 32x switch xbox xbox360 arcade, and
more (see the guide).

After adding games, use Settings > Rescan games (or restart Jewel Case).


COVERS
------
Covers come from the libretro thumbnail collection
(github.com/libretro-thumbnails) and are saved in /data/jewelcase/covers.
They are matched by file name, so No-Intro/Redump style names, such as
"Super Mario World (USA).sfc", match best.

To use your own art, put a .png or .jpg named like the game next to its
file, or in a "covers" folder there. Your own art is always used instead of
a download.

Cartridge labels show the box art without the box's system banner and
ratings. Discs show the art edge to edge with the game's title logo (also
from libretro-thumbnails, in /data/jewelcase/covers/<system>/logos).
GameCube and Wii discs use a scan of the real disc from GameTDB
(art.gametdb.com) when there is one, found by the game ID inside the disc
image (ISO, GCM, WBFS, CISO and RVZ files; not GCZ).

Settings: Download missing covers, Redownload all covers, and an on/off
switch for automatic downloads.


MUSIC
-----
Put MP3 files in /data/jewelcase/music. Jewel Case creates this folder
the first time it runs. Songs play shuffled and looped while you browse,
and fade out when a game starts.
Settings: music on/off, and volume.


CONTROLS
--------
Carousel:
  Left / Right    choose a game          L2 / R2    jump 10 games
  L1 / R1         change system           Cross      play
  Triangle        favorite                Square     details screen
  Circle          close Jewel Case        Options    settings

Details screen:
  Left / Right    next / previous game    Cross      play
  Triangle        favorite                Square     technical info
  Circle          back

The Recently Played and Favorites tabs appear once they have games in
them.


SETTINGS
--------
Rescan games, Create system folders, Download missing covers,
Redownload all covers, Download covers automatically, Background music,
Music volume, Rain effect, Show empty systems, Clear recently played,
Emulators (what was found), About.


TROUBLESHOOTING
---------------
- "ELF loader isn't answering": start etaHEN, then open Jewel Case again.
- A system shows "No emulator found": install one of the emulators above,
  or RetroArch with a matching core. Settings > Emulators shows what was
  found.
- Covers not downloading: check the console's internet connection.
  /data/jewelcase/covers/log.txt shows what the downloader did.
- The PS1 emulator (PSXS5) and PS2 emulator (PS5SX2) open their own game
  list instead of starting the game directly: they don't accept a game at
  launch yet. Pick the game again there.


FILES JEWEL CASE KEEPS
----------------------
Everything Jewel Case saves is in /data/jewelcase, outside the app folder,
so installing an update never touches it:
  recent.json, favorites.json, ui-settings.json
  jewelcase.json   (optional, advanced: your own folders and emulators)
  covers/          downloaded box art
  music/           your MP3s
Plus short-lived jewelcase-*.txt request files in /data.


CREDITS AND LICENSES
--------------------
Jewel Case by SkaRift. Licensed under the GNU GPL v3 (LICENSE.txt), because
it is built on GPL-3.0 code. The source code is published with the release
on GitHub.

Built with and thanks to:
  ps5-native-app-boilerplate (BlackBearReloaded, GPL-3.0)
  ps5-payload-sdk and ps5-payload-websrv (John Tornblom, GPL-3.0)
  SDL2 (zlib license), cJSON (MIT), stb_image / stb_truetype /
  stb_image_write (public domain), dr_mp3 (public domain / MIT-0),
  Poppins font (SIL Open Font License)
  Box art and logos: the libretro-thumbnails project
  GameCube and Wii disc scans: GameTDB (gametdb.com)
  Cartridge, card and disc renders: 3D models by Steven-Bennis (SNES),
  HippoStance (NES), Karolina D (Game Boy), Vxcl (GBA), Pcwer (DS),
  crocoman2019 (Switch) and enjoyboy (disc), CC BY 4.0, changed for
  Jewel Case (labels blanked, logos removed). Links in
  THIRD_PARTY_NOTICES.md in the source repository.
  etaHEN, ShadowMountPlus, and the authors of every emulator above.

Jewel Case doesn't include any games, BIOS files or covers. Use your own
legally made backups.
