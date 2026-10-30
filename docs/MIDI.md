# Columbia Street MIDI implementation chart

Columbia Street reads and sends MIDI over USB: plug a class-compliant drum machine, sequencer, controller or
interface into the TrimUI's USB host port (the Brick's top USB-C, through an OTG adapter or hub). Ports are found as
they are plugged in.

## Summary

| Function | Transmitted | Recognized | Remarks |
|---|---|---|---|
| Basic channel | none | 1-16 (any) | Every message is heard on any channel. |
| Mode | none | Omni on | |
| Note number | none | no | |
| Velocity | none | no | |
| Aftertouch | none | no | |
| Pitch bend | none | no | |
| Control change | none | yes | The settings in the CC table below. |
| Program change | none | no | |
| System exclusive | none | no | |
| Song position | none | yes | Places the sequencer, in 16ths. |
| Song select | none | no | |
| Clock | yes | yes | Out: 24 a quarter note while it plays and no clock comes in, timed to the sound as it is heard. In: the tempo follows it, and its phase is held by trimming the playback speed. `CS_MIDI_CLOCK=0` sends none; `CS_MIDI_FOLLOW=0` ignores it. |
| Start, Continue, Stop | yes | yes | Out with the sequencer. In: START from the top, CONTINUE from where it stopped, STOP. |
| Active sensing | none | no | |
| All notes off, all sound off | none | no | |
| Reset | none | no | |

`CS_MIDI=0` in `launch.sh` turns MIDI off.

## Settings by CC

The important channel, mixer, pattern and play settings, all on one channel (any channel is heard). A CC's 0-127
spans the setting's whole range; an on / off setting is on from 64. The channel settings act on the channel CC 2
chooses (breaks 1-4, or the bass): CC 14-26 are its params in the order of its page, so their meaning depends on
whether a break channel or the bass is chosen. The CC numbers are ones MIDI leaves free, in blocks: 14-31 the
channel, 85-90 the tempo and bus, 102-113 the channel's cells, 114-116 the scenes and pattern slot.

<!-- cc table -->
| CC | Setting | Range |
|---|---|---|
| 2 | CHANNEL: the one the channel CCs act on | 0-25 BREAK 1, 26-50 BREAK 2, 51-76 BREAK 3, 77-101 BREAK 4, 102-127 BASS |
| 14 | Channel param 1 | breaks: BREAK (what is loaded); bass: WAVE (what is loaded) |
| 15 | Channel param 2 | breaks: SLICE (what is loaded); bass: ROOT 0 to 11 |
| 16 | Channel param 3 | breaks: MODE 0 to 3; bass: OCTAVE 0 to 3 |
| 17 | Channel param 4 | breaks: RANGE 1 to 16; bass: RIFF (what is loaded) |
| 18 | Channel param 5 | breaks: PITCH -24 to 24; bass: LENGTH 1 to 17 |
| 19 | Channel param 6 | breaks: FIT 0 to 1; bass: GLIDE 0 to 100 |
| 20 | Channel param 7 | breaks: LENGTH 5 to 100; bass: CUTOFF 0 to 100 |
| 21 | Channel param 8 | breaks: ROLL 0 to 4; bass: RESO 0 to 100 |
| 22 | Channel param 9 | breaks: ALT 0 to 5; bass: WIDTH 0 to 100 |
| 23 | Channel param 10 | breaks: FILTER -50 to 50; bass: DRIVE 0 to 100 |
| 24 | Channel param 11 | breaks: BITS 1 to 24; bass: ROLL 0 to 4 |
| 25 | Channel param 12 | breaks: RATE 0 to 40; bass: ALT 0 to 4 |
| 26 | Channel param 13 | breaks: COMP 0 to 100; bass: nothing |
| 27 | Channel LEVEL | 0 to 100 |
| 28 | Channel PAN | -100 to 100 |
| 29 | Channel SEND (reverb) | 0 to 100 |
| 30 | Channel MUTE | 0-63 off, 64-127 on |
| 31 | Channel SOLO | 0-63 off, 64-127 on |
| 85 | BPM | 40 to 250 |
| 86 | MASTER | 0 to 100 |
| 87 | GLUE | 0 to 100 |
| 88 | DRIVE | 0 to 100 |
| 89 | VERB (reverb time) | 0 to 100 |
| 90 | VERB TONE | 0 to 100 |
| 102 | Channel HIT cell ON | 0-63 off, 64-127 on |
| 103 | Channel HIT cell STEPS | 1 to 32 |
| 104 | Channel HIT cell HITS | 0 to its steps |
| 105 | Channel HIT cell ROTATE | 0 to its steps - 1 |
| 106 | Channel ROLL cell ON | 0-63 off, 64-127 on |
| 107 | Channel ROLL cell STEPS | 1 to 32 |
| 108 | Channel ROLL cell HITS | 0 to its steps |
| 109 | Channel ROLL cell ROTATE | 0 to its steps - 1 |
| 110 | Channel ALT cell ON | 0-63 off, 64-127 on |
| 111 | Channel ALT cell STEPS | 1 to 32 |
| 112 | Channel ALT cell HITS | 0 to its steps |
| 113 | Channel ALT cell ROTATE | 0 to its steps - 1 |
| 114 | Scene M1 | 0-63 off, 64-127 on (if saved) |
| 115 | Scene M2 | 0-63 off, 64-127 on (if saved) |
| 116 | Pattern SLOT | 0-15 A, 16-31 B ... 112-127 H (saved slots; at the next bar while playing) |
<!-- end cc table -->
