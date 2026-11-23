"""The tags this gateway serves, saved as one JSON file (`data/tags.json`).

    {"FF:FF:92:85:05:77": {
        "api_key": "48151623",        # the X-API-Key we send for this tag (enter it on zivyobraz.eu)
        "added": 1763630000.0,
        "sdk_type": 46, "raw_type": 16430, "sw": 133, "hw": 1,   # from the last advertisement
        "timestamp": "05242116",      # zivyobraz Timestamp of the image now on the tag
        "next_poll": 1763630100.0,    # when to ask zivyobraz.eu again (epoch seconds)
        "last_poll": ..., "poll_result": "ok, unchanged",
        "last_push": ..., "push_result": "ok", "push_failures": 0,
        "battery_dv": 30, "rssi": -70, "last_seen": ...     # from advertisements
    }}

Everything runs on one asyncio loop, so there are no locks.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

from picksmart.advert import Advertisement

from .zivyobraz import new_api_key

MAX_TAGS = 64


class Registry:
    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.tags: dict[str, dict] = json.loads(self.path.read_text()) if self.path.exists() else {}

    def __contains__(self, mac: str) -> bool:
        return mac.upper() in self.tags

    def get(self, mac: str) -> dict:
        return self.tags[mac.upper()]

    def add(self, adv: Advertisement) -> dict:
        if len(self.tags) >= MAX_TAGS:
            raise ValueError(f"the registry is full ({MAX_TAGS} tags)")
        entry = {
            "api_key": new_api_key(),
            "added": time.time(),
            "timestamp": None,
            "next_poll": 0,  # = now
            "push_failures": 0,
        }
        self.tags[adv.address.upper()] = entry
        self.update_advert(adv)  # also saves
        return entry

    def update_advert(self, adv: Advertisement, save: bool = True) -> None:
        """Remember what a registered tag advertises (type, firmware, battery, signal)."""
        entry = self.tags.get(adv.address.upper())
        if entry is None:
            return
        entry.update(sdk_type=adv.sdk_type, raw_type=adv.raw_type, sw=adv.sw, hw=adv.hw,
                     battery_dv=adv.battery_dv, rssi=adv.rssi, last_seen=time.time())
        if save:
            self.save()

    def remove(self, mac: str) -> bool:
        found = self.tags.pop(mac.upper(), None) is not None
        self.save()
        return found

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")  # write + rename: a crash can't leave half a file
        tmp.write_text(json.dumps(self.tags, indent=2) + "\n")
        os.replace(tmp, self.path)
