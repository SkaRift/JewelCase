# Building Jewel Case

Jewel Case is a native PS5 app built on
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), whose build system this
repository keeps (`Makefile`, `tools/`, `tooling/`, `runtime/`). The app itself is in
`src/gs/`, and three small payload helpers are in `helper/`.

## Requirements

Linux or WSL with `make`, `python3`, `curl`, `unzip` and a host C/C++ compiler. On the first
build, the build scripts download the PS5 payload SDK and SDL2 (from PacBrew) into
`.deps/`.

## App

```sh
make
```

This builds the three helpers, then compiles, links and signs the app into
`dist/PPSA75837`, which you copy to the console.

`make release` also writes `release/PPSA75837.zip`, the zip for a GitHub release. It has the
`PPSA75837/` folder at the top, as the PS5 homebrew catalog expects, with `README.txt`
(from `docs/release-readme.txt`) and `LICENSE.txt` inside it. Raise `contentVersion` in
`sce_sys/param.json` and `GS_VERSION` in `src/gs/main.c` before each release.

Build settings (include paths, defines, SDL2, which helper files go into the app folder) are
in `jewelcase.mk`. A local `.env` (see `.env.example`) can override any of them, and also
sets `PS5_HOST` for `make deploy`.

## Helpers

| File | What it does |
|---|---|
| `helper/jc-launch.c` | Closes Jewel Case and starts the emulator app with the game. |
| `helper/jc-scan.c` | Lists the game, app and music folders. Apps can't list folders inside the sandbox. |
| `helper/jc-art.c` | Downloads cover art from libretro-thumbnails, streaming it to disk. |

`make helpers` builds only these, with the payload SDK in `.deps/` (or `PS5_PAYLOAD_SDK`).

## Generated sources

- `src/gs/media.h` holds the cartridge, card and disc templates, the icons and the background
  light map, all drawn by `tools/make_media.py`. Run `make media` (needs Pillow and NumPy) to
  regenerate it.
- `src/gs/splash.h` (the loading screen) and `sce_sys/pic0.dds` / `pic1.dds` (the launch
  splash and home-screen background, 3840×2160 BC7) are made from
  `assets/branding/splash.png`.
- `src/gs/fonts.h` (Poppins) and `src/gs/logo.h` are embedded files.

## PC test build

```sh
make host
build-host/jewelcase --fsroot /path/to/test-root
```

`--fsroot` stands in for the PS5's `/`. Put games in `<root>/data/roms/<system>/`.
`--script "right,next,info,shot:out.png"` runs inputs and saves screenshots, which is how
the README screenshots were made.
