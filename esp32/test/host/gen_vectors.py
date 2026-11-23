#!/usr/bin/env python3
"""Golden test vectors for the ESP32 C code, made with the Python gateway's own code.

For every display type in python/picksmart/models.py: random Z2/Z3 pictures, packed exactly
as the Python gateway does it (zivyobraz_ble_gateway decode_image -> Rotate ->
pack_image -> the §5.5 stored-block framing when the tag lacks the raw flag).
test_host.c must produce the same bytes.

    python/.venv/bin/python esp32/test/host/gen_vectors.py OUT_DIR
"""

import json
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "python"))


from picksmart import pack_image  # noqa: E402
from picksmart.encode import frame  # noqa: E402
from picksmart.models import MODELS  # noqa: E402
from zivyobraz_ble_gateway import zivyobraz  # noqa: E402


def encode_z(kind: str, pixels: list[int]) -> bytes:
    limit = {"Z2": 63, "Z3": 31}[kind]
    out, i = bytearray(kind.encode()), 0
    while i < len(pixels):
        run = 1
        while i + run < len(pixels) and run < limit and pixels[i + run] == pixels[i]:
            run += 1
        if kind == "Z2":
            out.append(pixels[i] << 6 | run)
        else:
            out.append(pixels[i] << 5 | run)
        i += run
    return bytes(out)


def random_pixels(rng: random.Random, total: int, colours: int) -> list[int]:
    pixels: list[int] = []
    while len(pixels) < total:
        pixels += [rng.randrange(colours)] * rng.randrange(1, 90)
    return pixels[:total]


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1234)
    lines = []
    n = 0
    for sdk, model in sorted(MODELS.items()):
        w, h = model.width, model.height
        cases = [("Z2", False, True), ("Z2", True, False), ("Z2", False, False), ("Z2", True, True),
                 ("Z3", True, True), ("Z3", False, False)]
        for kind, rotate, framed in cases:
            # Z3 has 3-bit colours: include indices 4..7 (shown white) to cover that path.
            pixels = random_pixels(rng, w * h, 8 if kind == "Z3" else 4)
            if kind == "Z2" and not rotate:
                pixels = pixels[: w * h - 777]  # short stream: the rest must stay white
            body = encode_z(kind, pixels)
            image = zivyobraz.decode_image(body, sdk)
            if rotate:
                image = image.rotate(180)
            payload = pack_image(sdk, image)
            if framed:
                payload = frame(payload, sdk)
            (out / f"{n}.body").write_bytes(body)
            (out / f"{n}.payload").write_bytes(payload)
            lines.append(f"img {n} {sdk} {int(rotate)} {int(framed)}")
            n += 1

    for mac, sdk, dv, rssi in [("FF:FF:92:81:34:40", 75, 30, -61), ("FF:FF:92:85:05:77", 46, 29, None),
                               ("AA:BB:CC:DD:EE:FF", 299, None, -90)]:
        req = zivyobraz.build_request(mac, sdk, dv / 10 if dv else None, rssi)
        text = json.dumps(req, separators=(",", ":"))
        lines.append(f"req {mac} {sdk} {dv or 0} {rssi or 0} {text}")

    (out / "manifest.txt").write_text("\n".join(lines) + "\n")
    print(f"{n} image vectors in {out}")


if __name__ == "__main__":
    main()
