"""Pictures -> colour indices -> payload, plus a manual `Layout` for working out new panels.

`pack_image()` turns a Pillow image into the canvas of colour indices of PROTOCOL.md §5.1 by
**nearest palette colour** (plain RGB distance to WHITE, BLACK, RED, YELLOW below; on a tie the
earlier one in that order wins), then encodes it (`encode.py`). The Živý obraz gateway only ever passes
pictures in exactly these four colours (decoded Z2/Z3); arbitrary pictures come only from the CLI
(`picksmart_tool.py image`). The ESP32 code maps Z indices straight to panel colours, with the same result.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from PIL import Image

from . import encode as enc
from .models import display_size, model

# The RGB the gateway draws with and shows in previews; also the nearest-colour palette.
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
RED = (220, 20, 12)
YELLOW = (244, 196, 0)
PALETTE = (WHITE, BLACK, RED, YELLOW)  # index = colour index of §5.1


def nearest_index(r: int, g: int, b: int) -> int:
    """The palette index nearest to an RGB colour (squared distance, first one wins a tie)."""
    best, best_d = 0, None
    for i, (pr, pg, pb) in enumerate(PALETTE):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best_d is None or d < best_d:
            best, best_d = i, d
    return best


def to_indices(image: Image.Image) -> bytes:
    """Row-major colour indices of an image (alpha is dropped, as `convert("RGB")` does)."""
    data = image.convert("RGB").tobytes()
    cache: dict[bytes, int] = {}
    out = bytearray(len(data) // 3)
    for i in range(len(out)):
        px = data[3 * i:3 * i + 3]
        index = cache.get(px)
        if index is None:
            index = cache[px] = nearest_index(*px)
        out[i] = index
    return bytes(out)


def canvas_of(sdk_type: int, image: Image.Image) -> bytes:
    """The §5.1 canvas for a supported panel: nearest colours, then the panel's colour rules."""
    if image.size != display_size(sdk_type):
        raise ValueError(f"image is {image.size[0]}x{image.size[1]}, SDK {sdk_type} needs "
                         f"{display_size(sdk_type)[0]}x{display_size(sdk_type)[1]}")
    return enc.apply_panel_rules(sdk_type, to_indices(image))


def pack_image(sdk_type: int, image: Image.Image, layout: Layout | None = None) -> bytes:
    """The unframed payload for `image` (exactly the panel's canvas size).

    `layout` replaces the per-type encoding with a manual one (PROTOCOL.md §6).
    """
    canvas = canvas_of(sdk_type, image)
    if layout is not None:
        width, height = display_size(sdk_type)
        return layout.pack(canvas, width, height)
    return enc.encode(sdk_type, canvas)


def quantize_preview(sdk_type: int, image: Image.Image) -> Image.Image:
    """What the panel will show: the same colour mapping as `pack_image`, back as RGB."""
    canvas = canvas_of(sdk_type, image)
    preview = Image.frombytes("P", image.size, canvas)
    flat = [c for rgb in PALETTE for c in rgb]
    preview.putpalette(flat + [255] * (768 - len(flat)))
    return preview.convert("RGB")


# --- manual layouts (PROTOCOL.md §6) ---------------------------------------------------------

_TURNS = {
    "r90": Image.Transpose.ROTATE_90,     # Pillow's ROTATE_* are counter-clockwise
    "r180": Image.Transpose.ROTATE_180,
    "r270": Image.Transpose.ROTATE_270,
    "lr": Image.Transpose.FLIP_LEFT_RIGHT,
    "tb": Image.Transpose.FLIP_TOP_BOTTOM,
}
_FLAGS = ("pad", "bw", "inv", "2bpp", "pairs")


@dataclass
class Layout:
    """A manual layout, parsed from comma-separated tokens, applied in this order:

    1. `rN` (rotate N degrees counter-clockwise, N = 0/90/180/270), `lr`, `tb` (flip):
       transform the canvas into the buffer, in the order given.
    2. Packing of the buffer, rows top to bottom, MSB first:
       * default: two planes (§5.2 style), plane A bit 1 = white, plane B bit 1 = red;
         `inv` inverts plane A, `bw` sends plane A only;
       * `2bpp`: two bits per pixel with the §5.3 values, buffer as is (no built-in rotation);
       * `pairs`: buffer rows in pairs, interleaved per column (§5.4 step 3; bit 1 = white), then an
         all-zero plane of the same length. A missing last row counts as white.
       `pad` starts every buffer row (every row pair for `pairs`) on a whole byte.

    For example `r90,2bpp` gives the SDK 46/270 encoding, `tb,inv` that of SDK 299.
    """

    tokens: list[str] = field(default_factory=list)

    @classmethod
    def parse(cls, spec: str) -> Layout:
        tokens = [t.strip().lower() for t in spec.split(",") if t.strip()]
        for t in tokens:
            if t not in _TURNS and t not in _FLAGS and t != "r0":
                raise ValueError(f"unknown layout token {t!r}")
        if "2bpp" in tokens and "pairs" in tokens:
            raise ValueError("2bpp and pairs exclude each other")
        return cls(tokens)

    def __str__(self) -> str:
        return ",".join(self.tokens)

    def pack(self, canvas: bytes, width: int, height: int) -> bytes:
        buffer = Image.frombytes("L", (width, height), canvas)
        for t in self.tokens:
            if t in _TURNS:
                buffer = buffer.transpose(_TURNS[t])
        bw, bh = buffer.size
        px = buffer.tobytes()
        rows = [px[y * bw:(y + 1) * bw] for y in range(bh)]
        pad = "pad" in self.tokens

        if "2bpp" in self.tokens:
            out = enc.Bits()
            for row in rows:
                for p in row:
                    v = enc.TWO_BIT[p]
                    out.put(v >> 1)
                    out.put(v & 1)
                if pad:
                    out.pad()
            return out.bytes()

        if "pairs" in self.tokens:
            out = enc.Bits()
            white_row = bytes(bw)  # index 0 = white
            for k in range(0, bh, 2):
                first, second = rows[k], rows[k + 1] if k + 1 < bh else white_row
                for c in range(bw):
                    out.put(int(first[c] == enc.WHITE))
                    out.put(int(second[c] == enc.WHITE))
                if pad:
                    out.pad()
            plane = out.bytes()
            return plane + bytes(len(plane))

        a_is_white = "inv" not in self.tokens
        plane_a, plane_b = enc.Bits(), enc.Bits()
        for row in rows:
            for p in row:
                plane_a.put(int((p == enc.WHITE) == a_is_white))
                plane_b.put(int(p == enc.RED))
            if pad:
                plane_a.pad()
                plane_b.pad()
        if "bw" in self.tokens:
            return plane_a.bytes()
        return plane_a.bytes() + plane_b.bytes()


__all__ = [
    "BLACK",
    "PALETTE",
    "RED",
    "WHITE",
    "YELLOW",
    "Layout",
    "canvas_of",
    "display_size",
    "model",
    "nearest_index",
    "pack_image",
    "quantize_preview",
    "to_indices",
]
