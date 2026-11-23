/*
 * Firmware updates from GitHub releases (SECURITY.md §3, option B, with A's signatures).
 *
 * release.sh puts latest.json next to the -ota.bin of every release:
 *     {"project": "zivyobraz-ble-gateway", "version": "0.3.0", "tag": "v0.3.0",
 *      "file": "zivyobraz-ble-gateway-esp32-0.3.0-ota.bin"}
 * The gateway task (update_service) fetches
 *     https://github.com/<CONFIG_ZO_OTA_REPO>/releases/latest/download/latest.json
 * every CONFIG_ZO_OTA_CHECK_HOURS or when asked, and if the version is newer than the running one the
 * settings page offers it. Install downloads releases/download/<tag>/<file> with esp_https_ota;
 * esp_ota_end() installs it only if it is signed with the running firmware's key, then the gateway
 * restarts (and rolls back if the new firmware doesn't confirm itself within a minute).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UPD_OFF,        /* no repository configured */
    UPD_IDLE,       /* not checked yet */
    UPD_CHECKING,
    UPD_LATEST,     /* the running version is the newest */
    UPD_AVAILABLE,  /* `latest` is newer */
    UPD_INSTALLING, /* `done` of `total` bytes */
    UPD_RESTARTING,
    UPD_FAILED,     /* `message` says what */
} upd_state_t;

typedef struct {
    upd_state_t state;
    char latest[32];
    char message[96];
    int64_t checked; /* gw_uptime() of the last check, 0 = never */
    uint32_t done, total;
    bool signed_app; /* the running firmware is signed, so it can verify an update */
} upd_status_t;

void update_init(void);
void update_status(upd_status_t *out);
/* from the web page; they wake the gateway task, which does the work in update_service() */
void update_request_check(void);
bool update_request_install(void);
/* gateway task, between polls */
void update_service(void);
