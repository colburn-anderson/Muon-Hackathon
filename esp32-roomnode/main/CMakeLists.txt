idf_component_register(
    SRCS "main.c" "wifi.c" "net.c" "link.c" "api.c" "console.c"
         "../../bsp/common/json/jhelp.c"
    INCLUDE_DIRS "." "../../bsp/common/json"
    # esp_http_client + mbedtls: the real API is HTTPS and esp_crt_bundle lives
    # in mbedtls. This pulled in ~60 KB of certificate bundle; it is the price of
    # not maintaining a socket TLS client by hand.
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_driver_uart esp_driver_gpio
             esp_http_client mbedtls
)
