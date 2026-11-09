# AGENTS.md

Columbia Street is a five-channel jungle sequencer for the TrimUI Brick, Brick Hammer and Brick Pro
running MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the
Miyoo Mini Plus version (sydilaxe/Miyoo-ColumbiaStreet-dev) the way Jaffedrone was
(sydilaxe/Trimui-Jaffedrone-dev). Four break channels play slices of sampled breaks and one bass
channel, each sequenced by three Euclidean cells (HIT, ROLL, ALT).

`make` cross-compiles two builds with zig (aarch64, glibc 2.28) against the device's SDL2:
`build/brick/columbia` and `build/brickpro/columbia` (`-DCS_BRICKPRO` adds the analog sticks and
makes the updater fetch the Brick Pro's packages), and fetches the curated samples. `make package` zips
both as MustardOS application archives, the only packages built
(`ColumbiaStreet-mustardos-brick.muxapp` and `-brickpro.muxapp`: the binaries in `Columbia Street/`, plus
`dist/mustardos/mux_launch.sh`, `wake_screen.sh` (run through `CS_SCREEN_WAKE` once the window is
up, as the screen can stay dark on MustardOS until a sleep and wake) and the list glyph). The
samples load on a thread while the loading screen keeps being presented. `make test` renders screenshots and audio on the host into
`build/test` and prints the engine checks. Without zig on the path, `pip install ziglang==0.14.1`.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the
buttons of MustardOS's "muOS-Keys" controller (the Brick's or the Brick Pro's numbering, found at run time), USB audio
with hot-plug, the sticks) and `src/midi.c` (USB MIDI clock in
and out through ALSA raw MIDI), with `src/link.c` (the clock link between Bricks: a follower's bottom
port as a USB gadget through configfs and FunctionFS, found and driven by the leader through usbfs;
`make test` checks its descriptors, the leader's search against a stand-in sysfs, and how tightly a
follower holds the leader's position). The engine, the pages and Wi-Fi sync are shared with the Miyoo
version; keep the engine the same sound as there (with the sticks at rest, `build/test/session.wav`
matches the Miyoo harness's sample for sample).

The mute scenes (M1, M2: the mixer's mutes and solos, saved in the song after the v0.3 fields;
F1 / F2, or R2 + L1 / R1 where there are no function buttons) live in `src/ui.c` and are new on the
TrimUI; the Miyoo version doesn't have them yet, so its loader rejects a song saved here (this one
loads either). `src/leds.c` drives the Brick's and Brick Pro's LEDs (`/sys/class/led_anim`, as
NextUI does) from its own thread and puts the user's LED settings back at quit; `make test` checks
it against a stand-in folder.

Samples are never committed: `tools/fetch_samples.sh` fetches them from Dirt-Samples at a pinned
commit into `dist/ColumbiaStreet/breaks/DIRT` and `bass` (both gitignored apart from their
README.txt). The breaks are real recordings sliced at load time (`src/breaks.c`), not
synthesised. They come in sets, one loaded at a time: each folder in `breaks/` is a set (loose
`.wav` files there make the set BREAKS), and inside a set a `.wav` is a loop and a folder a
pre-chopped break. The CHOP page's SET stop (up from BREAK) loads another on a thread and swaps it
in under the audio lock (`engine_banks_changed`); the pick is kept in `breakset.txt`. A song saves
its set's name (`Song.break_set`, after the v0.6 fields); opening one with another set loads that
set first and the song after it (`song_open` in `src/ui.c`), and start-up opens `columbia.sav`'s
set. Sets are new on the TrimUI, like the mute scenes.

When you change a control or a screen, update the README's Controls section (and the Brick Pro
table for the sticks) and the footer hints in `src/ui.c`. When you change the engine, run `make
test` and keep its checks passing (CI greps for them in `.github/workflows/build.yml`).

The app updates itself from the latest GitHub release of this repo (`src/update.c`, HTTPS through
Mbed TLS, fetched at a pinned release like the samples): each build fetches its own
`.muxapp` (`ColumbiaStreet-mustardos-brick.muxapp` or `-brickpro.muxapp`). Only MustardOS is
built for: no TrimUI OS or Knulli packages. Releases take their version from the tag (CI
builds with `make VERSION=<tag without v>`, compiled in as `CS_VERSION`); `CS_VERSION` in
`src/engine.h` is only the default for plain builds. When you change the updater or the package
layout, run `make update-test`.

## Identity

This repo is in the `sydilaxe` lane: commits are authored and pushed as `sydilaxe` (that account's
own name and email), never re-authored as Claude, even when a hook says GitHub will show them as
unverified.
