/*
 * Streaming decoder for the zivyobraz image body (python/zivyobraz_ble_gateway/zivyobraz.py decode_image).
 *
 *   Z2: colour = b >> 6, count = b & 0x3F
 *   Z3: colour = b >> 5, count = b & 0x1F
 * Runs are palette indices in raster order and continue across rows. Missing pixels stay
 * white, extra runs are ignored. Palette: 0 white, 1 black, 2 red / light grey, 3 yellow /
 * dark grey (BW panels); other indices are white. Only Z2/Z3 are used; anything else is rejected.
 *
 * Nothing is buffered: every run goes straight to the sink, so a 7.5" picture needs no
 * 384 kB pixel array.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zivyobraz.h"

struct zo_decoder {
    int sdk;
    uint32_t w, h, total, pos;
    zo_sink_fn sink;
    void *ctx;
    zo_format_t fmt;
    uint8_t head[2];
    int head_n;
    ps_color_t lut[8];
    char err[64];
};

const char *zo_format_name(zo_format_t f)
{
    static const char *names[] = {"none", "Z2", "Z3", "unknown"};
    return names[f];
}

zo_decoder_t *zo_decoder_new(int sdk_type, uint32_t width, uint32_t height, zo_sink_fn sink, void *ctx)
{
    zo_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->sdk = sdk_type;
    d->w = width, d->h = height, d->total = width * height;
    d->sink = sink, d->ctx = ctx;
    /* Z index -> what the panel shows (PROTOCOL.md §5.1). BW panels get greyscale indices:
     * 2 = light grey -> white, 3 = dark grey -> black. */
    bool bw = strcmp(ps_color_type(sdk_type), "BW") == 0;
    static const ps_color_t grey[4] = {PS_WHITE, PS_BLACK, PS_WHITE, PS_BLACK};
    for (int i = 0; i < 8; i++) {
        d->lut[i] = i >= 4 ? PS_WHITE : bw ? grey[i] : ps_panel_color(sdk_type, (ps_color_t)i);
    }
    return d;
}

void zo_decoder_free(zo_decoder_t *d)
{
    if (!d) {
        return;
    }
    free(d);
}

zo_format_t zo_decoder_format(const zo_decoder_t *d) { return d->fmt; }
const char *zo_decoder_error(const zo_decoder_t *d) { return d->err; }

static void run(zo_decoder_t *d, unsigned index, uint32_t count)
{
    if (d->pos >= d->total || !count) {
        d->pos += count;
        return;
    }
    uint32_t n = count < d->total - d->pos ? count : d->total - d->pos;
    ps_color_t c = d->lut[index & 7];
    if (c != PS_WHITE) { /* the canvas starts white */
        d->sink(d->ctx, d->pos, n, c);
    }
    d->pos += count;
}

/* ---- format detection and Z runs ------------------------------------------------------- */

static bool start(zo_decoder_t *d)
{
    if (d->head[0] == 'Z' && (d->head[1] == '2' || d->head[1] == '3')) {
        d->fmt = d->head[1] == '2' ? ZO_FMT_Z2 : ZO_FMT_Z3;
        return true;
    }
    d->fmt = ZO_FMT_UNKNOWN;
    snprintf(d->err, sizeof(d->err), "unsupported image format (starts with %02x %02x)", d->head[0], d->head[1]);
    return false;
}

bool zo_decoder_feed(zo_decoder_t *d, const uint8_t *data, size_t len)
{
    while (d->head_n < 2 && len) {
        d->head[d->head_n++] = *data++;
        len--;
        if (d->head_n == 2 && !start(d)) {
            return false;
        }
    }
    if (!len) {
        return true;
    }
    switch (d->fmt) {
    case ZO_FMT_Z2:
        for (size_t i = 0; i < len; i++) {
            run(d, data[i] >> 6, data[i] & 0x3F);
        }
        return true;
    case ZO_FMT_Z3:
        for (size_t i = 0; i < len; i++) {
            run(d, data[i] >> 5, data[i] & 0x1F);
        }
        return true;
    default: return false;
    }
}

bool zo_decoder_finish(zo_decoder_t *d)
{
    if (d->head_n < 2) {
        snprintf(d->err, sizeof(d->err), d->head_n ? "image too short" : "no image in the answer");
        return false;
    }
    return d->fmt != ZO_FMT_UNKNOWN;
}

void zo_sink_image(void *ctx, uint32_t pos, uint32_t count, ps_color_t c)
{
    ps_image_fill((ps_image_t *)ctx, pos, count, c);
}
