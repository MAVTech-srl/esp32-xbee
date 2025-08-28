/*
 * ESP32-XBee – Socket client UART→TCP robusto (micro-buffer)
 * Basato sul progetto originale: https://github.com/nebkat/esp32-xbee
 */

#include <sys/param.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>

#include <uart.h>
#include <util.h>
#include <status_led.h>
#include <wifi.h>
#include <esp_log.h>

#include "interface/socket_client.h"
#include <config.h>
#include <retry.h>
#include <stream_stats.h>
#include <tasks.h>

static const char *TAG = "SOCKET_CLIENT";

#define BUFFER_SIZE       1024      // chunk di invio verso TCP
#define SB_SIZE_BYTES     (4*1024)  // micro-buffer tra UART e TCP
#define SEND_BACKOFF_MS   5         // backoff se TCP è pieno (EAGAIN)

static int sock = -1;

static status_led_handle_t status_led = NULL;
static stream_stats_handle_t stream_stats = NULL;
static StreamBufferHandle_t uart_sb = NULL;

/* Handler chiamato quando arrivano dati dalla UART.
   Qui NON facciamo I/O di rete: mettiamo i byte nel micro-buffer e torniamo subito. */
static void socket_client_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer)
{
    if (!uart_sb || length <= 0) return;

    size_t written = xStreamBufferSend(uart_sb, buffer, (size_t)length, 0);
    if (stream_stats && written > 0) {
        stream_stats_increment(stream_stats, 0, (int32_t)written); // conteggio TX (UART->rete)
    }
    // Se written < length, il buffer era pieno: se vuoi, aggiungi un contatore di drop.
}

static esp_err_t socket_send_nonblock(int s, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s, buf + off, len - off, MSG_DONTWAIT);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(SEND_BACKOFF_MS));
            continue;
        }
        return ESP_FAIL; // errore serio
    }
    return ESP_OK;
}

static void socket_client_task(void *ctx)
{
    // Registra l'handler UART e crea il micro-buffer una sola volta
    uart_register_read_handler(socket_client_uart_handler);

    if (!uart_sb) {
        uart_sb = xStreamBufferCreate(SB_SIZE_BYTES, 1); // trigger level 1
        if (!uart_sb) {
            ESP_LOGE(TAG, "StreamBuffer create failed");
            vTaskDelete(NULL);
        }
    }

    // LED stato (opzionale)
    config_color_t status_led_color = config_get_color(CONF_ITEM(KEY_CONFIG_SOCKET_CLIENT_COLOR));
    if (status_led_color.rgba != 0) status_led = status_led_add(status_led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led) status_led->active = false;

    stream_stats = stream_stats_new("socket_client");

    retry_delay_handle_t delay_handle = retry_init(true, 5, 2000, 0);

    while (true) {
        retry_delay(delay_handle);
        wait_for_ip();

        char *host = NULL, *connect_message = NULL;
        uint16_t port = config_get_u16(CONF_ITEM(KEY_CONFIG_SOCKET_CLIENT_PORT));
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_SOCKET_CLIENT_HOST), (void **)&host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_SOCKET_CLIENT_CONNECT_MESSAGE), (void **)&connect_message);
        // Per RTCM vogliamo TCP (client)
        int socktype = SOCK_STREAM;

        if (!host || !*host) {
            ESP_LOGE(TAG, "Host non configurato");
            goto _error;
        }

        ESP_LOGI(TAG, "Connecting to %s host %s:%u", SOCKTYPE_NAME(socktype), host, port);
        uart_nmea("$PESP,SOCK,CLI,%s,CONNECTING,%s:%u", SOCKTYPE_NAME(socktype), host, port);

        sock = connect_socket(host, port, socktype);
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_RESOLVE, goto _error, "Could not resolve host");
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_CONNECT, goto _error, "Could not connect to host");

        // Socket non-bloccante + bassa latenza + keepalive
        int fl = fcntl(sock, F_GETFL, 0);
        if (fl != -1) fcntl(sock, F_SETFL, fl | O_NONBLOCK);
        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        if (connect_message && *connect_message) {
            ssize_t ce = send(sock, connect_message, strlen(connect_message), MSG_DONTWAIT);
            ERROR_ACTION(TAG, ce < 0 && errno != EAGAIN && errno != EWOULDBLOCK,
                         goto _error, "Could not send connection message: %d %s", errno, strerror(errno));
        }

        ESP_LOGI(TAG, "Successfully connected to %s:%u", host, port);
        uart_nmea("$PESP,SOCK,CLI,%s,CONNECTED,%s:%u", SOCKTYPE_NAME(socktype), host, port);
        retry_reset(delay_handle);
        if (status_led) status_led->active = true;

        // Loop di forwarding: svuota il micro-buffer e manda su TCP
        uint8_t buf[BUFFER_SIZE];
        while (sock != -1) {
            size_t n = xStreamBufferReceive(uart_sb, buf, sizeof(buf), pdMS_TO_TICKS(100));
            if (n == 0) continue; // niente da mandare
            if (socket_send_nonblock(sock, buf, n) != ESP_OK) {
                ESP_LOGW(TAG, "Socket send failed: %d %s", errno, strerror(errno));
                break; // forza riconnessione
            }
        }

        if (status_led) status_led->active = false;

        ESP_LOGW(TAG, "Disconnected from %s:%u: %d %s", host, port, errno, strerror(errno));
        uart_nmea("$PESP,SOCK,CLI,%s,DISCONNECTED,%s:%u", SOCKTYPE_NAME(socktype), host, port);

    _error:
        destroy_socket(&sock);
        if (host) free(host);
        if (connect_message) free(connect_message);
        host = NULL; connect_message = NULL;
    }

    vTaskDelete(NULL);
}

void socket_client_init(void)
{
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_SOCKET_CLIENT_ACTIVE))) return;
    xTaskCreate(socket_client_task, "socket_client_task", 4096, NULL, TASK_PRIORITY_INTERFACE, NULL);
}

