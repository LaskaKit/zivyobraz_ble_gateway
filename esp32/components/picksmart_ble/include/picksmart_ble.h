/*
 * Picksmart labels over NimBLE: a continuous scanner and the image transfer, written from
 * PROTOCOL.md §3–§4 (it mirrors python/picksmart/client.py).
 *
 * One radio, one job at a time: psb_send() stops the scanner, connects, sends, disconnects
 * and starts the scanner again. Call it from one task only. NimBLE callbacks only post to
 * queues; the whole transfer runs in the calling task. Only the PROTOCOL.md §2 types are sent.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "picksmart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char mac[18];      /* "FF:FF:92:81:34:40", upper case, as the Python gateway writes it */
    uint8_t addr_type; /* BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM */
    int8_t rssi;
    ps_adv_t adv;
} psb_advert_t;

/* Called from the NimBLE host task for every Picksmart advertisement. Keep it short. */
typedef void (*psb_advert_fn)(const psb_advert_t *advert);
/* Transfer step messages (the Python gateway's `-v` lines). */
typedef void (*psb_log_fn)(const char *message);

/* Start the controller (BLE only; the classic BT memory is released) and the scanner. */
esp_err_t psb_init(psb_advert_fn on_advert);
bool psb_ready(void); /* host synced with the controller */

typedef struct {
    const char *mac;
    uint8_t addr_type;
    int sdk_type;
    uint8_t sw;         /* from the advertisement: 0x80 = streaming firmware */
    bool framed;        /* tag lacks the 0x4000 raw flag: §5.5 stored-block framing */
    const ps_image_t *image;
    psb_log_fn log;     /* may be NULL */
} psb_transfer_t;

/* Send one picture. 0 = the tag has it (it is refreshing now); -1 = failed, reason in `err`. */
int psb_send(const psb_transfer_t *t, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
