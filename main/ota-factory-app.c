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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    for( ;; )
    {
        try_ota_update("speaker-factory-updater");
        // retry after one minute
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}

#endif  // #ifdef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE