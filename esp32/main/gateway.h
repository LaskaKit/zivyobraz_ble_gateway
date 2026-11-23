/*
 * The gateway: registered tags, tags in range, the poll scheduler and the BLE transfers.
 * Port of python/zivyobraz_ble_gateway/{service,registry}.py.
 *
 * Tasks: the gateway task (HTTP + transfers, one at a time), the NimBLE host task (adverts),
 * the web server task (reads a snapshot, calls gw_add / gw_remove / ...). Shared state is
 * guarded by gw_lock().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "picksmart_ble.h"

#define GW_MAX_TAGS 32
#define GW_MAX_SEEN 48
#define GW_LOG_LINES 40
#define GW_LOG_LEN 120

typedef struct {
    /* saved in /data/tags.json (same keys as the Python gateway's data/tags.json) */
    char mac[18];
    char api_key[9];  /* X-API-Key: what zivyobraz.eu knows the tag by */
    uint8_t addr_type;
    uint16_t sdk_type, raw_type;
    uint8_t sw, hw;
    char timestamp[32]; /* zivyobraz Timestamp of the image on the tag */
    bool rotate;
    int64_t added;      /* epoch seconds, 0 = unknown */
    /* runtime (times are uptime seconds, 0 = never) */
    uint8_t battery_dv;
    int8_t rssi;
    int64_t last_seen, last_poll, next_poll, last_push;
    char poll_result[96], push_result[112];
    uint16_t push_failures;
    bool forced;
    uint32_t preview_version; /* 0 = no preview */
} gw_tag_t;

typedef struct {
    psb_advert_t adv;
    int64_t seen_at; /* uptime seconds */
} gw_seen_t;

typedef struct {
    int min_interval, max_interval;
} gw_settings_t;

void gw_init(void);   /* load settings and tags (filesystem mounted) */
void gw_start(void);  /* BLE scanner + the gateway task */
void gw_wake(void);   /* make the gateway task look at its work now (web page actions) */

void gw_lock(void);
void gw_unlock(void);
/* Copies for the web page (malloc'ed, free them). */
gw_tag_t *gw_tags_copy(int *n);
gw_seen_t *gw_seen_copy(int *n);
void gw_busy(char out[18]); /* tag being sent to, "" = none */

/* Web page actions. false + reason when it can't be done. */
bool gw_add(const char *mac, char *err, size_t size);
bool gw_remove(const char *mac);
bool gw_refresh(const char *mac);
bool gw_import(const char *json, int *added, char *err, size_t size);
char *gw_export(void); /* tags.json text, malloc'ed */

gw_settings_t gw_get_settings(void);
void gw_set_settings(gw_settings_t s);

/* preview file of a tag: "/data/p/FFFF92813440.z" (the Z2/Z3 body) */
void gw_preview_path(const char *mac, char *out, size_t size);

/* The log on the page (and on the serial console). */
void gw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Calls fn for each line, newest first. */
void gw_log_each(void (*fn)(void *ctx, const char *line), void *ctx);

int64_t gw_uptime(void);
bool gw_clock_set(void); /* NTP time known */
/* uptime seconds -> "14:05:09" (today), "20.11. 14:05", "3 min ago" (no clock) or "never" */
void gw_when(int64_t t, char *out, size_t size);
