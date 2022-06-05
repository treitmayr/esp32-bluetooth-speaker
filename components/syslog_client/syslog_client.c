#include "syslog_client.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <string.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "mdns.h"

#define SYSLOG_USE_STACK
// #define SYSLOG_UTF8
#define MAX_PAYLOAD_LEN 200
#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))

/* from https://datatracker.ietf.org/doc/html/rfc5424#section-6 */

#define SYSLOG_NILVALUE "-"
#define SYSLOG_VERSION "1"
#define SYSLOG_SP " "
#define SYSLOG_TIMESTAMP SYSLOG_NILVALUE
#define SYSLOG_MSGID SYSLOG_NILVALUE
#define SYSLOG_STRUCTURED_DATA SYSLOG_NILVALUE
#ifdef SYSLOG_UTF8
#define SYSLOG_BOM "\xEF\xBB\xBF"         /* UTF-8 byte order mask */
#else
#define SYSLOG_BOM ""
#endif

#define SYSLOG_TEMPLATE "<%%d>" SYSLOG_VERSION SYSLOG_SP \
                        SYSLOG_TIMESTAMP SYSLOG_SP \
                        "%s" SYSLOG_SP \
                        "%s" SYSLOG_SP \
                        "%%s" SYSLOG_SP \
                        SYSLOG_MSGID SYSLOG_SP \
                        SYSLOG_STRUCTURED_DATA SYSLOG_SP \
                        SYSLOG_BOM \
                        "%%s"

#define SYSLOG_TEMPLATE_FALLBACK "<%d>" SYSLOG_VERSION SYSLOG_SP \
                                 SYSLOG_TIMESTAMP SYSLOG_SP \
                                 SYSLOG_NILVALUE SYSLOG_SP \
                                 SYSLOG_NILVALUE SYSLOG_SP \
                                 "%s" SYSLOG_SP \
                                 SYSLOG_MSGID SYSLOG_SP \
                                 SYSLOG_STRUCTURED_DATA SYSLOG_SP \
                                 SYSLOG_BOM \
                                 "%s"

static const char *TAG = "SYSLOG";

static const char *conflicting_tasks[] = {
    "tIT",     /* TCPIP_THREAD_NAME */
    "wifi",
};

/* see https://datatracker.ietf.org/doc/html/rfc5424#section-6.2.1 */
static const char *loglevel_chars = "EWIDV";
static const uint8_t severity_map[] = {3, 4, 6, 7, 7};   /* mapping from loglevel_chars */
static const uint8_t default_severity = 5;    /* notice */
static const uint8_t default_facility = 16;   /* local0 */

static const char *wifi_sta_if_key = "WIFI_STA_DEF";

static int syslog_fd;
static struct sockaddr_in dest_addr;
static vprintf_like_t old_func = NULL;
static char *intermediate_template = NULL;
static bool syslog_copy_to_serial;

static int get_socket_error_code(int socket)
{
	int result;
	u32_t optlen = sizeof(result);
	if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &result, &optlen) == -1)
    {
    	printf("getsockopt failed");
    	result = -1;
	}
	return result;
}

static void show_socket_error_reason(int socket)
{
	int err = get_socket_error_code(socket);
    if (err >= 0)
    {
    	printf("UDP socket error %d %s", err, strerror(err));
    }
}

static inline bool is_conflicting_task(const char *task_name)
{
    for (int i = 0; i < NELEMS(conflicting_tasks); i++)
    {
        if (strcmp(task_name, conflicting_tasks[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

static void shutdown_handler()
{
    /* allow sending final log messages to syslog host */
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_log_set_vprintf((old_func) ? old_func : vprintf);
}

static void clean_log_line(char * const line)
{
    char *src = line;
    char *dst = line;

    while (*src)
    {
        if ((*src == '\033') && (*(src + 1) == '['))
        {
            /* skip control sequence */
            src += 2;
            while (*src && (*src != 'm'))
            {
                src += 1;
            }
            if (*src)
            {
                src += 1;
            }
        }
        else
        {
            if (dst != src)
            {
                /* copy character */
                *dst = *src;
            }
            dst += 1;
            src += 1;
        }
    }
    *dst = '\0';
    /* strip trailing newlines */
    dst -= 1;
    while ((dst >= line) && (*dst == '\n'))
    {
        *dst = '\0';
        dst -= 1;
    }
}

static char *build_syslog_msg(const char *msg, const char *cur_task)
{
    uint8_t severity;
    uint8_t prival;
    const char *ptr;

    if (msg && msg[0] &&
        ((ptr = strchr(loglevel_chars, msg[0])) != NULL) &&
        (msg[1] == ' ') &&
        (msg[2] == '('))
    {
        severity = severity_map[ptr - loglevel_chars];
    }
    else
    {
        severity = default_severity;
    }
    prival = (default_facility << 3) + severity;
    /* conservative buffer size estimation */
    size_t buf_size = strlen(intermediate_template) +
                      3 +          /* PRIVAL */
                      strlen(cur_task) +
                      strlen(msg);
    char *buffer = (char *)malloc(buf_size);
    if (buffer)
    {
        snprintf(buffer, buf_size, intermediate_template, prival, cur_task, msg);
    }
    return buffer;
}

static int fallback_func(const char *str, va_list l)
{
    return (old_func) ? old_func(str, l) : vprintf(str, l);
}

static int send_to_host(const char *str, int len)
{
    int res = len;
    int err = sendto(syslog_fd, str, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0)
    {
        show_socket_error_reason(syslog_fd);
        printf("\nsendto failed -> restoring logging to serial line\n");
        syslog_client_stop();
        res = -1;
    }
    return res;
}

static int syslog_vprintf(const char *str, va_list l)
{
    int res = -1;
	char *cur_task = pcTaskGetTaskName(NULL);
    if (!is_conflicting_task(cur_task))
	{
#ifdef SYSLOG_USE_STACK
    	char buffer[MAX_PAYLOAD_LEN];
#else
        char *buffer= (char *)malloc(MAX_PAYLOAD_LEN);
#endif
		res = vsnprintf((char *)buffer, MAX_PAYLOAD_LEN, str, l);
        if (res >= 0)
        {
            clean_log_line(buffer);
            char *syslog_msg = build_syslog_msg(buffer, cur_task);
            if (syslog_msg)
            {
                res = send_to_host(syslog_msg, strlen(syslog_msg));
                free((void *)syslog_msg);
            }
        }
#ifndef SYSLOG_USE_STACK
        free((void *)buffer);
#endif
	}
    if (syslog_copy_to_serial || (res < 0))
    {
        res = fallback_func(str, l);
    }
	return res;
}


static int raw_vprintf(const char *str, va_list l)
{
    int res = -1;
	char *cur_task = pcTaskGetTaskName(NULL);
    if (!is_conflicting_task(cur_task))
	{
#ifdef SYSLOG_USE_STACK
    	char buffer[MAX_PAYLOAD_LEN];
#else
        char *buffer= (char *)malloc(MAX_PAYLOAD_LEN);
#endif
		res = vsnprintf((char *)buffer, MAX_PAYLOAD_LEN, str, l);
        if (res >= 0)
        {
            res = send_to_host(buffer, res);
        }
#ifndef SYSLOG_USE_STACK
        free((void *)buffer);
#endif
	}
    if (syslog_copy_to_serial || (res < 0))
    {
        res = fallback_func(str, l);
    }
	return res;
}


static uint32_t resolve_mdns_host(const char *host_name)
{
    ESP_LOGD(TAG, "Query A: %s.local", host_name);

    esp_ip4_addr_t addr;
    addr.addr = 0;
    uint32_t result = 0;

    esp_err_t err = mdns_query_a(host_name, 2000, &addr);
    if (err)
    {
        if (err == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Host '%s' was not found!", host_name);
        }
        else
        {
            ESP_LOGE(TAG, "MDNS query failed");
        }
    }
    else
    {
        ESP_LOGI(TAG, "%s has IP address " IPSTR, host_name, IP2STR(&addr));
        result = addr.addr;
    }

    return result;
}


static uint32_t resolve_host(const char *host)
{
    uint32_t result = 0;

    /*
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(wifi_sta_if_key);
    esp_netif_dns_info_t gdns1, gdns2, gdns3;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &gdns1));
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &gdns2));
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &gdns3));

    ESP_LOGI(TAG, "DNS servers : " IPSTR " , " IPSTR " , " IPSTR,
      IP2STR(&gdns1.ip.u_addr.ip4),
      IP2STR(&gdns2.ip.u_addr.ip4),
      IP2STR(&gdns3.ip.u_addr.ip4));
    */

    struct hostent *he = gethostbyname(host);
    if (he && (he->h_length >= sizeof(result)) && he->h_addr_list && he->h_addr_list[0])
    {
        result = *((uint32_t *)(he->h_addr_list[0]));
        ESP_LOGD(TAG, "dns result %s", inet_ntoa(result));
    }
    else
    {
        result = resolve_mdns_host(host);
    }
    return result;
}


static void syslog_close()
{
    if (syslog_fd > 0)
    {
        shutdown(syslog_fd, 2);
        close(syslog_fd);
    }
    syslog_fd = 0;
}


void syslog_client_start(const char *host, unsigned int port,
                         const char *app_name,
                         bool send_raw, bool copy_to_serial)
{
	struct timeval send_to = {1,0};
	syslog_fd = 0;
	ESP_LOGI(TAG, "initializing...");
    syslog_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (syslog_fd > 0)
    {
        const uint32_t dest_addr_bytes = resolve_host(host);
        if (dest_addr_bytes)
        {
        	ESP_LOGI(TAG, "logging to %s:%d", inet_ntoa(dest_addr_bytes), port);
            bzero(&dest_addr, sizeof(dest_addr));
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(port);
            dest_addr.sin_addr.s_addr = dest_addr_bytes;

            int err = setsockopt(syslog_fd, SOL_SOCKET, SO_SNDTIMEO, &send_to, sizeof(send_to));
            if (err >= 0)
            {
                syslog_copy_to_serial = copy_to_serial;

                if (!send_raw)
                {
                    const char *own_hostname;
                    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(wifi_sta_if_key);
                    if (esp_netif_get_hostname(netif, &own_hostname) != ESP_OK)
                    {
                        own_hostname = SYSLOG_NILVALUE;
                    }

                    const char *app_name_use = (app_name) ? app_name : SYSLOG_NILVALUE;

                    size_t int_tmpl_size = strlen(SYSLOG_TEMPLATE) +
                                           strlen(own_hostname) +
                                           strlen(app_name_use);
                    intermediate_template = (char *)malloc(int_tmpl_size);

                    if (snprintf(intermediate_template, int_tmpl_size,
                                 SYSLOG_TEMPLATE, own_hostname, app_name_use) < 0)
                    {
                        intermediate_template = strdup(SYSLOG_TEMPLATE_FALLBACK);
                    }
                	ESP_LOGI(TAG, "Intermediate template '%s'", intermediate_template);
                }

                esp_register_shutdown_handler(shutdown_handler);

                vprintf_like_t prev;
                prev = esp_log_set_vprintf((send_raw) ? raw_vprintf : syslog_vprintf);
                /* try to prevent an endless loop */
                if ((prev != raw_vprintf) && (prev != syslog_vprintf))
                {
                    old_func = prev;
                }

                ESP_LOGI(TAG, "Remote logging to %s:%d set up successfully", host, port);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to set SO_SNDTIMEO. Error %d", err);
                syslog_close();
            }
        }
        else
        {
            ESP_LOGE(TAG, "Cannot resolve syslog host name '%s'", host);
            syslog_close();
        }
    }
    else
    {
       ESP_LOGE(TAG, "Cannot open socket!");
    }
}

void syslog_client_start_simple(const char *app_name)
{
    bool send_raw = false;
    bool copy_to_serial = false;
#ifdef CONFIG_SYSLOG_SEND_RAW
    send_raw = true;
#endif
#ifdef CONFIG_SYSLOG_COPY_SERIAL
    copy_to_serial = true;
#endif
    syslog_client_start(CONFIG_SYSLOG_HOST, CONFIG_SYSLOG_PORT, app_name, send_raw, copy_to_serial);
}

void syslog_client_stop()
{
    esp_log_set_vprintf((old_func) ? old_func : vprintf);
    syslog_close();

    esp_unregister_shutdown_handler(shutdown_handler);

    if (intermediate_template)
    {
        free(intermediate_template);
        intermediate_template = NULL;
    }
}