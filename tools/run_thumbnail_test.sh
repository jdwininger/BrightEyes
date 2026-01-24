#!/bin/sh
# Simple helper to test the thumbnailer locally. Usage:
# ./tools/run_thumbnail_test.sh path/to/file.cbz /tmp/thumb.png 128

if [ ! -x build/brighteyes-thumbnailer ]; then
  echo "brighteyes-thumbnailer not built yet; try 'meson compile -C build brighteyes-thumbnailer' or build with meson" >&2
  exit 2
fi

IN=${1:-test.cbz}
OUT=${2:-/tmp/brighteyes-thumb.png}
SIZE=${3:-128}

./build/brighteyes-thumbnailer "$IN" "$OUT" "$SIZE"
RC=$?
if [ $RC -eq 0 ]; then
  echo "Wrote thumbnail to: $OUT"
  ls -l "$OUT"
else
  echo "Thumbnailer failed (exit $RC)" >&2
fi
exit $RC
