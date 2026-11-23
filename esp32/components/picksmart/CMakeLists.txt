# Portable C (no ESP-IDF API): also built on the PC by esp32/test/host/run.sh.
idf_component_register(SRCS "picksmart_models.c" "picksmart_image.c"
                       INCLUDE_DIRS "include")
