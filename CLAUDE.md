# Agent index: Živý obraz BLE Gateway

Python gateway for BLE e-paper price tags, run from Linux (BlueZ + bleak).
The user doesn't want to flash the tags; everything goes over the stock BLE protocol.
Long term goal: their own gateway, Python first.

## Read first
- `README.md`: usage, CLI, layout
- `PROTOCOL.md`: **the spec** the Picksmart code is written from: protocol, supported panels, encodings,
  pacing and timeouts. Update it whenever you learn something about a tag.

## Code map
- `picksmart_tool.py`: test tool (`scan`, `test`, `test-all`, `text`, `image`; `-v` = packet log)
(Python paths are under `python/`; comments cite PROTOCOL.md sections, e.g. `§4.3`.)
- `picksmart/models.py`: **the one table** `MODELS` of the 5 supported types (canvas, colours, encoding,
  `Pacing`, label); `BW_TYPES`/`BWRY_TYPES` are derived from it. Anything else is "unknown".
- `picksmart/advert.py`: §1 manufacturer data 0x5053 parsing (type, battery, sw, hw), `scan()`
- `picksmart/encode.py`: canvas of colour indices (0 white, 1 black, 2 red, 3 yellow) → payload
  (§5.1 panel rules, §5.2 planes, §5.3 2 bpp, §5.4 row pairs) and `frame()` (§5.5 stored blocks)
- `picksmart/imaging.py`: Pillow image → indices by nearest colour, `pack_image()`, `quantize_preview()`,
  the manual `Layout` for `--layout`
- `picksmart/client.py`: `Transfer` (the §4 state machine, runs against any `Link`; tests use a fake tag)
  and `PicksmartClient` (bleak: scan, connect retries, MTU, FEF1/FEF2, notifications)
- `tools/ble_explore.py`: read-only scan/GATT dump/notification listener
- ESP32: `esp32/components/picksmart/` (models, advert, paged image + framing; portable C, host-tested
  byte for byte against the Python by `esp32/test/host/run.sh`) and `picksmart_ble/` (NimBLE scanner and
  transfer; callbacks only fill queues, `psb_send()` runs in the gateway task).
  `esp32/SECURITY.md`: security review (open findings, signed OTA, …) and the OTA options.
  `esp32/main/guard.c` (host-tested) = `web.py` `host_ok`/`origin_ok`: every request must name our host
  (DNS rebinding), every POST must come from our page (CSRF), `/update` + `/import` also need `X-ZO-Upload`.
  Keep the two in sync (same test cases in `test_host.c` and `tests/test_gateway.py` `Guard`).
- **ESP32 firmware updates (2026-11-23, user's decision: GitHub releases + signatures, install on click):**
  `esp32/main/update.c` reads `releases/latest/download/latest.json` of `ZO_OTA_REPO` (menuconfig, empty by
  default), and *Install* pulls the `-ota.bin` with `esp_https_ota`. Apps are RSA-signed without secure boot
  (`sdkconfig.defaults`). `release.sh` signs with `signing_key.pem` / `$ZO_SIGNING_KEY`, and CI with the
  `ZO_SIGNING_KEY` secret on tags. An unsigned dev build refuses all updates. Never commit a key, and never
  sign `dist/` with a test key.

## The gateway (`zivyobraz_ble_gateway/`, run `python -m zivyobraz_ble_gateway`; 2026-11-20)
An always-on gateway that shows zivyobraz.eu content on the Picksmart tags. The Python (PC/RPi) version is
done; the **ESP32-S3 port comes next**, so keep the logic plain and portable (no JS on the page, JSON
registry, plain forms). The models were `../zo_gateway` (web page look, `www/style.css` copied)
and `../zivyobraz-fw` (the request format, `src/http_client.cpp`; colour indices, `src/image_handler.cpp`).
- **Tags don't call the gateway; the gateway polls zivyobraz.eu per tag** as a FW 3.0 device:
  `network.mac` = the tag's BLE address, `X-API-Key` = an 8-digit key generated on Add (stored in
  `data/tags.json`; losing it orphans the tag on zivyobraz.eu), `display` = `display_size()` + colorType
  `BW`/`3C`/`4C`, `type` = `Picksmart-<sdk>`, board `Picksmart_BLE`.
- Next poll = `PreciseSleep` (else `Sleep`×60) clamped to `--min-interval`/`--max-interval`. BLE only if the
  `Timestamp` header differs from the one stored after the last *successful* transfer (or Refresh now).
  The image body is in the same response. `Rotate` → 180°. Z index 0 white, 1 black, 2 red, 3 yellow.
  **Only Z2/Z3 are supported** (user's decisions: 2026-11-20 zivyobraz.eu doesn't send PNG, 2026-11-23 Z1 isn't
  used anymore; those parts of the zivyobraz-fw spec are outdated). Anything else is rejected as an unsupported format.
- `zivyobraz_ble_gateway/zivyobraz.py` (no BLE, no network state): request, answer parsing, Z2/Z3 → RGB image in
  the RED/YELLOW of `imaging.py` (packing maps every pixel to the nearest of the four colours). `service.py`: `poller()` (HTTP in a thread) +
  `ble_worker()` (continuous BleakScanner, **stopped during a transfer** because `PicksmartClient` scans
  itself). `registry.py`, `web.py` (aiohttp, port 8080). Tests: `python -m unittest discover tests`.
- Unregistered MAC → the server returns a Z2 "ePaper není registrovaný" screen with the MAC and key; it goes
  onto the tag, so the user pairs on zivyobraz.eu from what the tag shows.
- Verified 2026-11-20: Add of `…34:40` (4.2" BWR) → server Z2 5.5 kB → on the tag in 17 s.
- Ideas not done: portrait orientation per tag, per-tag interval override, auth on the page.

**Inch labels** in `models.py` are ours, not the SDK's. At the user's request (2026-11-20) SDK 46 (296x128)
is labelled 2.13" and SDK 270 plus the TFT (160) 2.9", matching their physical tags. Display text only;
don't "fix" it back to the usual panel naming.

## Scope and provenance (2026-11-20, user's decision)
- **Strictly a Živý obraz gateway for Picksmart `NEMR…` tags.** Only the panel types in PROTOCOL.md §2
  (SDK 46, 75, 160, 270, 299) are supported; add a type only after testing it on a real tag.
  Other tag brands (e.g. hoootag `EPD-…`) are out of scope; don't bring them back.
- **Clean-room rule:** the Picksmart code (Python and C) was rewritten from `PROTOCOL.md` alone (2026-11-20);
  spec gaps are marked `SPEC-GAP` in the code. Don't read, clone
  or fetch other Picksmart/Gicisky implementations (dratek-eink-homeassistant, hass-gicisky, gicisky,
  ATC_GICISKY_ESL, ha-bleep, …), and don't copy code from them. New facts come from our own tags.

## Working out an unknown panel layout
`picksmart_tool.py test <addr> --layout <spec> [--pattern ruler]` sends a pattern labelled with its layout.
The user reports what they see; record it in PROTOCOL.md §2/§5 first, then add the type to `MODELS`
(and an encoding to `encode.py` plus the C `picksmart_image.c` if none fits), and extend the host vectors.
SDK 160 (2.9" TFT) was worked out this way: `r270,lr,pairs` (= a transpose) on a 250x122 canvas padded to 132 rows.

## Running
- `python/.venv/bin/python …` (Python 3.14 venv, bleak 3.x, Pillow; CI runs 3.12, so stay compatible). Install with `pip install -r requirements.txt`.
- Real tags are in range of this PC's adapter. Sending a test image is harmless and the
  user has OK'd it. Run tags **sequentially**: a single adapter handles one transfer at a time well.
- BlueZ: MTU is negotiated through bleak's private `_backend._acquire_mtu()` (gives 247, so block size 244).
  Some tags miss the first connect (the TFT `…98:22` often needs 2–3 tries); the client retries 5×.
- Use `captures/` (git-ignored) for previews and logs.

## Conventions
- License: MIT (`LICENSE`, holder "Živý obraz BLE Gateway contributors"). Don't ship LaskaKit's
  logo (user's decision, 2026-11-23): the header is a styled "LaskaKit" text wordmark in `web/style.css`.
  Credits live in README.md (dratek-eink-homeassistant for the protocol, zivyobraz-fw for the request format).
- Keep the protocol notes in `PROTOCOL.md` in sync with the code.
- Don't commit unless asked. Don't add the user's personal details to anything.
