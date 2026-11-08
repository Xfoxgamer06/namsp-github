#!/bin/sh
# Launcher for muOS. Put this script and the "namsp" folder content together
# (see README for where your muOS version expects apps/ports).
HERE="$(dirname "$(readlink -f "$0")")"
cd "$HERE" || exit 1
export HOME="$HERE"
export SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
# Low-latency audio wants the CPU at full speed while playing:
for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done
chmod +x ./namsp 2>/dev/null
./namsp "$@" > "$HERE/namsp.log" 2>&1
