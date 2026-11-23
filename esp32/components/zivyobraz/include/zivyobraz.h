/*
 * The zivyobraz.eu side: the gateway plays one FW 3.0 device per Picksmart tag.
 * Port of python/zivyobraz_ble_gateway/zivyobraz.py; see that file for the protocol notes.
 *
 * zo_request.c and zo_image.c are plain C (built on the PC by esp32/test/host);
 * zo_http.c is the ESP-IDF HTTPS client.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "picksmart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZO_API_VERSION "3.0"
#define ZO_BOARD "Picksmart_BLE"

/* ---- request / answer --------------------------------------------------------------- */

/* The FW 3.0 JSON body for one tag. battery_dv 0 = unknown, rssi 0 = unknown.
 * Returns the length written (truncated output if it doesn't fit, like snprintf). */
int zo_build_request(char *out, size_t size, const char *mac, int sdk_type, int battery_dv, int rssi);

/* An 8-digit key without a leading zero, from a 32-bit random number. */
void zo_api_key(uint32_t random, char out[9]);

typedef struct {
    int status;          /* HTTP status */
    char timestamp[32];  /* content version, "" = none sent */
    int sleep;           /* seconds until the next request; -1 = none sent */
    int sleep_minutes;   /* the older `Sleep` header (minutes), -1 = none */
    bool rotate;         /* `Rotate` header present: picture upside down */
} zo_answer_t;

void zo_answer_init(zo_answer_t *a);
/* Feed one response header (name case-insensitive). */
void zo_answer_header(zo_answer_t *a, const char *name, const char *value);
/* PreciseSleep, else Sleep * 60, else -1. */
int zo_answer_sleep(const zo_answer_t *a);

/* ---- image body ----------------------------------------------------------------------- */

typedef enum { ZO_FMT_NONE, ZO_FMT_Z2, ZO_FMT_Z3, ZO_FMT_UNKNOWN } zo_format_t;

const char *zo_format_name(zo_format_t f);

/* Where decoded pixels go: `count` canvas pixels from raster position `pos` have colour `c`.
 * Runs arrive strictly in raster order. */
typedef void (*zo_sink_fn)(void *ctx, uint32_t pos, uint32_t count, ps_color_t c);

typedef struct zo_decoder zo_decoder_t;

/* A streaming decoder for a `width x height` canvas of panel `sdk_type` (sets the palette).
 * Returns NULL when out of memory. */
zo_decoder_t *zo_decoder_new(int sdk_type, uint32_t width, uint32_t height, zo_sink_fn sink, void *ctx);
/* Feed body bytes. false = can't decode (zo_decoder_error() says why); stop feeding. */
bool zo_decoder_feed(zo_decoder_t *d, const uint8_t *data, size_t len);
/* End of body. false if the body was empty or broken. Short images are fine (rest white). */
bool zo_decoder_finish(zo_decoder_t *d);
zo_format_t zo_decoder_format(const zo_decoder_t *d);
const char *zo_decoder_error(const zo_decoder_t *d);
void zo_decoder_free(zo_decoder_t *d);

/* The sink that paints into a Picksmart payload (ctx = ps_image_t *). */
void zo_sink_image(void *ctx, uint32_t pos, uint32_t count, ps_color_t c);

/* ---- HTTPS (ESP-IDF only, zo_http.c) ---------------------------------------------------- */

/* Called after the headers arrived. Return true to read the body. */
typedef bool (*zo_want_body_fn)(void *ctx, const zo_answer_t *answer);
/* Body bytes, as they arrive. Return false to stop. */
typedef bool (*zo_body_fn)(void *ctx, const uint8_t *data, size_t len);

/* POST `json` to `url` with the tag's X-API-Key. Returns 0, or -1 with a reason in `err`.
 * A status other than 200 is an error (as the zivyobraz firmware treats it). */
int zo_fetch(const char *url, const char *api_key, const char *json, zo_answer_t *answer,
             zo_want_body_fn want_body, zo_body_fn on_body, void *ctx, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
