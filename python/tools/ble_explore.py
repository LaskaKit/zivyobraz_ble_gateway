#!/usr/bin/env python3
"""Generic BLE exploration helper for unknown tags (read-only).

    python tools/ble_explore.py scan [--seconds 15]       all advertisements
    python tools/ble_explore.py gatt  FF:FF:BC:AA:60:67   services, chars, readable values
    python tools/ble_explore.py listen FF:FF:BC:AA:60:67  subscribe to all notifications
"""

from __future__ import annotations

import argparse
import asyncio

from bleak import BleakClient, BleakScanner


async def do_scan(seconds: float) -> None:
    seen = {}

    def on_adv(device, adv) -> None:
        seen[device.address] = (adv.local_name, adv.rssi, adv.manufacturer_data, adv.service_uuids, adv.service_data)

    async with BleakScanner(on_adv, scanning_mode="active"):
        await asyncio.sleep(seconds)
    for addr, (name, rssi, mfr, uuids, sdata) in sorted(seen.items(), key=lambda x: -(x[1][1] or -999)):
        mfr_s = " ".join(f"{k:04x}:{bytes(v).hex()}" for k, v in mfr.items())
        sd_s = " ".join(f"{k[4:8]}:{bytes(v).hex()}" for k, v in sdata.items())
        print(f"{addr} {rssi:>4} {name or '-':<20} mfr[{mfr_s}] uuids{[u[4:8] for u in uuids]} sdata[{sd_s}]")


async def connect(address: str) -> BleakClient:
    device = await BleakScanner.find_device_by_address(address, timeout=30)
    if device is None:
        raise SystemExit(f"{address} not found (tags advertise slowly - retry)")
    client = BleakClient(device, timeout=20)
    await client.connect()
    acquire = getattr(getattr(client, "_backend", None), "_acquire_mtu", None)
    if callable(acquire):
        try:
            await acquire()
        except Exception:  # noqa: BLE001
            pass
    print("connected, MTU", client.mtu_size)
    return client


async def do_gatt(address: str) -> None:
    client = await connect(address)
    try:
        for service in client.services:
            print("SVC", service.uuid, service.description)
            for char in service.characteristics:
                value = ""
                if "read" in char.properties:
                    try:
                        raw = bytes(await client.read_gatt_char(char))
                        value = f" = {raw.hex(' ')}  {raw!r}"
                    except Exception as err:  # noqa: BLE001
                        value = f" (read error: {err})"
                print(f"  CHR {char.uuid} handle={char.handle} {char.properties}{value}")
    finally:
        await client.disconnect()


async def do_listen(address: str, seconds: float) -> None:
    client = await connect(address)
    try:
        for char in client.services.characteristics.values():
            if {"notify", "indicate"} & set(char.properties):
                await client.start_notify(
                    char, lambda _s, d, u=char.uuid: print(f"NOTIFY {u[4:8]} {bytes(d).hex(' ')}")
                )
                print("subscribed", char.uuid)
        await asyncio.sleep(seconds)
    finally:
        await client.disconnect()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cmd", choices=("scan", "gatt", "listen"))
    parser.add_argument("address", nargs="?")
    parser.add_argument("--seconds", type=float, default=15)
    args = parser.parse_args()
    if args.cmd == "scan":
        asyncio.run(do_scan(args.seconds))
    elif not args.address:
        parser.error("address required")
    elif args.cmd == "gatt":
        asyncio.run(do_gatt(args.address))
    else:
        asyncio.run(do_listen(args.address, args.seconds))


if __name__ == "__main__":
    main()
