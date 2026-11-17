#ifndef CIVSWITCH_DHCPSERVER_H
#define CIVSWITCH_DHCPSERVER_H
#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
#include <stddef.h>

#include <lwip/ip_addr.h>
#include <lwip/netif.h>

#define DHCPS_BASE_IP (2)
#define DHCPS_MAX_IP (4)

typedef struct _dhcp_server_lease_t {
    uint8_t mac[6];
    uint16_t expiry;
} dhcp_server_lease_t;

typedef struct _dhcp_server_t {
    ip_addr_t ip;
    ip_addr_t nm;
    dhcp_server_lease_t lease[DHCPS_MAX_IP];
    struct udp_pcb *udp;
    struct netif *netif;
    uint8_t pending_reply[548];
    size_t pending_reply_length;
    uint8_t pending_reply_type;
} dhcp_server_t;

bool dhcp_server_init(dhcp_server_t *d, ip_addr_t *ip, ip_addr_t *nm, struct netif *netif);
void dhcp_server_poll(dhcp_server_t *d);
void dhcp_server_deinit(dhcp_server_t *d);
#ifdef __cplusplus
}
#endif


#endif
