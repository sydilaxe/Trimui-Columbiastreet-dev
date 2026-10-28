# Columbia Street

A five-channel jungle sequencer for the TrimUI Brick, Brick Hammer and Brick Pro running TrimUI OS,
ported from the [Miyoo Mini Plus version](https://github.com/sydilaxe/Miyoo-ColumbiaStreet-dev).

Four break channels play slices of sampled drum breaks: the Amen and other loops, chopped at their
hits. The fifth channel is a bass: a sub, a Reese, an 808, an FM growl, or one of 20 jungle bass
samples, played through a short riff. Each channel is sequenced by three polymetric Euclidean
cells:

- **HIT** plays a slice.
- **ROLL** ratchets it: snare rolls, ×2 up to ×8 in one step.
- **ALT** varies it: reverse, an octave up or down, the next slice, a random slice, or a stop.

Every break channel has its own pitch, a filter, a **bit depth from 1 to 24 bits**, sample-rate
reduction and a **compressor**. On the bus are a glue compressor, drive, a reverb, a
tempo-synced L2 filter sweep, and punch-in repeat, reverse and tape-stop effects. Eight
pattern slots switch on the bar line, and the unit syncs tempo over Wi-Fi with HI5 and
Jaffedrone, leads other gear from a mono + clock-pulse output, and sends and follows MIDI clock
over USB.

## Install

There are two builds: `ColumbiaStreet-brick.zip` for the **Brick** and **Brick Hammer**, and
`ColumbiaStreet-brickpro.zip` for the **Brick Pro** (it adds the analog sticks).

1. Download the zip for your device from the latest [release](../../releases), or from the
   artifacts of the latest run on the Actions tab, or build it (see below).
2. Unzip it at the root of the SD card, so the app lands in `SD:/Apps/ColumbiaStreet`. With the
   card mounted you can also run `make install SD=/path/to/card` (add `DEVICE=brickpro` for the
   Brick Pro).
3. Launch **Columbia Street** from Apps. Press MENU to quit.

The state autosaves to `columbia.sav` in the app folder. A song saved on the Miyoo loads as it is:
copy its `columbia.sav` into the folder.

### Over Wi-Fi

With the TrimUI's SSH server on, run this on a computer on the same network:

```sh
curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-Columbiastreet-dev/main/install.sh | bash
```

The script scans the local /24 for an SSH server (a handheld's dropbear first), checks the
installed version against the latest release, and if the release is newer copies it to
`SD:/Apps/ColumbiaStreet`. Your song (`columbia.sav`) and any breaks or basses you added are kept.
It logs in as `root` (`CS_USER` changes it). The first time, it installs the Brick build; set
`CS_DEVICE=brickpro` for the Brick Pro's. After that the installed copy says which build it is.
The address that worked is saved in `~/.config/columbiastreet/host`; `CS_HOST=192.168.1.28` skips
the scan. `CS_FORCE=1` reinstalls even when the version matches. Quit Columbia Street on the
TrimUI before updating.

### Updating from the TrimUI

Columbia Street keeps itself up to date. Five seconds after it starts, it asks GitHub for the
latest release, retrying each minute (up to five times) while Wi-Fi comes up. When there's a
newer one it asks **A: update and restart / B: not now**. A downloads the release's zip for this
device (about 5 MB) while the music keeps playing. It checks every file, swaps in the ones that
changed and starts the new version. Your song and your own breaks stay as they are. If anything
goes wrong on the way, nothing is changed. B asks again next time you open the app.
`CS_UPDATE=0` in `launch.sh` turns the check off. An update replaces `launch.sh`, so set any of
its switches again afterwards.

## The breaks

`make` fetches a curated set into `dist/ColumbiaStreet/breaks` and `bass` from TidalCycles'
[Dirt-Samples](https://github.com/tidalcycles/Dirt-Samples), pinned to one commit, with
`tools/fetch_samples.sh`:

| Bank | What it is |
| --- | --- |
| AMEN | The Amen break, 3 bars at 153 BPM, cut at its 28 hits |
| AMENCU | The Amen pre-chopped into 32 slices |
| PLEAD, RAWCLN, SDSTCK, BLLSTP | Four more breaks, 125-165 BPM |
| JUNGLE | A jungle hit kit: kicks, snares, hats, ride, crash, percussion |
| bass/ | 20 jungle subs and basses for the BASS channel |

The recordings are not committed to this repo. They keep their original rights: the Amen comes
from The Winstons' "Amen, Brother", and Dirt-Samples does not state a licence for these folders.
The release zip includes them, so check that this is acceptable to you before publishing
one.

### Your own breaks

Drop any break into `Apps/ColumbiaStreet/breaks/` (Apache, Think, Funky Drummer...). A `.wav`
file is a loop, sliced at its hits. Put the tempo in its name (`APACHE_115bpm.wav`) and the
slices land on the 16th grid; without a tempo, it is guessed from the length. A folder of
`.wav` files is a pre-chopped break, one slice per file in name order. Bass one-shots go in
`bass/`; their pitch is detected on load. See the README.txt in each folder.

## Controls

These work on every page:

| Button | Action |
| --- | --- |
| L1 / R1 | Previous / next page |
| START | Play / stop |
| SELECT | A new jungle beat on all five channels, keeping the mutes and the mixer; R2+SELECT undoes it |
| L2 (hold) | Low-pass sweep, closing over the time set on MIX > MASTER > L2 |
| R2 + up / right | Repeat the last 1/8 / 1/16 |
| R2 + left / down | Reverse / tape stop |
| R2 + X | Tap tempo (taps under 100 BPM count double) |
| MENU | Quit |

When a new version is out, a prompt covers the page: A updates and restarts, B (or MENU) says
not now. The sequencer keeps playing, and the rest of the buttons wait until it closes.

The pages are PATTERN, CHOP, BASS, MIX and SONG. The footer shows each page's keys.

- **PATTERN**: a 5 × 3 grid of cells, one row per channel. The d-pad picks a cell. A turns it
  on or off, X / Y add or remove hits. Hold B and press up / down to change the length (1-32
  steps), or left / right to rotate. A tap of B rolls a random Euclidean rhythm.
- **CHOP**: one break channel at a time (left / right). The waveform shows the bank's slices,
  the ones this channel can pick, and the slice playing. Up / down picks a parameter, X / Y
  change it, A plays the slice, B mutes the channel.
  - **BREAK, SLICE**: the bank, and the slice to play.
  - **MODE**: FIX plays SLICE every hit. WALK steps through RANGE slices from SLICE, one per
    hit. RAND picks one of them at random. GRID plays the slice that falls on this step of the
    break, so a full HIT cell plays the break straight and a thinner one drops hits out.
  - **PITCH** in semitones. **FIT** speeds the break up or slows it down to the tempo, as
    jungle does.
  - **LENGTH** cuts the slice short. **ROLL** is the ratchet count, **ALT** what the ALT cell
    does.
  - **FILTER**: left of OPEN is a low-pass, right a high-pass.
  - **BITS**: 24 down to 1. **RATE**: sample-rate reduction down to 1.5 kHz.
  - **COMP**: 0 is off; higher lowers the threshold and raises the ratio together (to -36 dB
    and 8:1, 2 ms attack), with make-up gain. The bar under it shows the gain reduction.
- **BASS**: WAVE, ROOT, OCTAVE and a RIFF (a short note pattern; each hit plays the next note,
  restarting every two bars). LENGTH is the note length in 16ths, or HOLD. Also GLIDE, CUTOFF,
  RESO, WIDTH (Reese detune, growl wobble, sub warmth), DRIVE, ROLL and ALT (octave up, fifth,
  octave down, slide, rest).
- **MIX**: a column per channel (LEVEL, PAN, SEND to the reverb) and the master (LEVEL, GLUE
  compressor, DRIVE, VERB time, TONE, and the L2 sweep time). A mutes, B solos.
- **SONG**: pattern slots A-H. A loads a slot (at the next bar while playing), X saves the
  pattern into it, R2+Y clears it. Below them are the tempo, the Wi-Fi sync mode and the
  output row (OUTPUT, CLOCK ON, PPQN, PULSE): left / right picks a setting, X / Y changes it.

## Brick Pro: the sticks

The Brick Pro build adds the two analog sticks, for playing the bus live. They spring back when
let go, and nothing they do is saved.

| Stick | Does |
| --- | --- |
| left, pushed any way | Closes the L2 low-pass by hand: the further, the darker. Let go, it opens again |
| right, up / right | Repeat the last 1/8 / 1/16, as R2 + up / right |
| right, left / down | Reverse / tape stop, as R2 + left / down |
| L3 (click) | Tap tempo |
| R3 (click) | A new jungle beat, as SELECT |

R2 + the d-pad wins over the right stick while both are held.

## USB: MIDI clock and audio

The Brick's USB-C port takes class-compliant MIDI and audio devices, through an OTG adapter or a
hub. They are found as they are plugged in, and a message names each one.

- **MIDI clock out:** while the sequencer plays and no clock is coming in, Columbia Street sends
  clock (24 a quarter note), START and STOP to every MIDI port. The clock is timed to the sound as
  it is heard, so a drum machine or synth lands on the beat.
- **MIDI clock in:** the tempo follows it, and its phase is held by trimming the playback speed, as
  Wi-Fi sync does. START plays from the first step, CONTINUE and song position pick up where they
  say, STOP stops.
- **Audio:** a USB sound card or DAC takes the output while it is plugged in, and the speaker and
  headphones take it back when it is unplugged.

Settings in `Apps/ColumbiaStreet/launch.sh`, before `./columbia`: `CS_MIDI=0` turns MIDI off,
`CS_MIDI_CLOCK=0` sends no clock, `CS_MIDI_FOLLOW=0` ignores clock coming in. `CS_AUDIO=internal`
keeps the speaker; `CS_AUDIO=<name>` picks a device by name (`log.txt` lists what was opened).

## Audio

The sound is made in 512-frame buffers at 48 kHz (about 11 ms, half the Miyoo's delay), with
memory locked so it never waits for a page to be read back in; the screen runs below it at 30
frames a second, and the buttons are read every few milliseconds. If you hear crackles, set
`CS_BUFFER=1024`. `CS_RATE=44100` does a little less work. `CS_ROTATE=180` turns the picture and
`CS_JOYMAP=A=1,B=0,...` renumbers the buttons, for a firmware that differs.

## Wi-Fi sync

Columbia Street speaks HI5S, the sync protocol of HI5 and Jaffedrone. Units on the same network
share tempo, bar phase and transport over UDP broadcast on port 5150. Set the mode on the SONG
page: OFF, AUTO, LEAD or FOLLOW. A unit set to LEAD wins; otherwise the lowest id leads.
The TrimUI's Wi-Fi must be on.

Followers line up what is heard, to within a millisecond or so: each packet carries the time it
was sent, so Wi-Fi delays don't count, and units echo each other's packets to time the trip.
Older HI5S units still sync with these, a few milliseconds less closely. Units of different apps
should already line up; if one sounds early or late against another, `CS_SYNC_LATENCY` in
`launch.sh` sets the frames its sound driver holds (the buffer size, 512, by default).

## Clock out (mono + clock)

As on HI5, the SONG page's **OUTPUT** switches between STEREO and **MONO+CLK**: the sound in mono
on one side of the headphone jack and clock pulses on the other, for a Pocket Operator, Volca or
other gear with a sync input (split it with a stereo-to-two-mono cable). **CLOCK ON** picks the
side (RIGHT by default), **PPQN** the pulses per quarter note (1, 2, 4, 8, 16, 24 or 32: 2 for
Pocket Operators and Volcas, the default; 24 for DIN sync) and **PULSE** the width (1-20 ms, 5 by
default). The pulses run only while the sequencer plays and follow tap tempo and Wi-Fi sync. In
mono + clock every channel plays from the centre: the PANs read C on the MIX page and come back
with STEREO. Songs saved by v0.1 load in STEREO.

## Building

You need [zig](https://ziglang.org/) 0.14 for the cross-compile: `zig` on the path, `ZIG=...`, or
`pip install ziglang==0.14.1` (the Makefile then uses `python3 -m ziglang`). You also need `curl`,
`bzip2`, `zip`, `python3` (with Pillow for the icon), and for `make test` a host C compiler and
ImageMagick.

```sh
make              # cross-compile build/brick/columbia and build/brickpro/columbia, fetch the samples
make package      # build/ColumbiaStreet-brick.zip and build/ColumbiaStreet-brickpro.zip, as CI does
make test         # host harness: screenshots, audio and engine checks into build/test
make update-test  # the self-update end to end, against tools/fake_github.py
make install SD=/path/to/mounted/sdcard [DEVICE=brickpro]
```

Both builds are aarch64 for glibc 2.28, for the Cortex-A53. They link against a stand-in
`libSDL2-2.0.so.0` made by `tools/mksdlstub.py`; the device's own SDL2 is used at run time.
`-DCS_BRICKPRO` adds the sticks and makes the updater fetch the Brick Pro's zip.

### Releases

CI (`.github/workflows/build.yml`) builds both TrimUI packages with zig and runs the tests on
every push.
To publish a release, push a tag (`git tag v0.3.1 && git push origin v0.3.1`), or run the
workflow from the Actions tab with the tag in `release_tag` (`v0.3.1` or `0.3.1`). The version
comes from the tag: CI builds the release with `make package VERSION=0.3.1`, which the app
compares with the latest release's tag to tell whether it's out of date, so there's nothing to
bump first. A tag must be `v` and dotted numbers. Plain builds use `CS_VERSION` in `src/engine.h`
(`make version` prints the version a build gets). Releases are full releases marked Latest, since the updater and `install.sh`
read GitHub's latest release, and that skips pre-releases. They have `ColumbiaStreet-brick.zip`
and `ColumbiaStreet-brickpro.zip` attached and `.github/release-notes.md` as their notes. Running again for the same tag replaces
the zips.

`build/cs_update APP_DIR [--force]` (built by `make update-test`) checks for a newer release
and installs it into `APP_DIR`, the way the app does. Point `CS_UPDATE_API` at
`tools/fake_github.py` (`http://127.0.0.1:8766/latest.json`) to test an update without
publishing one.

The updater's HTTPS comes from
[Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.7, which `make` downloads (pinned by its
SHA-256) into `third_party/mbedtls/` (not committed) and links in statically.

### Layout

| Path | Contents |
| --- | --- |
| `src/engine.c` | Sequencer, slice playback, channel effects, bass voices, bus, save / load |
| `src/breaks.c` | WAV loading, onset slicing, bass pitch detection |
| `src/ui.c` | Pages, input, drawing |
| `src/gfx.c`, `src/font_data.h` | Framebuffer drawing and the font |
| `src/sync.c` | Wi-Fi sync |
| `src/aclock.c` | Audio clock: where the music is at the speaker, for Wi-Fi sync |
| `src/update.c`, `src/net.c` | Self-update: GitHub's latest release, the download and unzip; HTTPS |
| `src/main_trimui.c` | Device entry point (SDL2: video scaled 1.6x, buttons, sticks, USB audio) |
| `src/midi.c` | USB MIDI clock in and out (ALSA raw MIDI) |
| `src/main_test.c` | Host test harness |
| `src/main_update.c` | Host updater (`build/cs_update`) for tests |
| `install.sh` | The one-line installer, over SSH |
| `tools/fetch_samples.sh` | Fetches the curated breaks and basses |
| `tools/fake_github.py`, `tools/update_test.sh` | A local GitHub stand-in and the update test |
| `tools/mbedtls.mk` | Builds Mbed TLS for each toolchain |
| `tools/mksdlstub.py` | The stand-in SDL2 library to link against |
| `dist/ColumbiaStreet/` | The app folder as installed on the card |

## License

MIT. See [LICENSE](LICENSE). The SDL2 headers in `third_party/` are under the zlib licence; see
`third_party/SDL2-2.30.9-headers/LICENSE.txt`. The font is Noto Sans Mono
(SIL OFL 1.1). Mbed TLS is under Apache-2.0 (or GPL-2.0-or-later); the app folder carries its
licence as `LICENSE-mbedtls.txt`. `dist/ColumbiaStreet/cacert.pem` is curl's extract of
Mozilla's CA certificates (MPL 2.0). The samples are not part of this repo and are not covered by its licence.
