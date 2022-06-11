#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stdint.h"

void syslog_early_buffering_start(uint32_t max_number_lines);
void syslog_early_buffering_stop();

void syslog_client_start(const char *host, unsigned int port,
                         const char *app_name,
                         bool send_raw, bool copy_to_serial);

void syslog_client_start_simple(const char *app_name);

void syslog_client_stop();

#ifdef __cplusplus
}
#endif