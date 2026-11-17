#ifndef CIVSWITCH_LWIPOPTS_H
#define CIVSWITCH_LWIPOPTS_H

#define NO_SYS 1
#define MEM_ALIGNMENT 4

/* The bridge has its own DHCP server and DNS server. Do not use the lwIP clients. */
#define LWIP_DHCP 0
#define LWIP_DNS 0
/* Accept DHCP requests on port 67. These requests come from clients that do not have an IP address. */
#define LWIP_IP_ACCEPT_UDP_PORT(port) ((port) == PP_NTOHS(67))

#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

#define LWIP_NETIF_HOSTNAME 0
#define LWIP_NETIF_STATUS_CALLBACK 0

#endif
