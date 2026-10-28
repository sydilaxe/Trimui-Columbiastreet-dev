# Columbia Street — jungle break sequencer for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS)
#
#   make          cross-compile both builds with zig (aarch64, glibc 2.28, against the device's SDL2):
#                   build/brick/columbia      TrimUI Brick and Brick Hammer
#                   build/brickpro/columbia   TrimUI Brick Pro (adds the analog sticks)
#                 and fetch the curated samples into dist/ColumbiaStreet/breaks and bass
#   make package  both, zipped to unzip at the root of the SD card:
#                   build/ColumbiaStreet-brick.zip, build/ColumbiaStreet-brickpro.zip
#   make test     build the host harness and render screenshots + audio into build/test
#   make install SD=/path/to/card [DEVICE=brickpro]   copy the app into SD/Apps/ColumbiaStreet
#   make update-test   install a fake release over a test app folder, the way the app updates itself
#   make version  print CS_VERSION (src/engine.h); releases are tagged v<version>
#
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG    ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
TARGET := aarch64-linux-gnu.2.28
CPU    := cortex_a53
SDL_INC := third_party/SDL2-2.30.9-headers

SRC    := src/engine.c src/breaks.c src/ui.c src/gfx.c src/sync.c src/aclock.c
HDR    := src/engine.h src/breaks.h src/ui.h src/gfx.h src/sync.h src/aclock.h src/font_data.h src/update.h
NET    := src/net.c src/update.c # the self-updater, on the device and in build/cs_update
PLAT   := src/midi.c src/main_trimui.c
PLATH  := src/midi.h src/net.h
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -fno-math-errno
APP    := dist/ColumbiaStreet
SAMPLES := $(APP)/breaks/AMEN_153bpm.wav
VERSION := $(shell sed -n 's/^\#define CS_VERSION "\([^"]*\)".*/\1/p' src/engine.h)
MAGICK := $(shell command -v magick 2>/dev/null || echo convert)

all: build/brick/columbia build/brickpro/columbia $(APP)/icon.png $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt samples

# read by install.sh to tell whether the device is up to date
$(APP)/VERSION: src/engine.h
	echo $(VERSION) > $@

samples: $(SAMPLES)
$(SAMPLES): tools/fetch_samples.sh
	sh tools/fetch_samples.sh $(APP)
	@touch $@

src/font_data.h: tools/mkfont.py
	python3 tools/mkfont.py /usr/share/fonts/google-noto/NotoSansMono-Bold.ttf $@

$(APP)/icon.png: tools/mkicon.py
	python3 tools/mkicon.py $@

# a stand-in for the device's libSDL2-2.0.so.0, to link against (TrimUI OS has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py src/main_trimui.c
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c src/main_trimui.c
	$(ZIG) cc -target $(TARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# Mbed TLS (Apache-2.0) for the updater's HTTPS, from a pinned release, not committed; each
# toolchain builds its own libmbedtls.a from it with tools/mbedtls.mk
MBED_VER := 3.6.7
MBED_SHA := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBED_DIR := third_party/mbedtls
MBED_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBED_VER)/mbedtls-$(MBED_VER).tar.bz2
MBED_SRC := $(MBED_DIR)/.v$(MBED_VER)
$(MBED_SRC):
	rm -rf $(MBED_DIR) && mkdir -p $(MBED_DIR)
	curl -sSL -o $(MBED_DIR).tar.bz2 $(MBED_URL) || wget -qO $(MBED_DIR).tar.bz2 $(MBED_URL)
	echo "$(MBED_SHA)  $(MBED_DIR).tar.bz2" | sha256sum -c --quiet || (rm -f $(MBED_DIR).tar.bz2; false)
	tar -xjf $(MBED_DIR).tar.bz2 -C $(MBED_DIR) --strip-components=1
	rm $(MBED_DIR).tar.bz2
	touch $@

# the binary carries Mbed TLS, so the app folder carries its licence
$(APP)/LICENSE-mbedtls.txt: $(MBED_SRC)
	cp $(MBED_DIR)/LICENSE $@

build/zig/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/zig MBED_DIR=$(MBED_DIR) \
		CC="$(ZIG) cc -target $(TARGET) -mcpu=$(CPU)" AR="$(ZIG) ar" MBED_CFLAGS=-O2

build/host/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/host MBED_DIR=$(MBED_DIR) CC=cc AR=ar MBED_CFLAGS=-O2

DEVICE_CC = $(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(CFLAGS) -I$(SDL_INC) -I$(MBED_DIR)/include -Isrc
DEVICE_LINK = build/zig/libmbedtls.a build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -Wl,--allow-shlib-undefined -s

build/brick/columbia: $(SRC) $(NET) $(PLAT) $(HDR) $(PLATH) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	@mkdir -p build/brick
	$(DEVICE_CC) -o $@ $(SRC) $(NET) $(PLAT) $(DEVICE_LINK)

build/brickpro/columbia: $(SRC) $(NET) $(PLAT) $(HDR) $(PLATH) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	@mkdir -p build/brickpro
	$(DEVICE_CC) -DCS_BRICKPRO -o $@ $(SRC) $(NET) $(PLAT) $(DEVICE_LINK)

build/cs_test: $(SRC) src/midi.c src/main_test.c $(HDR) src/midi.h
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/midi.c src/main_test.c -lm -lpthread

test: build/cs_test $(SAMPLES)
	@mkdir -p build/test
	build/cs_test build/test $(APP)
	cd build/test && for f in *.ppm; do $(MAGICK) $$f $${f%.ppm}.png && rm $$f; done

build/cs_update: $(NET) src/main_update.c src/engine.h src/net.h src/update.h build/host/libmbedtls.a
	cc $(CFLAGS) -I$(MBED_DIR)/include -o $@ $(NET) src/main_update.c build/host/libmbedtls.a -lpthread

update-test: build/cs_update
	sh tools/update_test.sh

version:
	@echo $(VERSION)

# one SD card layout per device: Apps/ColumbiaStreet, as TrimUI OS lists apps
build/ColumbiaStreet-%.zip: build/%/columbia $(APP)/icon.png $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt $(SAMPLES)
	rm -rf build/sd-$* $@
	mkdir -p build/sd-$*/Apps
	cp -r $(APP) build/sd-$*/Apps/
	rm -f build/sd-$*/Apps/ColumbiaStreet/*.sav* build/sd-$*/Apps/ColumbiaStreet/log.txt
	cp build/$*/columbia build/sd-$*/Apps/ColumbiaStreet/
	echo $* > build/sd-$*/Apps/ColumbiaStreet/DEVICE # which build: read by install.sh
	chmod +x build/sd-$*/Apps/ColumbiaStreet/launch.sh build/sd-$*/Apps/ColumbiaStreet/columbia
	cd build/sd-$* && zip -qr ../ColumbiaStreet-$*.zip Apps

package: build/ColumbiaStreet-brick.zip build/ColumbiaStreet-brickpro.zip

DEVICE ?= brick
install: build/$(DEVICE)/columbia $(APP)/icon.png $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt samples
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard [DEVICE=brickpro]" && exit 1)
	@test -d "$(SD)/Apps" || (echo "$(SD)/Apps not found — is this the TrimUI card?" && exit 1)
	mkdir -p "$(SD)/Apps/ColumbiaStreet"
	cp -r $(APP)/. "$(SD)/Apps/ColumbiaStreet/"
	cp build/$(DEVICE)/columbia "$(SD)/Apps/ColumbiaStreet/"
	echo $(DEVICE) > "$(SD)/Apps/ColumbiaStreet/DEVICE"
	sync

clean:
	rm -rf build $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt

.PHONY: all samples test update-test version package install clean
