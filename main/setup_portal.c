/**
 * @file setup_portal.c
 * @brief Setup mode: access point + captive DNS (see setup_portal.h).
 *
 * Like the Arduino firmware's web setup: join the open hotspot, and any
 * address a phone looks up (its connectivity check included) leads to this
 * board, whose HTTP server shows the settings page.
 */
#include "setup_portal.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "minischeme_lib.h"

static const char *TAG = "setup";

static bool s_active = false;
static char s_ssid[33] = "";
static char s_pass[64] = "12345678";
static char s_ip[16] = "192.168.4.1";
static uint32_t s_ip_addr = 0; /* network byte order, as an A record holds it */
static int s_preset_index = 0;

static void apply_preset(int idx) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    s_preset_index = (idx >= 0 ? idx : 0) % 2;
    if (s_preset_index == 0) {
        snprintf(s_ssid, sizeof(s_ssid), "Desk-%02X%02X", mac[4], mac[5]);
        snprintf(s_pass, sizeof(s_pass), "12345678");
    } else {
        snprintf(s_ssid, sizeof(s_ssid), "MiniScheme-%02X%02X", mac[4], mac[5]);
        snprintf(s_pass, sizeof(s_pass), "88888888");
    }
}

/* Answers every A question with the access point's address; other types
 * get an empty answer. Requests are handled in place (512-byte UDP). */
static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (n < 12 || (buf[2] & 0x80)) continue;          /* too short, or a response */
        int qdcount = (buf[4] << 8) | buf[5];
        /* end of the first question: its name labels, then type and class */
        int p = 12;
        while (p < n && buf[p] != 0) p += buf[p] + 1;
        if (qdcount < 1 || p + 5 > n) continue;
        int qtype = (buf[p + 1] << 8) | buf[p + 2];
        int end = p + 5;
        buf[2] = 0x84 | (buf[2] & 0x01);                    /* response, authoritative, RD kept */
        buf[3] = 0x80;                                      /* recursion available, no error */
        buf[4] = 0; buf[5] = 1;                             /* one question */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;           /* no authority / additional */
        int len = end;
        if (qtype == 1) {                                   /* A */
            static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
            memcpy(buf + len, answer, sizeof(answer));
            memcpy(buf + len + sizeof(answer), &s_ip_addr, 4);
            len += (int)sizeof(answer) + 4;
            buf[6] = 0; buf[7] = 1;
        } else {
            buf[6] = buf[7] = 0;
        }
        sendto(sock, buf, (size_t)len, 0, (struct sockaddr *)&from, from_len);
    }
}

bool setup_portal_start(void) {
    if (s_active) return true;
    apply_preset(0);
    /* a station that cannot connect would keep hopping channels */
    esp32_wifi_pause_station();
    if (!hal_wifi_ap(s_ssid, s_pass)) {
        ESP_LOGE(TAG, "Access point failed to start");
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        return false;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr) {
        s_ip_addr = info.ip.addr;
        esp_ip4addr_ntoa(&info.ip, s_ip, sizeof(s_ip));
    } else {
        s_ip_addr = PP_HTONL(LWIP_MAKEU32(192, 168, 4, 1));
    }
    if (xTaskCreate(dns_task, "setup_dns", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "DNS task failed: open http://%s/ by hand", s_ip);
    }
    s_active = true;
    ESP_LOGI(TAG, "Setup mode: join %s (pass: %s), open http://%s/", s_ssid, s_pass, s_ip);
    return true;
}

bool setup_portal_switch(void) {
    if (!s_active) return setup_portal_start();
    apply_preset(s_preset_index + 1);
    if (!hal_wifi_ap(s_ssid, s_pass)) {
        ESP_LOGE(TAG, "Access point switch failed");
        return false;
    }
    ESP_LOGI(TAG, "Setup mode switched: join %s (pass: %s), open http://%s/", s_ssid, s_pass, s_ip);
    return true;
}

bool setup_portal_active(void) { return s_active; }
const char *setup_portal_ssid(void) { return s_ssid; }
const char *setup_portal_pass(void) { return s_pass; }
const char *setup_portal_ip(void) { return s_ip; }

int setup_portal_clients(void) {
    if (!s_active) return 0;
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}
