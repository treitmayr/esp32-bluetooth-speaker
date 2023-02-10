idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS include
    REQUIRES esp_http_client esp_https_ota app_update nvs_flash esp_event
    # PRIV_REQUIRES 
)
