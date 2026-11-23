/* Firmware updates from GitHub releases: see update.h. */
#include "update.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway.h"
#include "status_led.h"
#include "version.h"

static const char *TAG = "update";

#define GITHUB "https://github.com/" CONFIG_ZO_OTA_REPO
#define FIRST_CHECK_SECONDS 300 /* after boot: let the polls and the firmware confirmation go first */
#define HTTP_TIMEOUT_MS 20000
#define LATEST_MAX 1024
/* GitHub answers a download with a redirect to a signed URL of about 1 kB */
#define RX_BUFFER 4096
#define TX_BUFFER 2048

#ifndef CONFIG_ZO_OTA_CHECK_HOURS
#define CONFIG_ZO_OTA_CHECK_HOURS 0
#endif

static SemaphoreHandle_t s_lock;
static upd_status_t s_st;
static bool s_check_wanted, s_install_wanted;
static char s_tag[40], s_file[96];
static char s_announced[32]; /* version already logged as available */

static void set_state(upd_state_t state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(upd_state_t state, const char *fmt, ...)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = state;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s_st.message, sizeof(s_st.message), fmt, ap);
        va_end(ap);
    } else {
        s_st.message[0] = 0;
    }
    xSemaphoreGive(s_lock);
}

void update_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_image_sig_public_key_digests_t keys = {0};
    s_st.signed_app = esp_secure_boot_get_signature_blocks_for_running_app(false, &keys) == ESP_OK;
    s_st.state = CONFIG_ZO_OTA_REPO[0] ? UPD_IDLE : UPD_OFF;
    if (!s_st.signed_app) {
        ESP_LOGW(TAG, "this firmware isn't signed: it can't verify updates, so it refuses them");
    }
}

void update_status(upd_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

void update_request_check(void)
{
    if (s_st.state != UPD_OFF) {
        s_check_wanted = true;
        gw_wake();
    }
}

bool update_request_install(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_st.state == UPD_AVAILABLE && s_st.signed_app;
    xSemaphoreGive(s_lock);
    if (ok) {
        s_install_wanted = true;
        gw_wake();
    }
    return ok;
}

/* ---- check: latest.json ------------------------------------------------------------------ */

typedef struct {
    char buf[LATEST_MAX];
    size_t n;
    bool too_big;
} body_t;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    /* a redirect's body (if any) comes before the final one: start again on every new answer */
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED || evt->event_id == HTTP_EVENT_REDIRECT) {
        b->n = 0, b->too_big = false;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (b->n + (size_t)evt->data_len >= sizeof(b->buf)) {
            b->too_big = true;
        } else {
            memcpy(b->buf + b->n, evt->data, (size_t)evt->data_len);
            b->n += (size_t)evt->data_len;
        }
    }
    return ESP_OK;
}

/* a tag or file name that can go into the download URL as it is */
static bool url_safe(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (!n || n >= max || s[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

static const char *str(const cJSON *root, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void check(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    set_state(UPD_CHECKING, NULL);
    body_t *b = calloc(1, sizeof(body_t));
    esp_http_client_config_t cfg = {
        .url = GITHUB "/releases/latest/download/latest.json",
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = b,
        .buffer_size = RX_BUFFER,
        .buffer_size_tx = TX_BUFFER,
    };
    esp_http_client_handle_t c = b ? esp_http_client_init(&cfg) : NULL;
    if (!c) {
        free(b);
        set_state(UPD_FAILED, "check: out of memory");
        return;
    }
    esp_err_t e = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    cJSON *root = NULL;
    b->buf[b->n] = 0;
    if (e != ESP_OK) {
        set_state(UPD_FAILED, "check: can't reach github.com (%s)", esp_err_to_name(e));
    } else if (status == 404) {
        set_state(UPD_FAILED, "check: no release with latest.json in %s", CONFIG_ZO_OTA_REPO);
    } else if (status != 200) {
        set_state(UPD_FAILED, "check: github.com answered %d", status);
    } else if (b->too_big || !(root = cJSON_Parse(b->buf))) {
        set_state(UPD_FAILED, "check: latest.json is not what release.sh makes");
    } else {
        const char *project = str(root, "project"), *version = str(root, "version");
        const char *tag = str(root, "tag"), *file = str(root, "file");
        if (strcmp(project, app->project_name) != 0) {
            set_state(UPD_FAILED, "check: latest.json is for \"%.32s\"", project);
        } else if (!version[0] || strlen(version) >= sizeof(s_st.latest) || !url_safe(tag, sizeof(s_tag)) ||
                   !url_safe(file, sizeof(s_file))) {
            set_state(UPD_FAILED, "check: latest.json is incomplete");
        } else {
            snprintf(s_tag, sizeof(s_tag), "%s", tag);
            snprintf(s_file, sizeof(s_file), "%s", file);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snprintf(s_st.latest, sizeof(s_st.latest), "%s", version);
            xSemaphoreGive(s_lock);
            if (version_cmp(version, app->version) > 0) {
                set_state(UPD_AVAILABLE, NULL);
                if (strcmp(s_announced, version) != 0) {
                    snprintf(s_announced, sizeof(s_announced), "%s", version);
                    gw_log("firmware %s is available (running %s): gateway settings -> Install", version,
                           app->version);
                }
            } else {
                set_state(UPD_LATEST, NULL);
            }
        }
    }
    cJSON_Delete(root);
    free(b);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.checked = gw_uptime();
    bool failed = s_st.state == UPD_FAILED;
    char msg[sizeof(s_st.message)];
    memcpy(msg, s_st.message, sizeof(msg));
    xSemaphoreGive(s_lock);
    if (failed) {
        gw_log("update %s", msg);
    }
}

/* ---- install --------------------------------------------------------------------------------- */

static void install(void)
{
    /* sized from its parts (the sizeofs count a NUL each: room for the '/' and the final NUL), so a
     * long ZO_OTA_REPO can't truncate it */
    char version[32], url[sizeof(GITHUB "/releases/download/") + sizeof(s_tag) + sizeof(s_file)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(version, sizeof(version), "%s", s_st.latest);
    s_st.done = s_st.total = 0;
    xSemaphoreGive(s_lock);
    snprintf(url, sizeof(url), GITHUB "/releases/download/%s/%s", s_tag, s_file);
    set_state(UPD_INSTALLING, NULL);
    gw_log("installing firmware %s from %s", version, CONFIG_ZO_OTA_REPO);
    status_led_set(STATUS_BUSY);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = RX_BUFFER,
        .buffer_size_tx = TX_BUFFER,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&cfg, &h);
    const char *problem = NULL;
    char why[96] = "";
    esp_app_desc_t desc;
    if (e != ESP_OK) {
        snprintf(why, sizeof(why), "download failed (%s)", esp_err_to_name(e));
        problem = why;
    } else if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) {
        problem = "not a firmware file";
    } else if (strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        problem = "not a zivyobraz-ble-gateway firmware";
    } else if (strcmp(desc.version, version) != 0) {
        snprintf(why, sizeof(why), "the file is version %.20s, not %.20s", desc.version, version);
        problem = why;
    }
    while (!problem) {
        e = esp_https_ota_perform(h);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.done = (uint32_t)esp_https_ota_get_image_len_read(h);
        s_st.total = (uint32_t)esp_https_ota_get_image_size(h);
        xSemaphoreGive(s_lock);
        if (e == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            continue;
        }
        if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
            snprintf(why, sizeof(why), "download interrupted (%s)", esp_err_to_name(e));
            problem = why;
        }
        break;
    }
    if (problem) {
        if (h) {
            esp_https_ota_abort(h);
        }
    } else {
        e = esp_https_ota_finish(h); /* verifies the image and its signature, sets the boot partition */
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
            problem = "the file isn't signed with this gateway's key (or is damaged)";
        } else if (e != ESP_OK) {
            snprintf(why, sizeof(why), "can't finish (%s)", esp_err_to_name(e));
            problem = why;
        }
    }
    status_led_set(STATUS_OK);
    if (problem) {
        set_state(UPD_FAILED, "install %s: %s", version, problem);
        gw_log("update failed: %s", problem);
        return;
    }
    set_state(UPD_RESTARTING, NULL);
    gw_log("firmware %s installed, restarting", version);
    vTaskDelay(pdMS_TO_TICKS(2000)); /* the page shows "restarting" meanwhile */
    esp_restart();
}

void update_service(void)
{
    if (s_st.state == UPD_OFF) {
        return;
    }
    if (s_install_wanted) {
        s_install_wanted = false;
        install();
        return;
    }
    int64_t now = gw_uptime(), period = (int64_t)CONFIG_ZO_OTA_CHECK_HOURS * 3600;
    bool due = period > 0 && (s_st.checked ? now - s_st.checked >= period : now >= FIRST_CHECK_SECONDS);
    if (s_check_wanted || due) {
        s_check_wanted = false;
        check();
    }
}
