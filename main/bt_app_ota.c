#include "sdkconfig.h"
#ifdef CONFIG_EXAMPLE_OTA_ENABLE

#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_https_ota.h"

#include "bt_app_ota.h"

#define DEFAULT_SSID "*********"
#define DEFAULT_PWD "*********"

#define MIN(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

static const char *TAG = "OTA";
static const char *SEPARATORS = " \t\n\r";
extern const uint8_t wifi_credentials_txt_start[] asm("_binary_wifi_credentials_txt_start");
extern const uint8_t wifi_credentials_txt_end[] asm("_binary_wifi_credentials_txt_end");

static const char* wifi_hostname = "default";
static esp_netif_t *sta_netif;
static xSemaphoreHandle s_semph_get_ip_addrs;

static const char* nvs_namespace = "OTA-UPDATER";
static const char* key_last_modified = "Last-Modified";
static char* value_last_modified = NULL;

/**
 * ESP WIFI event handler which is repsonsible for
 * invoking the WIFI sem_status callback.
 */
static void _wifi_event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, wifi_hostname);
            esp_wifi_connect();
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
    }
}

static inline char *terminated_strncpy(char *dest, size_t dest_max, const char *src, size_t src_len)
{
  size_t copy_count = MIN(dest_max - 1, src_len);
  strncpy(dest, src, copy_count);
  dest[copy_count] = '\0';
  return dest;
}

static bool has_sta_configured()
{
    static const uint8_t empty_bssid[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    wifi_config_t config = {};
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &config) != ESP_OK)
    {
        return false;
    }
    ESP_LOGI(TAG, "SSID: %s, BSSID: " MACSTR, config.sta.ssid, MAC2STR(config.sta.bssid));
    return (strlen((const char *)config.sta.ssid) >= 1) ||
          (memcmp(empty_bssid, config.sta.bssid, sizeof(empty_bssid)) != 0);
}

/**
 * Set new WIFI credentials (automatically stored in NVS).
 */
static void set_wifi_credentials(const char *credentials, size_t n)
{
    const char *ssid = NULL;
    size_t ssid_len = 0;
    const char *passphrase = NULL;
    size_t passphrase_len = 0;
    const char *search = credentials;
    size_t rest = n;

    while (rest > 0) {
        if (strchr(SEPARATORS, *search) == NULL) {
            ssid = search;
            break;
        } else {
            search++;
            rest--;
        }
    }
    while (rest > 0) {
        if (strchr(SEPARATORS, *search) == NULL) {
            search++;
            rest--;
            ssid_len++;
        } else {
            break;
        }
    }
    while (rest > 0) {
        if (strchr(SEPARATORS, *search) == NULL) {
            passphrase = search;
            break;
        } else {
            search++;
            rest--;
        }
    }
    while (rest > 0) {
        if (strchr(SEPARATORS, *search) == NULL) {
            search++;
            rest--;
            passphrase_len++;
        } else {
            break;
        }
    }

    wifi_config_t wifi_config;
    memset((void *)&wifi_config, 0, sizeof(wifi_config));
    terminated_strncpy((char *)&wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid, ssid_len);
    terminated_strncpy((char *)&wifi_config.sta.password, sizeof(wifi_config.sta.password), passphrase, passphrase_len);
    ESP_LOGD(TAG, "new wifi settings: ssid='%s', passphrase='%s'", wifi_config.sta.ssid, wifi_config.sta.password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;    // require WPA2 minimum

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
}

/**
 * Initialize WIFI and configure it from NVS, if available.
 */
static bool wifi_start(const char* hostname,
                       const char *credentials,
                       size_t n,
                       uint32_t conn_timeout_ms)
{
    bool success;
    wifi_hostname = hostname;
    s_semph_get_ip_addrs = xSemaphoreCreateCounting(1, 0);

    // see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-lwip-init-phase
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL));

    if (!has_sta_configured())
    {
        set_wifi_credentials(credentials, n);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    success = (xSemaphoreTake(s_semph_get_ip_addrs, conn_timeout_ms / portTICK_PERIOD_MS) == pdTRUE);
    if (!success) {
        // timeout
        ESP_LOGW(TAG, "Could not connect to OTA server");
    }
    return success;
}

static void wifi_stop(void)
{
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler));

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        return;
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif));
    esp_netif_destroy(sta_netif);
    vSemaphoreDelete(s_semph_get_ip_addrs);
    s_semph_get_ip_addrs = NULL;
    sta_netif = NULL;
    wifi_hostname = NULL;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            if (value_last_modified)
            {
                free(value_last_modified);
                value_last_modified = NULL;
            }
            break;
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
                ESP_LOGI(TAG, "found 'Last-modified' header: %s", value_last_modified);
            }
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

    ESP_LOGI(TAG, "Fetching 'Last-Modified' header from %s", url);

    esp_http_client_config_t http_config =
    {
        .url = url,
        .event_handler = _http_event_handler,
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

static bool perform_ota_update()
{
    bool result = false;
    ESP_LOGI(TAG, "Trying to contact OTA server at " CONFIG_EXAMPLE_OTA_URL);

    char *last_modified_rem = get_last_modified_from_url(CONFIG_EXAMPLE_OTA_URL);
    if (last_modified_rem)
    {
        char *last_modified_loc = get_last_modified_from_nvs();
        if (!last_modified_loc || (strcmp(last_modified_rem, last_modified_loc) != 0))
        {
            /* perform actual update */
            esp_http_client_config_t config = {
                .url = CONFIG_EXAMPLE_OTA_URL,
                .keep_alive_enable = true,
            };
            esp_err_t ret = esp_https_ota(&config);

            if (ret == ESP_OK)
            {
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

void try_ota_update(const char *hostname)
{
    bool connected;
    bool reboot = false;
    char *hn;
    const char *src;
    char *dst;

    /* create a filtered version of the name */
    hn = malloc(strlen(hostname) + 1);
    src = hostname;
    dst = hn;
    while (*src)
    {
        if (*src == '-' || *src == '_' ||
            (*src >= '0' && *src <= '9') ||
            (*src >= 'A' && *src <= 'Z') ||
            (*src >= 'a' && *src <= 'Z'))
        {
            *dst = *src;
            dst += 1;
        }
        src += 1;
    }
    *dst = '\0';

    ESP_LOGI(TAG, "Starting wifi as station '%s'", hn);
    connected = wifi_start(hn,
                           (const char *)wifi_credentials_txt_start,
                           wifi_credentials_txt_end - wifi_credentials_txt_start,
                           5000);

    if (connected)
    {
        reboot = perform_ota_update();
    }
    wifi_stop();
    free(hn);
    if (reboot)
    {
        ESP_LOGW(TAG, "Rebooting after firmware update");
        esp_restart();
    }
    else
    {
        ESP_LOGI(TAG, "No firmware update performed");
    }
}

#endif /* CONFIG_EXAMPLE_OTA_ENABLE */