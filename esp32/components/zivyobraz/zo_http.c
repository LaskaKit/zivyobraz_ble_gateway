/* HTTPS POST to zivyobraz.eu, streaming the body (python/zivyobraz_ble_gateway/zivyobraz.py fetch). */
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "zivyobraz.h"

#define HTTP_TIMEOUT_MS 20000 /* the zivyobraz firmware waits 10 s for the first byte */
#define BODY_MAX 2000000      /* a 960x640 Z2 is ~100 kB; anything this big is wrong */
#define READ_CHUNK 1024

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data) {
        zo_answer_header((zo_answer_t *)evt->user_data, evt->header_key, evt->header_value);
    }
    return ESP_OK;
}

int zo_fetch(const char *url, const char *api_key, const char *json, zo_answer_t *answer,
             zo_want_body_fn want_body, zo_body_fn on_body, void *ctx, char *err, size_t err_size)
{
    zo_answer_init(answer);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = answer,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "X-API-Key", api_key);
    esp_http_client_set_header(c, "Connection", "close");

    int rc = -1;
    int len = (int)strlen(json);
    esp_err_t e = esp_http_client_open(c, len);
    if (e != ESP_OK) {
        snprintf(err, err_size, "can't connect (%s)", esp_err_to_name(e));
        goto out;
    }
    if (esp_http_client_write(c, json, len) != len) {
        snprintf(err, err_size, "sending the request failed");
        goto out;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        snprintf(err, err_size, "no answer");
        goto out;
    }
    answer->status = esp_http_client_get_status_code(c);
    if (answer->status != 200) {
        snprintf(err, err_size, "server answered %d", answer->status);
        goto out;
    }
    if (want_body && want_body(ctx, answer)) {
        static char buf[READ_CHUNK]; /* only the gateway task fetches */
        size_t total = 0;
        for (;;) {
            int n = esp_http_client_read(c, buf, sizeof(buf));
            if (n < 0) {
                snprintf(err, err_size, "reading the image failed");
                goto out;
            }
            if (n == 0) {
                if (!esp_http_client_is_complete_data_received(c)) {
                    snprintf(err, err_size, "image cut off after %u bytes", (unsigned)total);
                    goto out;
                }
                break;
            }
            total += (size_t)n;
            if (total > BODY_MAX) {
                snprintf(err, err_size, "image too big");
                goto out;
            }
            if (!on_body(ctx, (const uint8_t *)buf, (size_t)n)) {
                break; /* the caller has what it needs (or gave up) */
            }
        }
    }
    rc = 0;
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}
