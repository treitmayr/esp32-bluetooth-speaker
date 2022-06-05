file(GLOB SOURCES *.c *.cpp)

idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS "."
                       REQUIRES bt wifi_helper ota_update
                       EMBED_TXTFILES ${project_dir}/data/wifi_credentials.txt)