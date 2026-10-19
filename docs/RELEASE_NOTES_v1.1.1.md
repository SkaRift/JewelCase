# Jewel Case 1.1.1

PS1 and PS2 games start straight into the game, Switch updates are tidier, and a few fixes.

## What's new

**PS1 and PS2 start the game directly**
- **PS2 (PS5SX2):** pick a game and PS5SX2 boots it, no second pick in its own list. Jewel Case
  hands it the game through the request file PS5SX2's own disc feature reads. Games in
  PS5SX2's own `games` folder (its disc copies) show up in Jewel Case too.
- **PS1 (SwanStationPS5):** darkxex's SwanStationPS5 is now supported and used ahead of PSXS5.
  It boots games directly when they're in its own folders (`/data/SwanStationPS5/games`,
  `/data/PSXS5/games`, or a `SwanStationPS5` / `PSXS5` folder on a drive), and Jewel Case now
  lists games from those folders. File info tells you when a game is somewhere it can't boot
  from.
- PSXS5 still opens its own game list (it doesn't accept a game at launch).

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

- PS2 direct start depends on PS5SX2's disc request file; a future PS5SX2 could change it,
  and then PS5SX2 opens its own list instead. PS5SX2 also needs its PS5SX2 Helper payload to
  start at all.
- SwanStationPS5 only boots games from its own folders; for games elsewhere it opens its list.
- Jewel Case can't install updates or DLC into an emulator itself; ProsperoEden doesn't need
  it, other emulators install them from their own menu.
