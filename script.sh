#!/usr/bin/env bash
set -euo pipefail

BUILD=./build/brighteyes
GIF_PATH="${1:-/home/jeremy/Downloads/sg.gif}"
LOG=/tmp/br_gif_test.txt

# kill existing instances (no error if none)
pkill -f brighteyes || true

# start app in background with logs
BRIGHTEYES_DEBUG_LOAD_RACE=1 G_MESSAGES_DEBUG=all "$BUILD" "$GIF_PATH" >"$LOG" 2>&1 &
APP_PID=$!
echo "Launched brighteyes pid=$APP_PID -> logging to $LOG"

# wait for X window for this PID (timeout after ~20s)
WIN=""
for i in $(seq 1 40); do
  # find a window owned by the app's PID (wmctrl fields: WIN PID ...)
  WIN=$(wmctrl -lp | awk -v pid="$APP_PID" '$3==pid {print $1; exit}')
  [ -n "$WIN" ] && break
  sleep 0.5
done

if [ -z "$WIN" ]; then
  echo "No BrightEyes window found after ~20s; see $LOG"
  echo "Tail logs: tail -n 200 $LOG"
  exit 1
fi
echo "Found window id=$WIN"

# get window geometry safely (use timeout in case of weird X issues)
if ! XINFO=$(timeout 2 xwininfo -id "$WIN" 2>/dev/null); then
  echo "xwininfo failed or timed out; aborting clicks"
  exit 1
fi
eval $(printf "%s\n" "$XINFO" | awk '
/Absolute upper-left X/ {print "X="$4}
/Absolute upper-left Y/ {print "Y="$4}
/Width/ {print "W="$2}
/Height/ {print "H="$2}
')

CX=$((X + W/2))
BY=$((Y + H - 48))

# raise/activate window
wmctrl -ia "$WIN"
sleep 0.2

# click controls; adjust offsets if your controls are elsewhere
xdotool mousemove --window "$WIN" "$CX" "$BY" click 1
sleep 0.2
xdotool mousemove --window "$WIN" "$CX" $((BY-36)) click 1
sleep 0.2
xdotool mousemove --window "$WIN" "$CX" $((BY-72)) click 1
sleep 0.5

echo "Done clicking. Check for handler logs:"
grep -E "on_gif_play_|on_gif_step_|on_gif_stop|on_animation_loaded" "$LOG" || echo "No matching log lines found; tail $LOG"
