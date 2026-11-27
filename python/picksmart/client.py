"""The image transfer (PROTOCOL.md §3–§4).

Two layers:

* `Transfer` is the protocol state machine. It only needs a `Link` (send a command, send a data
  block, receive a notification), so the tests run it against a fake tag without Bluetooth.
* `PicksmartClient` is the bleak connection: scan, connect with retries, MTU, characteristics,
  notifications (§3, §4.5), and a `BleLink` for `Transfer`.
"""

from __future__ import annotations

import asyncio
import logging
import struct
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol

from .encode import frame
from .models import MODELS, Pacing

logger = logging.getLogger("picksmart")

# Commands to FEF1 and the first byte of the answers (§4.1)
CMD_BLOCK_SIZE = 0x01
CMD_PREPARE = 0x02
CMD_START = 0x03
MSG_DATA = 0x05            # 05 <status> <u32 index>
DATA_REQUEST = 0x00        # "send from block <index>"
DATA_DONE = 0x08           # all data received
CMD_CANCEL = 0x04          # §4.1 note: in the vendor document, but our tags don't answer it; not used
FULL_REFRESH = 0x01
BLOCK_HEADER = 4           # u32 block index in front of every data block

MAX_REQUESTS = 5           # §4.2: the same index asked for more often than this -> abort


class TransferError(Exception):
    """The tag couldn't be reached or the transfer failed."""


@dataclass(frozen=True)
class Timeouts:
    """PROTOCOL.md §4.5, in seconds."""

    scan: float = 20
    connect: float = 20
    connect_attempts: int = 5
    connect_backoff: float = 2      # × attempt number
    mtu: float = 4
    settle: float = 0.4
    write: float = 8
    block_size: float = 4
    block_size_tries: int = 2
    prepare: float = 10
    first_request: float = 12
    legacy_next: float = 12         # §4.4
    disconnect: float = 5


TIMEOUTS = Timeouts()


def prepare_command(length: int) -> bytes:
    """§4.1: `02 <u32 length> 01 00 00`, exactly 8 bytes (a 6-byte version never refreshes)."""
    return struct.pack("<BIBBB", CMD_PREPARE, length, FULL_REFRESH, 0, 0)


def block_count(length: int, block_size: int) -> int:
    per_block = block_size - BLOCK_HEADER
    return -(-length // per_block)


def block(payload: bytes, index: int, block_size: int) -> bytes:
    """`<u32 index> <data>`, data = payload[index·(block_size−4) …], the last one short."""
    per_block = block_size - BLOCK_HEADER
    return struct.pack("<I", index) + payload[index * per_block:(index + 1) * per_block]


def parse_data_message(msg: bytes) -> tuple[int, int | None] | None:
    """`05 <status> [<u32 index>]` -> (status, index or None). None for anything else."""
    if len(msg) < 2 or msg[0] != MSG_DATA:
        return None
    index = struct.unpack_from("<I", msg, 2)[0] if len(msg) >= 6 else None
    return msg[1], index


def is_request(msg: bytes) -> bool:
    parsed = parse_data_message(msg)
    return parsed is not None and parsed[0] == DATA_REQUEST and parsed[1] is not None


def is_done(msg: bytes) -> bool:
    parsed = parse_data_message(msg)
    return parsed is not None and parsed[0] == DATA_DONE


def is_error(msg: bytes) -> bool:
    """§4.2: a `05` status other than 00 and 08 is a transfer error."""
    parsed = parse_data_message(msg)
    return parsed is not None and parsed[0] not in (DATA_REQUEST, DATA_DONE)


def tag_error(msg: bytes) -> TransferError:
    return TransferError(f"tag reported a transfer error ({msg.hex(' ')})")


class Link(Protocol):
    """What `Transfer` needs from a connection to a tag."""

    async def send_command(self, data: bytes) -> None:
        """Write to FEF1."""

    async def send_block(self, data: bytes, response: bool) -> None:
        """Write to FEF2, with or without a write response."""

    async def receive(self, timeout: float) -> bytes | None:
        """The next notification (FEF1 or FEF2), None after `timeout` seconds."""

    def poll(self) -> bytes | None:
        """A notification that has already arrived, else None."""


class Transfer:
    """One full-screen update over a `Link` (PROTOCOL.md §4).

    `time_scale` multiplies every timeout and pause (tests use a small one).
    """

    def __init__(self, link: Link, payload: bytes, *, pacing: Pacing, streaming: bool,
                 log: Callable[[str], None] | None = None, timeouts: Timeouts = TIMEOUTS,
                 time_scale: float = 1.0) -> None:
        self.link = link
        self.payload = payload
        self.pacing = pacing
        self.streaming = streaming
        self.log = log or (lambda message: logger.info(message))
        self.t = timeouts
        self.scale = time_scale
        self.block_size = 0
        self.count = 0
        self.requests: Counter[int] = Counter()
        self.blocks_sent = 0

    # --- helpers ----------------------------------------------------------------------------

    async def _command(self, data: bytes) -> None:
        try:
            await asyncio.wait_for(self.link.send_command(data), self.t.write * self.scale)
        except TimeoutError:
            raise TransferError(f"write of command {data[:1].hex()} timed out") from None

    async def _wait(self, wanted: Callable[[bytes], bool], timeout: float) -> bytes | None:
        """The first notification `wanted` accepts; others are logged and ignored (§4.1).

        A `05` error status aborts (§4.2).
        """
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout * self.scale
        while True:
            left = deadline - loop.time()
            if left <= 0:
                return None
            msg = await self.link.receive(left)
            if msg is None:
                return None
            if is_error(msg):
                raise tag_error(msg)
            if wanted(msg):
                return msg
            logger.debug(f"ignored notification {msg.hex(' ')}")

    def _request(self, msg: bytes) -> int:
        """Register a `05 00 <index>` request (§4.2) and return the index."""
        index = parse_data_message(msg)[1]
        self.requests[index] += 1
        if index >= self.count:
            raise TransferError(f"tag asked for block {index}, there are only {self.count}")
        if self.requests[index] > MAX_REQUESTS:
            raise TransferError(f"tag asked for block {index} more than {MAX_REQUESTS} times")
        return index

    async def _send_block(self, index: int) -> None:
        last = index == self.count - 1
        response = self.pacing.ack_every_block or (last and self.pacing.ack_last_block)
        try:
            await asyncio.wait_for(self.link.send_block(block(self.payload, index, self.block_size), response),
                                   self.t.write * self.scale)
        except TimeoutError:
            if not last:
                raise TransferError(f"write of block {index} timed out") from None
            # §4.3: on the last block the controller often starts refreshing instead of answering.
            self.log(f"no write response for the last block {index}, waiting for 05 08")
        self.blocks_sent += 1
        if self.pacing.gap:
            await asyncio.sleep(self.pacing.gap * self.scale)

    # --- the sequence -----------------------------------------------------------------------

    async def run(self) -> None:
        length = len(self.payload)
        if not length:
            raise TransferError("empty payload")

        for attempt in range(self.t.block_size_tries):
            await self._command(bytes((CMD_BLOCK_SIZE,)))
            msg = await self._wait(lambda m: len(m) >= 3 and m[0] == CMD_BLOCK_SIZE, self.t.block_size)
            if msg is not None:
                break
            self.log("no block size answer" + (", asking again" if attempt + 1 < self.t.block_size_tries else ""))
        else:
            raise TransferError("tag didn't tell its block size")
        self.block_size = struct.unpack_from("<H", msg, 1)[0]
        if self.block_size <= BLOCK_HEADER:
            raise TransferError(f"unusable block size {self.block_size}")
        self.count = block_count(length, self.block_size)
        self.log(f"block size {self.block_size}: {length} bytes in {self.count} blocks")

        await self._command(prepare_command(length))
        msg = await self._wait(lambda m: m[:1] == bytes((CMD_PREPARE,)), self.t.prepare)
        if msg is None:
            raise TransferError("no answer to the prepare command")
        # SPEC-GAP: a bare `02` (no second byte) is taken as accepted; §4.1 only rejects a non-zero byte.
        if len(msg) >= 2 and msg[1] != 0:
            raise TransferError(f"tag rejected the transfer ({msg.hex(' ')})")

        await self._command(bytes((CMD_START,)))
        msg = await self._wait(is_request, self.t.first_request)
        if msg is None:
            raise TransferError("tag didn't ask for data after start")
        index = self._request(msg)

        if self.streaming:
            await self._stream(index)
        else:
            await self._legacy(index)

    async def _stream(self, index: int) -> None:
        """§4.3: send everything from `index` back to back; follow resend requests."""
        self.log(f"streaming from block {index} ({self.pacing.name} pacing)")
        while True:
            restart = None
            i = index
            while i < self.count:
                if self.blocks_sent:
                    while (msg := self.link.poll()) is not None:
                        if is_done(msg):
                            # SPEC-GAP: §4.2 doesn't say what `05 08` before the last block means in a
                            # stream; the tag says it has everything, so it is taken as success.
                            self.log(f"05 08 after block {i - 1}: done")
                            return
                        if is_request(msg):
                            restart = self._request(msg)
                            break
                        if is_error(msg):
                            raise tag_error(msg)
                        logger.debug(f"ignored notification {msg.hex(' ')}")
                    if restart is not None:
                        break
                await self._send_block(i)
                i += 1
            if restart is not None:
                self.log(f"tag asks to resend from block {restart}")
                index = restart
                continue

            msg = await self._wait(lambda m: is_done(m) or is_request(m), self.pacing.done_wait)
            if msg is None:
                self.log("no 05 08 after the last block; counting it as done (§4.2)")
                return
            if is_done(msg):
                self.log("05 08: tag has the image, refreshing")
                return
            index = self._request(msg)
            self.log(f"tag asks to resend from block {index}")

    async def _legacy(self, index: int) -> None:
        """§4.4 (unverified): one block per request."""
        # SPEC-GAP: §4.4 doesn't say how legacy blocks are written; the type's §4.3 write mode is used.
        self.log(f"legacy firmware: one block per request, from block {index}")
        while True:
            await self._send_block(index)
            last = index == self.count - 1
            msg = await self._wait(lambda m: is_done(m) or is_request(m), self.t.legacy_next)
            if msg is None:
                if last:
                    # SPEC-GAP: §4.4 doesn't cover a silent tag after the last block; §4.2 counts a
                    # missing 05 08 after a fully sent stream as success, so the same here.
                    self.log("no 05 08 after the last block; counting it as done")
                    return
                raise TransferError(f"tag went quiet after block {index}")
            if is_done(msg):
                if not last:
                    raise TransferError(f"05 08 after block {index} of {self.count}")
                self.log("05 08: tag has the image, refreshing")
                return
            index = self._request(msg)


# --- Bluetooth (bleak) -----------------------------------------------------------------------

# §3: FEF0 service, FEF1 control, FEF2 data; some firmware uses FD.., FC.. or FB.. instead.
_SERVICE_LETTERS = "edcb"


def _uuid(letter: str, last: str) -> str:
    return f"0000f{letter}f{last}-0000-1000-8000-00805f9b34fb"


class BleLink:
    """`Link` over a connected bleak client."""

    def __init__(self, client, control, data) -> None:
        self.client = client
        self.control = control
        self.data = data
        self.queue: asyncio.Queue[bytes] = asyncio.Queue()

    def on_notify(self, sender, data: bytearray) -> None:
        msg = bytes(data)
        logger.debug(f"<- {msg.hex(' ')}")
        self.queue.put_nowait(msg)

    async def send_command(self, data: bytes) -> None:
        logger.debug(f"-> FEF1 {data.hex(' ')}")
        # §3: commands as write-with-response when the characteristic allows it
        await self.client.write_gatt_char(self.control, data, response="write" in self.control.properties)

    async def send_block(self, data: bytes, response: bool) -> None:
        logger.debug(f"-> FEF2 {data[:4].hex(' ')} +{len(data) - 4} B{' (ack)' if response else ''}")
        await self.client.write_gatt_char(self.data, data, response=response)

    async def receive(self, timeout: float) -> bytes | None:
        try:
            return await asyncio.wait_for(self.queue.get(), timeout)
        except TimeoutError:
            return None

    def poll(self) -> bytes | None:
        try:
            return self.queue.get_nowait()
        except asyncio.QueueEmpty:
            return None


class PicksmartClient:
    """A connection to one tag. `async with PicksmartClient(address) as tag: await tag.send_payload(...)`."""

    def __init__(self, address: str, log: Callable[[str], None] | None = None,
                 timeouts: Timeouts = TIMEOUTS) -> None:
        self.address = address.upper()
        self.log = log or (lambda message: logger.info(message))
        self.t = timeouts
        self.client = None
        self.link: BleLink | None = None

    async def __aenter__(self) -> PicksmartClient:
        try:
            await self.connect()
        except BaseException:
            await self.disconnect()
            raise
        return self

    async def __aexit__(self, *exc) -> None:
        await self.disconnect()

    async def connect(self) -> None:
        from bleak import BleakClient, BleakScanner

        t = self.t
        for attempt in range(1, t.connect_attempts + 1):
            try:
                device = await BleakScanner.find_device_by_address(self.address, timeout=t.scan)
                if device is None:
                    raise TransferError(f"{self.address} not seen within {t.scan:.0f} s")
                client = BleakClient(device, timeout=t.connect)
                await client.connect()
                self.client = client
                break
            except Exception as err:  # noqa: BLE001 - bleak raises many kinds
                self.log(f"connect attempt {attempt} failed: {type(err).__name__}: {err}")
                if attempt == t.connect_attempts:
                    raise TransferError(f"can't connect to {self.address}: {err}") from err
                await asyncio.sleep(t.connect_backoff * attempt)
        self.log(f"connected (attempt {attempt})")

        await self._mtu()
        control, data = self._characteristics()
        self.link = BleLink(self.client, control, data)
        await self.client.start_notify(control, self.link.on_notify)
        if "notify" in data.properties:
            try:
                await self.client.start_notify(data, self.link.on_notify)
            except Exception as err:  # noqa: BLE001 - FEF1 alone is enough (§3)
                self.log(f"no notifications on {data.uuid[4:8]}: {err}")
        await asyncio.sleep(t.settle)

    async def _mtu(self) -> None:
        """§3/§4.5: the largest MTU; on BlueZ via bleak's private `_acquire_mtu()`. Optional."""
        backend = getattr(self.client, "_backend", None)
        try:
            if hasattr(backend, "_acquire_mtu"):
                await asyncio.wait_for(backend._acquire_mtu(), self.t.mtu)
            self.log(f"MTU {self.client.mtu_size}")
        except Exception as err:  # noqa: BLE001
            self.log(f"MTU exchange failed ({type(err).__name__} {err}), using the default")

    def _characteristics(self):
        services = self.client.services
        for letter in _SERVICE_LETTERS:
            control = services.get_characteristic(_uuid(letter, "1"))
            data = services.get_characteristic(_uuid(letter, "2"))
            if control is not None and data is not None:
                return control, data
        raise TransferError("no Picksmart service (FEF0/FDF0/FCF0/FBF0) on this device")

    async def disconnect(self) -> None:
        client, self.client = self.client, None
        if client is None:
            return
        try:
            await asyncio.wait_for(client.disconnect(), self.t.disconnect)
        except Exception as err:  # noqa: BLE001 - §4.5: logged, not raised
            self.log(f"disconnect: {type(err).__name__} {err}")

    async def send_payload(self, payload: bytes, *, sdk_type: int, sw: int, raw_planes: bool) -> None:
        """Send an unframed payload (`pack_image`); framed here when the tag lacks the raw flag (§5.5)."""
        model = MODELS.get(sdk_type)
        if model is None:
            raise TransferError(f"unsupported panel type {sdk_type}")
        if self.link is None:
            raise TransferError("not connected")
        if not raw_planes:
            payload = frame(payload, sdk_type)
            self.log(f"framed for a tag without the raw flag: {len(payload)} bytes")
        streaming = bool(sw & 0x80)
        await Transfer(self.link, payload, pacing=model.pacing, streaming=streaming, log=self.log).run()


__all__ = [
    "BleLink",
    "Link",
    "PicksmartClient",
    "Timeouts",
    "Transfer",
    "TransferError",
    "block",
    "block_count",
    "is_error",
    "parse_data_message",
    "prepare_command",
]
