#include "sdkconfig.h"
#ifdef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE

#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_helper.h"
#include "ota_update.h"
#include "syslog_client.h"

#ifndef CONFIG_EXAMPLE_OTA_ENABLE
#error "You need to enable enable OTA updates via menuconfig"
#endif

static const char *TAG = "FACT-IMG";


static void try_ota_update()
{
    bool connected = wifi_start("factory-updater", 5000);
    if (connected)
    {
        syslog_client_start_simple("BT-A2DP-Sink");
        if (ota_update(CONFIG_EXAMPLE_OTA_URL))
        {
            ESP_LOGW(TAG, "Rebooting after firmware update");
            esp_restart();
        }
    }
    else
    {
        syslog_early_buffering_stop();
    }
}


void app_main(void)
{
    syslog_early_buffering_start(50);

    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    try_ota_update();
}

#endif  /* #ifdef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE */