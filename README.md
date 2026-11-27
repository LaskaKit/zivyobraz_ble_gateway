# Živý obraz BLE Gateway

Shows [zivyobraz.eu](https://zivyobraz.eu) content on cheap Bluetooth LE e-paper price tags, driven over
their stock BLE protocol (no tag firmware is flashed). The tags never talk to the server: the gateway asks
zivyobraz.eu on each tag's behalf and pushes the picture over Bluetooth when it changes.

Supported: **Picksmart / Gicisky labels** (BLE name `NEMRxxxxxxxx`, manufacturer id `0x5053`). Supported are the panels tested on real tags: 2.13" BWRY, 2.9" BWRY, 2.9"
black/white TFT, 4.2" BWR and 7.5" BWR (list in [PROTOCOL.md](PROTOCOL.md#2-supported-panels)).

Two gateways side by side, doing the same job:

| | [`python/`](python) | [`esp32/`](esp32) |
|---|---|---|
| runs on | Linux PC or Raspberry Pi (BlueZ) | LaskaKit ESPlan (ESP32 + Ethernet, PoE optional) |
| network | whatever the computer has | Ethernet port only; the radio does BLE only |
| extras | `picksmart_tool.py`: test patterns, text, images, layout probing | binaries to flash, firmware update from the web page |
| docs | [python/README.md](python/README.md) | [esp32/README.md](esp32/README.md) |

Both serve the same web page (look shared in [`web/`](web)) and use the same `tags.json`, so a tag list
moves from one to the other with its zivyobraz keys. Run only one of them for a given set of tags.

## How long an update takes

Measured 2026-11-27 on a Linux PC (BlueZ), from the first data block to the tag's "got it" (`05 08`).
The panel then refreshes on its own. Details and the measurements behind each value:
[PROTOCOL.md §4.3](PROTOCOL.md#43-block-pacing-streaming-firmware-sw--0x80).

| display (SDK type) | data | pause after each block | sending |
|---|---|---|---|
| 2.13" BWRY (46) | 9.5 kB, 40 blocks | 5 ms, and every block waits for the tag's write confirmation | ~18 s |
| 2.9" BWRY (270) | 8 kB, 34 blocks | 5 ms | ~0.8 s |
| 2.9" TFT (160) | 8 kB, 35 blocks | 5 ms | ~0.8 s |
| 4.2" BWR (75) | 30 kB, 125 blocks | 5 ms | ~1.8 s |
| 7.5" BWR (299) | 100 kB, 419 blocks | 20 ms, and the last block waits for confirmation | ~18 s |

The pauses are the smallest at which each tag takes data as fast as it can. Shorter ones
don't make the transfer faster (the tag just catches up afterwards). Up to 2026-11-27 they were 40 ms, and
100 ms on the 7.5", which made those transfers 3× and 2.5× slower.

**Finding and connecting to a tag usually takes longer than sending**: 5–30 s, sometimes a minute when
attempts fail. Part of that is the tag (it advertises about once a second). Part is the PC: on a
combined WiFi + Bluetooth chip (the test PC's MediaTek MT7922) most of the tag's adverts are missed.

## Layout

| path | content |
|---|---|
| `PROTOCOL.md` | the Picksmart protocol spec and supported panels (both gateways are written from it) |
| `web/` | the web page's `style.css`, used by both gateways |
| `python/` | Python gateway: `zivyobraz_ble_gateway/` (run with `python -m zivyobraz_ble_gateway`), `picksmart/`, `picksmart_tool.py`, tools, tests |
| `esp32/` | ESP-IDF firmware: `main/`, `components/`, PC tests against the Python code, `release.sh` |
| `.github/workflows/esp32.yml` | CI: tests, firmware build, release binaries on `v*` tags |
| `captures/` | (git-ignored) previews, logs, captures |

A Home Assistant integration may come later; it would sit next to these as `homeassistant/`.

## Credits

- **dratek-eink-homeassistant** (DRATEK's Home Assistant integration for these tags): our understanding
  of the Picksmart BLE protocol started from that project. Thank you!
- **[zivyobraz-fw](https://github.com/MultiTricker/zivyobraz-fw)** by MultiTricker: the zivyobraz.eu
  request format and the Z image colour indices the gateways follow.

The Picksmart code in both gateways (`python/picksmart/`, `esp32/components/picksmart*/`) is our own,
written from [PROTOCOL.md](PROTOCOL.md) and tested on our tags. No code was copied from the projects above.

## License

[MIT](LICENSE)
