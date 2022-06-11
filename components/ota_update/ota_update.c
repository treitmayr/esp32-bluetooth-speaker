#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"

#include "ota_update.h"

static const char *TAG = "OTA";

static const char* nvs_namespace = "OTA-UPDATER";
static const char* key_last_modified = "Last-Modified";
static char* value_last_modified = NULL;


static esp_err_t _http_event_handler_head(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
        case HTTP_EVENT_ON_CONNECTED:
            if (value_last_modified)
            {
                free(value_last_modified);
                value_last_modified = NULL;
            }
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (strcasecmp(evt->header_key, key_last_modified) == 0)
            {
                if (value_last_modified)
                {
                    free(value_last_modified);
                }
                value_last_modified = strdup(evt->header_value);
                ESP_LOGI(TAG, "found '%s' header: %s", key_last_modified, value_last_modified);
            }
            break;
        default:
            break;
    }
    return ESP_OK;      /* return value is unused by caller */
}


static esp_err_t _http_event_handler_update(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ota_mark_application_ok();
            break;
        default:
            break;
    }
    return ESP_OK;      /* return value is unused by caller */
}


static char *get_last_modified_from_url(const char *url)
{
    esp_err_t err;
    char *result = NULL;

    ESP_LOGI(TAG, "Fetching '%s' header from %s", key_last_modified, url);

    esp_http_client_config_t http_config =
    {
        .url = url,
        .event_handler = _http_event_handler_head,
        .keep_alive_enable = true,
    };

    /* Initiate HTTP Connection */
    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (http_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP connection");
        goto failure;
    }
    esp_http_client_set_method(http_client, HTTP_METHOD_HEAD);
    err = esp_http_client_perform(http_client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(http_client);
        if (status != HttpStatus_Ok)
        {
            ESP_LOGE(TAG, "Received incorrect http status %d", status);
            goto http_cleanup;
        }
        if (value_last_modified)
        {
            ESP_LOGI(TAG, "Update image last modified at %s", value_last_modified);
            result = value_last_modified;
            value_last_modified = NULL;
        }
        else
        {
            ESP_LOGE(TAG, "Did not receive 'Last-modified' header");
            goto http_cleanup;
        }
    }
    else
    {
        ESP_LOGE(TAG, "ESP HTTP client perform failed: %d", err);
        goto http_cleanup;
    }

http_cleanup:
    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);
failure:
    return result;
}


static char *get_last_modified_from_nvs()
{
    char *result = NULL;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(nvs_namespace, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /* allocate namespace in NVS */
        ESP_ERROR_CHECK(nvs_open(nvs_namespace, NVS_READWRITE, &nvs_handle));
    }
    else if (err != ESP_OK)
    {
        ESP_ERROR_CHECK(err);
    }

    size_t required_size;
    err = nvs_get_str(nvs_handle, key_last_modified, NULL, &required_size);
    if (err == ESP_OK)
    {
        result = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, key_last_modified, result, &required_size));
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_ERROR_CHECK(err);
    }
    nvs_close(nvs_handle);

    return result;
}


static void set_last_modified_in_nvs(const char *value)
{
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(nvs_namespace, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, key_last_modified, value));
    nvs_close(nvs_handle);
}


static bool perform_ota_update(const char *ota_url)
{
    bool result = false;
    ESP_LOGI(TAG, "Trying to contact OTA server at %s", ota_url);

    char *last_modified_rem = get_last_modified_from_url(ota_url);
    if (last_modified_rem)
    {
        char *last_modified_loc = get_last_modified_from_nvs();
        if (!last_modified_loc || (strcmp(last_modified_rem, last_modified_loc) != 0))
        {
            /* perform actual update */
            esp_http_client_config_t config = {
                .url = ota_url,
                .event_handler = _http_event_handler_update,
                .keep_alive_enable = true,
            };
            esp_err_t ret = esp_https_ota(&config);

            if (ret == ESP_OK)
            {
                ESP_LOGW(TAG, "Peformed firmware update to firmware from %s", last_modified_rem);
                set_last_modified_in_nvs(last_modified_rem);
                result = true;
            }
            if (last_modified_loc)
            {
                free(last_modified_loc);
            }
        }
        free(last_modified_rem);
    }
    return result;
}


bool ota_update(const char *ota_url)
{
    bool reboot = perform_ota_update(ota_url);
    if (!reboot)
    {
        ESP_LOGI(TAG, "No firmware update performed");
    }
    return reboot;
}


void ota_mark_application_ok()
{
    static bool called = false;
    if (!called)
    {
        called = true;
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        ESP_LOGI(TAG, "Mark current firmware as OK");
        esp_ota_mark_app_valid_cancel_rollback();
#endif
    }
}