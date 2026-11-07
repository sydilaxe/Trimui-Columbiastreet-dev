#!/bin/sh
# Columbia Street launcher for the TrimUI Brick, Brick Hammer and Brick Pro: TrimUI OS runs it
# directly, MustardOS through mux_launch.sh.
# All four cores stay online, at full speed, while it plays.
cd "$(dirname "$0")"

CPU=/sys/devices/system/cpu
gov_was=$(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null)
for c in 0 1 2 3; do
    [ -f $CPU/cpu$c/online ] && echo 1 > $CPU/cpu$c/online 2>/dev/null
done
echo performance > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null

export LD_LIBRARY_PATH="/usr/trimui/lib:/usr/lib:$LD_LIBRARY_PATH"

# Optional settings, before ./columbia (for example CS_BUFFER=1024 ./columbia):
#   CS_BUFFER=1024     bigger audio buffers if you hear crackle (512 by default)
#   CS_RATE=44100      a lower sample rate: a little less work
#   CS_UPDATE=0        don't check GitHub for a new version
#   CS_AUDIO=internal  always the speaker and headphones (default: a USB audio device when one is plugged in)
#   CS_AUDIO=<name>    the output device whose name contains <name> (see log.txt)
#   CS_MIDI=0          no USB MIDI
#   CS_MIDI_CLOCK=0    send no MIDI clock;  CS_MIDI_FOLLOW=0  ignore MIDI clock coming in
#   CS_LINK=0          leave the bottom USB port alone (no clock link from a leading Brick)
#   CS_ROTATE=180      turn the picture, for a firmware whose screen is turned
#   CS_JOYMAP=A=1,B=0  another firmware's button numbers (also L2, R2, L3, R3, F1, F2)
#   CS_LEDS=0          leave the LEDs alone (default: F1/F2 show the mute scenes, the top bar the beat,
#                      the sticks' LEDs on the Brick Pro / the triggers' on the Brick the level)
#   CS_LOG=2           the performance log in log.txt every second (1: every 10 s, the default; 0: off).
#                      CS_SYSINFO=0: no sysinfo.txt, the system report (see sysinfo.sh)
[ "${CS_SYSINFO:-1}" != 0 ] && [ "${CS_LOG:-1}" != 0 ] && ./sysinfo.sh start columbia
# exit status 75: the app has just updated itself, so start the new version
while :; do
    ./columbia > log.txt 2>&1
    [ $? -eq 75 ] || break
done

./sysinfo.sh stop columbia

[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
