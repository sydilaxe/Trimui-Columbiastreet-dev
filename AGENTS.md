# AGENTS.md

Columbia Street is a five-channel jungle sequencer for the TrimUI Brick, Brick Hammer and Brick Pro
running TrimUI OS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the
Miyoo Mini Plus version (sydilaxe/Miyoo-ColumbiaStreet-dev) the way Jaffedrone was
(sydilaxe/Trimui-Jaffedrone-dev). Four break channels play slices of sampled breaks and one bass
channel, each sequenced by three Euclidean cells (HIT, ROLL, ALT).

`make` cross-compiles two builds with zig (aarch64, glibc 2.28) against the device's SDL2:
`build/brick/columbia` and `build/brickpro/columbia` (`-DCS_BRICKPRO` adds the analog sticks and
makes the updater fetch the Brick Pro's zip), and fetches the curated samples. `make package` zips
both for `Apps/ColumbiaStreet`. `make test` renders screenshots and audio on the host into
`build/test` and prints the engine checks. Without zig on the path, `pip install ziglang==0.14.1`.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the
TrimUI's joystick buttons, USB audio with hot-plug, the sticks) and `src/midi.c` (USB MIDI clock in
and out through ALSA raw MIDI). The engine, the pages and Wi-Fi sync are shared with the Miyoo
version; keep the engine the same sound as there (with the sticks at rest, `build/test/session.wav`
matches the Miyoo harness's sample for sample).

Samples are never committed: `tools/fetch_samples.sh` fetches them from Dirt-Samples at a pinned
commit into `dist/ColumbiaStreet/breaks` and `bass` (both gitignored apart from their
README.txt). The breaks are real recordings sliced at load time (`src/breaks.c`), not
synthesised.

When you change a control or a screen, update the README's Controls section (and the Brick Pro
table for the sticks) and the footer hints in `src/ui.c`. When you change the engine, run `make
test` and keep its checks passing (CI greps for them in `.github/workflows/build.yml`).

The app updates itself from the latest GitHub release of this repo (`src/update.c`, HTTPS through
Mbed TLS, fetched at a pinned release like the samples): each build fetches its own zip
(`ColumbiaStreet-brick.zip` or `-brickpro.zip`). Releases take their version from the tag (CI
builds with `make VERSION=<tag without v>`, compiled in as `CS_VERSION`); `CS_VERSION` in
`src/engine.h` is only the default for plain builds. When you change the updater or the package
layout, run `make update-test`.
