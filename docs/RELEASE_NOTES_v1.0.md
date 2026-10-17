# Jewel Case 1.0

The first release of Jewel Case, a home-screen ROM launcher for jailbroken PS5 consoles, by
SkaRift.

## Highlights

- Every system's games in one carousel, drawn as cartridges, cards and discs with their box
  art, on a reflective floor with optional rain.
- Tabs by system (L1/R1), plus Recently Played and Favorites.
- Automatic cover downloads from libretro-thumbnails, matched by file name. Your own art
  takes priority.
- Launches standalone PS5 emulators first (PSXS5, PS5SX2, Mupen64Plus PS5, Porpoise,
  PS5CEMU-HAR, PSFlycast, ProsperoEden, XPSemu, PS5X360) and falls back to RetroArch cores.
- A details screen for each game.
- Settings: rescan games, create system folders, cover downloads, background music (your
  MP3s), rain effect, and more.
- Everything it saves (recent games, favorites, settings, covers, music) is kept in
  /data/jewelcase, so updates never touch it.

## Install

1. Download `PPSA75837.zip` below.
2. Extract it and copy the `PPSA75837` folder to `/data/homebrew/` on your PS5.
3. Install it with ShadowMountPlus and start Jewel Case from the home screen.

Needs etaHEN running (Jewel Case uses its ELF loader on port 9021). Tested on firmware 13.42.
See `README.txt` in the app folder for folders, controls and troubleshooting.

## Known limitations

- PSXS5 (PS1) and PS5SX2 (PS2) don't accept a game at launch yet, so they open their own
  game list.
