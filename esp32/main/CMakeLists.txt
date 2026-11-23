# The page's look is shared with the Python gateway: ../../web (repo root).
idf_component_register(SRCS "main.c" "eth.c" "gateway.c" "web.c" "guard.c" "update.c" "version.c" "status_led.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../../web/style.css"
                       REQUIRES picksmart picksmart_ble zivyobraz
                       PRIV_REQUIRES esp_eth esp_netif esp_event esp_http_server esp_timer app_update
                                     nvs_flash esp_app_format bootloader_support joltwallet__littlefs
                                     esp_http_client esp_https_ota esp-tls)
