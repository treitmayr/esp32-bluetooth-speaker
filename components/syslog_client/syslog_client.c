#include "syslog_client.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "mdns.h"

#define SYSLOG_USE_STACK
/* #define SYSLOG_UTF8 */
#define MAX_PAYLOAD_LEN 200
#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

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

char **line_buffer = NULL;
uint32_t line_buffer_size;
uint32_t line_buffer_used;
uint32_t line_buffer_start;
SemaphoreHandle_t line_buffer_semaphore = NULL;

static int syslog_fd;
static struct sockaddr_in dest_addr;
static vprintf_like_t old_func = NULL;
static char *intermediate_template = NULL;
static size_t intermediate_template_len = 0;
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
    const char *int_msg = (msg) ? msg : "";
    const char *int_cur_task = (cur_task) ? cur_task : SYSLOG_NILVALUE;

    if (int_msg[0] &&
        ((ptr = strchr(loglevel_chars, int_msg[0])) != NULL) &&
        (int_msg[1] == ' ') &&
        (int_msg[2] == '('))
    {
        severity = severity_map[ptr - loglevel_chars];
    }
    else
    {
        severity = default_severity;
    }
    prival = (default_facility << 3) + severity;
    /* conservative buffer size estimation */
    size_t buf_size = intermediate_template_len +
                      3 +          /* PRIVAL */
                      strlen(int_cur_task) +
                      strlen(int_msg);
    char *buffer = (char *)malloc(buf_size);
    if (buffer)
    {
        snprintf(buffer, buf_size, intermediate_template, prival, int_cur_task, int_msg);
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
    bool retry = true;
    int err;
    while (retry)
    {
        err = sendto(syslog_fd, str, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if ((err < 0) && (errno == ENOMEM))
        {
            /* let network stack empty out its send buffers,
               see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/lwip.html#limitations */
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        else
        {
            retry = false;
        }
    }
    if (err < 0)
    {
        show_socket_error_reason(syslog_fd);
        printf("\nsendto failed with %d -> restoring logging to serial line\n", err);
        syslog_client_stop();   /* FIXME: */
        res = -1;
    }
    return res;
}


static char *build_buffer_line(const char *cur_task, const char *msg)
{
    const size_t len_task = (cur_task) ? strlen(cur_task) : 0;
    const size_t len_msg = (msg) ? strlen(msg) : 0;
    const size_t len_total = len_task + 1 + len_msg;
    char *buffer = malloc(len_total + 1);
    if (len_task)
    {
        memcpy(buffer, cur_task, len_task);
    }
    buffer[len_task] = '\1';
    if (len_msg)
    {
        memcpy(buffer + len_task + 1, msg, len_msg);
    }
    buffer[len_total] = '\0';
    return buffer;
}


static bool line_task_match(const char *line, const char *task)
{
    bool result = false;
    if (line && task)
    {
        const size_t len = strlen(task);
        result = ((memcmp(line, task, len) == 0) && (line[len] == '\1'));
    }
    return result;
}


static bool append_line_buffer(const char *cur_task, const char *msg)
{
    bool success = false;

    xSemaphoreTake(line_buffer_semaphore, portMAX_DELAY);
    if (line_buffer && msg && msg[0] != '\0')
    {
        if (line_buffer_used > 0)
        {
            const uint32_t prev_index = (line_buffer_start + line_buffer_used - 1 + line_buffer_size) % line_buffer_size;
            char *prev_line = line_buffer[prev_index];
            if (prev_line && (prev_line[0] != '\0'))
            {
                const size_t prev_len = strlen(prev_line);
                if ((prev_line[prev_len - 1] != '\n') &&
                    (line_task_match(line_buffer[prev_index], cur_task)))
                {
                    /* append msg to previous entry */
                    const size_t msg_len = strlen(msg);
                    line_buffer[prev_index] = realloc(prev_line, prev_len + msg_len + 1);
                    memcpy(line_buffer[prev_index] + prev_len, msg, msg_len + 1);
                    success = true;
                }
            }
        }

        if (!success)
        {
            uint32_t index = line_buffer_start + line_buffer_used;
            if (index >= line_buffer_size)
            {
                index -= line_buffer_size;
            }
            if (line_buffer_used < line_buffer_size)
            {
                line_buffer[index] = build_buffer_line(cur_task, msg);
                line_buffer_used += 1;
            }
            else
            {
                /* overflow -> remove oldest entries */
                free(line_buffer[index]);
                line_buffer[index] = strdup(msg);
                line_buffer_start += 1;
                while (line_buffer_start >= line_buffer_size)
                {
                    line_buffer_start -= line_buffer_size;
                }
            }
            success = true;
        }
    }
    xSemaphoreGive(line_buffer_semaphore);
    return success;
}


static bool fetch_line_buffer(char **task, char **msg)
{
    bool result = false;

    xSemaphoreTake(line_buffer_semaphore, portMAX_DELAY);
    if (line_buffer)
    {
        char *line = NULL;
        while (line_buffer_used && !line)
        {
            line = line_buffer[line_buffer_start];
            if (line)
            {
                char *sep_index = strchr(line, '\1');
                if (sep_index)
                {
                    /* split string */
                    *sep_index = '\0';
                    *task = line;
                    *msg = sep_index + 1;
                }
                else
                {
                    *task = NULL;
                    *msg = line;
                }
                line_buffer[line_buffer_start] = NULL;
                result = true;
            }
            line_buffer_used -= 1;
            line_buffer_start += 1;
            if (line_buffer_start >= line_buffer_size)
            {
                line_buffer_start -= line_buffer_size;
            }
        }
    }
    xSemaphoreGive(line_buffer_semaphore);
    return result;
}


static int syslog_vprintf(const char *str, va_list l)
{
    int res = -1;
	char *cur_task = pcTaskGetName(NULL);
    bool in_conflicting_task = is_conflicting_task(cur_task);

    if (!in_conflicting_task)
    {
        char *task;
        char *msg;
        while ((fetch_line_buffer(&task, &msg)))
        {
            clean_log_line(msg);
            if (msg[0] != '\0')
            {
                char *syslog_msg = build_syslog_msg(msg, task);
                if (syslog_msg)
                {
                    res = send_to_host(syslog_msg, strlen(syslog_msg));
                    free(syslog_msg);
                }
            }
            free((task) ? task : msg);
        }
    }

#ifdef SYSLOG_USE_STACK
   	char buffer[MAX_PAYLOAD_LEN];
#else
    char *buffer= (char *)malloc(MAX_PAYLOAD_LEN);
#endif
	res = vsnprintf((char *)buffer, MAX_PAYLOAD_LEN, str, l);
    if (res >= 0)
    {
        if (!in_conflicting_task)
        {
            clean_log_line(buffer);
            if (buffer[0] != '\0')
            {
                char *syslog_msg = build_syslog_msg(buffer, cur_task);
                if (syslog_msg)
                {
                    res = send_to_host(syslog_msg, strlen(syslog_msg));
                    free(syslog_msg);
                }
            }
        }
        else
        {
            append_line_buffer(cur_task, buffer);
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
	char *cur_task = pcTaskGetName(NULL);

    if (!is_conflicting_task(cur_task))
    {
        char *task;
        char *msg;
        while ((fetch_line_buffer(&task, &msg)))
        {
            send_to_host(msg, strlen(msg));
            free((task) ? task : msg);
        }
    }

#ifdef SYSLOG_USE_STACK
   	char buffer[MAX_PAYLOAD_LEN];
#else
    char *buffer= (char *)malloc(MAX_PAYLOAD_LEN);
#endif
    res = vsnprintf((char *)buffer, MAX_PAYLOAD_LEN, str, l);
    if (res >= 0)
    {
        if (!is_conflicting_task(cur_task))
	    {
            res = send_to_host(buffer, res);
        }
        else
        {
            append_line_buffer(cur_task, buffer);
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


static int buffering_vprintf(const char *str, va_list l)
{
    int res = -1;
#ifdef SYSLOG_USE_STACK
   	char buffer[MAX_PAYLOAD_LEN];
#else
    char *buffer= (char *)malloc(MAX_PAYLOAD_LEN);
#endif
	char *cur_task = pcTaskGetName(NULL);

	res = vsnprintf((char *)buffer, MAX_PAYLOAD_LEN, str, l);
    if (res >= 0)
    {
        append_line_buffer(cur_task, buffer);
	}
    res = fallback_func(str, l);
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
            ESP_LOGE(TAG, "Host name '%s' was not found by mDNS query", host_name);
        }
        else
        {
            ESP_LOGE(TAG, "mDNS query failed for host name '%s'", host_name);
        }
    }
    else
    {
        ESP_LOGD(TAG, "Host '%s' has IP address " IPSTR, host_name, IP2STR(&addr));
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
        ESP_LOGD(TAG, "DNS query for host name '%s' returned %s", host, inet_ntoa(result));
    }
    else
    {
        result = resolve_mdns_host(host);
    }
    return result;
}


static void syslog_socket_close()
{
    if (syslog_fd > 0)
    {
        shutdown(syslog_fd, 2);
        close(syslog_fd);
    }
    syslog_fd = 0;
}


static void syslog_init(size_t max_number_lines)
{
    if (!line_buffer_semaphore)
    {
    	ESP_LOGI(TAG, "Initializing...");
        line_buffer_semaphore = xSemaphoreCreateBinary();
        assert(line_buffer_semaphore);
        xSemaphoreGive(line_buffer_semaphore);

        if (max_number_lines)
        {
            line_buffer = calloc(max_number_lines, sizeof(line_buffer[0]));
            assert(line_buffer);
            line_buffer_size = max_number_lines;
            line_buffer_used = 0;
            line_buffer_start = 0;
        }
    }
}


void syslog_early_buffering_start(uint32_t max_number_lines)
{
    syslog_init(max_number_lines);

    vprintf_like_t prev;
    prev = esp_log_set_vprintf(buffering_vprintf);
    /* try to prevent an endless loop */
    if ((prev != buffering_vprintf) && (prev != raw_vprintf) && (prev != syslog_vprintf))
    {
        old_func = prev;
    }

    ESP_LOGI(TAG, "Early log buffering set up successfully");
}


void syslog_early_buffering_stop()
{
    xSemaphoreTake(line_buffer_semaphore, portMAX_DELAY);
    if (line_buffer)
    {
        while (line_buffer_used)
        {
            char *line = line_buffer[line_buffer_start];
            if (line)
            {
                free(line);
            }
            line_buffer_used -= 1;
            line_buffer_start += 1;
            if (line_buffer_start >= line_buffer_size)
            {
                line_buffer_start -= line_buffer_size;
            }
        }
        free(line_buffer);
        line_buffer = NULL;
    }
    xSemaphoreGive(line_buffer_semaphore);
}


void syslog_client_start(const char *host, unsigned int port,
                         const char *app_name,
                         bool send_raw, bool copy_to_serial)
{
    syslog_init(10);

	syslog_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (syslog_fd > 0)
    {
        const uint32_t dest_addr_bytes = resolve_host(host);
        if (dest_addr_bytes)
        {
        	ESP_LOGI(TAG, "Logging to %s:%d", inet_ntoa(dest_addr_bytes), port);
            bzero(&dest_addr, sizeof(dest_addr));
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(port);
            dest_addr.sin_addr.s_addr = dest_addr_bytes;
        	struct timeval send_to = {100,0};

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

                    /* check validity of app_name */
                    const char *app_name_use = (app_name && *app_name) ? app_name : SYSLOG_NILVALUE;

                    size_t int_tmpl_size = strlen(SYSLOG_TEMPLATE) +
                                           strlen(own_hostname) +
                                           strlen(app_name_use);
                    intermediate_template = (char *)malloc(int_tmpl_size);
                    assert(intermediate_template);

                    if (snprintf(intermediate_template, int_tmpl_size,
                                 SYSLOG_TEMPLATE, own_hostname, app_name_use) < 0)
                    {
                        free(intermediate_template);
                        intermediate_template = strdup(SYSLOG_TEMPLATE_FALLBACK);
                        assert(intermediate_template);
                    }
                    intermediate_template_len = strlen(intermediate_template);
                	ESP_LOGD(TAG, "Intermediate template '%s'", intermediate_template);
                }

                esp_register_shutdown_handler(shutdown_handler);

                vprintf_like_t prev;
                prev = esp_log_set_vprintf((send_raw) ? raw_vprintf : syslog_vprintf);
                /* try to prevent an endless loop */
                if ((prev != buffering_vprintf) && (prev != raw_vprintf) && (prev != syslog_vprintf))
                {
                    old_func = prev;
                }

                ESP_LOGI(TAG, "Remote logging to %s:%d set up successfully", host, port);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to set SO_SNDTIMEO. Error %d", err);
                syslog_socket_close();
            }
        }
        else
        {
            ESP_LOGE(TAG, "Cannot resolve syslog host name '%s'", host);
            syslog_socket_close();
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
    syslog_socket_close();

    esp_unregister_shutdown_handler(shutdown_handler);

    if (intermediate_template && false)    /* FIXME: */
    {
        char *temp = intermediate_template;
        intermediate_template = NULL;
        intermediate_template_len = 0;
        free(temp);
    }
}