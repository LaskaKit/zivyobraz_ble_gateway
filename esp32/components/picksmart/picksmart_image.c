/*
 * Canvas pixels -> payload bytes (PROTOCOL.md §5), mirroring python/picksmart/encode.py.
 *
 * The packed payload is kept, not the canvas: every painted pixel goes straight to its bit(s).
 * The payload lives in 4 kB pages, either all in RAM or in a file with two RAM pages as a cache.
 * The §5.5 framing is computed on the fly when the payload is read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picksmart.h"

#define PAGE 4096u
#define SLOTS 2
#define ROW_PAIRS_ROWS 132u /* §5.4 */
#define FRAME_CHUNK 64u     /* §5.5 */
#define FRAME_STORED 0x74

struct ps_image {
    const ps_model_t *m;
    bool rotate;
    size_t len;    /* unframed payload */
    size_t npages;
    uint8_t **pages; /* RAM: every page */
    FILE *file;      /* file: the pages, with a small cache */
    struct {
        long page; /* -1 = empty */
        bool dirty;
        uint8_t *buf;
    } slot[SLOTS];
    int victim;
};

/* ---- geometry ------------------------------------------------------------------------------ */

static size_t payload_len(const ps_model_t *m)
{
    size_t pixels = (size_t)m->width * m->height;
    switch (m->encoding) {
    case PS_ENC_PLANES:
    case PS_ENC_PLANES_FLIPPED: return 2 * (pixels / 8);                     /* §5.2 */
    case PS_ENC_2BPP_ROTATED: return pixels / 4;                             /* §5.3 */
    case PS_ENC_ROW_PAIRS: return 2 * ((size_t)m->width * ROW_PAIRS_ROWS / 8); /* §5.4 */
    }
    return 0;
}

/* The value of each payload byte for an all-white canvas. */
static uint8_t white_byte(const ps_model_t *m, size_t len, size_t offset)
{
    switch (m->encoding) {
    case PS_ENC_PLANES: return offset < len / 2 ? 0xFF : 0x00; /* plane A bit = white */
    case PS_ENC_PLANES_FLIPPED: return 0x00;                    /* plane A bit = not white */
    case PS_ENC_2BPP_ROTATED: return 0x55;                      /* white = 01 */
    case PS_ENC_ROW_PAIRS: return offset < len / 2 ? 0xFF : 0x00;
    }
    return 0;
}

/* ---- pages --------------------------------------------------------------------------------- */

static uint8_t *page(ps_image_t *img, size_t n, bool write)
{
    if (img->pages) {
        return img->pages[n];
    }
    for (int i = 0; i < SLOTS; i++) {
        if (img->slot[i].page == (long)n) {
            img->slot[i].dirty |= write;
            return img->slot[i].buf;
        }
    }
    int i = img->victim;
    img->victim = (img->victim + 1) % SLOTS;
    if (img->slot[i].page >= 0 && img->slot[i].dirty) {
        fseek(img->file, img->slot[i].page * (long)PAGE, SEEK_SET);
        fwrite(img->slot[i].buf, 1, PAGE, img->file);
    }
    fseek(img->file, (long)(n * PAGE), SEEK_SET);
    size_t got = fread(img->slot[i].buf, 1, PAGE, img->file);
    memset(img->slot[i].buf + got, 0, PAGE - got);
    img->slot[i].page = (long)n;
    img->slot[i].dirty = write;
    return img->slot[i].buf;
}

static uint8_t get_byte(ps_image_t *img, size_t offset)
{
    return page(img, offset / PAGE, false)[offset % PAGE];
}

static void put_bit(ps_image_t *img, size_t bit, bool one)
{
    uint8_t *p = &page(img, bit / 8 / PAGE, true)[bit / 8 % PAGE];
    uint8_t mask = (uint8_t)(0x80 >> (bit % 8));
    *p = one ? (uint8_t)(*p | mask) : (uint8_t)(*p & ~mask);
}

static void put_2bits(ps_image_t *img, size_t n, unsigned value)
{
    uint8_t *p = &page(img, n / 4 / PAGE, true)[n / 4 % PAGE];
    unsigned shift = 6 - 2 * (unsigned)(n % 4);
    *p = (uint8_t)((*p & ~(3u << shift)) | value << shift);
}

static void fill_white(ps_image_t *img, uint8_t *buf, size_t first, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = first + i < img->len ? white_byte(img->m, img->len, first + i) : 0;
    }
}

static ps_image_t *create(int sdk_type, bool rotate180)
{
    const ps_model_t *m = ps_model(sdk_type);
    if (!m) {
        return NULL;
    }
    ps_image_t *img = calloc(1, sizeof(*img));
    if (!img) {
        return NULL;
    }
    img->m = m;
    img->rotate = rotate180;
    img->len = payload_len(m);
    img->npages = (img->len + PAGE - 1) / PAGE;
    for (int i = 0; i < SLOTS; i++) {
        img->slot[i].page = -1;
    }
    return img;
}

ps_image_t *ps_image_new(int sdk_type, bool rotate180)
{
    ps_image_t *img = create(sdk_type, rotate180);
    if (!img) {
        return NULL;
    }
    img->pages = calloc(img->npages, sizeof(uint8_t *));
    if (!img->pages) {
        free(img);
        return NULL;
    }
    for (size_t i = 0; i < img->npages; i++) {
        img->pages[i] = malloc(PAGE);
        if (!img->pages[i]) {
            ps_image_free(img);
            return NULL;
        }
        fill_white(img, img->pages[i], i * PAGE, PAGE);
    }
    return img;
}

ps_image_t *ps_image_new_file(int sdk_type, bool rotate180, const char *path)
{
    ps_image_t *img = create(sdk_type, rotate180);
    if (!img) {
        return NULL;
    }
    img->file = fopen(path, "w+b");
    for (int i = 0; i < SLOTS && img->file; i++) {
        img->slot[i].buf = malloc(PAGE);
        if (!img->slot[i].buf) {
            break;
        }
    }
    if (!img->file || !img->slot[SLOTS - 1].buf) {
        ps_image_free(img);
        return NULL;
    }
    uint8_t *buf = img->slot[0].buf;
    for (size_t i = 0; i < img->npages; i++) {
        fill_white(img, buf, i * PAGE, PAGE);
        if (fwrite(buf, 1, PAGE, img->file) != PAGE) {
            ps_image_free(img);
            return NULL;
        }
    }
    return img;
}

void ps_image_free(ps_image_t *img)
{
    if (!img) {
        return;
    }
    if (img->pages) {
        for (size_t i = 0; i < img->npages; i++) {
            free(img->pages[i]);
        }
        free(img->pages);
    }
    if (img->file) {
        fclose(img->file);
    }
    for (int i = 0; i < SLOTS; i++) {
        free(img->slot[i].buf);
    }
    free(img);
}

/* ---- painting (§5.2–§5.4) ------------------------------------------------------------------ */

void ps_image_set(ps_image_t *img, uint32_t x, uint32_t y, ps_color_t color)
{
    const ps_model_t *m = img->m;
    uint32_t w = m->width, h = m->height;
    if (x >= w || y >= h) {
        return;
    }
    if (img->rotate) {
        x = w - 1 - x, y = h - 1 - y;
    }
    ps_color_t c = ps_panel_color(m->sdk, color);
    size_t plane_bits = img->len / 2 * 8;
    switch (m->encoding) {
    case PS_ENC_PLANES: { /* bit y*w + x; A = white, B = red */
        size_t n = (size_t)y * w + x;
        put_bit(img, n, c == PS_WHITE);
        put_bit(img, plane_bits + n, c == PS_RED);
        break;
    }
    case PS_ENC_PLANES_FLIPPED: { /* bit (h-1-y)*w + x; A = not white, B = red */
        size_t n = (size_t)(h - 1 - y) * w + x;
        put_bit(img, n, c != PS_WHITE);
        put_bit(img, plane_bits + n, c == PS_RED);
        break;
    }
    case PS_ENC_2BPP_ROTATED: { /* buffer h wide: column y, row w-1-x; black 0 white 1 yellow 2 red 3 */
        static const uint8_t two_bit[4] = {1, 0, 3, 2};
        size_t n = (size_t)(w - 1 - x) * h + y;
        put_2bits(img, n, two_bit[c]);
        break;
    }
    case PS_ENC_ROW_PAIRS: /* transpose; rows (2k, 2k+1) interleaved per column; bit 1 = white */
        put_bit(img, (size_t)(x / 2) * (2 * ROW_PAIRS_ROWS) + 2 * y + (x & 1), c == PS_WHITE);
        break;
    }
}

void ps_image_fill(ps_image_t *img, uint32_t pos, uint32_t count, ps_color_t color)
{
    uint32_t w = img->m->width, total = w * img->m->height;
    for (uint32_t p = pos; p < pos + count && p < total; p++) {
        ps_image_set(img, p % w, p / w, color);
    }
}

uint32_t ps_image_width(const ps_image_t *img) { return img->m->width; }
uint32_t ps_image_height(const ps_image_t *img) { return img->m->height; }
int ps_image_sdk(const ps_image_t *img) { return img->m->sdk; }

/* ---- reading, with the §5.5 framing ------------------------------------------------------------ */

/* Odd SDK types carry two planes, each framed on its own; even types one buffer. */
static size_t frame_plane_len(const ps_image_t *img)
{
    return img->m->sdk % 2 ? img->len / 2 : img->len;
}

static size_t framed_plane_size(size_t plane_len)
{
    size_t chunks = (plane_len + FRAME_CHUNK - 1) / FRAME_CHUNK;
    return plane_len + 3 * chunks;
}

size_t ps_image_payload_len(const ps_image_t *img, bool framed)
{
    if (!framed) {
        return img->len;
    }
    size_t plane = frame_plane_len(img);
    return 4 + (img->len / plane) * framed_plane_size(plane);
}

static uint8_t framed_byte(ps_image_t *img, size_t i)
{
    size_t plane = frame_plane_len(img);
    if (i < 4) { /* u32 plane length; SPEC-GAP: byte order not stated in §5.5, little-endian as §4 */
        return (uint8_t)(plane >> (8 * i));
    }
    size_t j = i - 4, size = framed_plane_size(plane);
    size_t which = j / size, r = j % size;
    size_t chunk = r / (FRAME_CHUNK + 3), within = r % (FRAME_CHUNK + 3);
    size_t start = chunk * FRAME_CHUNK;
    size_t clen = plane - start < FRAME_CHUNK ? plane - start : FRAME_CHUNK;
    switch (within) {
    case 0: return FRAME_STORED;
    case 1: return (uint8_t)(clen + 3);
    case 2: return (uint8_t)clen;
    default: return get_byte(img, which * plane + start + within - 3);
    }
}

void ps_image_read(const ps_image_t *cimg, bool framed, size_t offset, uint8_t *out, size_t len)
{
    ps_image_t *img = (ps_image_t *)cimg; /* the page cache changes; the picture doesn't */
    size_t total = ps_image_payload_len(img, framed);
    for (size_t i = 0; i < len; i++) {
        size_t at = offset + i;
        out[i] = at >= total ? 0 : framed ? framed_byte(img, at) : get_byte(img, at);
    }
}
