#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CIVSWITCH_DIAGNOSTIC_BOOT
#define CIVSWITCH_DIAGNOSTIC_BOOT 0
#endif

#include "dhcpserver.h"
#include "cyw43.h"
#include "cyw43_ll.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#define AP_SSID "Switch2BridgePico"
#define AP_CHANNEL 6
#define MAX_CLIENTS 4
#define SERIAL_LINE_CAPACITY 320
#define MAX_PAYLOAD_LENGTH 262144u
#define DNS_PORT 53
#define HTTP_PORT 80
#define DNS_PACKET_CAPACITY 512
#define HTTP_REQUEST_CAPACITY 1024

static dhcp_server_t dhcp_server;
static struct udp_pcb *dns_pcb;
static bool radio_ready;
static bool services_ready;
static uint32_t received_frames[2];
static uint32_t received_dhcp_frames;
static uint32_t wifi_events;
static uint32_t last_wifi_event;
static uint32_t last_wifi_status;
static uint32_t last_wifi_reason;
static int last_wifi_interface;

static char serial_line[SERIAL_LINE_CAPACITY];
static size_t serial_line_length;
static bool serial_line_overflow;

static int last_client_count;
static uint32_t last_blink_time;
static int blink_phase;
static int last_led_state = -1;

typedef struct {
    struct tcp_pcb *pcb;
    char request[HTTP_REQUEST_CAPACITY];
    size_t request_length;
} http_connection_t;

typedef struct {
    struct tcp_pcb *pcb;
    const uint8_t *payload;
    size_t payload_length;
    size_t payload_offset;
    uint32_t connect_deadline;
    uint32_t response_deadline;
    bool connected;
    bool write_shutdown;
    bool got_data;
    bool done;
} relay_context_t;

void __real_cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len,
                                     const uint8_t *buf);

void __wrap_cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len,
                                     const uint8_t *buf) {
#if CIVSWITCH_DIAGNOSTIC_BOOT
    static unsigned int ethernet_logs;
    bool log_this = ethernet_logs++ < 40;
    if (log_this) printf("[RX] Ethernet callback interface=%d length=%u\n", itf, (unsigned)len);
#endif
    if (itf == CYW43_ITF_AP || itf == CYW43_ITF_STA) {
        ++received_frames[itf];
    }
    if (buf != NULL && len >= 42 && buf[12] == 0x08 && buf[13] == 0x00) {
        const uint8_t *ip = buf + 14;
        const size_t ip_header_length = (size_t)(ip[0] & 0x0f) * 4;
        if ((ip[0] >> 4) == 4 && ip[9] == 17 && ip_header_length >= 20 &&
            len >= 14 + ip_header_length + 8) {
            const uint8_t *udp = ip + ip_header_length;
            const uint16_t source_port = (uint16_t)((udp[0] << 8) | udp[1]);
            const uint16_t destination_port = (uint16_t)((udp[2] << 8) | udp[3]);
            if (source_port == 68 && destination_port == 67) {
                ++received_dhcp_frames;
            }
        }
    }
    __real_cyw43_cb_process_ethernet(cb_data, itf, len, buf);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    if (log_this) printf("[RX] Ethernet callback returned\n");
#endif
}

void __real_cyw43_cb_process_async_event(void *cb_data, const cyw43_async_event_t *ev);

void __wrap_cyw43_cb_process_async_event(void *cb_data, const cyw43_async_event_t *ev) {
#if CIVSWITCH_DIAGNOSTIC_BOOT
    static unsigned int event_logs;
    bool log_this = event_logs++ < 40;
    if (log_this && ev != NULL) printf("[RX] Event callback type=%lu interface=%u flags=0x%x\n",
                                       (unsigned long)ev->event_type, (unsigned)ev->interface,
                                       (unsigned)ev->flags);
#endif
    if (ev != NULL) {
        ++wifi_events;
        last_wifi_event = ev->event_type;
        last_wifi_status = ev->status;
        last_wifi_reason = ev->reason;
        last_wifi_interface = ev->interface;
    }
    __real_cyw43_cb_process_async_event(cb_data, ev);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    if (log_this) printf("[RX] Event callback returned\n");
#endif
}

void __real_cyw43_cb_tcpip_set_link_up(cyw43_t *self, int itf);
void __wrap_cyw43_cb_tcpip_set_link_up(cyw43_t *self, int itf) {
    if (itf == CYW43_ITF_AP) {
        struct netif *ap = &self->netif[itf];
        const bool had_etharp = (ap->flags & NETIF_FLAG_ETHARP) != 0;
        /* Prevent ARP output while the CYW43 link callback runs.
           Restore ARP output after the callback returns. */
        ap->flags &= (u8_t)~NETIF_FLAG_ETHARP;
        __real_cyw43_cb_tcpip_set_link_up(self, itf);
        if (had_etharp) ap->flags |= NETIF_FLAG_ETHARP;
    } else {
        __real_cyw43_cb_tcpip_set_link_up(self, itf);
    }
}

#if CIVSWITCH_DIAGNOSTIC_BOOT
int __real_cyw43_send_ethernet(cyw43_t *self, int itf, size_t len,
                               const void *buf, bool is_pbuf);
int __wrap_cyw43_send_ethernet(cyw43_t *self, int itf, size_t len,
                               const void *buf, bool is_pbuf) {
    printf("[TX] Sending Ethernet frame: interface=%d length=%u\n", itf, (unsigned)len);
    int rc = __real_cyw43_send_ethernet(self, itf, len, buf, is_pbuf);
    printf("[TX] Ethernet send returned %d\n", rc);
    return rc;
}

int __real_cyw43_ll_wifi_ap_init(cyw43_ll_t *self, size_t ssid_len,
                                 const uint8_t *ssid, uint32_t auth,
                                 size_t key_len, const uint8_t *key, uint32_t channel);
int __wrap_cyw43_ll_wifi_ap_init(cyw43_ll_t *self, size_t ssid_len,
                                 const uint8_t *ssid, uint32_t auth,
                                 size_t key_len, const uint8_t *key, uint32_t channel) {
    printf("[BOOT] Configuring AP radio\n");
    int rc = __real_cyw43_ll_wifi_ap_init(self, ssid_len, ssid, auth, key_len, key, channel);
    printf("[BOOT] AP radio configuration returned %d\n", rc);
    return rc;
}

void __real_cyw43_cb_tcpip_init(cyw43_t *self, int itf);
void __wrap_cyw43_cb_tcpip_init(cyw43_t *self, int itf) {
    printf("[BOOT] Initializing network interface %d\n", itf);
    __real_cyw43_cb_tcpip_init(self, itf);
    printf("[BOOT] Network interface %d initialized\n", itf);
}

int __real_cyw43_ll_wifi_ap_set_up(cyw43_ll_t *self, bool up);
int __wrap_cyw43_ll_wifi_ap_set_up(cyw43_ll_t *self, bool up) {
    printf("[BOOT] Enabling AP radio\n");
    int rc = __real_cyw43_ll_wifi_ap_set_up(self, up);
    printf("[BOOT] AP radio enable returned %d\n", rc);
    return rc;
}

bool __real_cyw43_ll_has_work(cyw43_ll_t *self);
bool __wrap_cyw43_ll_has_work(cyw43_ll_t *self) {
    static bool first = true;
    if (first) printf("[BOOT] Checking radio for pending work\n");
    bool ready = __real_cyw43_ll_has_work(self);
    if (first) {
        printf("[BOOT] Radio pending work: %d\n", ready);
        first = false;
    }
    return ready;
}

void __real_cyw43_ll_process_packets(cyw43_ll_t *self);
void __wrap_cyw43_ll_process_packets(cyw43_ll_t *self) {
    static bool first = true;
    if (first) printf("[BOOT] Processing radio packets\n");
    __real_cyw43_ll_process_packets(self);
    if (first) {
        printf("[BOOT] Radio packet processing returned\n");
        first = false;
    }
}
#endif

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void send_end(void) {
    printf("\nEND\n");
}

static void set_led(bool on) {
    const int state = on ? 1 : 0;
    if (last_led_state != state) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, state);
        last_led_state = state;
    }
}

static void update_led(int clients) {
    if (clients == 0) {
        set_led(true);
        return;
    }

    const uint32_t now = now_ms();
    const int total_phases = clients * 2 + 4;
    if ((uint32_t)(now - last_blink_time) > 200) {
        last_blink_time = now;
        blink_phase = (blink_phase + 1) % total_phases;
        if (blink_phase < clients * 2) {
            set_led((blink_phase % 2) == 0);
        } else {
            set_led(false);
        }
    }
}

static int get_client_count(uint8_t macs[MAX_CLIENTS * 6]) {
    int count = MAX_CLIENTS;
    cyw43_wifi_ap_get_stas(&cyw43_state, &count, macs);
    if (count < 0) {
        return 0;
    }
    return count > MAX_CLIENTS ? MAX_CLIENTS : count;
}

static void close_tcp_pcb(struct tcp_pcb *pcb) {
    if (pcb == NULL) {
        return;
    }
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
    }
}

static uint16_t read_be16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void write_be16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static bool build_dns_reply(const uint8_t *query, size_t query_length,
                            uint8_t *reply, size_t *reply_length) {
    if (query_length < 17 || read_be16(query + 4) != 1) {
        return false;
    }

    size_t question_end = 12;
    while (question_end < query_length) {
        const uint8_t label_length = query[question_end++];
        if (label_length == 0) {
            break;
        }
        if ((label_length & 0xc0) != 0 || label_length > 63 ||
            question_end + label_length > query_length) {
            return false;
        }
        question_end += label_length;
    }
    if (question_end + 4 > query_length) {
        return false;
    }

    const uint16_t query_type = read_be16(query + question_end);
    const uint16_t query_class = read_be16(query + question_end + 2);
    const bool answer_a_record = query_type == 1 && query_class == 1;
    const size_t question_length = question_end + 4 - 12;
    const size_t answer_length = answer_a_record ? 16 : 0;
    if (12 + question_length + answer_length > DNS_PACKET_CAPACITY) {
        return false;
    }

    memcpy(reply, query, 12 + question_length);
    // Keep the query opcode and RD flag in the authoritative reply.
    reply[2] = (uint8_t)((query[2] & 0x79) | 0x84);
    reply[3] = 0;
    write_be16(reply + 4, 1);
    write_be16(reply + 6, answer_a_record ? 1 : 0);
    write_be16(reply + 8, 0);
    write_be16(reply + 10, 0);

    size_t out = 12 + question_length;
    if (answer_a_record) {
        reply[out++] = 0xc0;
        reply[out++] = 0x0c;
        write_be16(reply + out, 1); out += 2; // A record
        write_be16(reply + out, 1); out += 2; // Internet class
        write_be32(reply + out, 60); out += 4;
        write_be16(reply + out, 4); out += 2;
        reply[out++] = 192;
        reply[out++] = 168;
        reply[out++] = 4;
        reply[out++] = 1;
    }

    *reply_length = out;
    return true;
}

static void dns_receive(void *arg, struct udp_pcb *pcb, struct pbuf *packet,
                        const ip_addr_t *source, u16_t source_port) {
    (void)arg;
    if (packet == NULL) {
        return;
    }

    uint8_t query[DNS_PACKET_CAPACITY];
    uint8_t reply[DNS_PACKET_CAPACITY];
    const size_t query_length = packet->tot_len;
    if (query_length > sizeof(query)) {
        pbuf_free(packet);
        return;
    }
    pbuf_copy_partial(packet, query, (u16_t)query_length, 0);
    pbuf_free(packet);

    size_t reply_length = 0;
    if (!build_dns_reply(query, query_length, reply, &reply_length)) {
        return;
    }

    struct pbuf *reply_packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)reply_length, PBUF_RAM);
    if (reply_packet == NULL) {
        return;
    }
    pbuf_take(reply_packet, reply, (u16_t)reply_length);
    udp_sendto(pcb, reply_packet, source, source_port);
    pbuf_free(reply_packet);
}

static bool ascii_equal_nocase(const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

static void get_http_host(const char *request, char *host, size_t host_capacity) {
    host[0] = '\0';
    const char *line = strstr(request, "\r\n");
    if (line == NULL) {
        return;
    }
    line += 2;

    while (*line != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL) {
            return;
        }
        if ((size_t)(line_end - line) >= 5 && ascii_equal_nocase(line, "Host:", 5)) {
            const char *value = line + 5;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                ++value;
            }
            size_t length = (size_t)(line_end - value);
            if (length >= host_capacity) {
                length = host_capacity - 1;
            }
            memcpy(host, value, length);
            host[length] = '\0';
            for (size_t i = 0; i < length; ++i) {
                host[i] = (char)tolower((unsigned char)host[i]);
            }
            char *port = strrchr(host, ':');
            if (port != NULL && port[1] != '\0') {
                bool numeric_port = true;
                for (char *p = port + 1; *p != '\0'; ++p) {
                    if (!isdigit((unsigned char)*p)) {
                        numeric_port = false;
                        break;
                    }
                }
                if (numeric_port) {
                    *port = '\0';
                }
            }
            return;
        }
        line = line_end + 2;
    }
}

static bool request_is_complete(const char *request, size_t length) {
    if (length < 4) {
        return false;
    }
    for (size_t i = 3; i < length; ++i) {
        if (request[i - 3] == '\r' && request[i - 2] == '\n' &&
            request[i - 1] == '\r' && request[i] == '\n') {
            return true;
        }
    }
    return false;
}

static err_t http_send_response(struct tcp_pcb *pcb, http_connection_t *connection,
                                int status, const char *reason, const char *content_type,
                                const char *body, size_t body_length, bool nintendo_headers) {
    char response[512];
    int response_length = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "%s"
        "Connection: close\r\n\r\n"
        "%s",
        status, reason, content_type, (unsigned long)body_length,
        nintendo_headers ? "Cache-Control: no-store\r\nX-Organization: Nintendo\r\n" : "",
        body);
    if (response_length < 0 || (size_t)response_length >= sizeof(response)) {
        response_length = 0;
    }

    err_t error = tcp_write(pcb, response, (u16_t)response_length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK) {
        tcp_output(pcb);
    }

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    free(connection);
    error = tcp_close(pcb);
    if (error != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t http_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    http_connection_t *connection = (http_connection_t *)arg;
    if (packet == NULL) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        free(connection);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    if (error != ERR_OK) {
        pbuf_free(packet);
        return error;
    }

    const size_t available = sizeof(connection->request) - 1 - connection->request_length;
    const size_t copied = packet->tot_len < available ? packet->tot_len : available;
    pbuf_copy_partial(packet, connection->request + connection->request_length,
                      (u16_t)copied, 0);
    connection->request_length += copied;
    connection->request[connection->request_length] = '\0';
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);

    if (request_is_complete(connection->request, connection->request_length)) {
        char host[256];
        get_http_host(connection->request, host, sizeof(host));
        if (strcmp(host, "conntest.nintendowifi.net") == 0) {
            static const char body[] =
                "<html><head><title>HTML Page</title></head>"
                "<body>This is test.html page</body></html>";
            return http_send_response(pcb, connection, 200, "OK", "text/html",
                                      body, sizeof(body) - 1, true);
        }

        if (strcmp(host, "ctest.cdn.nintendo.net") == 0 ||
            strcmp(host, "ctest.p01.ctest.srv.nintendo.net") == 0 ||
            strcmp(host, "ctest-dl-lp1.cdn.nintendo.net") == 0 ||
            strcmp(host, "ctest-ul-lp1.cdn.nintendo.net") == 0 ||
            strcmp(host, "api.p01.lp1.ctest.srv.nintendo.net") == 0 ||
            strcmp(host, "api-ipv6.p01.lp1.ctest.srv.nintendo.net") == 0) {
            static const char body[] = "ok";
            return http_send_response(pcb, connection, 200, "OK", "text/plain",
                                      body, sizeof(body) - 1, true);
        }

        return http_send_response(pcb, connection, 404, "Not Found", "text/plain",
                                  "", 0, false);
    }

    if (connection->request_length >= sizeof(connection->request) - 1) {
        return http_send_response(pcb, connection, 400, "Bad Request", "text/plain",
                                  "", 0, false);
    }
    return ERR_OK;
}

static void http_error(void *arg, err_t error) {
    (void)error;
    free(arg);
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t error) {
    (void)arg;
    if (error != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }

    http_connection_t *connection = (http_connection_t *)calloc(1, sizeof(*connection));
    if (connection == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    connection->pcb = pcb;
    tcp_arg(pcb, connection);
    tcp_recv(pcb, http_receive);
    tcp_err(pcb, http_error);
    return ERR_OK;
}

static bool start_dns_server(struct netif *ap_netif) {
    dns_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (dns_pcb == NULL) {
        return false;
    }
    if (udp_bind(dns_pcb, IP_ADDR_ANY, DNS_PORT) != ERR_OK) {
        udp_remove(dns_pcb);
        dns_pcb = NULL;
        return false;
    }
    udp_bind_netif(dns_pcb, ap_netif);
    udp_recv(dns_pcb, dns_receive, NULL);
    return true;
}

static bool start_http_server(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        return false;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }
    struct tcp_pcb *listener = tcp_listen(pcb);
    if (listener == NULL) {
        tcp_close(pcb);
        return false;
    }
    tcp_accept(listener, http_accept);
    return true;
}

static void relay_complete(relay_context_t *context) {
    if (context->done) {
        return;
    }
    if (!context->got_data) {
        printf("[RELAY] (no response)\n");
    }
    send_end();
    context->done = true;
}

static void relay_abort_from_callback(relay_context_t *context,
                                      struct tcp_pcb *pcb, const char *message) {
    printf("%s\n", message);
    context->pcb = NULL;
    context->done = true;
    tcp_abort(pcb);
    send_end();
}

static bool relay_queue_payload(relay_context_t *context, struct tcp_pcb *pcb) {
    while (context->payload_offset < context->payload_length) {
        u16_t available = tcp_sndbuf(pcb);
        if (available == 0) {
            break;
        }
        size_t remaining = context->payload_length - context->payload_offset;
        size_t chunk = remaining < available ? remaining : available;
        if (chunk > TCP_MSS) {
            chunk = TCP_MSS;
        }
        err_t error = tcp_write(pcb, context->payload + context->payload_offset,
                                (u16_t)chunk, TCP_WRITE_FLAG_COPY);
        if (error == ERR_MEM) {
            break;
        }
        if (error != ERR_OK) {
            return false;
        }
        context->payload_offset += chunk;
    }

    tcp_output(pcb);
    if (context->payload_offset == context->payload_length && !context->write_shutdown) {
        err_t error = tcp_shutdown(pcb, 0, 1);
        if (error == ERR_OK) {
            context->write_shutdown = true;
            context->response_deadline = now_ms() + 120000;
            printf("[RELAY] Sent. Reading response...\n");
        } else if (error != ERR_MEM) {
            return false;
        }
    }
    return true;
}

static err_t relay_connected(void *arg, struct tcp_pcb *pcb, err_t error) {
    relay_context_t *context = (relay_context_t *)arg;
    if (error != ERR_OK) {
        relay_abort_from_callback(context, pcb, "[RELAY] ERROR: connection failed");
        return ERR_ABRT;
    }

    context->connected = true;
    if (!relay_queue_payload(context, pcb)) {
        relay_abort_from_callback(context, pcb, "[RELAY] ERROR: write failed");
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t relay_sent(void *arg, struct tcp_pcb *pcb, u16_t length) {
    (void)length;
    relay_context_t *context = (relay_context_t *)arg;
    if (!relay_queue_payload(context, pcb)) {
        relay_abort_from_callback(context, pcb, "[RELAY] ERROR: write failed");
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t relay_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    relay_context_t *context = (relay_context_t *)arg;
    if (packet == NULL) {
        context->pcb = NULL;
        relay_complete(context);
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    if (error != ERR_OK) {
        pbuf_free(packet);
        return error;
    }

    uint8_t buffer[128];
    u16_t offset = 0;
    while (offset < packet->tot_len) {
        const u16_t remaining = packet->tot_len - offset;
        const u16_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        pbuf_copy_partial(packet, buffer, amount, offset);
        stdio_put_string((const char *)buffer, amount, false, false);
        offset += amount;
    }
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    context->got_data = true;
    context->response_deadline = now_ms() + 5000;
    return ERR_OK;
}

static void relay_error(void *arg, err_t error) {
    (void)error;
    relay_context_t *context = (relay_context_t *)arg;
    context->pcb = NULL;
    if (!context->connected) {
        printf("[RELAY] ERROR: connection failed\n");
    } else if (!context->got_data) {
        printf("[RELAY] (no response)\n");
    }
    send_end();
    context->done = true;
}

static void relay_close(relay_context_t *context) {
    if (context->pcb != NULL) {
        struct tcp_pcb *pcb = context->pcb;
        context->pcb = NULL;
        close_tcp_pcb(pcb);
    }
}

static void relay_payload(const char *host, int port, const uint8_t *payload,
                          size_t payload_length) {
    printf("[RELAY] Connecting to %s:%d (%lu bytes)...\n",
           host, port, (unsigned long)payload_length);

    ip_addr_t remote_address;
    if (!ipaddr_aton(host, &remote_address) || !IP_IS_V4(&remote_address)) {
        printf("[RELAY] ERROR: connection failed\n");
        send_end();
        return;
    }

    relay_context_t context;
    memset(&context, 0, sizeof(context));
    context.payload = payload;
    context.payload_length = payload_length;
    context.connect_deadline = now_ms() + 8000;

    context.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (context.pcb == NULL) {
        printf("[RELAY] ERROR: connection failed\n");
        send_end();
        return;
    }

    tcp_arg(context.pcb, &context);
    tcp_err(context.pcb, relay_error);
    tcp_recv(context.pcb, relay_receive);
    tcp_sent(context.pcb, relay_sent);

    err_t error = tcp_connect(context.pcb, &remote_address, (u16_t)port, relay_connected);
    if (error != ERR_OK) {
        relay_close(&context);
        printf("[RELAY] ERROR: connection failed\n");
        send_end();
        return;
    }

    while (!context.done) {
        cyw43_arch_poll();
        dhcp_server_poll(&dhcp_server);
        const uint32_t now = now_ms();
        if (!context.connected && (int32_t)(now - context.connect_deadline) >= 0) {
            relay_close(&context);
            printf("[RELAY] ERROR: connection failed\n");
            send_end();
            break;
        }
        if (context.write_shutdown &&
            (int32_t)(now - context.response_deadline) >= 0) {
            relay_close(&context);
            relay_complete(&context);
            break;
        }

        update_led(last_client_count);
        sleep_ms(1);
    }
}

static bool parse_long_token(char *token, long *value) {
    if (token == NULL || *token == '\0') {
        return false;
    }
    char *end = NULL;
    *value = strtol(token, &end, 10);
    return end != token && *end == '\0';
}

static void process_send_command(void) {
    char *save = NULL;
    char *command = strtok_r(serial_line, " \t", &save);
    char *host = strtok_r(NULL, " \t", &save);
    char *port_token = strtok_r(NULL, " \t", &save);
    char *length_token = strtok_r(NULL, " \t", &save);
    char *extra_token = strtok_r(NULL, " \t", &save);
    long port = 0;
    long payload_length = 0;

    if (command == NULL || strcmp(command, "SEND") != 0 || host == NULL ||
        port_token == NULL || length_token == NULL || extra_token != NULL ||
        !parse_long_token(port_token, &port) ||
        !parse_long_token(length_token, &payload_length) ||
        port < 1 || port > 65535) {
        printf("[ERROR] Usage: SEND <ip> <port> <length>\n");
        send_end();
        return;
    }
    if (payload_length <= 0 || payload_length > MAX_PAYLOAD_LENGTH) {
        printf("[ERROR] Invalid payload length\n");
        send_end();
        return;
    }

    uint8_t *payload = (uint8_t *)malloc((size_t)payload_length);
    if (payload == NULL) {
        printf("[ERROR] Out of memory\n");
        send_end();
        return;
    }

    size_t received = 0;
    uint32_t deadline = now_ms() + 10000;
    while (received < (size_t)payload_length && (int32_t)(now_ms() - deadline) < 0) {
        int value = getchar_timeout_us(0);
        if (value >= 0) {
            payload[received++] = (uint8_t)value;
            deadline = now_ms() + 5000;
        } else {
            cyw43_arch_poll();
            dhcp_server_poll(&dhcp_server);
            sleep_ms(1);
        }
    }

    if (received < (size_t)payload_length) {
        printf("[ERROR] Only got %lu of %ld bytes\n",
               (unsigned long)received, payload_length);
        send_end();
        free(payload);
        return;
    }

    relay_payload(host, (int)port, payload, (size_t)payload_length);
    free(payload);
}

static void process_serial_command(void) {
    serial_line[serial_line_length] = '\0';
    if (serial_line_overflow) {
        printf("[ERROR] Command too long\n");
        send_end();
    } else if (serial_line_length > 0) {
        if (strcmp(serial_line, "STATUS") == 0) {
            uint8_t macs[MAX_CLIENTS * 6];
            last_client_count = get_client_count(macs);
            printf("[STATUS] AP clients: %d\n", last_client_count);
            send_end();
        } else if (strcmp(serial_line, "SCAN") == 0) {
            uint8_t macs[MAX_CLIENTS * 6];
            const int count = get_client_count(macs);
            last_client_count = count;
            printf("[SCAN] %d clients\n", count);
            for (int i = 0; i < count; ++i) {
                const uint8_t *mac = macs + i * 6;
                printf("  %02X:%02X:%02X:%02X:%02X:%02X\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            send_end();
        } else if (strncmp(serial_line, "SEND ", 5) == 0) {
            process_send_command();
        } else {
            printf("[ERROR] Unknown command. Use: SEND, STATUS, SCAN\n");
            send_end();
        }
    }

    serial_line_length = 0;
    serial_line_overflow = false;
}

static bool start_access_point(void) {
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Initializing Wi-Fi driver\n");
#endif
    if (cyw43_arch_init() != 0) {
        return false;
    }
    radio_ready = true;

    cyw43_wifi_ap_set_channel(&cyw43_state, AP_CHANNEL);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Starting access point\n");
#endif
    cyw43_arch_enable_ap_mode(AP_SSID, NULL, CYW43_AUTH_OPEN);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Access point startup returned\n");
#endif

    struct netif *ap_netif = &cyw43_state.netif[CYW43_ITF_AP];
    printf("[NET] AP interface %u flags=0x%02x\n",
           netif_get_index(ap_netif), ap_netif->flags);

    ip_addr_t ap_address;
    ip_addr_t netmask;
    IP_ADDR4(&ap_address, 192, 168, 4, 1);
    IP_ADDR4(&netmask, 255, 255, 255, 0);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Starting DHCP server\n");
#endif
    if (!dhcp_server_init(&dhcp_server, &ap_address, &netmask, ap_netif)) {
        return false;
    }
    printf("[DHCP] Server ready; lease pool is 192.168.4.2-192.168.4.5\n");

    return start_dns_server(ap_netif) && start_http_server();
}

static void handle_serial_input(void) {
    int value;
    while ((value = getchar_timeout_us(0)) >= 0) {
        const char character = (char)value;
        if (character == '\n') {
            process_serial_command();
        } else if (character != '\r' && !serial_line_overflow) {
            if (serial_line_length < sizeof(serial_line) - 1) {
                serial_line[serial_line_length++] = character;
            } else {
                serial_line_overflow = true;
            }
        }
    }
}

int main(void) {
    stdio_init_all();
#if CIVSWITCH_DIAGNOSTIC_BOOT
    while (!stdio_usb_connected()) {
        sleep_ms(10);
    }
    sleep_ms(250);
    printf("\n[BOOT] USB serial connected\n");
#endif
    services_ready = start_access_point();
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Network initialization %s\n", services_ready ? "complete" : "FAILED");
#endif
    bool serial_was_connected = false;
    uint32_t last_diagnostic_time = 0;
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[BOOT] Entering main loop; beginning first Wi-Fi poll\n");
    bool first_poll = true;
#endif

    for (;;) {
        if (radio_ready) {
            cyw43_arch_poll();
            if (services_ready) dhcp_server_poll(&dhcp_server);
        }
#if CIVSWITCH_DIAGNOSTIC_BOOT
        if (first_poll) {
            printf("[BOOT] First Wi-Fi poll returned\n");
            first_poll = false;
        }
#endif
        const bool serial_connected = stdio_usb_connected();
        if (serial_connected && !serial_was_connected) {
            sleep_ms(100);
            printf("\nCivSwitch Serial Bridge\nSSID: %s\n", AP_SSID);
            printf("[INFO] AP services: %s; AP IP: 192.168.4.1\n",
                   services_ready ? "ready" : "FAILED");
            if (services_ready) {
                printf("[DHCP] Lease pool: 192.168.4.2-192.168.4.5\n");
            }
            last_diagnostic_time = now_ms() - 5000;
        }
        serial_was_connected = serial_connected;
        if (serial_connected && radio_ready &&
            (uint32_t)(now_ms() - last_diagnostic_time) >= 5000) {
            struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
            printf("[NET] flags=0x%02x frames AP=%lu STA=%lu DHCP=%lu events=%lu last=%lu/%lu/%lu/%d\n",
                   (unsigned)ap->flags, (unsigned long)received_frames[CYW43_ITF_AP],
                   (unsigned long)received_frames[CYW43_ITF_STA],
                   (unsigned long)received_dhcp_frames, (unsigned long)wifi_events,
                   (unsigned long)last_wifi_event, (unsigned long)last_wifi_status,
                   (unsigned long)last_wifi_reason, last_wifi_interface);
            last_diagnostic_time = now_ms();
        }
        if (services_ready) {
            handle_serial_input();
            update_led(last_client_count);
        }
        sleep_ms(10);
    }
}
