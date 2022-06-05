idf_component_register(
    SRCS "syslog_client.c"
    INCLUDE_DIRS include
    REQUIRES log lwip mdns
    # PRIV_REQUIRES
)
