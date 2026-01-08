#!/usr/bin/env bash
# Simple smoke script to run BrightEyes with thumbnail instrumentation enabled.
# Usage: tools/smoke.sh /path/to/large/directory [duration]

DIR=${1:-$HOME/Pictures}
DUR=${2:-10}
LOG=/tmp/brighteyes-smoke.log

export BRIGHTEYES_THUMBNAILS_DEBUG=1

echo "Running BrightEyes (instrumentation enabled) against: $DIR for ${DUR}s" | tee $LOG

# Start app in background and capture logs
./build/brighteyes "$DIR" > $LOG 2>&1 &
PID=$!
# Let the UI mount and populate thumbnails
sleep $DUR
kill $PID 2>/dev/null || true
sleep 1

echo "--- Instrumentation summary from logs ---"
grep 'THUMBS-INSTR' $LOG || echo "No instrumentation lines found in $LOG"

echo "Log: $LOG"
