#!/bin/sh
# Build and run the PC tests of the portable ESP32 code against the Python gateway's output.
#   esp32/test/host/run.sh            (needs gcc and python/.venv with Pillow)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
COMP="$ROOT/esp32/components"
OUT="$HERE/build"
PY="${PYTHON:-$ROOT/python/.venv/bin/python}"
mkdir -p "$OUT"

"$PY" "$HERE/gen_vectors.py" "$OUT/vectors"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
    -I"$COMP/picksmart/include" -I"$COMP/zivyobraz/include" -I"$ROOT/esp32/main" \
    "$HERE/test_host.c" \
    "$COMP/picksmart/picksmart_models.c" "$COMP/picksmart/picksmart_image.c" \
    "$COMP/zivyobraz/zo_request.c" "$COMP/zivyobraz/zo_image.c" "$ROOT/esp32/main/guard.c" "$ROOT/esp32/main/version.c" \
    -o "$OUT/test_host" -lm 2> "$OUT/cc.log" || { cat "$OUT/cc.log"; exit 1; }
cat "$OUT/cc.log"
"$OUT/test_host" "$OUT/vectors"
