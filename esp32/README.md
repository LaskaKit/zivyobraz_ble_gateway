# ESP32 gateway (LaskaKit ESPlan)

The Živý obraz BLE Gateway as ESP-IDF firmware, for the
[LaskaKit ESPlan](https://github.com/LaskaKit/ESPlan) (ESP32-WROOM-32E 16 MB, LAN8720A Ethernet,
optional PoE). It does what the [Python gateway](../python) does: it polls zivyobraz.eu for
every registered tag and sends changed pictures to the tags over Bluetooth LE. Same web page,
same `tags.json`.

* **Network: the Ethernet port only.** Wi-Fi is never started, so the 2.4 GHz radio does Bluetooth only.
  The gateway uses DHCP and answers at **http://zo-ble-gateway.local/** (mDNS) or at its IP address.
* **Status LED** (the SK6812 on IO0): red = no network, green = running, blue = sending to a tag.
* **Firmware updates** come from the project's GitHub releases (*gateway settings* → *Install*) or a file
  upload. Only firmware signed with the project's key is installed, and a new firmware that doesn't start
  rolls back to the old one. See [Updates](#updates).

## Install a release

A release has these files (`./release.sh` makes them, see below):

| file | use |
|---|---|
| `zivyobraz-ble-gateway-esp32-<version>-full.bin` | first install over USB: the whole flash, written at `0x0` |
| `zivyobraz-ble-gateway-esp32-<version>-ota.bin` | updates: the gateway downloads it, or upload it on *gateway settings* |
| `latest.json` | what the gateways' update check reads |

The image is for the **16 MB** ESPlan. A change of the partition table (this one included) needs a
full USB flash; OTA only replaces the running app slot.

First install, with the board on USB-C ([esptool](https://docs.espressif.com/projects/esptool/)):

```bash
pip install esptool
esptool --chip esp32 write-flash 0x0 zivyobraz-ble-gateway-esp32-0.2.0-full.bin
```

Any browser-based ESP flasher (for example [ESP Web Tools](https://esphome.github.io/esp-web-tools/) or
<https://espressif.github.io/esptool-js/>) works too: same file, offset `0x0`.

Plug in the Ethernet cable and open http://zo-ble-gateway.local/. Then add tags as described in
[`python/README.md`](../python/README.md#živý-obraz-ble-gateway-python--m-zivyobraz_ble_gateway): click **Add**, and register the
tag on zivyobraz.eu with the MAC and the **zivyobraz key** it shows.

**Moving over from the Python gateway:** on *gateway settings → Tag list backup*, **Restore** the
Python gateway's `python/data/tags.json`. The keys stay the same, so zivyobraz.eu needs no changes.
Stop the Python gateway first. **Download tags.json** on the same page makes a backup; keep one.

## Web page

Same as the Python one (look shared in [`../web`](../web)), plus **gateway settings** (link in the footer):

* poll limits (default 60 s … 24 h; between them the server's `PreciseSleep` decides)
* tag list backup and restore (`tags.json`, compatible with the Python gateway)
* firmware version, network, uptime, free memory; firmware update

There is no password: like the Python page, it is meant for a home LAN. Other web sites can't use your
browser against it, though: the page answers only to its IP address and `zo-ble-gateway` (alone or with
`.local`, `.lan`, `.home`, `.home.arpa`, `.localdomain`, `.internal`), its buttons work only from the page
itself, and the two uploads (tags.json, firmware) also need an `X-ZO-Upload` header that the page sends. To
update from a script:

```bash
curl -H 'X-ZO-Upload: 1' --data-binary @zivyobraz-ble-gateway-esp32-<version>-ota.bin http://zo-ble-gateway.local/update
```

Turn off the web update in `menuconfig` (`ZO_WEB_OTA`) if anyone on your LAN shouldn't be able to replace
the firmware. See [`SECURITY.md`](SECURITY.md) for the security review and the OTA options.

## Updates

*Gateway settings → Firmware update* shows the newest release of the GitHub repository set in `menuconfig`
(`ZO_OTA_REPO`, *owner/name*). The gateway looks every `ZO_OTA_CHECK_HOURS` (24, the first time 5 min after
start) or when you press **Check now**. A newer version is announced in the log and in the page footer.
Nothing is installed until you press **Install**: the gateway downloads the `-ota.bin` of that release from
GitHub, between two polls, and restarts into it.

**Signed firmware.** Release builds carry an RSA-3072 signature (ESP-IDF "signed app images without
secure boot"; no eFuses are burned). A gateway installs an update, whether from GitHub or uploaded, only if
it is signed with **the same key as the firmware it is running**. So someone who can upload to the
gateway or publish on the GitHub repository still can't install their own firmware without the key.

* The key is `esp32/signing_key.pem` (git-ignored) or the file in `$ZO_SIGNING_KEY`. Make it once.
  `espsecure` is part of esptool and comes with ESP-IDF, so activate ESP-IDF first (or `pip install esptool`):
  ```bash
  . ~/.espressif/tools/activate_idf_v6.1.1.sh
  cd esp32
  espsecure generate-signing-key --version 2 --scheme rsa3072 signing_key.pem
  ``` For releases built by CI
  it is also the repository secret **`ZO_SIGNING_KEY`** (the whole PEM text), given to tag builds only.
* **Keep a backup offline. If the key is lost, the gateways can be updated only over USB.** If it leaks,
  make a new one and reflash every gateway over USB.
* A development build (`idf.py build`/`flash`) is unsigned: it runs fine, but it can't verify anything, so it
  refuses every update (the settings page says so). Flash a release's `-full.bin` to get back to updates.
* Coming from 0.2.0 (unsigned, no signature check): upload the first signed release's `-ota.bin` on the
  settings page once. From then on only signed updates are accepted.
* Signing needs chip revision 3 or later (every ESP32-WROOM-32E). An older chip refuses to install the update
  and keeps the old firmware.

**Publishing a release:** bump `version.txt`, commit, then tag `v<version>` and push the tag. CI signs the
binaries and attaches them and `latest.json` to the GitHub release; the gateways see it at their next
check. Locally, `./release.sh --tag v<version>` does the same into `dist/` and fails without the key.

## Limits compared with the Python gateway

* **No PSRAM on the ESPlan, so one picture at a time.** The zivyobraz body is saved to LittleFS first
  (that file is the preview), then decoded into the packed payload and sent. Small panels keep the
  payload in RAM. If the heap can't hold it (7.5" 800x480 BWR is 96 kB), it goes to `/data/work.img`
  with an 8 kB RAM cache (two 4 kB pages). Up to 32 tags.
* Previews are the zivyobraz body kept in the ~10 MB `storage` partition (Z2/Z3, drawn as BMP). Gateway settings shows RAM (free / largest block / lowest) and how full that partition is.

## Build

ESP-IDF **v6.1** (built with v6.1.1; CI uses the same version):

```bash
. ~/.espressif/tools/activate_idf_v6.1.1.sh    # EIM install; with a git checkout use its export.sh
cd esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Since v6.1, `idf.py flash` rewrites only what changed since the last flash. On a new or erased board use
`idf.py -p /dev/ttyUSB0 flash --all`.

`idf.py menuconfig` → *Zivy obraz BLE Gateway* holds the host name, the server URL, the time zone,
the default poll limits, the web update switch, the release repository and update check interval, and the board pins (ESPlan defaults: MDC 23, MDIO 18,
PHY reset 5, RMII clock out on 17, LED on 0). Older ESPlan boards (v2.0 and earlier) had the LED on IO35,
an input-only pin: turn the LED off there.

**Binaries to hand out:** `./release.sh` builds, signs (see [Updates](#updates); without a key it
warns and leaves them unsigned), and writes `dist/zivyobraz-ble-gateway-esp32-<version>-{full,ota}.bin`,
`latest.json` and `SHA256SUMS`. The version is in `version.txt`. The GitHub workflow
[`.github/workflows/esp32.yml`](../.github/workflows/esp32.yml) runs the tests, builds, and on a `v*` tag
signs and attaches the files to a GitHub release.

## Tests

`test/host/run.sh` builds the portable C code (panel types, colour mapping, payload encoding,
stored-block framing, Z2/Z3 decoding, the request JSON, the web guard, version numbers) with the PC's compiler. It checks the
result **byte for byte** against payloads made by the Python gateway's own code (`gen_vectors.py`): each of
the 5 supported panel types, Z1/Z2/Z3 input, with and without rotation and framing. It needs `python/.venv`.

```bash
esp32/test/host/run.sh       # "30 image vectors, 3 requests, all OK"
```

The BLE transfer (`components/picksmart_ble`) is written from PROTOCOL.md §3–§4 and follows
`python/picksmart/client.py` step by step (same log lines). It can only be tested with real tags.

## Layout

| path | content |
|---|---|
| `main/main.c` | start-up: storage, Ethernet, web server, BLE, NTP, mDNS, firmware rollback check |
| `main/gateway.c` | tag list (`/data/tags.json`), poll scheduler, transfers, log (port of `service.py` + `registry.py`) |
| `main/web.c` | web page, settings, backup/restore, firmware update, previews (port of `web.py`) |
| `main/guard.c` | Host / Origin checks and import validation for the web page (portable, host-tested) |
| `main/update.c` | update check and install from GitHub releases (`latest.json`, `esp_https_ota`) |
| `main/version.c` | version number comparison (portable, host-tested) |
| `main/eth.c` | ESPlan Ethernet: internal EMAC + LAN8720A, RMII clock out on GPIO17 |
| `main/status_led.c` | SK6812 status LED |
| `main/Kconfig.projbuild` | `menuconfig` options |
| `components/picksmart/` | the 5 panel types, advert parsing, paged payload + framing (portable C, from PROTOCOL.md) |
| `components/picksmart_ble/` | NimBLE scanner and transfer (from PROTOCOL.md §3–§4, mirrors `client.py`) |
| `components/zivyobraz/` | request, answer headers, streaming Z2/Z3 decoder (portable), HTTPS client |
| `partitions.csv` | 2 × 3 MB app slots, ~10 MB LittleFS (16 MB flash) |
| `sdkconfig.defaults` | BLE-only NimBLE central, Ethernet, small-RAM TLS, signed app images |
| `test/host/` | PC tests against the Python gateway |
| `release.sh` | builds and signs the binaries to hand out, and `latest.json` |
