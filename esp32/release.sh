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
# With the key, the build uses sdkconfig.defaults + sdkconfig.release (signature check on update) in
# build-release/. Without it, a plain unsigned dev build in build-unsigned/ (it boots, but refuses updates):
# ESP-IDF won't boot an unsigned app that has the signature check on.
# Both start from the defaults alone (their own sdkconfig), so a local menuconfig never leaks into a release.
#
#   ./release.sh                 sign if the key is there, else build unsigned (with a warning)
#   ./release.sh --tag v0.3.0    a release: the key is required and the tag must be v<version.txt>
#   --out DIR                    write the files to DIR instead of dist/ (DIR is emptied first)
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
OUT=dist
while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="${2:-}"; shift 2 ;;
        --out) OUT="${2:?--out needs a directory}"; shift 2 ;;
        *) echo "unknown option $1"; exit 1 ;;
    esac
done
if [ -n "$TAG" ]; then
    [ "$TAG" = "v$VER" ] || { echo "tag '$TAG' doesn't match version.txt ($VER): expected v$VER"; exit 1; }
    [ -s "$KEY" ] || { echo "a release must be signed, but there is no key at $KEY"; exit 1; }
fi

if [ -s "$KEY" ]; then
    SIGNED=yes
    BUILD=build-release
    DEFAULTS="sdkconfig.defaults;sdkconfig.release"
else
    SIGNED=no
    BUILD=build-unsigned
    DEFAULTS="sdkconfig.defaults"
fi
APP="$BUILD/zivyobraz-ble-gateway.bin"
build() { idf -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -D SDKCONFIG_DEFAULTS="$DEFAULTS" "$@"; }

build build

# sign before merging, so the full image carries the signed app too
if [ "$SIGNED" = yes ]; then
    "$PY" -m espsecure sign-data --version 2 --keyfile "$KEY" --output "$APP.signed" "$APP"
    mv "$APP.signed" "$APP"
    "$PY" -m espsecure verify-signature --version 2 --keyfile "$KEY" "$APP"
fi
build merge-bin -f raw -o "$PWD/$BUILD/$NAME-full.bin"

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/$NAME-full.bin" "$OUT/$NAME-full.bin"
cp "$APP" "$OUT/$NAME-ota.bin"
printf '{"project": "zivyobraz-ble-gateway", "version": "%s", "tag": "v%s", "file": "%s"}\n' \
    "$VER" "$VER" "$NAME-ota.bin" > "$OUT/latest.json"
(cd "$OUT" && sha256sum *.bin latest.json > SHA256SUMS)
ls -l "$OUT"
echo
if [ "$SIGNED" = no ]; then
    echo "WARNING: UNSIGNED build (no key at $KEY). Gateways running signed firmware refuse it as an update,"
    echo "         and this firmware itself can't verify (so refuses) any update. Fine for USB development only."
    echo
fi
echo "First install (USB):  esptool --chip esp32 write-flash 0x0 $OUT/$NAME-full.bin"
echo "Update:               GitHub release v$VER with these files, or web page -> gateway settings -> upload $OUT/$NAME-ota.bin"
