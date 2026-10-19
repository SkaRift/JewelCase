# Jewel Case build settings and payload helpers.
# Included by the Makefile after .env, so a local .env can still override any of these.
# SPDX-License-Identifier: GPL-3.0-or-later

PACBREW_PACKAGES ?= sdl2
# LaunchBox (game info, disc scans, more box art): `make LAUNCHBOX=0` builds without it
LAUNCHBOX ?= 1
APP_INCLUDE_PATHS ?= include
# launchbox/: the LaunchBox game data shipped in the app (tools/launchbox_pack.py), when it has been made
APP_ASSETS ?= $(if $(filter 1,$(LAUNCHBOX)),$(if $(wildcard launchbox/VERSION),launchbox,),)
APP_DEFINITIONS ?= GS_NATIVE NDEBUG JC_LAUNCHBOX=$(if $(filter 1,$(LAUNCHBOX)),1,0)
APP_ROOT_FILES ?= helper/jc-launch.elf helper/jc-scan.elf helper/jc-art.elf $(wildcard assets-music/music.mp3)
# assets-music/music.mp3: the rain played when /data/jewelcase/music is empty (tools/make_rain.py)
APP_LAPY_HELPER ?= 0
# use ccache only when it is installed
USE_CCACHE ?= $(if $(shell command -v ccache 2>/dev/null),1,0)

# The three helpers are PS5 payloads that Jewel Case sends to etaHEN's ELF loader:
#   jc-launch  closes Jewel Case and starts the emulator app with the game
#   jc-scan    lists folders (an app can't, inside its sandbox)
#   jc-art     downloads cover art
# They build with the payload SDK the app build fetches into .deps/ (or PS5_PAYLOAD_SDK).
PS5_PAYLOAD_SDK ?= $(firstword $(wildcard $(CURDIR)/.deps/native/ps5-payload-sdk) /opt/ps5-payload-sdk)
PAYLOAD_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PAYLOAD_CFLAGS := -Wall -O2

helper/jc-launch.elf: helper/jc-launch.c
	$(PAYLOAD_CC) $(PAYLOAD_CFLAGS) -o $@ $< -lSceSystemService -lSceUserService
helper/jc-scan.elf: helper/jc-scan.c
	$(PAYLOAD_CC) $(PAYLOAD_CFLAGS) -o $@ $<
# jc-art does HTTPS itself with PacBrew's curl and OpenSSL (the console's SSL refuses payloads)
PACBREW_HOMEBREW = $(firstword $(wildcard $(CURDIR)/.deps/pacbrew/*/sysroot/user/homebrew) $(PS5_PAYLOAD_SDK)/target/user/homebrew)
helper/jc-art.elf: helper/jc-art.c helper/jc-lbdb.c helper/cacert.h
	$(PAYLOAD_CC) $(PAYLOAD_CFLAGS) -DCURL_STATICLIB -I$(PACBREW_HOMEBREW)/include -o $@ $< \
	    -L$(PACBREW_HOMEBREW)/lib -lcurl -lpsl -lssl -lcrypto -lzstd -lz -lpthread -lSceNet -lSceSsl -lSceHttp2

.PHONY: helpers media host
helpers: helper/jc-launch.elf helper/jc-scan.elf helper/jc-art.elf

# `make` builds the helpers first, then the app (dist/PPSA75837).
app: helpers

# Regenerate src/gs/media.h (cartridge and disc templates, icons, background light map).
media:
	python3 tools/make_media.py

# PC test build (needs SDL2 development files): build-host/jewelcase --fsroot <folder>
HOST_SDL_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
HOST_SDL_LIBS ?= $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)
host:
	mkdir -p build-host
	cc -O1 -g -Wall -Wno-unused-function -DGS_HOST -Iinclude -Isrc/gs $(HOST_SDL_CFLAGS) \
	    -o build-host/jewelcase src/gs/main.c src/gs/cJSON.c src/gs/alloc.c src/gs/music.c \
	    $(HOST_SDL_LIBS) -lm -lpthread -ldl

# Release zip in the PS5 homebrew catalog layout: the PPSA75837/ folder at the top,
# with the user README and the license inside it.
.PHONY: release
release: app
	rm -rf release && mkdir -p release
	cp -r dist/PPSA75837 release/PPSA75837
	cp docs/release-readme.txt release/PPSA75837/README.txt
	cp LICENSE release/PPSA75837/LICENSE.txt
	cp THIRD_PARTY_NOTICES.md release/PPSA75837/THIRD_PARTY_NOTICES.txt
	cp assets/media/CREDITS.md release/PPSA75837/CREDITS-models.txt
	cd release && zip -qr PPSA75837.zip PPSA75837
	sha256sum release/PPSA75837.zip
