idf_component_register(SRCS "bt_app_ota.c"
                            "bt_app_volume_control.c"
                            "bt_app_av.c"
                            "bt_app_core.c"
                            "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_netif esp_http_client esp_https_ota
                    EMBED_TXTFILES ${project_dir}/data/ca_cert.pem ${project_dir}/data/wifi_credentials.txt)