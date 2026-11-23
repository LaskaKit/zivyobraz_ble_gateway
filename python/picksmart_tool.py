#!/usr/bin/env python3
"""Picksmart test tool: scan for tags and send test patterns, text or pictures (no Živý obraz).

    python picksmart_tool.py scan
    python picksmart_tool.py text FF:FF:92:85:05:77 "Hello"
    python picksmart_tool.py image FF:FF:92:85:05:77 picture.png
    python picksmart_tool.py test FF:FF:92:85:05:77
    python picksmart_tool.py test-all
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import time

from PIL import Image, ImageDraw, ImageFont

from picksmart import PicksmartClient, display_size, pack_image, quantize_preview, scan
from picksmart.imaging import BLACK, RED, WHITE, YELLOW, Layout


def log(message: str) -> None:
    print(f"{time.strftime('%H:%M:%S')}  {message}", flush=True)


def font(size: int) -> ImageFont.ImageFont:
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def test_pattern(width: int, height: int, label: str | None = None) -> Image.Image:
    """Border, readable text (shows rotation/mirroring), colour bars, top-right triangle."""
    image = Image.new("RGB", (width, height), WHITE)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, width - 1, height - 1), outline=BLACK, width=3)
    bar = (width - 20) // 3
    for i, colour in enumerate((BLACK, RED, YELLOW)):
        draw.rectangle((10 + i * bar, height // 2, 10 + (i + 1) * bar - 4, height - 10), fill=colour)
    draw.text((10, 8), label or time.strftime("%H:%M:%S"), fill=BLACK, font=font(height // 4))
    draw.polygon([(width - 40, 10), (width - 10, 10), (width - 10, 40)], fill=BLACK)  # top-right marker
    return image


def ruler_pattern(width: int, height: int, label: str | None = None) -> Image.Image:
    """Numbered ticks every 25 px on both axes + 10 px checkerboard, to measure scale/crop."""
    image = Image.new("RGB", (width, height), WHITE)
    draw = ImageDraw.Draw(image)
    small = font(11)
    for x in range(0, width, 5):
        length = 14 if x % 25 == 0 else 5
        draw.line((x, 0, x, length), fill=RED if x % 50 == 0 else BLACK)
        if x % 50 == 0 and x:
            draw.text((x - 8, 15), str(x), fill=BLACK, font=small)
    for y in range(0, height, 5):
        length = 14 if y % 25 == 0 else 5
        draw.line((0, y, length, y), fill=RED if y % 50 == 0 else BLACK)
        if y % 50 == 0 and y:
            draw.text((16, y - 6), str(y), fill=BLACK, font=small)
    for cy in range(40, height - 10, 10):  # checkerboard block in the middle
        for cx in range(60, min(width - 10, 200), 10):
            if (cx // 10 + cy // 10) % 2:
                draw.rectangle((cx, cy, cx + 9, cy + 9), fill=BLACK)
    draw.rectangle((0, 0, width - 1, height - 1), outline=RED)
    draw.text((width - 60, height - 16), label or f"{width}x{height}", fill=BLACK, font=small)
    return image


def text_image(width: int, height: int, text: str, colour: tuple[int, int, int]) -> Image.Image:
    image = Image.new("RGB", (width, height), WHITE)
    draw = ImageDraw.Draw(image)
    size = height
    while size > 8:
        f = font(size)
        box = draw.multiline_textbbox((0, 0), text, font=f, align="center")
        if box[2] - box[0] <= width - 8 and box[3] - box[1] <= height - 8:
            break
        size -= 2
    box = draw.multiline_textbbox((0, 0), text, font=f, align="center")
    xy = ((width - (box[2] - box[0])) / 2 - box[0], (height - (box[3] - box[1])) / 2 - box[1])
    draw.multiline_text(xy, text, fill=colour, font=f, align="center")
    return image


async def send(
    address: str,
    make_image,
    preview: str | None,
    sdk_override: int | None,
    adv=None,
    layout: Layout | None = None,
) -> int:
    if adv is None:
        log(f"looking for {address} ...")
        found = await scan(15, address)
        adv = found.get(address.upper())
        if adv is None:
            log("label not seen advertising (is it in range / a Picksmart label?)")
            return 1
    log(str(adv))
    sdk_type = sdk_override or adv.sdk_type
    image = make_image(*display_size(sdk_type))
    if preview:
        quantize_preview(sdk_type, image).save(preview)
        log(f"preview saved to {preview}")
    payload = pack_image(sdk_type, image, layout)
    log(f"packed {len(payload)} bytes for SDK type {sdk_type}" + (f", layout {layout}" if layout else ""))
    async with PicksmartClient(address, log=log) as client:
        await client.send_payload(payload, sdk_type=sdk_type, sw=adv.sw, raw_planes=adv.raw_planes)
    log("done")
    return 0


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-v", "--verbose", action="store_true", help="log every BLE packet")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("scan")
    p.add_argument("--seconds", type=float, default=10)
    p = sub.add_parser("test-all", help="test pattern on every Picksmart label in range")
    p.add_argument("--seconds", type=float, default=15)
    for name in ("test", "text", "image"):
        p = sub.add_parser(name)
        p.add_argument("address")
        if name == "text":
            p.add_argument("text", help="use \\n for new lines")
            p.add_argument("--colour", choices=("black", "red", "yellow"), default="black")
        if name == "image":
            p.add_argument("path")
        if name == "test":
            p.add_argument("--pattern", choices=("default", "ruler"), default="default")
        p.add_argument("--preview", help="save quantised preview PNG here")
        p.add_argument("--sdk-type", type=int, help="override the advertised display type")
        p.add_argument(
            "--layout",
            type=Layout.parse,
            help="experimental manual layout (PROTOCOL.md §6), e.g. r90,2bpp "
            "(rN=rotate CCW, lr/tb=flip, pad=byte-pad rows, bw=first plane only, inv=invert it, "
            "2bpp=§5.3 2 bits/pixel, pairs=§5.4 row pairs)",
        )
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING, format="%(message)s")
    if args.verbose:
        logging.getLogger("bleak").setLevel(logging.INFO)

    if args.cmd == "scan":
        found = await scan(args.seconds)
        for adv in sorted(found.values(), key=lambda a: -(a.rssi or -999)):
            print(adv)
        if not found:
            print("no Picksmart labels found")
        return 0
    if args.cmd == "test-all":
        found = await scan(args.seconds)
        log(f"{len(found)} labels found")
        results = {}
        for adv in sorted(found.values(), key=lambda a: -(a.rssi or -999)):
            log(f"=== {adv.address}")
            try:
                results[adv.address] = await send(adv.address, test_pattern, None, None, adv) == 0
            except Exception as err:  # noqa: BLE001 - keep going with the other labels
                log(f"FAILED: {type(err).__name__}: {err}")
                results[adv.address] = False
            await asyncio.sleep(2)
        for address, ok in results.items():
            print(f"{address}  {found[address].model:<22} {'OK' if ok else 'FAILED'}")
        return 0 if all(results.values()) else 1
    if args.cmd == "test":
        label = str(args.layout) if args.layout else None
        pattern = ruler_pattern if args.pattern == "ruler" else test_pattern
        make = lambda w, h: pattern(w, h, label)  # noqa: E731
    elif args.cmd == "text":
        colour = {"black": BLACK, "red": RED, "yellow": YELLOW}[args.colour]
        make = lambda w, h: text_image(w, h, args.text.replace("\\n", "\n"), colour)  # noqa: E731
    else:
        source = Image.open(args.path).convert("RGB")
        make = lambda w, h: source.resize((w, h), Image.Resampling.LANCZOS)  # noqa: E731
    return await send(args.address, make, args.preview, args.sdk_type, layout=args.layout)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
