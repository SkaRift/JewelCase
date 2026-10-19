<p align="center">
  <img src="assets/branding/splash.png" alt="Jewel Case by SkaRift" width="720">
</p>

<p align="center">
  A home-screen ROM launcher for jailbroken PS5 consoles.<br>
  Every system's games on one shelf, as cartridges and discs with box art, each started in the right emulator.
</p>

<p align="center">
  <a href="../../releases/latest"><b>Download the latest release</b></a>
</p>

---

![Carousel](docs/screenshots/carousel.jpg)

| | |
|---|---|
| ![A system tab](docs/screenshots/snes.jpg) | ![Details screen](docs/screenshots/details.jpg) |
| ![Settings](docs/screenshots/settings.jpg) | |

## Features

- **One launcher for everything.** NES to Xbox 360, sorted into tabs by system, with
  **Recently Played** and **Favorites** tabs.
- **Real-looking media.** Every game is drawn as its system's cartridge, card or disc, with
  its cover printed on it, in a carousel with reflections on a glossy floor.
- **Automatic box art.** Covers download from the
  [libretro thumbnail collection](https://github.com/libretro-thumbnails) and are matched by
  file name, ignoring region tags and punctuation. Systems that collection lacks (like Switch)
  get the box front from the [LaunchBox Games Database](https://gamesdb.launchbox-app.com).
  Your own art always takes priority.
- **Labels, not boxes.** Cartridge labels show the box art without the box's system banner
  and ratings. Discs are printed edge to edge with the art and the game's title logo, inside a silver
  rim around a clear hub, with the system's band on PS1, PS2 and GameCube discs (GameCube
  discs are the smaller 8 cm size). When there's a scan of the real disc, that's used
  instead: from the [LaunchBox Games Database](https://gamesdb.launchbox-app.com) for every
  disc system, and from [GameTDB](https://www.gametdb.com) for GameCube and Wii, found by the
  game ID in the disc image.
- **Standalone emulators first.** Jewel Case starts each game in a dedicated PS5 emulator
  app when you have one installed, and falls back to RetroArch with a matching core.
- **Details screen** for each game: its year, genre, developer, players and description from
  the LaunchBox Games Database, an in-game screenshot behind it (from
  libretro-thumbnails, or its title screen when there's none), its emulator and when you last played it. Square shows the
  file, region and size.
- **Updates and DLC** sit with their game instead of showing as extra games. With ProsperoEden,
  Switch updates and DLC just work from one folder; for other emulators an amber or green chip
  shows whether they're installed yet ([how it works](#updates-and-dlc)). The game's version
  comes from its file name.
- **Feels like a console menu.** Soft rain sounds (your own MP3s instead, if you add some), a
  low retro boop as you move between games, and a rain effect; each can be turned off. Starting
  a game sends it off the screen: discs spin up, cartridges turn, and both fly away.
- **Native app.** It has its own home-screen icon, installs with ShadowMountPlus, and needs no
  browser or Homebrew Launcher.

## Requirements

- A jailbroken PS5 with an **ELF loader** listening on port 9021. etaHEN has one built in;
  a standalone ELF loader works too. Jewel Case sends small helper payloads to it. Tested on
  firmware 13.42 with etaHEN.
- **ShadowMountPlus**, or another way to install a homebrew app folder.
- One or more emulators:

| System | Emulator used (first one found) |
|---|---|
| PlayStation | PSXS5, then RetroArch (SwanStation, PCSX ReARMed, Beetle PSX) |
| PlayStation 2 | PS5SX2, then RetroArch (LRPS2) |
| Nintendo 64 | Mupen64Plus PS5, then RetroArch (Mupen64Plus-Next, ParaLLEl) |
| GameCube / Wii | Porpoise, then RetroArch (Dolphin) |
| DS / 3DS | PS5CEMU-HAR, then RetroArch (melonDS, DeSmuME, Azahar) |
| Wii U | PS5CEMU-HAR |
| Dreamcast | PSFlycast, then RetroArch (Flycast) |
| Switch | ProsperoEden |
| Xbox / Xbox 360 | XPSemu / PS5X360 |
| Everything else | RetroArch (PPSA99169) with the matching core |

## Install

1. Download `PPSA75837.zip` from [Releases](../../releases).
2. Extract it and copy the `PPSA75837` folder to `/data/homebrew/` on your PS5.
3. Install it with ShadowMountPlus, then start **Jewel Case** from the home screen.

To update, overwrite the files in the same folder. Your settings, favorites and downloaded
covers are kept in `/data/jewelcase`, so an update never touches them.

## Adding games

Put each system's games in its own folder under `/data/roms` (or `/mnt/usb0/roms` on a USB
drive):

```
/data/roms/snes/Super Mario World (USA).sfc
/data/roms/psx/Final Fantasy VII (USA).m3u
/data/roms/ps2/Kingdom Hearts (USA).iso
```

**Settings → Create system folders** makes every folder for you. It also writes a guide in
`/data/roms` listing each folder and the file types it takes. After adding games, use
**Settings → Rescan games**.

No-Intro / Redump style file names, such as `Super Mario World (USA).sfc`, match covers
best. To use your own art, put a `.png` or `.jpg` named like the game next to its file, or
in a `covers` folder there.

Music: Jewel Case comes with a soft rain track (generated by `tools/make_rain.py`, no recording
or license involved). To play your own instead, put MP3 files in `/data/jewelcase/music`. Jewel
Case creates this folder on first run.

### Updates and DLC

**Switch with ProsperoEden (v1.005 or newer): nothing to install.** ProsperoEden applies a
game's newest update and all its DLC every time the game starts, from its own game files
folder only: each game in a folder of its own in its `roms` folder, with the update and DLC
files beside it (any names).

```
<ProsperoEden game files folder>/roms/Super Mario Odyssey/
    Super Mario Odyssey.nsp
    Super Mario Odyssey 1.1 update.nsp
    Super Mario Odyssey DLC.nsp
```

The game files folder is the one picked in ProsperoEden's **Settings → Game files**
(`/data/prosperoeden` if you never picked one). Jewel Case reads that setting and lists the
Switch games there too. Everything in a game's folder shows as that one game.

**Updates next to a game somewhere else** (such as `/data/roms/switch`) also show as part of
that game, even with a hash for a name: Jewel Case reads the title ID inside each `.nsp` /
`.nsz`. The game shows **Update available** in amber. Open its details (□), press □ again for
**File info**, then **✕ Update**: Jewel Case moves the update and DLC into ProsperoEden's
`updates` folder (renamed after the game, like `Super Mario Odyssey update [0100000000010800].nsp`),
where ProsperoEden applies them every time the game starts. Still one copy of each file. The
game then shows **Updated** in green, and Update isn't offered again.

**Other emulators** (Wii U in Cemu, older ProsperoEden versions) need updates and DLC installed
into their own storage once, from their own menu. Jewel Case starts games; it doesn't install
anything into an emulator. Keep the files next to the game:

```
/data/roms/wiiu/Some Game.wua
/data/roms/wiiu/Some Game update.wua
```

Jewel Case shows these as one game. It recognises updates and DLC by "update", "upd", "patch"
or "DLC" in the name, or by the Switch title ID in brackets (an update's ends in `800`). To
install one, with no extra copies:

1. In the emulator, use its install option and pick the file straight from your Jewel Case
   games folder.
2. In Jewel Case, open the game's details (□), press □ again for **File info**, then
   **✕ Mark installed**.
3. Optional: delete the file from the games folder. The emulator keeps its installed copy,
   and Jewel Case still shows **Update 1.1 installed**.

Amber **not installed** means the file is there but not marked yet; green **installed** means
you've marked it. A newer update file turns it amber again. Jewel Case can't look inside the
emulator, so "installed" is whatever you marked; **✕ Mark not installed** undoes it. Marks are
kept in `/data/jewelcase/installed.json`. Versions come from file names (`1.0`, `v1.2.0`,
`[v131072]`) and show on the details screen for any system.

## Controls

| Carousel | | Details screen | |
|---|---|---|---|
| Left / Right | choose a game | Left / Right | previous / next game |
| L1 / R1 | change system (only these) | Cross | play |
| L2 / R2 | jump 10 games | Triangle | favorite |
| Cross | play | Square | technical info |
| Triangle | favorite | Circle | back |
| Square | details screen | Square | file info (Update, or marking updates installed) |
| Right stick | turn the game around | Right stick | turn the game around |
| Options | settings | | |
| Circle | close Jewel Case | | |

## Building from source

See [BUILDING.md](BUILDING.md). In short, on Linux or WSL:

```sh
make          # builds the helpers and the app into dist/PPSA75837
make host     # PC test build, needs SDL2 (build-host/jewelcase --fsroot <folder>)
```

## Known limitations

- PSXS5 and PS5SX2 don't accept a game at launch yet, so they open their own game list.
- Cover matching works from file names, so heavily renamed files may get no cover. Add your
  own art for those.
- Jewel Case can't install updates or DLC into an emulator, or check what the emulator has
  installed. ProsperoEden applies them by itself from its game files folder; for other
  emulators, install them from the emulator's menu and mark them in File info.

## Credits

Jewel Case by **SkaRift**.

Built on [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (BlackBearReloaded)
and the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk), with SDL2, cJSON, stb,
dr_mp3 and the Poppins font; the cover helper's HTTPS uses curl and OpenSSL (from PacBrew) with
Mozilla's root certificates. The rain music and the menu sounds are generated by Jewel Case's own
code (`tools/make_rain.py`), not recorded. The cartridges, cards and discs are rendered from CC BY 4.0 models by
Steven-Bennis, HippoStance, Karolina D, Vxcl, Pcwer, crocoman2019 and enjoyboy, and the N64
cartridge from a CC BY-NC 4.0 model by ejpman ([credits](assets/media/CREDITS.md)). Box art and logos come from
[libretro-thumbnails](https://github.com/libretro-thumbnails), game info, more box art and disc scans from the
[LaunchBox Games Database](https://gamesdb.launchbox-app.com) and
[GameTDB](https://www.gametdb.com). Thanks to the etaHEN and
ShadowMountPlus developers and to the authors of every emulator listed above. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Jewel Case includes no games, BIOS files or covers. Use backups you made yourself.

## License

[GNU GPL v3](LICENSE).
