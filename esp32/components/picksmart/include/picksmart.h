/*
 * Picksmart BLE e-paper labels: the supported panel types, advertisement parsing and the image
 * encodings. Plain C99 with no ESP-IDF dependency, so it also builds on the PC for tests
 * (esp32/test/host), where its output is compared byte for byte with the Python gateway.
 *
 * Written from PROTOCOL.md (section numbers in the comments refer to it); it mirrors
 * python/picksmart/{models,advert,encode,imaging}.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS_COMPANY_ID 0x5053    /* manufacturer data company id (§1) */
#define PS_RAW_DATA_FLAG 0x4000 /* in raw_type: the tag takes the packed image as is, no §5.5 framing */
#define PS_STREAMING_FLAG 0x80  /* in sw: streaming transfer (§4.3) */

/* ---- panel types (§2, python/picksmart/models.py) ------------------------------------------ */

typedef enum { PS_COLOURS_BW, PS_COLOURS_BWR, PS_COLOURS_BWRY } ps_colours_t;

typedef enum {
    PS_ENC_PLANES,         /* §5.2, plane A bit = white, canvas order (SDK 75) */
    PS_ENC_PLANES_FLIPPED, /* §5.2, plane A bit = not white, rows bottom up (SDK 299) */
    PS_ENC_2BPP_ROTATED,   /* §5.3 (SDK 46, 270) */
    PS_ENC_ROW_PAIRS,      /* §5.4 (SDK 160) */
} ps_encoding_t;

/* How a streaming tag takes its data blocks (§4.3). */
typedef struct {
    const char *name;
    bool ack_every_block;  /* every write to FEF2 asks for a write response */
    bool ack_last_block;   /* the last block asks for one (a delivery fence) */
    uint16_t gap_ms;       /* pause after each block */
    uint32_t done_wait_ms; /* wait for `05 08` after the last block */
} ps_pacing_t;

typedef struct {
    uint16_t sdk;
    uint16_t width, height; /* canvas, landscape as the tag is normally held (§5.1) */
    ps_colours_t colours;
    ps_encoding_t encoding;
    const ps_pacing_t *pacing;
    const char *desc; /* e.g. "4.2in 400x300 BWR" (the inch sizes are ours, §2) */
} ps_model_t;

const ps_model_t *ps_model(int sdk_type); /* NULL if not supported */
int ps_sdk_from_raw(int raw_type);        /* §1: & 0x3FFF, else & 0xFF if that is supported */
bool ps_is_bwry(int sdk_type);
bool ps_is_bw(int sdk_type);
const char *ps_color_type(int sdk_type); /* zivyobraz colorType: "BW", "3C" or "4C" */

/* ---- advertisement (§1) ----------------------------------------------------------------------- */

typedef struct {
    uint16_t raw_type;
    uint16_t sdk_type;
    uint8_t battery_dv; /* decivolts */
    uint8_t sw;         /* bit 0x80: streaming firmware */
    uint8_t hw;
} ps_adv_t;

/* `data` is the whole manufacturer-specific AD field payload, company id first (LE). */
bool ps_parse_mfg(const uint8_t *data, size_t len, ps_adv_t *out);

/* ---- picture -> payload (§5) ------------------------------------------------------------------ */

typedef enum { PS_WHITE = 0, PS_BLACK = 1, PS_RED = 2, PS_YELLOW = 3 } ps_color_t; /* §5.1 indices */

/* The §5.1 panel rules: BWR shows yellow as red, BW shows red and yellow as black. */
ps_color_t ps_panel_color(int sdk_type, ps_color_t c);

typedef struct ps_image ps_image_t;

/* The unframed payload of a full-screen update of `sdk_type`, all white. `rotate180` turns the
 * picture upside down (zivyobraz `Rotate` header). Memory is allocated in 4 kB pages, so a big
 * panel doesn't need one large free block. Returns NULL for an unsupported type or out of memory.
 * `ps_image_new_file` keeps those pages in the file `path` (two 4 kB RAM slots as a cache) so a
 * 7.5" picture can live on LittleFS when the heap can't hold ~96 kB. */
ps_image_t *ps_image_new(int sdk_type, bool rotate180);
ps_image_t *ps_image_new_file(int sdk_type, bool rotate180, const char *path);
void ps_image_free(ps_image_t *img);

/* Paint canvas pixels in raster order: pixels pos .. pos+count-1 of the width x height canvas
 * (runs continue across rows). Pixels outside the canvas are ignored. The panel rules apply. */
void ps_image_fill(ps_image_t *img, uint32_t pos, uint32_t count, ps_color_t color);
void ps_image_set(ps_image_t *img, uint32_t x, uint32_t y, ps_color_t color);

uint32_t ps_image_width(const ps_image_t *img); /* canvas = display size */
uint32_t ps_image_height(const ps_image_t *img);
int ps_image_sdk(const ps_image_t *img);

/* The bytes sent over BLE. `framed` = wrapped in §5.5 stored blocks (tag without the raw flag).
 * The image keeps a page cache, so reading isn't thread-safe; use one task. */
size_t ps_image_payload_len(const ps_image_t *img, bool framed);
void ps_image_read(const ps_image_t *img, bool framed, size_t offset, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
