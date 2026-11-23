# Python gateway (PC / Raspberry Pi)

The Python version of the gateway: a command-line tool for the tags, and the always-on
Živý obraz BLE Gateway with a web page. It runs on Linux with BlueZ. The ESP32 version in
[`../esp32`](../esp32) does the same job on a LaskaKit ESPlan board.

All commands below run from this directory (`python/`).

## Requirements

* Linux with BlueZ and a Bluetooth LE adapter (any recent laptop works)
* Python 3.11+

```bash
cd python
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # or: uv pip install --python .venv/bin/python -r requirements.txt
```

## Test tool (`picksmart_tool.py`)

```bash
# list Picksmart tags in range: model, battery, firmware, framing
.venv/bin/python picksmart_tool.py scan

# test pattern (timestamp, colour bars, triangle in the top-right corner)
.venv/bin/python picksmart_tool.py test FF:FF:92:85:05:77
.venv/bin/python picksmart_tool.py test-all            # every tag in range, one after another

# text, auto-sized and centred; use \n for line breaks
.venv/bin/python picksmart_tool.py text FF:FF:92:81:34:40 "Hello\nworld" --colour red

# any image; it is resized to the panel and reduced to its colours
.venv/bin/python picksmart_tool.py image FF:FF:92:83:09:53 photo.png --preview ../captures/preview.png
```

Options:
* `-v` (before the sub-command) logs every BLE packet.
* `--sdk-type N` overrides the display type taken from the advertisement.
* `--preview FILE` saves the colour-reduced image the tag will roughly show.
* `test --pattern ruler` draws numbered rulers and a checkerboard, for checking scale and cropping.
* `--layout SPEC` overrides the pixel layout. It is meant for working out a new panel (PROTOCOL.md §6). SPEC is
  a comma-separated list of: `r0|r90|r180|r270` (rotate counter-clockwise) and `lr|tb` (mirror), applied in
  the order given; then the packing: two bit planes by default (A = white, B = red), `bw` (plane A only),
  `inv` (invert plane A), `2bpp` (the §5.3 two bits per pixel, 4 px/byte), `pairs` (the §5.4 row-pair
  interleave), and `pad` (start every row on a whole byte). `r90,2bpp` is the SDK 46/270 encoding,
  `tb,inv` that of SDK 299. `test` prints the layout name on the panel, so you can tell the variants apart.

Only the panel types in PROTOCOL.md §2 are sent to; `scan` shows any other type as `unknown type N`.
Colours: every pixel becomes the nearest of white, black, red and yellow, then what the panel can show
(yellow → red on BWR, red/yellow → black on BW). A transfer takes 2–45 s depending on panel size. The
tag redraws its screen after `05 08: tag has the image, refreshing` (with `-v`; some tags never send it,
the transfer then still counts as done).

### Use as a library

```python
import asyncio
from PIL import Image
from picksmart import PicksmartClient, scan, pack_image, display_size

async def main(address):
    adv = (await scan(15, address))[address]
    image = Image.new("RGB", display_size(adv.sdk_type), "white")
    # ... draw on image ...
    payload = pack_image(adv.sdk_type, image)
    async with PicksmartClient(address) as tag:
        await tag.send_payload(payload, sdk_type=adv.sdk_type, sw=adv.sw, raw_planes=adv.raw_planes)

asyncio.run(main("FF:FF:92:85:05:77"))
```

## Živý obraz BLE Gateway (`python -m zivyobraz_ble_gateway`)

`zivyobraz_ble_gateway` is an always-on gateway that shows [zivyobraz.eu](https://zivyobraz.eu) content on
the Picksmart tags.

The tags never call anybody; they only advertise. So the gateway does what a Wi-Fi zivyobraz
display does after waking up, on each tag's behalf:

1. It POSTs the zivyobraz FW 3.0 request. The tag's Bluetooth address is the MAC, and the
   gateway generates an 8-digit API key per tag (the "zivyobraz key"). The request also carries
   the resolution, the colour type (`BW` / `3C` / `4C`) and the battery voltage from the advertisement.
2. It asks again after `PreciseSleep` seconds, clamped to `--min-interval` / `--max-interval`.
3. Only when the `Timestamp` header changes does it decode the image (Z2/Z3, `Rotate` honoured)
   and send it over BLE. Unchanged content costs the tag no connection. A failed transfer is retried
   after 1, 2, 4 … 30 min.

```bash
.venv/bin/python -m zivyobraz_ble_gateway   # then open http://<host>:8080/
```

| Option | Default | What it does |
|---|---|---|
| `--port N` | 8080 | web page port |
| `--data DIR` | `python/data` | `tags.json` (the tag list) and `previews/` |
| `--min-interval S` | 60 | ask zivyobraz.eu at most every S seconds per tag |
| `--max-interval S` | 86400 | … and at least every S seconds |
| `--url URL` | zivyobraz.eu | server endpoint |
| `--allow-host NAME` | – | another name the page is opened by (repeatable, e.g. behind a reverse proxy) |
| `-v` | off | also log the BLE transfer steps |

**The web page** lists:
* **Tags**: each tag with its MAC, its **zivyobraz key**, a preview of the last image, battery, signal,
  the last server answer and transfer, and **Refresh now** (fetch and send even if unchanged) / **Remove**.
* **In range, not added**: Picksmart tags the scanner hears, each with an **Add** button.
* **Log**: newest first.

There is no password, but the page answers only when it is opened by an IP address, `localhost`, or this
machine's own name (alone or with `.local`, `.lan`, `.home`, `.home.arpa`, `.localdomain`, `.internal`),
and its buttons work only from the page itself. That stops other web sites (cross-site forms, DNS
rebinding) from driving the gateway through your browser. Another name gets `403`: add it with `--allow-host`.

Its look (`style.css`) lives in [`../web`](../web) and is shared with the ESP32 gateway.

**Adding a tag:** click **Add**. The first server answer is zivyobraz's "ePaper není registrovaný" screen
with the MAC and key, and it goes straight onto the tag. Register the display on zivyobraz.eu with that MAC
and key. Your content then appears at the next poll, or click **Refresh now**.

Keep `data/tags.json`: the keys in it are what zivyobraz.eu knows the tags by. The ESP32 gateway can
import this file (its page → gateway settings → Tag list backup → Restore), so moving over keeps the keys.
Don't run `picksmart_tool.py` transfers at the same time: one adapter handles one connection at a time.
Don't run the Python and the ESP32 gateway for the same tags at once either.

**Start at boot:** `zivyobraz-ble-gateway.service` is a systemd unit (adjust `User=` and the paths), see the comments in it.

Tests (no Bluetooth or network needed): `.venv/bin/python -m unittest discover tests`

## Exploring unknown tags

`tools/ble_explore.py` is read-only. It never writes to a tag.

```bash
.venv/bin/python tools/ble_explore.py scan               # all BLE advertisements
.venv/bin/python tools/ble_explore.py gatt   <address>   # services, characteristics, values
.venv/bin/python tools/ble_explore.py listen <address>   # print all notifications
```

## Layout

| path | content |
|---|---|
| `picksmart_tool.py` | test tool: scan, test patterns, text, pictures, layout probing |
| `picksmart/` | the Picksmart protocol, written from [`../PROTOCOL.md`](../PROTOCOL.md) (section numbers in comments) |
| `picksmart/models.py` | the 5 supported panel types: canvas, colours, encoding, pacing, label (one table) |
| `picksmart/advert.py` | advertisement parsing (§1), `scan()` |
| `picksmart/encode.py` | canvas of colour indices → payload (§5.2–§5.4), stored-block framing (§5.5) |
| `picksmart/imaging.py` | Pillow image → colour indices (nearest colour), previews, manual `Layout` |
| `picksmart/client.py` | `Transfer` state machine (§4, runs against any `Link`) and the bleak `PicksmartClient` |
| `tools/ble_explore.py` | generic read-only BLE explorer |
| `zivyobraz_ble_gateway/__main__.py` | the gateway's entry point (web page + scheduler + BLE worker) |
| `zivyobraz_ble_gateway/zivyobraz.py` | zivyobraz.eu request / answer / Z2/Z3 decoding |
| `zivyobraz_ble_gateway/service.py` | poll scheduler and BLE worker |
| `zivyobraz_ble_gateway/registry.py` | tag list (`data/tags.json`) |
| `zivyobraz_ble_gateway/web.py` | web page (look in `../web/`) |
| `tests/` | offline tests |
