# Changelog

## Unreleased

- Switch: update and DLC files with hash names no longer show as extra games; Jewel Case reads
  the title ID inside each .nsp / .nsz and groups them with their game.
- Switch with ProsperoEden: **Update** in File info moves a game's update and DLC into
  ProsperoEden's updates folder; the game then shows **Updated** and isn't offered it again.

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
