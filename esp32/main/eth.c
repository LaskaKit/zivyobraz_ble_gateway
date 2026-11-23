/* Wired Ethernet of the LaskaKit ESPlan: ESP32 internal EMAC + LAN8720A over RMII, DHCP. */
#include "eth.h"

#include <string.h>

#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "eth";
#define GOT_IP BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_link;

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        s_link = true;
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link = false;
        xEventGroupClearBits(s_events, GOT_IP);
        ESP_LOGW(TAG, "link down");
        break;
    default: break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, GOT_IP);
}

esp_err_t eth_start(const char *hostname)
{
    s_events = xEventGroupCreate();

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = CONFIG_ZO_ETH_MDC_GPIO;
    emac.smi_gpio.mdio_num = CONFIG_ZO_ETH_MDIO_GPIO;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_OUT; /* the ESP32 feeds the PHY its 50 MHz */
    emac.clock_config.rmii.clock_gpio = CONFIG_ZO_ETH_CLK_OUT_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ZO_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ZO_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (!mac || !phy) {
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    esp_err_t err = esp_eth_driver_install(&config, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_config);
    esp_netif_set_hostname(s_netif, hostname);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    return esp_eth_start(handle);
}

bool eth_has_ip(void)
{
    return s_events && (xEventGroupGetBits(s_events) & GOT_IP);
}

bool eth_wait_ip(int timeout_ms)
{
    return s_events && (xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP);
}

void eth_status(char *out, size_t size)
{
    esp_netif_ip_info_t ip;
    if (eth_has_ip() && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out, size, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, size, s_link ? "link up, waiting for DHCP" : "no Ethernet link");
    }
}
