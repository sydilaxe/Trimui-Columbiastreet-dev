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

`mux_launch.sh` holds MustardOS's idle off while the app is open (its Idle Mute would stop the sound two
minutes after the last press): `idle_inhibit` re-set every 2 s and `caffeine`, given back on exit. The block
is the same in Jaffedrone, Columbia Street, HI5, RadioScan and Conduit; keep the copies the same.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the
buttons of MustardOS's "muOS-Keys" controller (the Brick's or the Brick Pro's numbering, found at run time), the
sticks) and `src/midi.c` (the notes, CCs and the SONG page's MIDI setting). Audio, MIDI I/O, the clock
and the Brick link are `lib/brickio`, a copy of sydilaxe/Brickio-dev's `src/` at the commit in
`lib/brickio/BRICKIO_VERSION`: update it with Brickio-dev's `tools/brickio-sync.sh`, never by editing it here
(a change belongs in brickio, where Conduit and TrimFX get it too). The sound goes through brickio's
`usbaudio` (`bio_process`, holding `bio_mx`: a USB interface straight through ALSA, DUPLEX on one interface
both ways, the speaker through SDL otherwise); `CS_AUDIO_PATH=sdl` keeps the old SDL path (`audio_cb`):
keep both working. The engine makes 16-bit stereo on both. MIDI is brickio's reader and its musical clock
wired by `mclockio` (`mclock` follows, leads and passes on the clock over MIDI and the link; a 1 ms
real-time clock thread); the clock, the link and the sampler's input are all timed to the heard sound
from brickio's latency for the open interface (`bio_latency`). `make test` checks the link's descriptors,
the leader's search against a stand-in sysfs, and how tightly a follower holds the leader's position, and
brickio's own tests check the rest (`make test` there). Wi-Fi sync is brickio's `sync` (HI5S, the same packets
as the Miyoo version's and the other apps'), on the engine through `wifi_clock()` in `src/midi.c`. The engine
and the pages are shared with the Miyoo version; keep the engine the same sound as there (with the sticks at rest,
`build/test/session.wav` matches the Miyoo harness's sample for sample).

The sampler (`src/sampler.c`, the CHOP page's SAMPLE stop, and its CHOP stop to cut a break again live) is
new on the TrimUI: it records the input or the output into a break channel in time (from the next bar line,
exactly its bars long, the input lined up by the round trip), cuts it on a thread of its own and swaps it
in as RECn under the audio lock (`engine_bank_replaced`), and keeps it in `rec/`, loaded after every set
(`breaks_set_rec_dir`). `make test` records both sources and checks they start on the bar line to the
sample. `rec/` and `audio.txt` are the user's: the updater never writes them (`keep_users`).

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
