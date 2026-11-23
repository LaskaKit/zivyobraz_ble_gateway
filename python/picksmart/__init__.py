"""Picksmart BLE e-paper tags: advertisement, image encoding and transfer.

Written from this repo's PROTOCOL.md (the section numbers in the comments refer to it).
"""

from .advert import (
    COMPANY_ID,
    RAW_DATA_FLAG,
    Advertisement,
    parse_manufacturer_data,
    scan,
)
from .client import PicksmartClient, TransferError
from .imaging import Layout, pack_image, quantize_preview
from .models import MODELS, display_size

__all__ = [
    "COMPANY_ID",
    "MODELS",
    "RAW_DATA_FLAG",
    "Advertisement",
    "Layout",
    "PicksmartClient",
    "TransferError",
    "display_size",
    "pack_image",
    "parse_manufacturer_data",
    "quantize_preview",
    "scan",
]
