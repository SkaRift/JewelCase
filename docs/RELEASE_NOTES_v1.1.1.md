# Jewel Case 1.1.1

SwanStationPS5 support, tidier Switch updates, and a few fixes.

## What's new

**PS1: SwanStationPS5**
- darkxex's SwanStationPS5 is now supported and used ahead of PSXS5. Like PSXS5, it opens its
  own main menu, where you pick the game.
- PS1 games in SwanStationPS5's and PSXS5's own folders (`/data/SwanStationPS5/games`,
  `/data/PSXS5/games`, or a `SwanStationPS5` / `PSXS5` folder on a drive) now show up in
  Jewel Case too.

**Switch**
- Update and DLC files with hash names (like `001d8d5a7fe8d0cbd148ff67d3b830a1.nsp`) no longer
  show as extra games. Jewel Case reads the title ID inside each file and groups it with its
  game.
- **Update** in File info (Square, then Square again) moves a game's update and DLC into
  ProsperoEden's updates folder, where ProsperoEden applies them. The game then shows
  **Updated**.
- A `firmware` folder in your Switch games folder no longer shows up as a game.
- Updates kept in an `updates`, `update` or `dlc` folder of your own only show with their game.

**Details screen and feel**
- Simpler update chip: **Update available** (amber) or **Updated** (green), and nothing when
  a game has no update files. No more confusing file version numbers like v0 or v327680.
- Turning a game round with the right stick now reflects its back on the floor, not its front.
- The rain music is quieter by default (40%).

## Install

1. Download `PPSA75837.zip` below.
2. Extract it and copy the `PPSA75837` folder to `/data/homebrew/` on your PS5 (to update,
   copy it over your existing one).
3. Install it with ShadowMountPlus and start Jewel Case from the home screen.

Needs an ELF loader on port 9021 (etaHEN's, or a standalone one). Tested on firmware 13.42.
Your settings, favorites and covers in `/data/jewelcase` are kept. See `README.txt` in the app
folder for folders, controls and troubleshooting.

## Known limitations

- SwanStationPS5, PSXS5 (PS1) and PS5SX2 (PS2) open their own main menu instead of starting the
  game; pick it again there.
- Jewel Case can't install updates or DLC into an emulator itself; ProsperoEden doesn't need
  it, other emulators install them from their own menu.
