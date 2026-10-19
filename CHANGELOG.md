# Changelog

## Unreleased

- Switch: update and DLC files with hash names no longer show as extra games; Jewel Case reads
  the title ID inside each .nsp / .nsz and groups them with their game.
- Switch with ProsperoEden: **Update** in File info moves a game's update and DLC into
  ProsperoEden's updates folder; the game then shows **Updated** and isn't offered it again.
- Switch: a `firmware` folder in the Switch games folder is skipped (its .nca files showed as a
  game), and an update or DLC in an `updates`, `update` or `dlc` folder of your own shows only
  with its game, like ProsperoEden's updates folder.
- Details screen: no version numbers. Without update files there's no version chip; with one
  the chip says **Update available** (yellow) or **Updated** (green). A file's title version
  (v327680) isn't the version the game shows.
- A game turned round (right stick, on the shelf and the details screen) reflects its back on
  the floor, not its front.
- Music volume defaults to 40% (a saved 60%, the old default, moves to 40% once).
- PS2 with PS5SX2: games start directly. jc-launch.elf writes the game into PS5SX2's
  `logs/disc-launch.txt` (its disc payload's request) before starting it; PS5SX2 takes no
  launch arguments. PS5SX2's own `games` folder is listed too (its disc copies).
- PS1 with SwanStationPS5 (darkxex's PSXS5 fork): games start directly (`--rom`), and it's
  preferred over PSXS5 when installed. It starts only games in its own folders
  (`/data/PSXS5/games`, `/data/SwanStationPS5/games`, `PSXS5` / `SwanStationPS5` folders on
  drives), so Jewel Case lists those too, and File info says so for a game elsewhere.
- PSXS5 still only opens (it takes no launch arguments).
- Custom installed-app emulators in `jewelcase.json` can name a request file to write first
  (`requestFile`, `requestLine`).

## 1.1

- Cartridges, cards and discs rendered from 3D models; labels without box branding; discs
  printed edge to edge with a silver rim, clear hub, system bands and the smaller GameCube disc.
- Real disc scans (LaunchBox, GameTDB) and your own disc images.
- Game info, box art for systems libretro lacks (Switch), and disc scans from the LaunchBox
  Games Database, from a reduced game list shipped with the app.
- HTTPS downloads through curl and OpenSSL in the cover helper (the console's SSL refuses
  payloads), with Mozilla's root certificates.
- Updates and DLC grouped under their game; ProsperoEden's game files folder listed, with its
  updates and DLC applied when the game starts; installed / not installed chips and marking for
  other emulators; versions from file names.
- Take-off animation when a game starts; right-stick turning; built-in rain track and menu
  sounds (both generated); 60 fps; a much faster details screen.
- Games folder setting; Create system folders on the chosen drive; Menu sounds setting;
  Credits in Settings.
- L1 / R1 are the only way to change system.

See [docs/RELEASE_NOTES_v1.1.md](docs/RELEASE_NOTES_v1.1.md).

## 1.0

First release. See [docs/RELEASE_NOTES_v1.0.md](docs/RELEASE_NOTES_v1.0.md).
