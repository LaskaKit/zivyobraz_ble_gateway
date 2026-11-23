"""The gateway loop: ask zivyobraz.eu for each tag on its schedule, push changed images over BLE.

The tags never call us: they only advertise. So the gateway does what the zivyobraz firmware
does after waking up, on the tag's behalf:

  poller  (one task, HTTP only)
      for every registered tag whose `next_poll` has come: POST the request, then
      next_poll = now + PreciseSleep (clamped to --min-interval .. --max-interval).
      If the server's Timestamp differs from the one of the image on the tag (or a refresh was
      forced), decode the image and queue it for the BLE worker.

  BLE worker  (one task, owns the Bluetooth adapter)
      scan continuously (keeps battery / signal / "in range" fresh, finds new tags);
      when an image is queued: stop scanning, send it (picksmart.PicksmartClient), resume.
      Only one transfer at a time: one adapter handles one connection well.

The tag's stored Timestamp changes only after a successful transfer. A failed transfer is
retried by polling again soon (RETRY_SECONDS, growing with each failure).
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

from picksmart import PicksmartClient, pack_image, quantize_preview
from picksmart.advert import COMPANY_ID, RAW_DATA_FLAG, Advertisement, parse_manufacturer_data

from . import zivyobraz
from .registry import Registry

log = logging.getLogger("zivyobraz_ble_gateway")

ERROR_RETRY_SECONDS = 300      # server unreachable / bad answer: ask again in 5 min
RETRY_SECONDS = 60             # failed BLE transfer: 1, 2, 4, ... min, at most MAX_RETRY_SECONDS
MAX_RETRY_SECONDS = 1800
TRANSFER_TIMEOUT = 240         # a 7.5" QuickLZ tag takes ~45 s; connect retries add up to ~2 min
SEEN_FORGET_SECONDS = 600      # "in range" list: drop tags not heard for 10 min
SAVE_EVERY_SECONDS = 60        # advertisement updates are written at most this often


@dataclass
class Push:
    """An image waiting for the BLE worker."""

    mac: str
    image: Image.Image
    timestamp: str | None
    reason: str


class Gateway:
    def __init__(self, registry: Registry, data_dir: Path, url: str = zivyobraz.SERVER_URL,
                 min_interval: int = 60, max_interval: int = 86400) -> None:
        self.registry = registry
        self.preview_dir = Path(data_dir) / "previews"
        self.url = url
        self.min_interval = min_interval
        self.max_interval = max_interval
        self.seen: dict[str, Advertisement] = {}   # every Picksmart tag heard, by address
        self.seen_at: dict[str, float] = {}
        self.pending: dict[str, Push] = {}         # images for the BLE worker (newest wins)
        self.busy: str | None = None               # address being transferred to, for the page
        self._wake_ble = asyncio.Event()
        self._wake_poller = asyncio.Event()
        self._forced: set[str] = set()

    # --- what the web page calls ---------------------------------------------------------

    def add(self, mac: str) -> None:
        adv = self.seen.get(mac.upper())
        if adv is None:
            raise ValueError(f"{mac} is not in range")
        if adv.sdk_type not in zivyobraz.MODELS:
            raise ValueError(f"{mac}: unknown display type {adv.sdk_type}")
        entry = self.registry.add(adv)
        log.info(f"added {adv.address} ({adv.model}), zivyobraz key {entry['api_key']}")
        self._wake_poller.set()

    def remove(self, mac: str) -> None:
        mac = mac.upper()
        self.pending.pop(mac, None)
        if self.registry.remove(mac):
            (self.preview_dir / preview_name(mac)).unlink(missing_ok=True)
            log.info(f"removed {mac}")

    def refresh(self, mac: str) -> None:
        """Ask zivyobraz.eu now and send the image even if the Timestamp didn't change."""
        mac = mac.upper()
        if mac in self.registry:
            self._forced.add(mac)
            self.registry.get(mac)["next_poll"] = 0
            self._wake_poller.set()
            log.info(f"{mac}: refresh requested")

    # --- poller ---------------------------------------------------------------------------

    async def poller(self) -> None:
        last_save = time.time()
        while True:
            now = time.time()
            due = [mac for mac, e in self.registry.tags.items() if e.get("next_poll", 0) <= now]
            for mac in due:
                if mac in self.registry:  # (may have been removed meanwhile)
                    await self.poll(mac)
            if time.time() - last_save > SAVE_EVERY_SECONDS:
                self.registry.save()  # keeps battery / last seen from advertisements
                last_save = time.time()
            self._wake_poller.clear()
            try:
                await asyncio.wait_for(self._wake_poller.wait(), 1)
            except TimeoutError:
                pass

    async def poll(self, mac: str) -> None:
        entry = self.registry.get(mac)
        forced = mac in self._forced
        self._forced.discard(mac)
        sdk_type = entry["sdk_type"]
        battery = entry.get("battery_dv")
        request = zivyobraz.build_request(mac, sdk_type, battery / 10 if battery else None, entry.get("rssi"))
        entry["last_poll"] = time.time()
        try:
            answer = await asyncio.to_thread(zivyobraz.fetch, self.url, entry["api_key"], request)
        except Exception as err:  # noqa: BLE001 - network errors come in many types
            entry["poll_result"] = f"error: {err}"
            entry["next_poll"] = time.time() + ERROR_RETRY_SECONDS
            self.registry.save()
            log.info(f"{mac}: zivyobraz.eu failed: {err}")
            return

        sleep = answer.sleep if answer.sleep is not None else ERROR_RETRY_SECONDS
        sleep = max(self.min_interval, min(self.max_interval, sleep))
        entry["next_poll"] = time.time() + sleep
        entry["rotate"] = answer.rotate
        changed = answer.timestamp is None or answer.timestamp != entry.get("timestamp")
        fmt = zivyobraz.image_format(answer.body)
        summary = f"timestamp {answer.timestamp}, {fmt} {len(answer.body)} B, next in {sleep} s"

        if not (changed or forced):
            entry["poll_result"] = "ok, unchanged"
            self.registry.save()
            log.info(f"{mac}: unchanged ({summary})")
            return
        try:
            image = zivyobraz.decode_image(answer.body, sdk_type)
        except Exception as err:  # noqa: BLE001
            entry["poll_result"] = f"bad image: {err}"
            self.registry.save()
            log.info(f"{mac}: can't use the image: {err} ({summary})")
            return
        if answer.rotate:
            image = image.rotate(180)
        self.preview_dir.mkdir(parents=True, exist_ok=True)
        quantize_preview(sdk_type, image).save(self.preview_dir / preview_name(mac))
        entry["poll_result"] = "ok, new image" if changed else "ok, forced"
        self.registry.save()
        log.info(f"{mac}: {'new image' if changed else 'forced refresh'} ({summary})")
        self.pending[mac] = Push(mac, image, answer.timestamp, entry["poll_result"])
        self._wake_ble.set()

    # --- BLE worker -----------------------------------------------------------------------

    def _on_advert(self, device, adv) -> None:
        data = adv.manufacturer_data.get(COMPANY_ID)
        if data is None:
            return
        parsed = parse_manufacturer_data(device.address, adv.local_name, adv.rssi, bytes(data))
        if parsed is None:
            return
        if parsed.address not in self.seen:
            known = "registered" if parsed.address in self.registry else "not registered"
            log.info(f"in range: {parsed.address} {parsed.model}, {parsed.battery_volts:.1f} V ({known})")
        self.seen[parsed.address] = parsed
        self.seen_at[parsed.address] = time.time()
        self.registry.update_advert(parsed, save=False)

    async def ble_worker(self) -> None:
        from bleak import BleakScanner

        while True:
            if not self.pending:
                try:
                    async with BleakScanner(self._on_advert, scanning_mode="active"):
                        self._wake_ble.clear()
                        try:  # (leave the scanner every few minutes, in case BlueZ got stuck)
                            await asyncio.wait_for(self._wake_ble.wait(), 300)
                        except TimeoutError:
                            pass
                except Exception as err:  # noqa: BLE001 - e.g. adapter off
                    log.info(f"scanner failed: {err}")
                    await asyncio.sleep(10)
                self._forget_old()
                continue
            mac, push = next(iter(self.pending.items()))
            del self.pending[mac]
            if mac in self.registry:
                await self.transfer(push)
            await asyncio.sleep(1)  # let BlueZ settle before the next scan / connection

    def _forget_old(self) -> None:
        limit = time.time() - SEEN_FORGET_SECONDS
        for mac in [m for m, t in self.seen_at.items() if t < limit]:
            del self.seen[mac], self.seen_at[mac]

    async def transfer(self, push: Push) -> None:
        entry = self.registry.get(push.mac)
        sdk_type = entry["sdk_type"]
        self.busy = push.mac
        started = time.time()
        log.info(f"{push.mac}: sending image ({push.reason})")
        try:
            payload = pack_image(sdk_type, push.image)
            async with PicksmartClient(push.mac, log=lambda m: log.debug(f"  {m}")) as tag:
                await asyncio.wait_for(
                    tag.send_payload(payload, sdk_type=sdk_type, sw=entry.get("sw", 0),
                                     raw_planes=bool(entry.get("raw_type", 0) & RAW_DATA_FLAG)),
                    TRANSFER_TIMEOUT)
        except Exception as err:  # noqa: BLE001
            failures = entry.get("push_failures", 0) + 1
            retry = min(MAX_RETRY_SECONDS, RETRY_SECONDS * 2 ** (failures - 1))
            entry.update(push_failures=failures, last_push=time.time(),
                         push_result=f"failed: {type(err).__name__} {err}".strip())
            # Poll again soon: the Timestamp still differs, so the image comes again.
            entry["next_poll"] = min(entry.get("next_poll", 0), time.time() + retry)
            log.info(f"{push.mac}: transfer failed ({err or type(err).__name__}), retry in {retry} s")
        else:
            entry.update(timestamp=push.timestamp, push_failures=0, last_push=time.time(), push_result="ok")
            log.info(f"{push.mac}: image on the tag, {time.time() - started:.0f} s")
        finally:
            self.busy = None
            self.registry.save()


def preview_name(mac: str) -> str:
    return mac.upper().replace(":", "") + ".png"
