/*
 * Phase 0 configuration loader.
 *
 * Reads /sdcard/phase0.json, for example:
 * {
 *   "wifi_ssid": "ShopWiFi",
 *   "wifi_pass": "********",
 *   "dav_base_url": "https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/",
 *   "dav_user": "tensile-hmi",
 *   "dav_app_password": "xxxxx-xxxxx-xxxxx-xxxxx-xxxxx",
 *   "tz": "EST5EDT,M3.2.0,M11.1.0"
 * }
 * Secrets stay on the card; nothing here logs them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "p0.h"

static const char *TAG = "p0_config";

p0_config_t g_p0_cfg = {
    .dav_base_url = "https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/",
    .dav_user = "tensile-hmi",
    .tz = "EST5EDT,M3.2.0,M11.1.0",
};

static void copy_str(const cJSON *root, const char *key, char *dst, size_t dst_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

esp_err_t p0_config_load(void)
{
    FILE *f = fopen(P0_CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "%s not found", P0_CONFIG_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = calloc(1, 2048);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, 2047, f);
    fclose(f);

    cJSON *root = cJSON_ParseWithLength(buf, n);
    memset(buf, 0, 2048); /* file contains secrets */
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "%s is not valid JSON", P0_CONFIG_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    copy_str(root, "wifi_ssid", g_p0_cfg.wifi_ssid, sizeof(g_p0_cfg.wifi_ssid));
    copy_str(root, "wifi_pass", g_p0_cfg.wifi_pass, sizeof(g_p0_cfg.wifi_pass));
    copy_str(root, "dav_base_url", g_p0_cfg.dav_base_url, sizeof(g_p0_cfg.dav_base_url));
    copy_str(root, "dav_user", g_p0_cfg.dav_user, sizeof(g_p0_cfg.dav_user));
    copy_str(root, "dav_app_password", g_p0_cfg.dav_app_password, sizeof(g_p0_cfg.dav_app_password));
    copy_str(root, "tz", g_p0_cfg.tz, sizeof(g_p0_cfg.tz));
    cJSON_Delete(root);

    g_p0_cfg.loaded = true;
    ESP_LOGI(TAG, "Loaded: ssid=\"%s\" wifi_pass=%s dav_user=%s app_password=%s tz=%s",
             g_p0_cfg.wifi_ssid, g_p0_cfg.wifi_pass[0] ? "***" : "(empty)", g_p0_cfg.dav_user,
             g_p0_cfg.dav_app_password[0] ? "***" : "(empty)", g_p0_cfg.tz);
    return ESP_OK;
}
