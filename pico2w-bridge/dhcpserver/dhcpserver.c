#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "dhcpserver.h"
#include <lwip/udp.h>
#include <pico/time.h>

#define DHCPDISCOVER    (1)
#define DHCPOFFER       (2)
#define DHCPREQUEST     (3)
#define DHCPDECLINE     (4)
#define DHCPACK         (5)
#define DHCPNACK        (6)
#define DHCPRELEASE     (7)
#define DHCPINFORM      (8)

#define DHCP_OPT_PAD                (0)
#define DHCP_OPT_SUBNET_MASK        (1)
#define DHCP_OPT_ROUTER             (3)
#define DHCP_OPT_DNS                (6)
#define DHCP_OPT_HOST_NAME          (12)
#define DHCP_OPT_REQUESTED_IP       (50)
#define DHCP_OPT_IP_LEASE_TIME      (51)
#define DHCP_OPT_MSG_TYPE           (53)
#define DHCP_OPT_SERVER_ID          (54)
#define DHCP_OPT_PARAM_REQUEST_LIST (55)
#define DHCP_OPT_MAX_MSG_SIZE       (57)
#define DHCP_OPT_VENDOR_CLASS_ID    (60)
#define DHCP_OPT_CLIENT_ID          (61)
#define DHCP_OPT_END                (255)

#define PORT_DHCP_SERVER (67)
#define PORT_DHCP_CLIENT (68)

#define DEFAULT_LEASE_TIME_S (24 * 60 * 60)

#define MAC_LEN (6)
#define MAKE_IP4(a, b, c, d) ((a) << 24 | (b) << 16 | (c) << 8 | (d))

typedef struct {
    uint8_t op; // Message operation.
    uint8_t htype; // Hardware address type.
    uint8_t hlen; // Hardware address length.
    uint8_t hops;
    uint32_t xid; // Client transaction ID.
    uint16_t secs; // Time passed, in seconds.
    uint16_t flags;
    uint8_t ciaddr[4]; // Client IP address.
    uint8_t yiaddr[4]; // IP address from the server.
    uint8_t siaddr[4]; // Next server IP address.
    uint8_t giaddr[4]; // Relay agent IP address.
    uint8_t chaddr[16]; // Client hardware address.
    uint8_t sname[64]; // Server name.
    uint8_t file[128]; // Boot file name.
    uint8_t options[312]; // DHCP options.
} dhcp_msg_t;

static inline uint32_t cyw43_hal_ticks_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static int dhcp_socket_new_dgram(struct udp_pcb **udp, void *cb_data, udp_recv_fn cb_udp_recv) {
    *udp = udp_new();
    if (*udp == NULL) {
        return -ENOMEM;
    }

    udp_recv(*udp, cb_udp_recv, (void *)cb_data);

    return 0;
}

static void dhcp_socket_free(struct udp_pcb **udp) {
    if (*udp != NULL) {
        udp_remove(*udp);
        *udp = NULL;
    }
}

static int dhcp_socket_bind(struct udp_pcb **udp, uint32_t ip, uint16_t port) {
    ip_addr_t addr;
    IP_ADDR4(&addr, ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
    /* OPEN TASK: Change lwIP error codes to errno values. */
    return udp_bind(*udp, &addr, port);
}

static err_t dhcp_socket_sendto(struct udp_pcb **udp, struct netif *netif,
                                const void *buf, size_t len, uint32_t ip, uint16_t port) {
    if (len > 0xffff) {
        len = 0xffff;
    }

#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[DHCP] Allocating reply packet (%u bytes)\n", (unsigned)len);
#endif
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[DHCP] Reply packet allocation %s\n", p == NULL ? "failed" : "complete");
#endif
    if (p == NULL) {
        return ERR_MEM;
    }

    memcpy(p->payload, buf, len);

    ip_addr_t dest;
    IP_ADDR4(&dest, ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[DHCP] Passing reply to UDP\n");
#endif
    err_t err = udp_sendto_if(*udp, p, &dest, port, netif);
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[DHCP] UDP send returned %d\n", (int)err);
#endif

    pbuf_free(p);

    return err;
}

static uint8_t *opt_find(uint8_t *options, size_t options_length, uint8_t cmd) {
    size_t offset = 0;
    while (offset < options_length) {
        const uint8_t code = options[offset];
        if (code == DHCP_OPT_END) {
            return NULL;
        }
        if (code == DHCP_OPT_PAD) {
            ++offset;
            continue;
        }
        if (offset + 1 >= options_length) {
            return NULL;
        }
        const size_t value_length = options[offset + 1];
        if (value_length > options_length - offset - 2) {
            return NULL;
        }
        if (code == cmd) {
            return &options[offset];
        }
        offset += 2 + value_length;
    }
    return NULL;
}

static void opt_write_n(uint8_t **opt, uint8_t cmd, size_t n, void *data) {
    uint8_t *o = *opt;
    *o++ = cmd;
    *o++ = n;
    memcpy(o, data, n);
    *opt = o + n;
}

static void opt_write_u8(uint8_t **opt, uint8_t cmd, uint8_t val) {
    uint8_t *o = *opt;
    *o++ = cmd;
    *o++ = 1;
    *o++ = val;
    *opt = o;
}

static void opt_write_u32(uint8_t **opt, uint8_t cmd, uint32_t val) {
    uint8_t *o = *opt;
    *o++ = cmd;
    *o++ = 4;
    *o++ = val >> 24;
    *o++ = val >> 16;
    *o++ = val >> 8;
    *o++ = val;
    *opt = o;
}

static void dhcp_server_process(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *src_addr, u16_t src_port) {
    dhcp_server_t *d = arg;
    (void)upcb;
    (void)src_addr;
    (void)src_port;
    if (p == NULL) {
        return;
    }
    printf("[DHCP] RX %u bytes\n", (unsigned int)p->tot_len);

    /* Keep this buffer static. It is too large for the stack. */
    static dhcp_msg_t dhcp_msg;

    static const uint8_t magic_cookie[4] = {99, 130, 83, 99};
    const size_t options_offset = offsetof(dhcp_msg_t, options);
    const size_t minimum_size = options_offset + sizeof(magic_cookie) + 3;
    if (p->tot_len < minimum_size) {
        goto ignore_request;
    }

    const size_t len = pbuf_copy_partial(p, &dhcp_msg, sizeof(dhcp_msg), 0);
    if (len < minimum_size || dhcp_msg.op != 1 || dhcp_msg.hlen < MAC_LEN ||
        memcmp(dhcp_msg.options, magic_cookie, sizeof(magic_cookie)) != 0) {
        goto ignore_request;
    }

    uint8_t *request_options = dhcp_msg.options + sizeof(magic_cookie);
    const size_t request_options_length = len - options_offset - sizeof(magic_cookie);
    uint8_t *message_type_option = opt_find(request_options, request_options_length,
                                             DHCP_OPT_MSG_TYPE);
    if (message_type_option == NULL || message_type_option[1] != 1) {
        goto ignore_request;
    }
    const uint8_t message_type = message_type_option[2];

    uint8_t requested_ip[4];
    if (message_type == DHCPREQUEST) {
        uint8_t *requested_ip_option = opt_find(request_options, request_options_length,
                                                 DHCP_OPT_REQUESTED_IP);
        if (requested_ip_option == NULL || requested_ip_option[1] != sizeof(requested_ip)) {
            printf("[DHCP] Ignoring request without a valid requested IP\n");
            goto ignore_request;
        }
        memcpy(requested_ip, requested_ip_option + 2, sizeof(requested_ip));
    }

    uint8_t response_type;
    memset(dhcp_msg.yiaddr, 0, sizeof(dhcp_msg.yiaddr));
    switch (message_type) {
    case DHCPDISCOVER: {
        int yi = -1;
        for (int i = 0; i < DHCPS_MAX_IP; ++i) {
            if (memcmp(d->lease[i].mac, dhcp_msg.chaddr, MAC_LEN) == 0) {
                yi = i;
                break;
            }
            const uint32_t expiry = ((uint32_t)d->lease[i].expiry << 16) | 0xffff;
            const bool expired = (int32_t)(expiry - cyw43_hal_ticks_ms()) < 0;
            const bool unused = memcmp(d->lease[i].mac, "\x00\x00\x00\x00\x00\x00", MAC_LEN) == 0;
            if (unused || expired) {
                if (expired) {
                    memset(d->lease[i].mac, 0, MAC_LEN);
                }
                yi = i;
                break;
            }
        }
        if (yi < 0) {
            printf("[DHCP] No free leases\n");
            goto ignore_request;
        }
        memcpy(dhcp_msg.yiaddr, ip_2_ip4(&d->ip), 4);
        dhcp_msg.yiaddr[3] = DHCPS_BASE_IP + yi;
        response_type = DHCPOFFER;
        break;
    }

    case DHCPREQUEST: {
        if (memcmp(requested_ip, ip_2_ip4(&d->ip), 3) != 0) {
            printf("[DHCP] Requested IP is outside the AP subnet\n");
            goto ignore_request;
        }
        const uint8_t yi = (uint8_t)(requested_ip[3] - DHCPS_BASE_IP);
        if (yi >= DHCPS_MAX_IP) {
            printf("[DHCP] Requested IP is outside the lease pool\n");
            goto ignore_request;
        }
        if (memcmp(d->lease[yi].mac, dhcp_msg.chaddr, MAC_LEN) == 0) {
            /* Keep the current lease. */
        } else if (memcmp(d->lease[yi].mac, "\x00\x00\x00\x00\x00\x00", MAC_LEN) == 0) {
            memcpy(d->lease[yi].mac, dhcp_msg.chaddr, MAC_LEN);
        } else {
            printf("[DHCP] Requested lease belongs to another client\n");
            goto ignore_request;
        }
        d->lease[yi].expiry = (cyw43_hal_ticks_ms() + DEFAULT_LEASE_TIME_S * 1000) >> 16;
        memcpy(dhcp_msg.yiaddr, ip_2_ip4(&d->ip), 4);
        dhcp_msg.yiaddr[3] = DHCPS_BASE_IP + yi;
        response_type = DHCPACK;
        break;
    }

    default:
        goto ignore_request;
    }

    /* Set the op field to BOOTREPLY (2). */
    dhcp_msg.op = 2;
    memset(dhcp_msg.options, 0, sizeof(dhcp_msg.options));
    memcpy(dhcp_msg.options, magic_cookie, sizeof(magic_cookie));
    uint8_t *opt = dhcp_msg.options + sizeof(magic_cookie);
    opt_write_u8(&opt, DHCP_OPT_MSG_TYPE, response_type);
    opt_write_n(&opt, DHCP_OPT_SERVER_ID, 4, ip_2_ip4(&d->ip));
    opt_write_n(&opt, DHCP_OPT_SUBNET_MASK, 4, ip_2_ip4(&d->nm));
    opt_write_n(&opt, DHCP_OPT_ROUTER, 4, ip_2_ip4(&d->ip));
    opt_write_n(&opt, DHCP_OPT_DNS, 4, ip_2_ip4(&d->ip));
    opt_write_u32(&opt, DHCP_OPT_IP_LEASE_TIME, DEFAULT_LEASE_TIME_S);
    *opt++ = DHCP_OPT_END;

    const size_t reply_length = (size_t)(opt - (uint8_t *)&dhcp_msg);
    if (d->pending_reply_length == 0 && reply_length <= sizeof(d->pending_reply)) {
        memcpy(d->pending_reply, &dhcp_msg, reply_length);
        d->pending_reply_length = reply_length;
        d->pending_reply_type = response_type;
#if CIVSWITCH_DIAGNOSTIC_BOOT
        printf("[DHCP] Queued %s\n", response_type == DHCPOFFER ? "OFFER" : "ACK");
#endif
    } else {
        printf("[DHCP] Reply queue full\n");
    }

ignore_request:
    pbuf_free(p);
}

bool dhcp_server_init(dhcp_server_t *d, ip_addr_t *ip, ip_addr_t *nm, struct netif *netif) {
    ip_addr_copy(d->ip, *ip);
    ip_addr_copy(d->nm, *nm);
    d->netif = netif;
    d->pending_reply_length = 0;
    memset(d->lease, 0, sizeof(d->lease));
    if (dhcp_socket_new_dgram(&d->udp, d, dhcp_server_process) != 0) {
        printf("[DHCP] Could not allocate UDP server\n");
        return false;
    }
    const err_t bind_error = (err_t)dhcp_socket_bind(&d->udp, 0, PORT_DHCP_SERVER);
    if (bind_error != ERR_OK) {
        printf("[DHCP] Could not bind UDP port %u: %d\n",
               PORT_DHCP_SERVER, (int)bind_error);
        dhcp_socket_free(&d->udp);
        return false;
    }
    /* Do not bind this PCB to one interface. This lets DHCP broadcast packets reach the AP interface. */
    return true;
}

void dhcp_server_poll(dhcp_server_t *d) {
    if (d->pending_reply_length == 0 || d->udp == NULL) {
        return;
    }
    const size_t reply_length = d->pending_reply_length;
    const uint8_t reply_type = d->pending_reply_type;
#if CIVSWITCH_DIAGNOSTIC_BOOT
    printf("[DHCP] Sending queued %s\n", reply_type == DHCPOFFER ? "OFFER" : "ACK");
#endif
    const err_t error = dhcp_socket_sendto(&d->udp, d->netif, d->pending_reply,
                                            reply_length, 0xffffffff, PORT_DHCP_CLIENT);
    d->pending_reply_length = 0;
    if (error != ERR_OK) {
        printf("[DHCP] Reply send failed: %d\n", (int)error);
    } else {
        const uint8_t *address = d->pending_reply + offsetof(dhcp_msg_t, yiaddr);
        printf("[DHCP] %s %u.%u.%u.%u\n",
               reply_type == DHCPOFFER ? "OFFER" : "ACK",
               address[0], address[1], address[2], address[3]);
    }
}

void dhcp_server_deinit(dhcp_server_t *d) {
    dhcp_socket_free(&d->udp);
}
