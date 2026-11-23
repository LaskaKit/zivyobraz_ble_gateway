# zo_request.c and zo_image.c are portable (also built by esp32/test/host); zo_http.c is ESP-IDF.
idf_component_register(SRCS "zo_request.c" "zo_image.c" "zo_http.c"
                       INCLUDE_DIRS "include"
                       REQUIRES picksmart
                       PRIV_REQUIRES esp_http_client mbedtls)
