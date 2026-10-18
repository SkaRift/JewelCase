# Third-party notices

## Credits and acknowledgements

| Project | Role |
| --- | --- |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | Public PS5 headers, libc++ headers, sysroot, and Clang target support |
| [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) | Optional prebuilt PS5 ports and static libraries |
| [SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero) | Source of the ELF converter and FSELF writer in `tooling/native/` (GPL-3.0) |
| [LLVM/Clang](https://github.com/llvm/llvm-project) | Native compiler |
| [GoogleTest](https://github.com/google/googletest) | Pinned host-only C++ unit-test framework |
| [zlib](https://zlib.net/) | Pinned source-built compression library used by the host FSELF tool |
| [Microsoft DirectXTex](https://github.com/microsoft/DirectXTex) | `texconv` presentation-image preparation |
| [FFmpeg](https://ffmpeg.org/) | Developer-supplied selection-audio preparation |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | Directory-style deployment and hardware validation |
| [ArkSama/PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon) | Original Lapy project and owned-root design |
| [mpereiraesaa/PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) | Exact-title one-shot helper and cooperative elevation protocol |

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and the public
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). The bootstrapper
downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. The application does not redistribute or dynamically load the
complete libc++ or libc++abi archives.

The PS5 ELF converter and FSELF writer in `tooling/native/` are derived from
[SharpProspero](https://github.com/SvenGDK/SharpProspero), Copyright (C) 2026
SvenGDK, GPL-3.0, and were translated to C++ and modified by BlackBearReloaded.

## Host test dependency

The host unit-test target downloads
[GoogleTest](https://github.com/google/googletest) 1.17.0 after verifying
SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
It remains under ignored `.deps/test/`, retains its BSD-3-Clause license, and
is not linked into any PS5 application, runtime, or package artifact.

## Optional PS5-Lapy-JB-Daemon integration

The sandbox-elevation build fetches
[BlackBearReloaded's fork of PS5-Lapy-JB-Daemon](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)
at commit `57fcc19b7f57e46c823d6f2a6c6233ebf3288079`, invokes its unmodified
`owned-helper` target for the selected application's exact title and packages
the generated helper ELF with Lapy's MIT license. The shared protocol header
published upstream is LGPL-2.1-or-later; this repository's application-side
wire implementation is GPL-3.0-or-later.

Lapy was created by
[ArkSama / Team PHU](https://github.com/ArkSama/PS5-Lapy-JB-Daemon). Credit
belongs to ArkSama, mpereiraesaa and the Lapy contributors. No Lapy kernel
source is copied or modified here.

The helper build also uses the pinned `ps5log/1` header from
[mpereiraesaa/ps5-agc-gears](https://github.com/mpereiraesaa/ps5-agc-gears/tree/1ae1f9182abd2770c131b97419034fb85173c2dc/native/ps5log),
GPL-3.0-or-later, and the official PS5 Payload SDK v0.42. Those build inputs
remain under ignored `.deps/lapy/`; the normal application toolchain remains
the separately pinned Payload SDK v0.42.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

The update-check example (`make update-check-example`) selects `libcurl`, which
statically links these PacBrew libraries into the example title. Its build
output stays under ignored `dist/` and is not distributed by this repository; an
application that ships the update check ships them and must carry their notices:

| Component | Version in PacBrew v0.40.2 | License |
| --- | --- | --- |
| [libcurl](https://curl.se/) | 8.18.0 | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | 3.5.2 | Apache License 2.0 |
| [zlib](https://zlib.net/) | 1.3.2 | zlib license |
| [zstd](https://github.com/facebook/zstd) | 1.5.6 | BSD-3-Clause (dual-licensed with GPL-2.0) |
| [libpsl](https://github.com/rockdaboot/libpsl) | 0.21.5 | MIT; built-in Public Suffix List data MPL-2.0 |

`examples/update-check/console_curl.c`, which makes these libraries run in a
native title, is original BlackBearReloaded code (GPL-3.0-or-later), taken from
the ProsperoRadio and ProsperoLichess projects. Its `gmtime_r` follows Howard
Hinnant's public-domain `civil_from_days` algorithm.

## Vendored miniz

`third_party/miniz/` holds [miniz](https://github.com/richgel999/miniz) 3.0.2
(commit `293d4db1b7d0ffee9756d035b9ac6f7431ef8492`), MIT, unmodified, with its
`LICENSE`; `SOURCE.json` records each file's SHA-256. Jewel Case's cover
helper (`helper/jc-lbdb.c`, in `jc-art.elf`) uses its inflate code to unpack LaunchBox game
data (a feature currently turned off), so Jewel Case ships miniz and carries its notice here. The
self-update helper (`examples/self-update-helper`) and its host test also link it. The
helper's archive validation and file helpers are original BlackBearReloaded
code (GPL-3.0-or-later), taken from the ProsperoStore project.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Original presentation assets

The BlackBear icon, selection artwork, and default selection track
`sce_sys/snd0.at9` are original assets supplied by BlackBearReloaded, Copyright
(C) 2026 BlackBearReloaded, and distributed under GPL-3.0-or-later. The track
is titled `Night Drive`.

No proprietary runtime module, encryption key, or game file is included.

## Jewel Case

Jewel Case (by SkaRift) is built on this boilerplate and adds these components:

| Component | License | Where |
| --- | --- | --- |
| [SDL2](https://www.libsdl.org/) (PacBrew build, linked statically) | zlib | input, rendering |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT | `src/gs/cJSON.c`, `include/cJSON.h` |
| [stb_image, stb_image_write, stb_truetype](https://github.com/nothings/stb) | public domain / MIT | `include/` |
| [dr_mp3](https://github.com/mackron/dr_libs) | public domain / MIT-0 | `include/dr_mp3.h` |
| [Poppins](https://fonts.google.com/specimen/Poppins) | SIL Open Font License 1.1 | embedded in `src/gs/fonts.h` |
| Launch logic after [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv) | GPL-3.0 | `helper/jc-launch.c` |
| HTTP sample from the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) | GPL-3.0 | `helper/jc-art.c` |
| [miniz](https://github.com/richgel999/miniz) 3.0.2 (inflate, from `third_party/miniz/`) | MIT | `helper/jc-lbdb.c` (in `jc-art.elf`) |

The cartridge, card and disc artwork contains no console makers' logos: it is drawn by
`tools/make_media.py`, or rendered from the models below with their logos removed.

## Cartridge and disc models (CC BY 4.0)

The cartridge, card and disc images are renders of 3D models licensed under the Creative
Commons Attribution 4.0 license (https://creativecommons.org/licenses/by/4.0/), changed for
Jewel Case (rendered front-on, labels blanked, logos removed, some recoloured):

- "SNES Cartridge" by Steven-Bennis — https://sketchfab.com/3d-models/snes-cartridge-1caba5630eb0435187d6e975d4eed6c5
- "NES Cartridge" by HippoStance — https://sketchfab.com/3d-models/nes-cartridge-e3c8de99be1740ae8fa551aa9b2596d2
- "Gameboy Cartridge" by Karolina D — https://sketchfab.com/3d-models/gameboy-cartridge-7c0160c8097a43e0a5e146bf4d7e01a1
- "Gameboy Advance Cartridge" by Vxcl — https://sketchfab.com/3d-models/gameboy-advance-cartridge-38c1e6702e5d4f21af1d0930689b1d10
- "Nintendo DS Cartridge" by Pcwer — https://sketchfab.com/3d-models/nintendo-ds-cartridge-364e487e31fd4135b6c6941a0ffaba82
- "Nintendo Switch Cartridge" by crocoman2019 — https://sketchfab.com/3d-models/nintendo-switch-cartridge-94e7c74e664849d490f136cb8acca86a
- "CD Jewel Case" (the disc) by enjoyboy, Blend Swap

The N64 cartridge is a render of "N64 Cartridge" by ejpman
(https://www.printables.com/model/166312-n64-cartridge), licensed under Creative Commons
Attribution-NonCommercial 4.0 (https://creativecommons.org/licenses/by-nc/4.0/), rendered
front-on with a blank label for Jewel Case. It may not be used commercially.

See assets/media/CREDITS.md.

## Downloaded art

Jewel Case includes no game art. On the console it downloads box art and title logos from
the libretro-thumbnails project (https://github.com/libretro-thumbnails) and GameCube / Wii
disc scans from GameTDB (https://www.gametdb.com). That art belongs to its owners.

Support for the LaunchBox Games Database (game info, disc scans, more box art) is in the source
(`helper/jc-lbdb.c`, `tools/launchbox_pack.py`) but turned off: release builds include no
LaunchBox data and download nothing from LaunchBox.
