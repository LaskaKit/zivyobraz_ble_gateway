"""Živý obraz BLE Gateway: zivyobraz.eu content on Picksmart BLE e-paper tags (PC or Raspberry Pi).

    .venv/bin/python -m zivyobraz_ble_gateway                     # web page on http://<host>:8080/
    .venv/bin/python -m zivyobraz_ble_gateway --min-interval 300

The tags never call anybody. For each tag added on the web page, the gateway asks zivyobraz.eu
on the tag's behalf (as a FW 3.0 device: the tag's address is the MAC, the gateway invents the API
key), waits as long as the server says (PreciseSleep), and connects to the tag over Bluetooth only
when the content changed (the Timestamp header).

On zivyobraz.eu, add each tag with its MAC and the "zivyobraz key" shown on the web page.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

from zivyobraz_ble_gateway import web, zivyobraz
from zivyobraz_ble_gateway.registry import Registry
from zivyobraz_ble_gateway.service import Gateway, log


async def main() -> None:
    here = Path(__file__).resolve().parents[1]  # python/
    ap = argparse.ArgumentParser(prog="python -m zivyobraz_ble_gateway", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080, help="web page port (default 8080; 80 needs root)")
    ap.add_argument("--data", type=Path, default=here / "data", metavar="DIR",
                    help="tag list and previews (default: python/data)")
    ap.add_argument("--url", default=zivyobraz.SERVER_URL, help="zivyobraz endpoint")
    ap.add_argument("--min-interval", type=int, default=60, metavar="S",
                    help="ask zivyobraz.eu at most every S seconds per tag, whatever PreciseSleep says (default 60)")
    ap.add_argument("--max-interval", type=int, default=86400, metavar="S",
                    help="ask at least every S seconds (default 86400)")
    ap.add_argument("--allow-host", action="append", default=[], metavar="NAME",
                    help="another host name the page may be opened by (e.g. a reverse proxy's); "
                         "IP addresses and this machine's own names always work")
    ap.add_argument("-v", "--verbose", action="store_true", help="also log the BLE transfer steps")
    args = ap.parse_args()

    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(message)s", datefmt="%H:%M:%S",
                        stream=sys.stdout)
    log.setLevel(logging.DEBUG if args.verbose else logging.INFO)
    ring = web.LogRing()
    log.addHandler(ring)

    registry = Registry(args.data / "tags.json")
    gw = Gateway(registry, args.data, args.url, args.min_interval, args.max_interval)
    await web.start(gw, ring, args.port, args.allow_host)
    log.info(f"{len(registry.tags)} tag(s) in {registry.path}")
    try:
        await asyncio.gather(gw.poller(), gw.ble_worker())
    finally:
        registry.save()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
