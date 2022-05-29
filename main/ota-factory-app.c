#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE

#include "bt_app_ota.h"

#ifndef CONFIG_EXAMPLE_OTA_ENABLE
#error "You need to enable enable OTA updates via menuconfig"
#endif

void app_main(void)
{
    /* initialize NVS — it is used to store PHY calibration data */
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    try_ota_update("speaker-factory-updater");
}

#endif  // #ifdef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE