/*
 * Phase 0 Wi-Fi test. On the Tab5 the radio is the ESP32-C6 coprocessor; with
 * esp_wifi_remote + esp_hosted the standard esp_wifi API is forwarded over SDIO.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "bsp/esp-bsp.h"

#include "p0.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_ev;
static bool s_inited;
static volatile bool s_up;
static int s_disconnect_reason;

bool p0_wifi_is_up(void)
{
    return s_up;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_disconnect_reason = d->reason;
        s_up = false;
        xEventGroupSetBits(s_wifi_ev, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        p0_log("Got IP " IPSTR "  gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        s_up = true;
        xEventGroupSetBits(s_wifi_ev, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    /* The BSP gates the C6 behind an IO-expander enable line. */
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), "p0_wifi", "C6 enable");
    vTaskDelay(pdMS_TO_TICKS(300));

    s_wifi_ev = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), "p0_wifi", "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), "p0_wifi", "esp_wifi_init (hosted link to C6)");
    p0_log("esp_wifi_init over esp_hosted took %lld ms", (esp_timer_get_time() - t0) / 1000);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "p0_wifi", "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "p0_wifi", "start");
    s_inited = true;
    return ESP_OK;
}

static void log_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        p0_log("Scan failed");
        return;
    }
    uint16_t n = 12;
    wifi_ap_record_t aps[12];
    esp_wifi_scan_get_ap_records(&n, aps);
    p0_log("Scan: %u networks", n);
    for (int i = 0; i < n; i++) {
        p0_log("  %-32s ch%2d %4d dBm auth %d", (char *)aps[i].ssid, aps[i].primary, aps[i].rssi, aps[i].authmode);
    }
}

p0_result_t p0_test_wifi(void)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        p0_record("wifi", P0_FAIL, "init failed: %s (check esp_hosted / C6 firmware)", esp_err_to_name(err));
        return P0_FAIL;
    }
    log_scan();

    if (!g_p0_cfg.wifi_ssid[0]) {
        p0_record("wifi", P0_FAIL, "no wifi_ssid in %s", P0_CONFIG_PATH);
        return P0_FAIL;
    }
    if (s_up) {
        p0_record("wifi", P0_PASS, "already connected");
        return P0_PASS;
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, g_p0_cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_p0_cfg.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));

    xEventGroupClearBits(s_wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    int64_t t0 = esp_timer_get_time();
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    int64_t dt = (esp_timer_get_time() - t0) / 1000;

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        p0_record("wifi", P0_PASS, "connected to \"%s\" in %lld ms, RSSI %d dBm", g_p0_cfg.wifi_ssid, dt, rssi);
        return P0_PASS;
    }
    p0_record("wifi", P0_FAIL, "connect failed after %lld ms (reason %d)", dt, s_disconnect_reason);
    return P0_FAIL;
}
