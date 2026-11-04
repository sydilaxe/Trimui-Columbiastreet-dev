# Columbia Street

A five-channel jungle sequencer for the TrimUI Brick, Brick Hammer and Brick Pro running TrimUI OS
or [MustardOS](https://muos.dev), ported from the [Miyoo Mini Plus version](https://github.com/sydilaxe/Miyoo-ColumbiaStreet-dev).

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

**Latest release:** [v0.6.0](../../releases/tag/v0.6.0) · [all releases](../../releases)

## Install

Each release has a package for each device and firmware. The Brick Pro's adds the analog sticks.

| | TrimUI OS | MustardOS |
| --- | --- | --- |
| **Brick**, **Brick Hammer** | `ColumbiaStreet-brick.zip` | `ColumbiaStreet-mustardos-brick.muxapp` |
| **Brick Pro** | `ColumbiaStreet-brickpro.zip` | `ColumbiaStreet-mustardos-brickpro.muxapp` |

### TrimUI OS

1. Download the zip for your device from the latest [release](../../releases), or from the
   artifacts of the latest run on the Actions tab, or build it (see below).
2. Unzip it at the root of the SD card, so the app lands in `SD:/Apps/ColumbiaStreet`. With the
   card mounted you can also run `make install SD=/path/to/card` (add `DEVICE=brickpro` for the
   Brick Pro).
3. Launch **Columbia Street** from Apps. Press MENU to quit.

### MustardOS

1. Download the `.muxapp` for your device from the latest [release](../../releases).
2. Copy it into the `ARCHIVE` folder at the root of the first SD card (SD1).
3. On the device, open **Applications → Archive Manager** and install it. It lands in
   `MUOS/application/Columbia Street` (on SD2 if you have one, else SD1).
4. Launch **Columbia Street** from **Applications**. Press MENU to quit.

By hand instead: unzip the `.muxapp` (it is a zip) and copy its `Columbia Street` folder into
`MUOS/application/` on the card. `make install SD=/path/to/card OS=mustardos` (add
`DEVICE=brickpro` for the Brick Pro) does the same from a build.

The app reads MustardOS's own controller, so the buttons are as labelled whatever A/B layout
MustardOS is set to. `log.txt` in the app folder names the controller it found.

On the Brick Pro the screen could stay dark after the app opened (it played, but nothing showed
until the power button slept and woke the device). Once its window is up, the app now runs
`wake_screen.sh`, which does what MustardOS does on waking: the framebuffer blanked and unblanked,
and the saved brightness applied again. `CS_SCREEN_WAKE=` (empty) in `mux_launch.sh` turns it off.

The state autosaves to `columbia.sav` in the app folder. A song saved on the Miyoo, or under the
other firmware, loads as it is: copy its `columbia.sav` into the folder.

### Over Wi-Fi

With the TrimUI's SSH server on (on MustardOS, Secure Shell under Web Services),
run this on a computer on the same network:

```sh
curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-Columbiastreet-dev/main/install.sh | bash
```

The script scans the local /24 for an SSH server (a handheld's dropbear first), checks the
installed version against the latest release, and if the release is newer copies it to
`SD:/Apps/ColumbiaStreet`, or on MustardOS to its application folder. Your song (`columbia.sav`) and any breaks or basses you added are kept.
It logs in as `root` (`CS_USER` changes it). It tells TrimUI OS and MustardOS apart by itself, and
MustardOS says which device it is. On TrimUI OS, the first time, it installs the Brick build; set
`CS_DEVICE=brickpro` for the Brick Pro's. After that the installed copy says which build it is.
The address that worked is saved in `~/.config/columbiastreet/host`; `CS_HOST=192.168.1.28` skips
the scan. `CS_FORCE=1` reinstalls even when the version matches. Quit Columbia Street on the
TrimUI before updating.

### Updating from the TrimUI

Columbia Street keeps itself up to date. Five seconds after it starts, it asks GitHub for the
latest release, retrying each minute (up to five times) while Wi-Fi comes up. When there's a
newer one it asks **A: update and restart / B: not now**. A downloads the release's package for
this device and firmware (about 5 MB) while the music keeps playing. It checks every file, swaps in the ones that
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
| L2 (hold) | Brick Pro: low-pass sweep, closing over the time set on MIX > MASTER > L2. Brick: see below |
| L2 + d-pad (Brick) | Low-pass sweep, as the Brick Pro's left stick: any direction closes it over the MIX > MASTER > L2 time, let go and it opens |
| L2 + X / A (Brick) | Repeat the last 1/8 / 1/16, as the Brick Pro's right stick up / right |
| L2 + Y / B (Brick) | Reverse / tape stop, as the Brick Pro's right stick left / down |
| R2 + up / down (Brick) | The groove, as the Brick Pro's left stick with the switch up: lock the cells toward the breakbeats / the clave family |
| R2 + right / left (Brick) | Ghost notes after / before the hits |
| R2 + up / right | Brick Pro: repeat the last 1/8 / 1/16. Brick: the groove (see below) |
| R2 + left / down | Brick Pro: reverse / tape stop. Brick: the groove (see below) |
| R2 + X | Tap tempo (taps under 100 BPM count double) |
| F1 / F2 | Mute scene M1 / M2 on or off (see below) |
| R2 + F1 / F2 | Save the mixer's mutes and solos as M1 / M2 |
| R2 + L1 / R1 | M1 / M2 on or off; on the MIX page, save M1 / M2 |
| Side switch (Brick) | Half time: up halves the tempo, down brings back the tempo it was |
| HOME (Brick Pro, MustardOS) | Half time: the tempo halved; press again for the tempo it was |
| MENU | Quit |

### Mute scenes

Two saved sets of the mixer's mutes and solos, M1 and M2, to drop channels in and out in one press
while the beat plays. Set the mutes and solos on MIX (A mutes, B solos), then R2 + L1 saves them as
M1 and R2 + R1 as M2 (R2 + F1 / F2 does the same on any page). F1 switches M1 on: the mixer takes
its mutes and solos. F1 again switches it off, and the mutes and solos come back as they were. F2
does the same for M2; pressing F2 while M1 is on swaps straight to M2, and off then goes back to how
things were before either. Without F1 and F2 (the Miyoo, a keyboard), R2 + L1 / R1 switch the
scenes from any page but MIX. The header shows M1 and M2 once saved, lit while on, and F1's and
F2's own LEDs light while their scene is on. The scenes are saved with the song.

Until you save over them, **M1 (F1) is the bass alone** (every break channel muted) and **M2 (F2)
the breaks alone** (the bass muted), so F1 and F2 drop the drums or the bass out from the start.

### LEDs

On the Brick, Columbia Street plays the LEDs along:

- **F1 and F2** light red while their mute scene is on.
- **The top bar** keeps the tempo: a flash on each beat that fades through it, orange on the
  first beat of the bar. It is dark while stopped.
- **The triggers' LEDs** follow the sound: as bright as the output is loud, in the colour of the
  loudest channel, red when it clips.

On the Brick Pro each LED is set on its own:

- **F1 and F2** light red while their mute scene is on.
- **The top bar** is a stereo level meter: from the centre out, the left channel to the left and
  the right to the right, green rising to amber and red near full scale.
- **The L1, L2, R1 and R2 LEDs** keep the tempo, as the top bar does on the Brick.
- **The left stick's ring** points where the stick is pushed (the filter sweep), its corners
  brighter the further it goes; **the right stick's ring** the same, in the colour of the effect
  that way: orange for repeat 1/8 (up), yellow for repeat 1/16 (right), violet for reverse (left),
  red for tape stop (down).

`CS_LED_FRAME=0` drives the Brick Pro's LEDs as on the Brick instead.

Your own LED settings are put back when you quit. `CS_LEDS=0` in `launch.sh` leaves the LEDs alone.

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
  compressor, DRIVE, VERB time, TONE, and the L2 sweep time). A mutes, B solos; R2 + L1 / R1
  saves the mutes and solos as mute scene M1 / M2.
- **SONG**: pattern slots A-H. A loads a slot (at the next bar while playing), X saves the
  pattern into it, R2+Y clears it. Below them are the tempo, the Wi-Fi sync mode and the
  output row (OUTPUT, CLOCK ON, PPQN, PULSE): left / right picks a setting, X / Y changes it.

## Brick Pro: the sticks

The Brick Pro build adds the two analog sticks, for playing the bus live. They spring back when
let go, and nothing they do is saved.

The **side switch** (on the right, above the power button) picks what the left stick plays:
down for the FILTER, up for the GROOVE (a GRV badge in the header). A message names it as
you flip it.

| Stick | Does |
| --- | --- |
| left, pushed any way (switch down) | Closes the L2 low-pass by hand: the further, the darker. Let go, it opens again |
| left, up (switch up) | Locks the Euclidean cells toward breakbeat patterns: the two-step and amen kicks, the backbeat, the amen snare, four on the floor, offbeat hats. Each channel finds the one closest to what its HIT cell plays. A little way changes a few steps; all the way, every bar plays the pattern |
| left, down (switch up) | The same toward the clave family: tresillo, son and rumba clave, bossa nova, cinquillo |
| left, right / left (switch up) | Quiet ghost notes on the empty 16ths just after / just before the hits, more the further it goes |
| right, up / right | Repeat the last 1/8 / 1/16, as R2 + up / right |
| right, left / down | Reverse / tape stop, as R2 + left / down |
| L3 (click) | Tap tempo |
| R3 (click) | A new jungle beat, as SELECT |

R2 + the d-pad wins over the right stick while both are held.

**Half time** is the HOME key on the left, under the volume keys (MustardOS's button 4; the volume keys are 2 and 3). It
halves the tempo (a 1/2 badge in the header); a second press brings back the tempo it was, or
doubles a tempo changed meanwhile. The song is saved at the full tempo. On another firmware,
`CS_JOYMAP=HALF=n` in `launch.sh` puts it on button n (`log.txt` names the buttons it doesn't use).

The groove changes the steps, never the cells: let go, and the pattern plays as programmed again.
Diagonals combine the lock and the ghost notes.

On the **Brick**, which has no sticks, **L2 stands in for them**. L2 alone does nothing; L2 + the
d-pad is the left stick (any direction closes the low-pass, over the MIX > MASTER > L2 time, and
let go it opens), and L2 + A / B / X / Y is the right stick, each button where the stick would
point: X (top) repeats the last 1/8, A (right) the last 1/16, Y (left) reverses, B (bottom) stops
the tape. While L2 is held, the d-pad and A / B / X / Y don't reach the page.

**R2 + the d-pad plays the groove** on the Brick, as the left stick does on the Brick Pro with the
switch up: up locks the cells toward the breakbeat patterns, down toward the clave family, right
adds ghost notes after the hits, left before them; two directions at once combine, as a diagonal
does. The d-pad can't be pushed part way, so each direction plays a set amount (the lock half way,
a few ghost notes), and a GRV badge shows while it plays. Let go of R2 and the pattern plays as
programmed again. The punch-in effects are on L2 + A / B / X / Y.

The Brick has no HOME key either, so its side switch is **half time**:
up halves the tempo (the 1/2 badge), down brings back the tempo it was, or doubles a tempo changed
meanwhile. If the switch is up when the app starts, it starts in half time; the song is still saved
at the full tempo.

The switch is GPIO 243 (up 0, down 1). `CS_SWITCH=0` in `launch.sh` leaves it alone,
`CS_SWITCH=/path` reads another file, `CS_SWITCH_FLIP=1` swaps the two positions. On TrimUI OS,
check what the system itself does with the switch (its FN switch setting), as it may act too.
MustardOS reads the same pin on the Brick and the Brick Pro (its input daemon passes it on as a
switch on the muOS-Keys controller) but does nothing with it, so nothing else happens when you flip it.

## USB: MIDI clock and audio

The Brick has two USB-C ports: the top one is the USB host port, the bottom one charges. Plug
class-compliant MIDI and audio devices into the **top** port, directly with a USB-C cable or
through a hub. They are found as they are plugged in, and a message names each one. The port
gives about 500 mA, so a device that draws more needs a powered hub.

- **MIDI clock out:** while the sequencer plays and no clock is coming in, Columbia Street sends
  clock (24 a quarter note), START and STOP to every MIDI port. The clock is timed to the sound as
  it is heard, so a drum machine or synth lands on the beat.
- **MIDI clock in:** the tempo follows it, and its phase is held by trimming the playback speed, as
  Wi-Fi sync does. START plays from the first step, CONTINUE and song position pick up where they
  say, STOP stops.
- **MIDI notes:** channels 1-4 play breaks 1-4's slices (note 36, C1, is the first slice, each note
  up the next), channel 5 plays the bass at the key's pitch, held while the key is down.
- **Settings by MIDI CC:** the important channel, mixer, pattern and play settings each have a CC,
  on any channel: CC 2 chooses the channel, 14-26 its params, 27-31 its level, pan, send, mute and
  solo, 85-90 the tempo and bus, 102-113 its HIT, ROLL and ALT cells, 114-116 the scenes and the
  pattern slot. [docs/MIDI.md](docs/MIDI.md) is the full MIDI implementation chart.
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
`CS_JOYMAP=A=1,B=0,...` renumbers the buttons (also `L2`, `R2`, `L3`, `R3`, `F1`, `F2`), for a firmware that
differs. `log.txt` lists the controllers found and any button the app doesn't use.

## Logs and tuning

Two files in the app folder tell you how Columbia Street runs on your device:

- **`log.txt`**, written by the app: the firmware and board, the processor, the screen, the controllers, the sound
  device and SDL's driver, then every 10 seconds a **STAT** line (the audio load, dropouts, late calls, clicks
  made in the app, the longest wait between two buffers, the screen's frame rate, the processor's clock and
  temperature) and a **PROC** line (the processor's time by thread with the audio thread marked, memory and its
  peak, page faults, free memory, how often the audio thread was preempted). **JUMP** lines mark each click the
  app's own output made, with its time; a click heard with no JUMP and no dropout comes from the sound server.
  **HINT** lines say what to change, and an **END** line sums up the session.
- **`sysinfo.txt`**, written by `sysinfo.sh` from `launch.sh`: the system and its sound path (PipeWire's devices,
  rate and quantum, the mixer), then every 10 seconds the load, clock, temperature, busiest processes and
  PipeWire's nodes with their **ERR** (xrun) counts.

`CS_LOG=2` in `launch.sh` writes the lines every second; `CS_LOG=0` turns the log off, `CS_SYSINFO=0` the report.

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
make package      # the TrimUI OS zips and the MustardOS .muxapps, as CI does
make test         # host harness: screenshots, audio and engine checks into build/test
make update-test  # the self-update end to end, against tools/fake_github.py
make install SD=/path/to/mounted/sdcard [DEVICE=brickpro] [OS=mustardos]
```

Both builds are aarch64 for glibc 2.28, for the Cortex-A53. They link against a stand-in
`libSDL2-2.0.so.0` made by `tools/mksdlstub.py`; the device's own SDL2 is used at run time.
`-DCS_BRICKPRO` adds the sticks and makes the updater fetch the Brick Pro's packages.

The same binaries go into both firmwares' packages. At run time the app reads MustardOS's
"muOS-Keys" controller when it is there (its numbering is MustardOS's own, from its
`device/tui-brick*` controller maps) and TrimUI OS's controller otherwise. It updates from the
MustardOS package when its folder has `mux_launch.sh` (`dist/mustardos/`), which only that
package carries.

### Releases

CI (`.github/workflows/build.yml`) builds both TrimUI packages with zig and runs the tests on
every push.
To publish a release, push a tag (`git tag v1.2.3 && git push origin v1.2.3`), or run the
workflow from the Actions tab with the tag in `release_tag` (`v1.2.3` or `1.2.3`). The version
comes from the tag: CI builds the release with `make package VERSION=1.2.3`, which the app
compares with the latest release's tag to tell whether it's out of date, so there's nothing to
bump first. A tag must be `v` and dotted numbers. Plain builds use `CS_VERSION` in `src/engine.h`
(`make version` prints the version a build gets); keep it, and the README's **Latest release** line, at the latest release (CI checks the two match). Releases are full releases marked Latest, since the updater and `install.sh`
read GitHub's latest release, and that skips pre-releases. They have `ColumbiaStreet-brick.zip`,
`ColumbiaStreet-brickpro.zip`, `ColumbiaStreet-mustardos-brick.muxapp` and
`ColumbiaStreet-mustardos-brickpro.muxapp` attached and `.github/release-notes.md` as their notes. Running again for the same tag replaces
the packages.

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
