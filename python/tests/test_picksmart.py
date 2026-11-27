"""Tests for the Picksmart protocol layer against PROTOCOL.md (no Bluetooth).

    .venv/bin/python -m unittest discover tests
"""

import asyncio
import struct
import unittest

from PIL import Image

from picksmart import (
    Advertisement,
    pack_image,
    parse_manufacturer_data,
    quantize_preview,
)
from picksmart.client import (
    Transfer,
    TransferError,
    block,
    block_count,
    parse_data_message,
    prepare_command,
)
from picksmart.encode import encode, frame
from picksmart.imaging import BLACK, RED, WHITE, YELLOW, Layout, nearest_index
from picksmart.models import (
    ACKED,
    BW_TYPES,
    BWRY_TYPES,
    FENCED,
    MODELS,
    STREAM,
    display_size,
)


def canvas(sdk, pixels=()):
    """An all-white canvas of colour indices with some pixels set: ((x, y, index), ...)."""
    w, h = display_size(sdk)
    out = bytearray(w * h)
    for x, y, c in pixels:
        out[y * w + x] = c
    return bytes(out)


def bit(data, n):
    return data[n // 8] >> (7 - n % 8) & 1


def two_bits(data, n):
    return data[n // 4] >> (6 - 2 * (n % 4)) & 3


class Models(unittest.TestCase):
    def test_table(self):
        self.assertEqual(sorted(MODELS), [46, 75, 160, 270, 299])
        self.assertEqual(BW_TYPES, {160})
        self.assertEqual(BWRY_TYPES, {46, 270})
        self.assertEqual({s: display_size(s) for s in MODELS},
                         {46: (296, 128), 75: (400, 300), 160: (250, 122), 270: (250, 128), 299: (800, 480)})
        with self.assertRaises(ValueError):
            display_size(78)

    def test_pacing(self):  # §4.3
        self.assertIs(MODELS[46].pacing, ACKED)
        for sdk in (75, 160, 270):
            self.assertIs(MODELS[sdk].pacing, STREAM)
        self.assertIs(MODELS[299].pacing, FENCED)
        self.assertEqual((ACKED.ack_every_block, ACKED.ack_last_block, ACKED.gap, ACKED.done_wait),
                         (True, True, 0.005, 2))
        self.assertEqual((STREAM.ack_every_block, STREAM.ack_last_block, STREAM.gap, STREAM.done_wait),
                         (False, False, 0.005, 20))
        self.assertEqual((FENCED.ack_every_block, FENCED.ack_last_block, FENCED.gap, FENCED.done_wait),
                         (False, True, 0.020, 60))


class Advert(unittest.TestCase):  # §1
    def test_parse(self):
        a = parse_manufacturer_data("ff:ff:92:85:05:77", "NEMR92850577", -60, bytes([0x2E, 0x1E, 0x81, 0x01, 0x40]))
        self.assertEqual((a.address, a.raw_type, a.sdk_type, a.battery_dv, a.sw, a.hw),
                         ("FF:FF:92:85:05:77", 0x402E, 46, 30, 0x81, 1))
        self.assertEqual(a.battery_volts, 3.0)
        self.assertTrue(a.raw_planes and a.streaming)
        self.assertEqual(a.model, "2.13in 296x128 BWRY")

    def test_framed_type(self):
        a = parse_manufacturer_data("x", None, None, bytes([0x2B, 0x1D, 0x01, 0x01, 0x01]))  # 299, flag clear
        self.assertEqual((a.raw_type, a.sdk_type, a.raw_planes, a.streaming), (0x012B, 299, False, False))

    def test_low_byte_fallback_and_unknown(self):
        a = parse_manufacturer_data("x", None, None, bytes([0x4B, 30, 0x81, 1, 0x20]))  # 0x204B: & 0xFF = 75
        self.assertEqual(a.sdk_type, 75)
        a = parse_manufacturer_data("x", None, None, bytes([78, 30, 0x81, 1, 0x40]))
        self.assertEqual(a.sdk_type, 78)
        self.assertEqual(a.model, "unknown type 78")
        self.assertIsNone(parse_manufacturer_data("x", None, None, bytes(4)))
        self.assertIsInstance(a, Advertisement)


class TwoPlanes(unittest.TestCase):  # §5.2
    def test_sdk75(self):
        p = encode(75, canvas(75))
        self.assertEqual(p, b"\xff" * 15000 + b"\x00" * 15000)
        p = encode(75, canvas(75, [(0, 0, 1), (1, 0, 2), (0, 1, 1), (399, 299, 3)]))
        self.assertEqual(bit(p, 0), 0)                   # black: not white, not red
        self.assertEqual(bit(p, 120000), 0)
        self.assertEqual((bit(p, 1), bit(p, 120000 + 1)), (0, 1))   # red: only plane B
        self.assertEqual(bit(p, 400), 0)                 # rows run on without padding: (0, 1) = bit 400
        n = 299 * 400 + 399                               # yellow is drawn red on BWR
        self.assertEqual((bit(p, n), bit(p, 120000 + n)), (0, 1))

    def test_sdk299(self):
        p = encode(299, canvas(299))
        self.assertEqual(p, bytes(96000))
        p = encode(299, canvas(299, [(0, 0, 1), (5, 479, 2)]))
        n = (479 - 0) * 800 + 0
        self.assertEqual((bit(p, n), bit(p, 384000 + n)), (1, 0))   # black: not white
        self.assertEqual((bit(p, 5), bit(p, 384000 + 5)), (1, 1))   # red, flipped to the first row
        self.assertEqual(sum(p), 0x80 + 0x04 * 2)


class Rotated2bpp(unittest.TestCase):  # §5.3
    def test_examples(self):
        for sdk, length in ((46, 9472), (270, 8000)):
            p = encode(sdk, canvas(sdk))
            self.assertEqual(p, b"\x55" * length)
        p = encode(46, canvas(46, [(0, 0, 1), (295, 0, 2), (0, 127, 3), (1, 0, 1)]))
        self.assertEqual(p[9440] >> 6, 0)          # (0, 0) black, byte 9440 bits 7-6
        self.assertEqual(p[0] >> 6, 3)             # (295, 0) red, byte 0 bits 7-6
        self.assertEqual(p[9471] & 3, 2)           # (0, 127) yellow, byte 9471 bits 1-0
        self.assertEqual(two_bits(p, (296 - 1 - 1) * 128), 0)  # (1, 0) -> row 294, column 0

    def test_bw_and_bwr_rules(self):
        # BWRY keeps all four; values black 0 white 1 yellow 2 red 3
        p = encode(270, canvas(270, [(249, 0, 1), (249, 1, 2), (249, 2, 3)]))
        self.assertEqual([two_bits(p, n) for n in range(4)], [0, 3, 2, 1])


class RowPairs(unittest.TestCase):  # §5.4
    def test_examples(self):
        p = encode(160, canvas(160))
        self.assertEqual(p, b"\xff" * 4125 + bytes(4125))
        for (x, y), (byte, b) in {(0, 0): (0, 7), (1, 0): (0, 6), (0, 1): (0, 5), (249, 0): (4092, 6)}.items():
            p = encode(160, canvas(160, [(x, y, 1)]))
            self.assertEqual(p[byte], 0xFF ^ (1 << b), (x, y))
            self.assertEqual(p.count(0xFF), 4124)

    def test_bw_rules(self):
        # red and yellow are drawn black on the BW panel
        self.assertEqual(encode(160, canvas(160, [(0, 0, 2), (1, 0, 3)]))[0], 0xFF ^ 0xC0)


class Framing(unittest.TestCase):  # §5.5
    def test_sdk299(self):
        f = frame(bytes(96000), 299)
        self.assertEqual(len(f), 100504)
        self.assertEqual(f[:4], struct.pack("<I", 48000))
        self.assertEqual(f[4:7], bytes([0x74, 67, 64]))
        self.assertEqual(f[4 + 67 * 750:4 + 67 * 750 + 3], bytes([0x74, 67, 64]))  # plane B starts a new block

    def test_short_chunks(self):
        data = bytes(range(130))
        f = frame(data, 46)  # even: one buffer
        self.assertEqual(f, struct.pack("<I", 130) + b"\x74\x43\x40" + data[:64] + b"\x74\x43\x40" + data[64:128]
                         + b"\x74\x05\x02" + data[128:])
        f = frame(data, 75)  # odd: two planes of 65, framed separately
        self.assertEqual(f, struct.pack("<I", 65) + b"\x74\x43\x40" + data[:64] + b"\x74\x04\x01" + data[64:65]
                         + b"\x74\x43\x40" + data[65:129] + b"\x74\x04\x01" + data[129:])


class Images(unittest.TestCase):
    def test_nearest_palette(self):
        self.assertEqual(nearest_index(255, 0, 0), 2)
        self.assertEqual(nearest_index(255, 255, 0), 3)
        self.assertEqual(nearest_index(170, 170, 170), 0)
        self.assertEqual(nearest_index(85, 85, 85), 1)
        self.assertEqual(nearest_index(128, 128, 128), 2)  # mid grey is nearer RED (220, 20, 12) than white
        self.assertEqual(nearest_index(0, 0, 0), 1)
        self.assertEqual(nearest_index(*YELLOW), 3)

    def test_pack_image_lengths(self):
        lengths = {46: 9472, 75: 30000, 160: 8250, 270: 8000, 299: 96000}
        for sdk, n in lengths.items():
            image = Image.new("RGB", display_size(sdk), WHITE)
            self.assertEqual(len(pack_image(sdk, image)), n)
        with self.assertRaises(ValueError):
            pack_image(75, Image.new("RGB", (10, 10)))

    def test_pack_image_colours(self):
        image = Image.new("RGB", (400, 300), WHITE)
        image.putpixel((0, 0), BLACK)
        image.putpixel((1, 0), RED)
        image.putpixel((2, 0), YELLOW)
        p = pack_image(75, image)
        self.assertEqual(p[0] >> 5, 0b000)      # plane A: black, red, yellow (drawn red): not white
        self.assertEqual(p[15000] >> 5, 0b011)
        preview = quantize_preview(160, image.crop((0, 0, 250, 122)))
        self.assertEqual([preview.getpixel((x, 0)) for x in range(4)], [BLACK, BLACK, BLACK, WHITE])

    def test_layouts(self):
        layout = Layout.parse("r90,lr,pad,inv")
        self.assertEqual(str(layout), "r90,lr,pad,inv")
        with self.assertRaises(ValueError):
            Layout.parse("r45")
        with self.assertRaises(ValueError):
            Layout.parse("2bpp,pairs")
        import random
        rng = random.Random(5)
        for sdk, spec in ((46, "r90,2bpp"), (270, "r90,2bpp"), (75, ""), (299, "tb,inv")):
            w, h = display_size(sdk)
            image = Image.frombytes("P", (w, h), bytes(rng.randrange(4) for _ in range(w * h)))
            image.putpalette([c for rgb in (WHITE, BLACK, RED, YELLOW) for c in rgb])
            self.assertEqual(pack_image(sdk, image, Layout.parse(spec)), pack_image(sdk, image), spec)
        bw = pack_image(75, Image.new("RGB", (400, 300), WHITE), Layout.parse("bw"))
        self.assertEqual(bw, b"\xff" * 15000)


# --- transfer against a fake tag ----------------------------------------------------------------

class FakeTag:
    """A `Link` that behaves like a streaming tag, with switches for the misbehaviours."""

    def __init__(self, block_size=244, *, block_size_silent=0, prepare_status=0, first_index=0,
                 on_block=None, done=True, hang=()):
        self.queue = asyncio.Queue()
        self.block_size = block_size
        self.block_size_silent = block_size_silent
        self.prepare_status = prepare_status
        self.first_index = first_index
        self.on_block = on_block or (lambda tag, index, last: None)
        self.done = done
        self.hang = set(hang)
        self.commands = []
        self.blocks = []      # (index, data length, response)
        self.count = None

    def say(self, *data):
        self.queue.put_nowait(bytes(data))

    def request(self, index):
        self.queue.put_nowait(bytes([5, 0]) + struct.pack("<I", index))

    async def send_command(self, data):
        self.commands.append(bytes(data))
        if data[0] == 1:
            if self.block_size_silent:
                self.block_size_silent -= 1
            else:
                self.say(1, *struct.pack("<H", self.block_size))
        elif data[0] == 2:
            self.length = struct.unpack_from("<I", data, 1)[0]
            self.count = block_count(self.length, self.block_size)
            self.say(0x77)  # unrelated noise, must be ignored
            self.say(2, self.prepare_status)
        elif data[0] == 3:
            self.request(self.first_index)

    async def send_block(self, data, response):
        index = struct.unpack_from("<I", data)[0]
        self.blocks.append((index, len(data), response))
        last = index == self.count - 1
        self.on_block(self, index, last)
        if last and self.done:
            self.say(5, 8)
        if index in self.hang:
            await asyncio.sleep(3600)

    async def receive(self, timeout):
        try:
            return await asyncio.wait_for(self.queue.get(), timeout)
        except TimeoutError:
            return None

    def poll(self):
        try:
            return self.queue.get_nowait()
        except asyncio.QueueEmpty:
            return None


def run(tag, payload, sdk=75, streaming=True):
    logs = []
    transfer = Transfer(tag, payload, pacing=MODELS[sdk].pacing, streaming=streaming, log=logs.append,
                        time_scale=0.001)
    asyncio.run(transfer.run())
    return logs


class Commands(unittest.TestCase):  # §4.1
    def test_prepare_is_8_bytes(self):
        self.assertEqual(prepare_command(100504), bytes([2]) + (100504).to_bytes(4, "little") + bytes([1, 0, 0]))
        self.assertEqual(len(prepare_command(1)), 8)

    def test_blocks(self):
        self.assertEqual(block_count(100504, 244), 419)
        self.assertEqual(block_count(240, 244), 1)
        self.assertEqual(block_count(241, 244), 2)
        payload = bytes(range(256)) * 2
        self.assertEqual(block(payload, 0, 244), bytes(4) + payload[:240])
        self.assertEqual(block(payload, 2, 244), bytes([2, 0, 0, 0]) + payload[480:])  # short, not padded
        self.assertEqual(parse_data_message(bytes([5, 0, 3, 1, 0, 0])), (0, 259))
        self.assertEqual(parse_data_message(bytes([5, 8])), (8, None))
        self.assertIsNone(parse_data_message(bytes([2, 0])))


class Transfers(unittest.TestCase):
    payload = bytes(range(256)) * 4  # 1024 bytes = 5 blocks of 240 (the last 64)

    def test_stream_in_order(self):
        tag = FakeTag()
        run(tag, self.payload)
        self.assertEqual(tag.commands, [b"\x01", prepare_command(1024), b"\x03"])
        self.assertEqual([b[0] for b in tag.blocks], [0, 1, 2, 3, 4])
        self.assertEqual([b[1] for b in tag.blocks], [244, 244, 244, 244, 68])

    def test_pacing_per_type(self):
        acks = {}
        for sdk in MODELS:
            tag = FakeTag()
            run(tag, self.payload, sdk)
            acks[sdk] = [b[2] for b in tag.blocks]
        self.assertEqual(acks[46], [True] * 5)                     # every block acknowledged
        for sdk in (75, 160, 270):
            self.assertEqual(acks[sdk], [False] * 5)
        self.assertEqual(acks[299], [False] * 4 + [True])          # only the last block, a fence

    def test_first_request_not_zero(self):
        tag = FakeTag(first_index=3)
        run(tag, self.payload)
        self.assertEqual([b[0] for b in tag.blocks], [3, 4])

    def test_resend_request(self):
        def on_block(tag, index, last):
            if index == 3 and len(tag.blocks) == 4:
                tag.request(1)
        tag = FakeTag(on_block=on_block)
        run(tag, self.payload)
        self.assertEqual([b[0] for b in tag.blocks], [0, 1, 2, 3, 1, 2, 3, 4])

    def test_resend_after_last_block(self):
        def on_block(tag, index, last):
            if last and len(tag.blocks) == 5:
                tag.request(4)
        tag = FakeTag(on_block=on_block, done=False)
        tag.on_block = lambda t, i, last: (on_block(t, i, last), last and len(t.blocks) == 6 and t.say(5, 8))
        run(tag, self.payload)
        self.assertEqual([b[0] for b in tag.blocks], [0, 1, 2, 3, 4, 4])

    def test_request_limit(self):
        def on_block(tag, index, last):
            if last:
                tag.request(2)
        tag = FakeTag(on_block=on_block, done=False)
        with self.assertRaisesRegex(TransferError, "more than 5"):
            run(tag, self.payload)
        self.assertEqual(sum(1 for b in tag.blocks if b[0] == 2), 6)  # first pass + 5 requested resends

        tag = FakeTag(on_block=lambda t, i, last: last and t.request(1), done=False)
        with self.assertRaisesRegex(TransferError, "more than 5"):
            run(tag, self.payload)

    def test_request_out_of_range(self):
        tag = FakeTag(on_block=lambda t, i, last: i == 1 and t.request(5))
        with self.assertRaisesRegex(TransferError, "only 5"):
            run(tag, self.payload)
        with self.assertRaises(TransferError):
            run(FakeTag(first_index=9), self.payload)

    def test_missing_done_is_success(self):
        tag = FakeTag(done=False)
        logs = run(tag, self.payload, 299)
        self.assertEqual(len(tag.blocks), 5)
        self.assertTrue(any("no 05 08" in line for line in logs))

    def test_last_block_write_timeout_is_not_an_error(self):
        tag = FakeTag(hang={4})
        logs = run(tag, self.payload, 299)
        self.assertEqual([b[0] for b in tag.blocks], [0, 1, 2, 3, 4])  # not resent
        self.assertTrue(any("no write response" in line for line in logs))

    def test_mid_stream_write_timeout_is_an_error(self):
        with self.assertRaisesRegex(TransferError, "block 2"):
            run(FakeTag(hang={2}), self.payload, 46)

    def test_block_size_asked_twice(self):
        tag = FakeTag(block_size_silent=1)
        run(tag, self.payload)
        self.assertEqual(tag.commands[:2], [b"\x01", b"\x01"])
        with self.assertRaisesRegex(TransferError, "block size"):
            run(FakeTag(block_size_silent=2), self.payload)

    def test_error_status_aborts(self):  # §4.2
        # after the last block: an error, not "no 05 08, counting it as done"
        tag = FakeTag(on_block=lambda t, i, last: last and t.say(5, 1, 4, 0, 0, 0), done=False)
        with self.assertRaisesRegex(TransferError, "transfer error .05 01"):
            run(tag, self.payload)
        # mid-stream: stops sending at once
        tag = FakeTag(on_block=lambda t, i, last: i == 1 and t.say(5, 3))
        with self.assertRaisesRegex(TransferError, "transfer error"):
            run(tag, self.payload)
        self.assertEqual([b[0] for b in tag.blocks], [0, 1])
        # legacy
        tag = FakeTag(on_block=lambda t, i, last: t.say(5, 2, i, 0, 0, 0))
        with self.assertRaisesRegex(TransferError, "transfer error"):
            run(tag, self.payload, streaming=False)

    def test_prepare_rejected(self):
        with self.assertRaisesRegex(TransferError, "rejected"):
            run(FakeTag(prepare_status=1), self.payload)

    def test_legacy(self):
        tag = FakeTag(on_block=lambda t, i, last: not last and t.request(i + 1))
        run(tag, self.payload, streaming=False)
        self.assertEqual([b[0] for b in tag.blocks], [0, 1, 2, 3, 4])

        tag = FakeTag(on_block=lambda t, i, last: i == 1 and t.say(5, 8) or (not last and t.request(i + 1)))
        with self.assertRaisesRegex(TransferError, "05 08 after block"):
            run(tag, self.payload, streaming=False)


if __name__ == "__main__":
    unittest.main()
