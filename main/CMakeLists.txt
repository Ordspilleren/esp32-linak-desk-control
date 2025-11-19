idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES mqtt nvs_flash esp_netif esp_wifi esp_driver_uart esp_driver_gpio
                    INCLUDE_DIRS ".")