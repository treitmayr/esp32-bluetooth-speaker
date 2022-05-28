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
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
extern const uint8_t wifi_credentials_txt_start[] asm("_binary_wifi_credentials_txt_start");
extern const uint8_t wifi_credentials_txt_end[] asm("_binary_wifi_credentials_txt_end");

static bool wifi_connected = false;
static const char* wifi_hostname = "default";
static esp_netif_t *sta_netif;

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
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, "thermostat");
        esp_wifi_connect();
        wifi_connected = false;
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
        wifi_connected = false;
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
            wifi_connected = true;
        }
        else if (event_id == IP_EVENT_GOT_IP6)
        {
            ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
            ESP_LOGI(TAG, "got ip6: " IPV6STR, IPV62STR(event->ip6_info.ip));
            wifi_connected = true;
        }
        else if (event_id == IP_EVENT_STA_LOST_IP)
        {
            wifi_connected = false;
        }
    }
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
 * Initialize WIFI and configure it from NVS, if available.
 */
static void wifi_start(const char* hostname)
{
    wifi_hostname = hostname;

    // see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-lwip-init-phase
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL));
}

static inline char *terminated_strncpy(char *dest, size_t dest_max, const char *src, size_t src_len)
{
  size_t copy_count = MIN(dest_max - 1, src_len);
  strncpy(dest, src, copy_count);
  dest[copy_count] = '\0';
  return dest;
}

/**
 * Set new WIFI credentials (automatically stored in NVS).
 */
void set_wifi_credentials(const char *credentials, size_t n)
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
    ESP_LOGI(TAG, "ssid='%s', passphrase='%s'", wifi_config.sta.ssid, wifi_config.sta.password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;    // require WPA2 minimum

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
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
    sta_netif = NULL;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}


static void contact_ota_server()
{
    ESP_LOGI(TAG, "Trying to contact OTA server at " CONFIG_EXAMPLE_OTA_URL);

    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_OTA_URL,
        .cert_pem = (char *)server_cert_pem_start,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };

#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif

    esp_err_t ret = esp_https_ota(&config);
    if (ret == ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
}

void try_ota_update(const char *hostname)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* initialize wifi */
    if (!has_sta_configured())
    {
        set_wifi_credentials((const char *)wifi_credentials_txt_start,
                             wifi_credentials_txt_end - wifi_credentials_txt_start);
    }
    wifi_start(hostname);
    contact_ota_server();
    wifi_stop();
}

