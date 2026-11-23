#!/bin/sh
# Build the binaries to hand out, into esp32/dist/:
#   zivyobraz-ble-gateway-esp32-<version>-full.bin   whole flash image, write at 0x0 (first install)
#   zivyobraz-ble-gateway-esp32-<version>-ota.bin    the app alone (web page upload, GitHub update)
#   latest.json                                      what gateways read to find this release (update.h)
#   SHA256SUMS
# The version comes from version.txt. Needs ESP-IDF activated in the shell.
#
# The app is signed with the RSA-3072 key in $ZO_SIGNING_KEY (default: esp32/signing_key.pem, git-ignored;
# make one, with ESP-IDF activated: `espsecure generate-signing-key --version 2 --scheme rsa3072 signing_key.pem`).
# A gateway running signed firmware installs only updates signed with the same key.
#
#   ./release.sh                 sign if the key is there, else build unsigned (with a warning)
#   ./release.sh --tag v0.3.0    a release: the key is required and the tag must be v<version.txt>
set -eu
cd "$(dirname "$0")"
# (EIM's activation script makes idf.py a shell function, which scripts don't inherit)
[ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH not set: activate ESP-IDF first (e.g. . ~/.espressif/tools/activate_idf_v6.1.1.sh)"; exit 1; }
PY="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/}python"
idf() { "$PY" "$IDF_PATH/tools/idf.py" "$@"; }
VER=$(cat version.txt)
NAME="zivyobraz-ble-gateway-esp32-$VER"
APP=build/zivyobraz-ble-gateway.bin
KEY="${ZO_SIGNING_KEY:-signing_key.pem}"

TAG=""
if [ "${1:-}" = "--tag" ]; then
    TAG="${2:-}"
    [ "$TAG" = "v$VER" ] || { echo "tag '$TAG' doesn't match version.txt ($VER): expected v$VER"; exit 1; }
    [ -s "$KEY" ] || { echo "a release must be signed, but there is no key at $KEY"; exit 1; }
fi

idf build

# sign before merging, so the full image carries the signed app too
if [ -s "$KEY" ]; then
    "$PY" -m espsecure sign-data --version 2 --keyfile "$KEY" --output "$APP.signed" "$APP"
    mv "$APP.signed" "$APP"
    "$PY" -m espsecure verify-signature --version 2 --keyfile "$KEY" "$APP"
    SIGNED=yes
else
    SIGNED=no
fi
idf merge-bin -f raw -o "$PWD/build/$NAME-full.bin"

rm -rf dist
mkdir dist
cp "build/$NAME-full.bin" "dist/$NAME-full.bin"
cp "$APP" "dist/$NAME-ota.bin"
printf '{"project": "zivyobraz-ble-gateway", "version": "%s", "tag": "v%s", "file": "%s"}\n' \
    "$VER" "$VER" "$NAME-ota.bin" > dist/latest.json
(cd dist && sha256sum *.bin latest.json > SHA256SUMS)
ls -l dist
echo
if [ "$SIGNED" = no ]; then
    echo "WARNING: UNSIGNED build (no key at $KEY). Gateways running signed firmware refuse it as an update,"
    echo "         and this firmware itself can't verify (so refuses) any update. Fine for USB development only."
    echo
fi
echo "First install (USB):  esptool --chip esp32 write-flash 0x0 dist/$NAME-full.bin"
echo "Update:               GitHub release v$VER with these files, or web page -> gateway settings -> upload dist/$NAME-ota.bin"
