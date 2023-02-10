idf_component_register(
    SRCS "wifi_helper.c"
    INCLUDE_DIRS include
    REQUIRES log
    PRIV_REQUIRES esp_netif esp_wifi esp_pm esp_hw_support
)
