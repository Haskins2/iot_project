/**
 * @file sensor_packet.c
 * @brief JSON packet formatter for cloud transmission
 *
 * Uses snprintf to avoid external dependencies.  The cloud team can
 * parse this with any standard JSON library.
 */

#include "sensor_packet.h"

#include <stdio.h>
#include "esp_log.h"

int FormatSensorPacket(const water_data_t *water,
                       const raindrop_data_t *rain,
                       bool ble_ok,
                       char *buf,
                       int buf_len)
{
    if (buf == NULL || buf_len <= 0) {
        return -1;
    }

    int n = snprintf(buf, (size_t)buf_len,
        "{"
            "\"water_sensor\":{\"raw\":%d},"
            "\"raindrop_sensor\":{\"digital\":%d,\"analog\":%d},"
            "\"ble_connected\":%s,"
            "\"timestamp_ms\":%lu"
        "}",
        water  ? water->raw     : -1,
        rain   ? rain->digital  : -1,
        rain   ? rain->analog   : -1,
        ble_ok ? "true" : "false",
        (unsigned long)esp_log_timestamp());

    if (n < 0 || n >= buf_len) {
        ESP_LOGE("sensor_packet", "JSON buffer too small (need %d, have %d)", n, buf_len);
        return -1;
    }

    return n;
}
