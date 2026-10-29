Columbia Street, a five-channel jungle sequencer for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS
and MustardOS).

**Install on MustardOS**

Copy `ColumbiaStreet-mustardos-brick.muxapp` (Brick, Brick Hammer) or `ColumbiaStreet-mustardos-brickpro.muxapp`
(Brick Pro) into the `ARCHIVE` folder of the first SD card, install it from **Applications → Archive Manager**, then
open **Applications → Columbia Street**. Press MENU to quit.

**Install on TrimUI OS**

Unzip `ColumbiaStreet-brick.zip` (Brick, Brick Hammer) or `ColumbiaStreet-brickpro.zip` (Brick Pro) at the
root of the SD card (it contains `Apps/ColumbiaStreet/`), then open **Apps → Columbia Street**. Press MENU to
quit.

Or, on either firmware, with the TrimUI's SSH server on, run this on a computer on the same network (on TrimUI
OS, add `CS_DEVICE=brickpro` before `bash` the first time on a Brick Pro):

```sh
curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-Columbiastreet-dev/main/install.sh | bash
```

**New: MustardOS**
- Packages for MustardOS on the Brick, Brick Hammer and Brick Pro, installed from the Archive Manager. The app
  reads MustardOS's controller, so every button (and the Brick Pro's sticks) works as on TrimUI OS, and it
  updates itself from the MustardOS package.

**New on TrimUI**
- Ported from the Miyoo Mini Plus: the same sequencer and the same sound, on SDL2, the pages scaled 1.6x to
  fill the 1024x768 screen. Songs saved on the Miyoo load as they are.
- Half the Miyoo's audio delay (512-frame buffers), memory locked, the screen below the sound.
- **USB MIDI clock:** clock, START and STOP out to a drum machine or synth while it plays; clock in leads the
  tempo and phase, START / CONTINUE / STOP / song position follow it.
- **USB audio:** a USB sound card or DAC takes over the output while it is plugged in.
- **Brick Pro:** the left stick closes the bus low-pass by hand; the right stick plays repeat 1/8, repeat 1/16,
  reverse and tape stop; L3 taps the tempo, R3 makes a new beat.
- **Updates itself** from this repo's releases, each device and firmware fetching its own package.

**From the Miyoo version — 0.3.0**
- Updates itself: a few seconds after it starts, Columbia Street asks GitHub for the latest release and, when
  there's a newer one, offers to download it and restart (A) or wait (B). The music keeps playing while it
  downloads; your song and your own breaks are kept, and a failed update changes nothing. `CS_UPDATE=0` in
  `launch.sh` turns it off.
- Mono + clock out, as on HI5: on SONG, set **OUTPUT** to **MONO+CLK** and the sound plays in mono on one side
  of the headphone jack, with clock pulses on the other to lead a Pocket Operator, Volca or other gear.
- Four break channels playing slices of sampled breaks (the Amen, four more breaks, the Amen pre-chopped and a
  jungle hit kit) and a bass channel (SUB, REESE, 808, GROWL or 20 jungle bass samples through a riff).
- Three Euclidean cells per channel: HIT, ROLL (ratchets) and ALT (reverse, octave, next or random slice, stop).
- Per break channel: FIX / WALK / GRID / RAND slice modes, pitch, fit to tempo, filter, bit depth 1-24,
  sample-rate reduction and a compressor. On the bus: glue compressor, drive, reverb, the L2 sweep and R2 +
  d-pad repeat, reverse and tape stop.
- SELECT makes a new jungle beat (R2+SELECT undoes it); 8 pattern slots switch on the bar line; Wi-Fi sync with
  HI5 and Jaffedrone.
- Your own breaks: drop a WAV loop into `Apps/ColumbiaStreet/breaks/` (with the tempo in its name, e.g.
  `APACHE_115bpm.wav`) and it is sliced at its hits when the app starts.

If you hear crackles, set `CS_BUFFER=1024` in `launch.sh` in the app folder (`Apps/ColumbiaStreet` on TrimUI OS,
`MUOS/application/Columbia Street` on MustardOS). The log is written to `log.txt` in the same folder.
