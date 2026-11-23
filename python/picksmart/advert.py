"""Advertisement parsing and scanning (PROTOCOL.md §1)."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass

from .models import MODELS, label

COMPANY_ID = 0x5053        # manufacturer-specific data company id
RAW_DATA_FLAG = 0x4000     # in raw_type: the tag takes the packed image as is (no §5.5 framing)
STREAMING_FLAG = 0x80      # in sw: streaming transfer (§4.3)
TYPE_MASK = 0x3FFF


@dataclass
class Advertisement:
    address: str            # upper case
    name: str | None
    rssi: int | None
    raw_type: int           # byte4 << 8 | byte0
    sdk_type: int           # the panel type (§1); may be one we don't support
    battery_dv: int         # battery in 0.1 V
    sw: int
    hw: int

    @property
    def model(self) -> str:
        return label(self.sdk_type)

    @property
    def supported(self) -> bool:
        return self.sdk_type in MODELS

    @property
    def battery_volts(self) -> float:
        return self.battery_dv / 10

    @property
    def raw_planes(self) -> bool:
        return bool(self.raw_type & RAW_DATA_FLAG)

    @property
    def streaming(self) -> bool:
        return bool(self.sw & STREAMING_FLAG)

    def __str__(self) -> str:
        rssi = f"{self.rssi} dBm" if self.rssi is not None else "? dBm"
        return (f"{self.address}  {self.name or '-':<13} {self.model:<22} {self.battery_volts:.1f} V  {rssi:>8}  "
                f"type 0x{self.raw_type:04X} sw 0x{self.sw:02X} hw 0x{self.hw:02X}"
                f"{'' if self.raw_planes else ' framed'}")


def sdk_type_from_raw(raw_type: int) -> int:
    """`raw_type & 0x3FFF` if that's a supported panel, else `raw_type & 0xFF` if that is (§1).

    Neither: the masked value is returned and the tag counts as unknown.
    """
    sdk = raw_type & TYPE_MASK
    if sdk in MODELS:
        return sdk
    if raw_type & 0xFF in MODELS:
        return raw_type & 0xFF
    return sdk


def parse_manufacturer_data(address: str, name: str | None, rssi: int | None, data: bytes) -> Advertisement | None:
    """`data` = the manufacturer data for COMPANY_ID, without the company id. None if too short."""
    if len(data) < 5:
        return None
    raw_type = data[4] << 8 | data[0]
    return Advertisement(address=address.upper(), name=name, rssi=rssi, raw_type=raw_type,
                         sdk_type=sdk_type_from_raw(raw_type), battery_dv=data[1], sw=data[2], hw=data[3])


async def scan(timeout: float, address: str | None = None) -> dict[str, Advertisement]:
    """Actively scan for Picksmart tags, by upper-case address.

    With `address`, return as soon as that tag has been heard.
    """
    from bleak import BleakScanner

    found: dict[str, Advertisement] = {}
    wanted = address.upper() if address else None
    seen_wanted = asyncio.Event()

    def on_advert(device, adv) -> None:
        data = adv.manufacturer_data.get(COMPANY_ID)
        if data is None:
            return
        parsed = parse_manufacturer_data(device.address, adv.local_name, adv.rssi, bytes(data))
        if parsed is None:
            return
        found[parsed.address] = parsed
        if parsed.address == wanted:
            seen_wanted.set()

    async with BleakScanner(on_advert, scanning_mode="active"):
        try:
            await asyncio.wait_for(seen_wanted.wait(), timeout)
        except TimeoutError:
            pass
    return found
