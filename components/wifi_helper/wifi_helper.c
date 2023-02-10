#include "sdkconfig.h"

#include "string.h"
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_pm.h"

#include "wifi_helper.h"

#define MIN(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

static const char *TAG = "WIFIHLP";
static const char *SEPARATORS = " \t\n\r";
extern const uint8_t wifi_credentials_start[] asm("_binary_" CONFIG_WIFI_HELPER_CREDENTIALS_SYMBOL "_start");
extern const uint8_t wifi_credentials_end[] asm("_binary_" CONFIG_WIFI_HELPER_CREDENTIALS_SYMBOL "_end");

static char *wifi_hostname = NULL;
static esp_netif_t *sta_netif;
static SemaphoreHandle_t s_semph_get_ip_addrs;
bool credentials_set;


static inline char *terminated_strncpy(char *dest, size_t dest_max, const char *src, size_t src_len)
{
  size_t copy_count = MIN(dest_max - 1, src_len);
  strncpy(dest, src, copy_count);
  dest[copy_count] = '\0';
  return dest;
}


/* create a clean version of the given hostname */
static char *clean_hostname(const char *hostname)
{
    char *hn;
    const char *src;
    char *dst;

    hn = malloc(strlen(hostname) + 1);
    src = hostname;
    dst = hn;
    while (*src)
    {
        if (*src == '-' || *src == '_' ||
            (*src >= '0' && *src <= '9') ||
            (*src >= 'A' && *src <= 'Z') ||
            (*src >= 'a' && *src <= 'z'))
        {
            *dst = *src;
            dst += 1;
        }
        src += 1;
    }
    *dst = '\0';
    return hn;
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
static void set_wifi_credentials()
{
    const char *credentials = (const char *)wifi_credentials_start;
    const size_t n = wifi_credentials_end - wifi_credentials_start;
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
 * ESP WIFI event handlers
 */
static void wifi_event_handler_start(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
    }
}


static void wifi_event_handler_reconnect(void* arg, esp_event_base_t event_base,
                                            int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            const char *note = "";
            bool *credentials_set = (bool *) arg;
            if (!*credentials_set)
            {
                /* if authentication fails, try to reauthenticate with updated credentials */
                set_wifi_credentials();
                *credentials_set = true;
                note = " with updated credentials";
            }
            ESP_LOGI(TAG, "WIFI disconnected, reconnecting%s...", note);
            esp_wifi_connect();
        }
    }
}


static void wifi_event_handler_got_ip(void* arg, esp_event_base_t event_base,
                                       int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
            if (s_semph_get_ip_addrs)
            {
                xSemaphoreGive(s_semph_get_ip_addrs);
            }
        }
    }
}


/**
 * Initialize WIFI and configure it from NVS, if available.
 */
bool wifi_start(const char* hostname, const uint32_t conn_timeout_ms)
{
    wifi_hostname = clean_hostname(hostname);
    ESP_LOGI(TAG, "Starting wifi with host name '%s'", wifi_hostname);

    // see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-lwip-init-phase
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, wifi_hostname));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    credentials_set = false;
    if (!has_sta_configured())
    {
        set_wifi_credentials();
        credentials_set = true;
    }

    s_semph_get_ip_addrs = xSemaphoreCreateCounting(1, 0);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                               wifi_event_handler_start, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               wifi_event_handler_reconnect, (void *) &credentials_set));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler_got_ip, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    bool success = (xSemaphoreTake(s_semph_get_ip_addrs, conn_timeout_ms / portTICK_PERIOD_MS) == pdTRUE);

    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                                 wifi_event_handler_start));
    SemaphoreHandle_t semph_copy = s_semph_get_ip_addrs;
    s_semph_get_ip_addrs = NULL;
    vSemaphoreDelete(semph_copy);
    credentials_set = true;

    if (!success) {
        // timeout
        ESP_LOGW(TAG, "Could not connect to OTA server");
        wifi_stop();
    }

    return success;
}


void wifi_stop(void)
{
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 wifi_event_handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                 wifi_event_handler_reconnect));

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        return;
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif));
    esp_netif_destroy(sta_netif);
    sta_netif = NULL;
    free(wifi_hostname);
    wifi_hostname = NULL;
}