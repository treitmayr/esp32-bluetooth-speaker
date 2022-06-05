#include "sdkconfig.h"
#ifdef CONFIG_EXAMPLE_OTA_ENABLE

#include "esp_https_ota.h"

#include "wifi_helper.h"
#include "ota_update.h"
#include "syslog_client.h"


void try_ota_update(const char *hostname, const char *app_name, const char *ota_url)
{
    bool connected = wifi_start(hostname, 5000);
    if (connected)
    {
        syslog_client_start_simple(app_name);
        ota_update(ota_url);
        /* wifi_stop(); */
    }
}

#endif /* CONFIG_EXAMPLE_OTA_ENABLE */