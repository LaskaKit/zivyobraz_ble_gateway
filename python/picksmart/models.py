"""The panel types this gateway drives (PROTOCOL.md §2) and how each one is paced (§4.3).

One table, `MODELS`. A type that isn't in it is "unknown": it shows up in scans but is never
sent to. Add a type only after it has been tested on a real tag and written into PROTOCOL.md.
"""

from __future__ import annotations

from dataclasses import dataclass

# Encodings (PROTOCOL.md §5)
PLANES = "planes"                      # §5.2, plane A bit = white, canvas order (SDK 75)
PLANES_FLIPPED = "planes-flipped"      # §5.2, plane A bit = not white, rows bottom up (SDK 299)
ROTATED_2BPP = "2bpp-rotated"          # §5.3 (SDK 46, 270)
ROW_PAIRS = "row-pairs"                # §5.4 (SDK 160)


@dataclass(frozen=True)
class Pacing:
    """How a streaming tag takes its data blocks (PROTOCOL.md §4.3)."""

    name: str
    ack_every_block: bool   # every write to FEF2 asks for a write response
    ack_last_block: bool    # the last block asks for a write response (a delivery fence)
    gap: float              # seconds to wait after each block
    done_wait: float        # seconds to wait for `05 08` after the last block


ACKED = Pacing("acked", ack_every_block=True, ack_last_block=True, gap=0.005, done_wait=2.0)
STREAM = Pacing("stream", ack_every_block=False, ack_last_block=False, gap=0.040, done_wait=20.0)
FENCED = Pacing("fenced", ack_every_block=False, ack_last_block=True, gap=0.100, done_wait=60.0)


@dataclass(frozen=True)
class Model:
    sdk: int
    width: int              # canvas, landscape as the tag is normally held (§5.1)
    height: int
    colours: str            # "BW", "BWR" or "BWRY"
    encoding: str
    pacing: Pacing
    label: str              # our display text; the inch sizes are ours on purpose (§2)

    @property
    def size(self) -> tuple[int, int]:
        return self.width, self.height


MODELS: dict[int, Model] = {
    m.sdk: m
    for m in (
        Model(46, 296, 128, "BWRY", ROTATED_2BPP, ACKED, "2.13in 296x128 BWRY"),
        Model(75, 400, 300, "BWR", PLANES, STREAM, "4.2in 400x300 BWR"),
        Model(160, 250, 122, "BW", ROW_PAIRS, STREAM, "2.9in TFT 250x122 BW"),
        Model(270, 250, 128, "BWRY", ROTATED_2BPP, STREAM, "2.9in 250x122 BWRY"),
        Model(299, 800, 480, "BWR", PLANES_FLIPPED, FENCED, "7.5in 800x480 BWR"),
    )
}

BW_TYPES = frozenset(s for s, m in MODELS.items() if m.colours == "BW")
BWRY_TYPES = frozenset(s for s, m in MODELS.items() if m.colours == "BWRY")


def model(sdk_type: int) -> Model:
    """The model for a supported type. Raises ValueError for anything else."""
    try:
        return MODELS[sdk_type]
    except KeyError:
        raise ValueError(f"unsupported panel type {sdk_type}") from None


def display_size(sdk_type: int) -> tuple[int, int]:
    """Canvas (width, height) of a supported type."""
    return model(sdk_type).size


def label(sdk_type: int) -> str:
    m = MODELS.get(sdk_type)
    return m.label if m else f"unknown type {sdk_type}"
