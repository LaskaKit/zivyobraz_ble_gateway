# Picksmart BLE e-paper labels: protocol specification

This is the **single source of truth** for how both gateways (`python/`, `esp32/`) talk to the tags.
The code is written from this document; if they disagree, fix whichever is wrong and keep them in sync.

Scope: only the panel types listed in [§2](#2-supported-panels) are supported. Each has been driven
end to end on our own hardware. Other Picksmart types exist; add one only after testing it on a real tag
and recording the result here.

Provenance: the facts below are protocol and hardware facts (UUIDs, opcodes, byte layouts, timings).
Each was confirmed on the tags listed in §2 (see the "verified" notes). The wording, structure and
diagrams are ours.
Our starting point for the protocol was the dratek-eink-homeassistant project (see Credits in
[README.md](README.md#credits)).

## 1. Hardware and identification

Picksmart labels (also sold as Gicisky, and by DRATEK). Telink TLSR825x SoC, CR2450 cell.

* BLE name `NEMR` followed by 8 hex digits (the last 4 address bytes). GAP device name `PICKSMART`.
* The Telink OTA service `00010203-0405-0607-0809-0a0b0c0d1912` is also exposed. **We never use it.**
  The gateway must not touch tag firmware.

### Advertisement

Manufacturer-specific data, company id **`0x5053`**, at least 5 bytes:

| byte | meaning |
|---|---|
| 0 | display type, low byte |
| 1 | battery voltage in units of 0.1 V (`0x1E` = 3.0 V) |
| 2 | firmware ("software") version. Bit `0x80` set = streaming transfer (§4.3) |
| 3 | hardware version |
| 4 | display type, high byte |

* `raw_type = byte4 << 8 | byte0`.
* Bit `0x4000` of `raw_type` = **raw-planes flag**. Set: the tag takes the packed image as is. Clear:
  the image must be wrapped in the stored-block framing of §5.5.
* `sdk_type = raw_type & 0x3FFF` selects the panel (§2). If that isn't a supported type, try
  `raw_type & 0xFF`. Anything else is unsupported: show it as unknown and don't transfer.

## 2. Supported panels

Every tag seen here advertises `sw = 0x81` (streaming) and `hw = 0x01`. The inch sizes are our own labels
for the physical tags (they differ from the usual panel naming, on purpose).

| SDK | label | colours | canvas (w×h) | raw flag | encoding (§5) | payload bytes | tags here | verified |
|---|---|---|---|---|---|---|---|---|
| 46  | 2.13" | BWRY | 296×128 | set   | 2 bpp, rotated     | 9 472  | `…92:85:05:77`, `…92:85:05:74` | picture correct, ~18 s |
| 75  | 4.2"  | BWR  | 400×300 | set   | two planes         | 30 000 | `…92:81:34:40`, `…92:81:34:41` | picture correct, ~5 s |
| 160 | 2.9" TFT | BW | 250×122 | set   | row pairs          | 8 250  | `…92:82:98:22` | picture correct; connect often needs 2–3 tries |
| 270 | 2.9"  | BWRY | 250×128 | set   | 2 bpp, rotated     | 8 000  | `…92:80:96:05`, `…92:80:93:93` | transfer OK and refreshes, ~2 s; **orientation not yet checked by eye** |
| 299 | 7.5"  | BWR  | 800×480 | **clear** | two planes, flipped, inverted first plane; framed | 96 000 (100 504 framed) | `…92:83:09:53` | picture correct, 419 blocks, ~45 s |

2026-11-20: the clean-room rewrite of both gateways (from this document) was run on the hardware with
`…92:81:34:41` (SDK 75, transfer OK twice, `05 08` received). The other tags were out of range that day, so
the other rows are still verified by the earlier code only.

The "colours" column also gives the Živý obraz `colorType` the gateway reports: BW → `BW`, BWR → `3C`,
BWRY → `4C`.

## 3. GATT

Primary service `FEF0`:

| char | properties | use |
|---|---|---|
| `FEF1` | write, write-without-response, notify | control: commands out, responses in (notifications) |
| `FEF2` | write, write-without-response, notify | image data blocks |
| `FEF3` | read, write | reads ASCII `DEPG`; not used |

Some firmware puts the same layout at `FDF0`/`FDF1`/`FDF2`, `FCF0`… or `FBF0`… Look up the
characteristics by UUID (`0000f?f1-…`/`0000f?f2-…` with `?` ∈ {e, d, c, b}), not by handle.

* Subscribe to notifications on `FEF1` (required) and on `FEF2` if it supports notify. Responses arrive
  on either one, so treat both as one incoming queue.
* Negotiate the largest MTU the stack allows (247 on both BlueZ and NimBLE). The tag's block size follows it.
* Commands to `FEF1` are sent as write-with-response if the characteristic has `write`.

## 4. Transfer

All integers little-endian. `→` = gateway to tag, `←` = notification from tag.

### 4.1 Sequence

```
→ FEF1  01                                    ask for block size
← FEF1  01 <u16 block_size>                   e.g. 01 F4 00 = 244 at MTU 247
→ FEF1  02 <u32 length> 01 00 00              prepare full refresh. Exactly 8 bytes (see below)
← FEF1  02 00 …                               accepted (any 2nd byte other than 00 = rejected)
→ FEF1  03                                    start
← FEF1  05 00 <u32 index>                     "send from block <index>" (normally 0)
→ FEF2  <u32 index> <data>                    data = payload[index·(block_size−4) … +block_size−4]
   …                                          see 4.3 for how blocks are paced
← FEF1  05 08 …                               all data received; the panel refreshes
```

* `length` is the length of the payload as sent, i.e. **after** framing (§5.5) if the tag needs it.
* `block count = ceil(length / (block_size − 4))`. The last block is short; don't pad it.
* The `02` command **must be 8 bytes**, with the two trailing zeros. With a 6-byte version, tags
  accept every block and answer `05 08` but never refresh the panel.
* The byte after the length (`01`) asks for a full refresh. It's the only mode we use.
* The block-size request may go unanswered. Ask again once before giving up.
* Ignore notifications that don't match what you're waiting for (log them at debug level).
* Seen 2026-11-20 on SDK 75 (`…34:41`): the prepare answer is 3 bytes, `02 00 75`. Only the second byte matters.

### 4.2 Errors and resends

* `← 05 00 <index>` arriving **after** blocks have been sent is a resend request: continue from `index`.
* If the same `index` is requested more than **5** times, abort.
* `index ≥ block count` → abort (protocol error).
* `05 08` is the success signal. Some firmware never sends it after the last block; a missing `05 08`
  after a fully sent stream counts as success (the panel usually refreshes anyway).

### 4.3 Block pacing (streaming firmware, `sw & 0x80`)

All tags in §2 use streaming firmware: after the first `05 00 <index>` the gateway sends every remaining
block back to back without waiting for notifications. The tags differ in how fast they can take them:

| SDK | data writes to `FEF2` | gap after each block | last block | wait for `05 08` after last block |
|---|---|---|---|---|
| 46 | **with response**, every block | 5 ms | with response | 2 s |
| 75, 160, 270 | without response | 40 ms | without response | 20 s |
| 299 | without response | 100 ms | **with response** (a delivery fence) | 60 s |

* SDK 46 silently drops a stream of unacknowledged writes: transfer "succeeds", picture doesn't change.
* SDK 299 doesn't answer acknowledged writes reliably mid-stream, so only the last block asks for a
  response. On the last block of any stream, a **timeout waiting for the write response is not an error**:
  the controller often starts refreshing instead of answering. Go on and wait for `05 08`. Don't resend it.
* A block is never sent twice unless the tag asks for it (§4.2). Resending framed data corrupts the image.
* Measured 2026-11-20 (rewritten Python gateway, BlueZ, SDK 75 `…34:41`): with the 40 ms gap after each
  unacknowledged write, the 125 blocks took 5.1 s (~41 ms per block); `05 08` came 5.9 s after the last block.

### 4.4 Legacy firmware (`sw & 0x80` clear)

Not seen on any tag here, so **unverified**. Supported only as a fallback: send exactly one block per
`← 05 00` notification, then wait (12 s) for the next one. `05 08` after the last block ends the transfer; `05 08` earlier is an error.

### 4.5 Timeouts

| step | timeout | notes |
|---|---|---|
| find the tag by scanning before connecting | 20 s | |
| connect | 20 s | **5 attempts**, backoff 2 s × attempt, rescan before each. Cheap stacks often miss the first connect. On BlueZ the rescan right after a scan sometimes doesn't see the tag within 20 s (seen 2026-11-20); the next attempt does. |
| MTU exchange | 4 s | if it fails, carry on with the default MTU |
| settle after subscribing to notifications | 0.4 s | the first command can get lost without it |
| any single GATT write | 8 s | except the last-block rule in §4.3 |
| block size answer | 4 s | × 2 tries |
| `02` prepare answer | 10 s | |
| first `05 00` after `03` start | 12 s | |
| disconnect | 5 s | errors on disconnect are logged, not raised |
| whole transfer (gateway level) | 240 s | covers the slowest tag (299, ~45 s) plus connect retries |

Only one transfer at a time per radio. A single adapter does one transfer reliably, not several in parallel.

## 5. Image encoding

### 5.1 Input

The protocol layer takes a picture of exactly the canvas size from §2, as **colour indices**:
`0 = white, 1 = black, 2 = red, 3 = yellow` (the same numbering Živý obraz uses for Z2/Z3).
Before encoding:

* BWR panels: yellow (3) is drawn as red (2).
* BW panels: red and yellow are drawn as black. For Živý obraz's `BW` greyscale indices
  (`2` = light grey, `3` = dark grey), light grey → white and dark grey → black.
* The gateways take only Živý obraz's Z2/Z3 images, which are already colour indices. PNG and Z1
  aren't supported (zivyobraz.eu doesn't send them). The only arbitrary pictures come from the Python test CLI
  (`picksmart_tool.py image`), which maps each pixel to the nearest of white `(255,255,255)`, black `(0,0,0)`,
  red `(220,20,12)` and yellow `(244,196,0)` by squared RGB distance. That is fine for test pictures; it
  is not meant for photos or anti-aliased greys (a mid grey comes out red).

Canvas coordinates: `(x, y)`, origin top left, `x` to the right, as the picture should appear on the
tag held in its normal landscape orientation.

### 5.2 Two-plane encoding (SDK 75, 299)

Payload = **plane A ‖ plane B**, each `w·h/8` bytes. Each plane is 1 bit per pixel, row-major,
the most significant bit first, rows **not** padded (bits run continuously from one row into the next).

| SDK | pixel order | plane A bit = 1 when | plane B bit = 1 when |
|---|---|---|---|
| 75  | canvas order: bit `y·400 + x` | pixel is **white** | pixel is red |
| 299 | **vertically flipped**: bit `(479 − y)·800 + x` | pixel is **not white** | pixel is red |

So for SDK 75, an all-white image is 15 000 × `FF` followed by 15 000 × `00`. For SDK 299 it is 96 000 × `00`.
A red pixel sets its bit in both planes for 299 (not white, red) and only in plane B for 75.

### 5.3 Two bits per pixel, rotated (SDK 46, 270)

The buffer is portrait: `128` pixels wide, `W` rows tall, where `W` = canvas width (296 or 250).

* Canvas `(x, y)` → buffer column `y`, buffer row `W − 1 − x`.
  (The picture is turned 90° counter-clockwise; canvas column 0 ends up as the last buffer row.)
* Pixel number `n = row · 128 + column`. Byte `n / 4`, bits `7−6` for `n mod 4 = 0`, then `5−4`, `3−2`, `1−0`.
* Two-bit values: **black 0, white 1, yellow 2, red 3**. An all-white buffer is all `0x55`.

Example (SDK 46): canvas (0, 0) is byte 9 440, bits 7–6. Canvas (295, 0) is byte 0, bits 7–6.
Canvas (0, 127) is byte 9 471, bits 1–0.

### 5.4 Row pairs (SDK 160, 2.9" TFT)

The SDK names it "250×132 BWR", but it is black and white with 122 visible rows. Worked out by trial on hardware
(things that failed: plain planes showed two stacked copies; 2 bpp stretched the picture 2× along the long edge).

1. Extend the 250×122 canvas to 250×132 with 10 white rows at the bottom.
2. **Transpose**: canvas `(x, y)` → buffer column `y`, buffer row `x`. The buffer is 132 wide and 250 tall.
3. Pack buffer rows in pairs `(2k, 2k+1)`: for column `c = 0 … 131`, emit the bit of row `2k`, then the bit
   of row `2k+1`. That's 264 bits = 33 bytes per pair, MSB first, bit 1 = white. 125 pairs = 4 125 bytes.
4. Append 4 125 zero bytes (a second plane the tag expects but ignores). Total 8 250 bytes.

Examples: canvas (0, 0) is byte 0 bit 7; (1, 0) is byte 0 bit 6; (0, 1) is byte 0 bit 5; (249, 0) is byte
4 092 bit 6.

### 5.5 Stored-block framing (when the raw-planes flag is clear, e.g. SDK 299)

The tag runs a QuickLZ 1.5 streaming decompressor with 64-byte blocks. The gateway doesn't compress.
It sends every block as a QuickLZ **stored** (uncompressed) block, which the decompressor copies verbatim:

```
framed = u32 plane_length
       ‖ blocks(plane A) ‖ blocks(plane B)          two-plane types (odd SDK type)
       ‖ blocks(payload)                            single-buffer types (even SDK type)
blocks(p) = for each 64-byte chunk c of p (the last may be shorter):  74  <len(c)+3>  <len(c)>  c
```

* `plane_length` = length of one plane (half the payload for odd SDK types, the whole payload for even ones).
* `0x74` = header byte for a stored block with the tag's compression settings.
* SDK 299: 4 + 2 × (48 000 + 750 × 3) = **100 504** bytes. That's the `length` in the `02` command.

## 6. Checking a new or changed encoding

`python/picksmart_tool.py test <addr> --layout <spec> --pattern ruler` sends a labelled test pattern. Look at the
tag, then record the result in §2/§5 before changing code. Both implementations have to produce identical
bytes; `esp32/test/host` checks the C against the Python.
