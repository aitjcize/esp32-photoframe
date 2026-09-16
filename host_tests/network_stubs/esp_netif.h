#pragma once
#include <stdint.h>

#include "esp_err.h"
typedef struct {
    int dummy;
} esp_netif_t;
typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip, netmask, gw;
} esp_netif_ip_info_t;
typedef struct {
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;
typedef struct {
    struct {
        int ip;
    } ip6_info;
} ip_event_got_ip6_t;
typedef struct {
    struct {
        int type;
        union {
            esp_ip4_addr_t ip4;
        } u_addr;
    } ip;
} esp_netif_dns_info_t;
#define IPSTR "%u"
#define IP2STR(ip) ((ip)->addr)
#define IPV6STR "%d"
#define IPV62STR(ip) (*(ip))
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *netif);
esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *name);
esp_err_t esp_netif_str_to_ip4(const char *str, esp_ip4_addr_t *ip);
esp_err_t esp_netif_dhcpc_start(esp_netif_t *netif);
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, int type, const esp_netif_dns_info_t *dns);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip);
