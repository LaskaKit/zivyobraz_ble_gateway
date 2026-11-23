/*
 * Živý obraz gateway for Picksmart BLE e-paper tags, on a LaskaKit ESPlan (ESP32 + LAN8720).
 * The network is the Ethernet port only; the 2.4 GHz radio does Bluetooth LE only.
 * Same behaviour as the Python gateway (python -m zivyobraz_ble_gateway); see esp32/README.md.
 */
#include <stdlib.h>
#include <time.h>

#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "update.h"
#include "web.h"

static const char *TAG = "main";

static void mount_storage(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage: %s (the tag list won't be kept)", esp_err_to_name(err));
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_ZO_NTP_SERVER);
    esp_netif_sntp_init(&sntp);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_ZO_HOSTNAME);
        mdns_instance_name_set("Zivy obraz BLE Gateway");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

/* A new firmware counts as good once it has run for a minute (else the bootloader rolls back). */
static void confirm_firmware(void *arg)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        gw_log("new firmware confirmed");
    }
}

void app_main(void)
{
    status_led_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    setenv("TZ", CONFIG_ZO_TIMEZONE, 1);
    tzset();

    mount_storage();
    gw_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    err = eth_start(CONFIG_ZO_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet failed to start: %s", esp_err_to_name(err));
        status_led_set(STATUS_NO_NET);
    }
    update_init();
    ESP_ERROR_CHECK(web_start());
    gw_start();

    const esp_timer_create_args_t confirm = {.callback = confirm_firmware, .name = "confirm"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&confirm, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 60 * 1000000LL);
    }
    gw_log("zivyobraz-ble-gateway %s started", esp_app_get_description()->version);
}
