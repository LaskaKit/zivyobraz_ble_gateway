idf_component_register(SRCS "picksmart_ble.c"
                       INCLUDE_DIRS "include"
                       REQUIRES picksmart esp_common
                       PRIV_REQUIRES bt esp_timer)
