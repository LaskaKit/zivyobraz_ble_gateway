/* The ESPlan's SK6812: red = no network, green = running, blue = sending to a tag. */
#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_ZO_STATUS_LED
#include "esp_log.h"
#include "led_strip.h"

static led_strip_handle_t s_strip;

void status_led_init(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = CONFIG_ZO_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&strip, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGW("led", "no status LED");
        s_strip = NULL;
    }
    status_led_set(STATUS_BOOT);
}

void status_led_set(status_t s)
{
    static const uint8_t rgb[][3] = {
        [STATUS_BOOT] = {8, 6, 0}, [STATUS_NO_NET] = {12, 0, 0}, [STATUS_OK] = {0, 6, 0}, [STATUS_BUSY] = {0, 0, 16},
    };
    if (s_strip) {
        led_strip_set_pixel(s_strip, 0, rgb[s][0], rgb[s][1], rgb[s][2]);
        led_strip_refresh(s_strip);
    }
}
#else
void status_led_init(void) {}
void status_led_set(status_t s) { (void)s; }
#endif
