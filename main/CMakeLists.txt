file(GLOB SOURCES *.c *.cpp)

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS "."
                    REQUIRES bt esp_netif esp_http_client esp_https_ota
                    EMBED_TXTFILES ${project_dir}/data/ca_cert.pem ${project_dir}/data/wifi_credentials.txt)