"""The zivyobraz.eu side: the gateway plays one FW 3.0 device per Picksmart tag.

zivyobraz.eu doesn't know about Bluetooth. For every registered tag the gateway sends the same
request a Wi-Fi zivyobraz display would send (see ../zivyobraz-fw `src/http_client.cpp`):

    POST https://cdn.zivyobraz.eu/index.php?timestampCheck=1
    X-API-Key: <8-digit key, generated once per tag by the gateway>
    {"apiVersion": "3.0", "network": {"mac": "<tag's BLE address>"}, "display": {...}, ...}

The answer: headers (`Timestamp` = content version, `PreciseSleep` = seconds until the next
request, `Rotate` present = upside down) and, in the same response, the image body:
Z2 / Z3 (run-length palette indices; the server sends nothing else). The image has no size header: it is
`width x height` as we announced in the request, row-major, runs continue across rows.

Nothing here touches Bluetooth, so it is easy to test and to port.
"""

from __future__ import annotations

import json
import random
import urllib.error
import urllib.request
from dataclasses import dataclass

from PIL import Image

from picksmart.imaging import BLACK, RED, WHITE, YELLOW
from picksmart.models import BW_TYPES, BWRY_TYPES, MODELS, display_size, label  # noqa: F401 (MODELS: used as zivyobraz.MODELS)

SERVER_URL = "https://cdn.zivyobraz.eu/index.php?timestampCheck=1"
API_VERSION = "3.0"
BOARD = "Picksmart_BLE"   # what zivyobraz.eu shows as the board
HTTP_TIMEOUT = 20         # seconds (the ESP firmware waits 10 s for the first byte)
BODY_MAX = 2_000_000      # a 960x640 Z2 is ~100 kB; anything this big is wrong

LIGHT_GRAY, DARK_GRAY = (170, 170, 170), (85, 85, 85)


def new_api_key() -> str:
    """An 8-digit key without a leading zero, like the zivyobraz firmware's `initializeAPIKey()`."""
    return str(random.SystemRandom().randrange(10_000_000, 100_000_000))


def color_type(sdk_type: int) -> str:
    """The zivyobraz `display.colorType` for a Picksmart panel (the firmware's COLOR_TYPE names)."""
    if sdk_type in BW_TYPES:
        return "BW"
    if sdk_type in BWRY_TYPES:
        return "4C"  # black, white, red, yellow
    return "3C"      # black, white, red


def palette(color: str) -> list[tuple[int, int, int]]:
    """Z-format colour index -> RGB, as the firmware's `mapColorValue()` (0 white, 1 black, 2, 3).

    `picksmart.pack_image` maps every pixel to the nearest of white/black/red/yellow, so these
    come back as indices 0..3; on a BW panel light grey becomes white and dark grey black (§5.1).
    """
    if color in ("BW", "GRAYSCALE"):
        return [WHITE, BLACK, LIGHT_GRAY, DARK_GRAY]
    return [WHITE, BLACK, RED, YELLOW]


def build_request(mac: str, sdk_type: int, battery_v: float | None, rssi: int | None) -> dict:
    """The FW 3.0 JSON body for one tag."""
    width, height = display_size(sdk_type)
    system: dict = {"resetReason": "deepsleep"}
    if battery_v:
        system["vccVoltage"] = round(battery_v, 2)
    network: dict = {"mac": mac.upper(), "ssid": "BLE"}
    if rssi is not None:
        network["rssi"] = rssi
    return {
        "apiVersion": API_VERSION,
        "fwVersion": API_VERSION,
        "board": BOARD,
        "system": system,
        "network": network,
        "display": {
            "type": f"Picksmart-{sdk_type}",
            "width": width,
            "height": height,
            "colorType": color_type(sdk_type),
        },
    }


@dataclass
class Answer:
    """What the server said, already parsed."""

    timestamp: str | None   # content version; None = the server sent none
    sleep: int | None       # seconds until the next request (PreciseSleep, else Sleep * 60)
    rotate: bool            # `Rotate` header present: show the image upside down
    body: bytes             # the image (may be empty)
    headers: dict[str, str]


def parse_answer(headers: list[tuple[str, str]], body: bytes) -> Answer:
    """Pick the zivyobraz headers out of an HTTP response (names case-insensitive)."""
    h = {name.lower(): value.strip() for name, value in headers}

    def integer(name: str) -> int | None:
        try:
            return int(h[name])
        except (KeyError, ValueError):
            return None

    sleep = integer("precisesleep")
    if sleep is None and integer("sleep") is not None:
        sleep = integer("sleep") * 60
    return Answer(
        timestamp=h.get("timestamp") or None,
        sleep=sleep,
        rotate="rotate" in h,
        body=body,
        headers=dict(headers),
    )


def fetch(url: str, api_key: str, request: dict) -> Answer:
    """POST the request and read the whole answer. Blocking: run it in a thread.

    Raises on network errors and on any status other than 200 (as the firmware does).
    """
    data = json.dumps(request, separators=(",", ":")).encode()
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/json", "X-API-Key": api_key, "Connection": "close"},
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            body = resp.read(BODY_MAX + 1)
            headers = resp.headers.items()
    except urllib.error.HTTPError as err:
        raise RuntimeError(f"server answered {err.code} {err.reason}") from None
    if len(body) > BODY_MAX:
        raise RuntimeError("image too big")
    return parse_answer(headers, body)


def image_format(body: bytes) -> str:
    """'Z2', 'Z3', 'none' or 'unknown', from the first two bytes."""
    if not body:
        return "none"
    if body[:2] in (b"Z2", b"Z3"):
        return body[:2].decode()
    return "unknown"


def decode_z(body: bytes, width: int, height: int) -> bytes:
    """Z2/Z3 -> `width * height` palette indices, row-major (as `image_handler.cpp`).

    Z2: colour = b >> 6, count = b & 0x3F.
    Z3: colour = b >> 5, count = b & 0x1F. Missing pixels stay 0 (white); extra runs are ignored.
    """
    kind = body[:2]
    stream = body[2:]
    if kind == b"Z2":
        runs = ((b >> 6, b & 0x3F) for b in stream)
    elif kind == b"Z3":
        runs = ((b >> 5, b & 0x1F) for b in stream)
    else:
        raise ValueError(f"not a Z image: {kind!r}")
    total = width * height
    out = bytearray(total)
    pos = 0
    for colour, count in runs:
        if pos >= total:
            break
        if colour:  # 0 = white is already there
            end = min(pos + count, total)
            out[pos:end] = bytes((colour,)) * (end - pos)
        pos += count
    return bytes(out)


def decode_image(body: bytes, sdk_type: int) -> Image.Image:
    """The server's image as an RGB picture of the tag's size, in the tag's colours."""
    width, height = display_size(sdk_type)
    kind = image_format(body)
    if kind not in ("Z2", "Z3"):
        raise ValueError(f"unsupported image format ({kind}, starts with {body[:4]!r})")
    pixels = decode_z(body, width, height)
    image = Image.frombytes("P", (width, height), pixels)
    colours = palette(color_type(sdk_type))
    flat = [c for rgb in colours for c in rgb]
    image.putpalette(flat + flat[:3] * (256 - len(colours)))  # unknown indices -> white
    return image.convert("RGB")


def model_text(sdk_type: int) -> str:
    return label(sdk_type)
