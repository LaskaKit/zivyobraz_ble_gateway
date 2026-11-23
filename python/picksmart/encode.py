"""Canvas of colour indices -> payload bytes (PROTOCOL.md §5).

A canvas is `bytes` of `width * height` colour indices, row-major (§5.1):
0 white, 1 black, 2 red, 3 yellow. The panel rules of §5.1 are applied here, so any of the
four indices may be passed for any panel.
"""

from __future__ import annotations

import struct

from .models import PLANES, PLANES_FLIPPED, ROTATED_2BPP, ROW_PAIRS, model

WHITE, BLACK, RED, YELLOW = 0, 1, 2, 3

# §5.1: what each index becomes on a panel
_PANEL_RULES = {
    "BWRY": bytes((WHITE, BLACK, RED, YELLOW)),
    "BWR": bytes((WHITE, BLACK, RED, RED)),       # yellow is drawn as red
    "BW": bytes((WHITE, BLACK, BLACK, BLACK)),    # red and yellow are drawn as black
}

# §5.3: two-bit values, indexed by colour index
TWO_BIT = (1, 0, 3, 2)  # white 1, black 0, red 3, yellow 2

ROW_PAIRS_ROWS = 132      # §5.4: the 250x122 canvas is extended to 132 rows

FRAME_CHUNK = 64          # §5.5
FRAME_STORED = 0x74


def apply_panel_rules(sdk_type: int, canvas: bytes) -> bytes:
    """§5.1: map the four indices to the ones the panel can show."""
    table = _PANEL_RULES[model(sdk_type).colours]
    return canvas.translate(table + bytes(256 - len(table)))


class Bits:
    """MSB-first bit writer."""

    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, bit: int) -> None:
        self.acc = self.acc << 1 | bit
        self.n += 1
        if self.n == 8:
            self.out.append(self.acc)
            self.acc = self.n = 0

    def pad(self) -> None:
        """Fill the current byte with zero bits."""
        while self.n:
            self.put(0)

    def bytes(self) -> bytes:
        self.pad()
        return bytes(self.out)


def pack_bits(bits) -> bytes:
    """An iterable of 0/1, MSB first, the last byte zero-padded."""
    writer = Bits()
    for bit in bits:
        writer.put(bit)
    return writer.bytes()


def two_planes(canvas: bytes, width: int, height: int, *, flip: bool, a_is_white: bool) -> bytes:
    """§5.2: plane A (white, or not white) then plane B (red), 1 bit per pixel, rows unpadded."""
    rows = range(height - 1, -1, -1) if flip else range(height)
    order = [canvas[y * width:(y + 1) * width] for y in rows]
    flat = b"".join(order)
    plane_a = pack_bits((p == WHITE) == a_is_white for p in flat)
    plane_b = pack_bits(p == RED for p in flat)
    return plane_a + plane_b


def rotated_2bpp(canvas: bytes, width: int, height: int) -> bytes:
    """§5.3: portrait buffer `height` wide, `width` rows; canvas (x, y) -> column y, row W-1-x."""
    out = bytearray(width * height // 4)
    n = 0
    for row in range(width):
        x = width - 1 - row
        for column in range(height):
            value = TWO_BIT[canvas[column * width + x]]
            out[n >> 2] |= value << (6 - 2 * (n & 3))
            n += 1
    return bytes(out)


def row_pairs(canvas: bytes, width: int, height: int) -> bytes:
    """§5.4: pad to 132 rows, transpose, pack buffer rows in pairs, then an all-zero second plane."""
    def bit(x: int, y: int) -> int:  # 1 = white; the padding rows are white
        return 1 if y >= height else int(canvas[y * width + x] == WHITE)

    def bits():
        for k in range(width // 2):
            for c in range(ROW_PAIRS_ROWS):
                yield bit(2 * k, c)
                yield bit(2 * k + 1, c)

    plane = pack_bits(bits())
    return plane + bytes(len(plane))


def encode(sdk_type: int, canvas: bytes) -> bytes:
    """The unframed payload for a canvas of colour indices (§5.2–§5.4)."""
    m = model(sdk_type)
    if len(canvas) != m.width * m.height:
        raise ValueError(f"canvas has {len(canvas)} pixels, SDK {sdk_type} needs {m.width}x{m.height}")
    canvas = apply_panel_rules(sdk_type, canvas)
    if m.encoding == PLANES:
        return two_planes(canvas, m.width, m.height, flip=False, a_is_white=True)
    if m.encoding == PLANES_FLIPPED:
        return two_planes(canvas, m.width, m.height, flip=True, a_is_white=False)
    if m.encoding == ROTATED_2BPP:
        return rotated_2bpp(canvas, m.width, m.height)
    if m.encoding == ROW_PAIRS:
        return row_pairs(canvas, m.width, m.height)
    raise AssertionError(m.encoding)


def stored_blocks(data: bytes) -> bytes:
    """§5.5: every 64-byte chunk as `74 <len+3> <len> chunk`."""
    out = bytearray()
    for i in range(0, len(data), FRAME_CHUNK):
        chunk = data[i:i + FRAME_CHUNK]
        out += bytes((FRAME_STORED, len(chunk) + 3, len(chunk))) + chunk
    return bytes(out)


def frame(payload: bytes, sdk_type: int) -> bytes:
    """§5.5 stored-block framing, for tags without the raw-planes flag.

    Odd SDK types carry two planes, each framed on its own; even types one buffer.
    """
    # SPEC-GAP: §5.5 doesn't state the byte order of `plane_length`; little-endian as all of §4.
    if sdk_type % 2:
        half = len(payload) // 2
        planes = (payload[:half], payload[half:])
    else:
        half = len(payload)
        planes = (payload,)
    return struct.pack("<I", half) + b"".join(stored_blocks(p) for p in planes)
