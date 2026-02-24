// // captures and streams jpeg over UART (to python script)

// #include <stdio.h>
// #include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "esp_log.h"
// #include "esp_camera.h"
// #include "driver/uart.h"

// // Camera GPIO pins — ESP-EYE (ESP32-D0WD)
// #define CAM_PIN_PWDN    -1  // Not used
// #define CAM_PIN_RESET   -1  // Not used (software reset)
// #define CAM_PIN_XCLK     4
// #define CAM_PIN_SIOD    18  // SCCB data
// #define CAM_PIN_SIOC    23  // SCCB clock
// #define CAM_PIN_D7      36
// #define CAM_PIN_D6      37
// #define CAM_PIN_D5      38
// #define CAM_PIN_D4      39
// #define CAM_PIN_D3      35
// #define CAM_PIN_D2      14
// #define CAM_PIN_D1      13
// #define CAM_PIN_D0      34
// #define CAM_PIN_VSYNC    5
// #define CAM_PIN_HREF    27
// #define CAM_PIN_PCLK    25

// static const camera_config_t CAMERA_CONFIG = {
//     .pin_pwdn     = CAM_PIN_PWDN,
//     .pin_reset    = CAM_PIN_RESET,
//     .pin_xclk     = CAM_PIN_XCLK,
//     .pin_sccb_sda = CAM_PIN_SIOD,
//     .pin_sccb_scl = CAM_PIN_SIOC,
//     .pin_d7       = CAM_PIN_D7,
//     .pin_d6       = CAM_PIN_D6,
//     .pin_d5       = CAM_PIN_D5,
//     .pin_d4       = CAM_PIN_D4,
//     .pin_d3       = CAM_PIN_D3,
//     .pin_d2       = CAM_PIN_D2,
//     .pin_d1       = CAM_PIN_D1,
//     .pin_d0       = CAM_PIN_D0,
//     .pin_vsync    = CAM_PIN_VSYNC,
//     .pin_href     = CAM_PIN_HREF,
//     .pin_pclk     = CAM_PIN_PCLK,

//     .xclk_freq_hz = 10000000,        // 10 MHz, conservative for OV2640
//     .ledc_timer   = LEDC_TIMER_0,
//     .ledc_channel = LEDC_CHANNEL_0,

//     .pixel_format = PIXFORMAT_JPEG,
//     .frame_size   = FRAMESIZE_QVGA,  // 320×240
//     .jpeg_quality = 12,              // 0–63, lower = better quality
//     .fb_count     = 1,
//     .fb_location  = CAMERA_FB_IN_DRAM,
//     .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
// };

// // UART functions

// /* Initialise UART0 for raw binary output.
//  * TX buf = 0: uart_write_bytes() blocks until FIFO is drained (no data loss).
//  * printf/fwrite must not be used after this — they conflict with the driver. */
// static void uart_init(void)
// {
//     uart_driver_install(UART_NUM_0, /*rx_buf=*/256, /*tx_buf=*/0, 0, NULL, 0);
// }

// static void uart_send(const char *msg)
// {
//     uart_write_bytes(UART_NUM_0, msg, strlen(msg));
// }

// // Camera functions

// static esp_err_t camera_init(void)
// {
//     return esp_camera_init(&CAMERA_CONFIG);
// }

// static void send_frame(const camera_fb_t *fb)
// {
//     char header[32];
//     int n = snprintf(header, sizeof(header), "FRAME:%zu\n", fb->len);
//     uart_write_bytes(UART_NUM_0, header, n);
//     uart_write_bytes(UART_NUM_0, fb->buf, fb->len);
// }




// void app_main(void)
// {
//     esp_log_level_set("*", ESP_LOG_NONE); // silence logs — they corrupt the UART stream (Just temporary as we use UART)
//     uart_init();

//     esp_err_t err = camera_init();
//     if (err != ESP_OK) {
//         char msg[32];
//         snprintf(msg, sizeof(msg), "CAM_ERR:0x%x\n", err);
//         uart_send(msg);
//         return;
//     }

//     while (1) {
//         camera_fb_t *fb = esp_camera_fb_get();
//         if (!fb) {
//             uart_send("CAM_ERR:capture_failed\n");
//             continue;
//         }
//         send_frame(fb);
//         esp_camera_fb_return(fb);
//     }
// }


/*
 * ESP-EYE  —  BLE GATT Server
 *
 * Advertises as "ESP-EYE", exposes one notify characteristic, and sends
 * "Hello from ESP-EYE!" to the connected client every 2 seconds.
 *
 * Target:   ESP-EYE (ESP32)
 * IDF:      v5.x  (Bluedroid BLE stack)
 *
 * Menuconfig requirements:
 *   Component config → Bluetooth → [*] Bluetooth
 *   Bluetooth Host   → Bluedroid
 *   Bluedroid options → [*] BLE
 *   (Classic BT can be left off to save RAM)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

/*  Tag for log messages  */
#define TAG "BLE_SERVER"

/*  UUIDs  (must match the client exactly) */
#define SERVICE_UUID        0xABCD
#define CHAR_UUID           0xFF01

/*  GATT profile bookkeeping  */
#define PROFILE_APP_ID      0
#define PROFILE_NUM         1

/* Attribute table index enum */
enum {
    IDX_SVC,

    IDX_CHAR_DECL,       /* characteristic declaration */
    IDX_CHAR_VAL,        /* characteristic value       */
    IDX_CHAR_CFG,        /* CCCD descriptor            */

    IDX_NB               /* total number of attributes */
};

static uint16_t attr_handle_table[IDX_NB];

/*  Profile instance  */
struct gatts_profile_inst {
    esp_gatts_cb_t      gatts_cb;
    uint16_t            gatts_if;
    uint16_t            conn_id;
    bool                connected;
    bool                notify_enabled;   /* client wrote 0x0001 to CCCD */
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

/*  Advertising data (raw bytes)  */
/*
 * Format:  [len] [type] [data ...]
 *   0x02 0x01 0x06       → Flags: LE General Discoverable, BR/EDR not supported
 *   0x03 0x03 0xCD 0xAB  → Complete list of 16-bit UUIDs (SERVICE_UUID = 0xABCD, LSB first)
 */
static const uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, 0xCD, 0xAB,
};

/*
 * Scan-response carries the full device name so the client can identify us.
 *   0x09 0x09 'E','S','P','-','E','Y','E'
 *   (length byte = 1 + strlen("ESP-EYE") = 8, but name is 7 chars → 8 total)
 */
static const uint8_t raw_scan_rsp_data[] = {
    0x08, 0x09, 'E', 'S', 'P', '-', 'E', 'Y', 'E',
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,          /* 20 ms */
    .adv_int_max        = 0x40,          /* 40 ms */
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* Flags to coordinate GAP adv-config callbacks before starting advertising */
#define ADV_CONFIG_FLAG       (1 << 0)
#define SCAN_RSP_CONFIG_FLAG  (1 << 1)
static uint8_t adv_config_done = 0;

/*  GATT attribute table  */
static const uint16_t primary_service_uuid    = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t char_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t  char_prop_read_notify   = ESP_GATT_CHAR_PROP_BIT_READ |
                                                 ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static const uint16_t service_uuid            = SERVICE_UUID;
static const uint16_t char_uuid               = CHAR_UUID;

/* Initial CCCD value — notifications disabled */
static uint8_t cccd_value[2] = {0x00, 0x00};

/* Initial characteristic value */
static const uint8_t char_value[] = "Hello from ESP-EYE!";

static const esp_gatts_attr_db_t gatt_db[IDX_NB] = {

    /*  Service declaration  */
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&primary_service_uuid,
            ESP_GATT_PERM_READ,
            sizeof(service_uuid),
            sizeof(service_uuid),
            (uint8_t *)&service_uuid
        }
    },

    /*  Characteristic declaration  */
    [IDX_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            sizeof(char_prop_read_notify),
            sizeof(char_prop_read_notify),
            (uint8_t *)&char_prop_read_notify
        }
    },

    /*  Characteristic value  */
    [IDX_CHAR_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_uuid,
            ESP_GATT_PERM_READ,
            /* max_length */ 64,
            sizeof(char_value),
            (uint8_t *)char_value
        }
    },

    /*  Client Characteristic Configuration Descriptor (CCCD)  */
    [IDX_CHAR_CFG] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_client_config_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(cccd_value),
            sizeof(cccd_value),
            (uint8_t *)cccd_value
        }
    },
};

/*  Helper: send a notification  */
static void send_notification(esp_gatt_if_t gatts_if, uint16_t conn_id,
                               const char *msg)
{
    size_t len = strlen(msg);
    esp_err_t ret = esp_ble_gatts_send_indicate(
        gatts_if,
        conn_id,
        attr_handle_table[IDX_CHAR_VAL],
        len,
        (uint8_t *)msg,
        false   /* false = notification (no ack required) */
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_indicate failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Sent notification: \"%s\"", msg);
    }
}

/*  GAP event handler  */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= ~ADV_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d",
                     param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed, status %d",
                     param->adv_stop_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising stopped");
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG,
                 "Connection params updated: status=%d, min_int=%d, max_int=%d, "
                 "latency=%d, timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

/*  GATTS profile event handler  */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    /* Application registered — set up advertising and create the attribute table */
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "REG_EVT, app_id=%d", param->reg.app_id);

        adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;

        esp_ble_gap_config_adv_data_raw((uint8_t *)raw_adv_data,
                                        sizeof(raw_adv_data));
        esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)raw_scan_rsp_data,
                                             sizeof(raw_scan_rsp_data));

        esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, IDX_NB,
                                      /* service instance id */ 0);
        break;

    /* Attribute table created — store handles and start the service */
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "create attr table failed, status=%x",
                     param->add_attr_tab.status);
            break;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "unexpected handle count %d (expected %d)",
                     param->add_attr_tab.num_handle, IDX_NB);
            break;
        }
        memcpy(attr_handle_table, param->add_attr_tab.handles,
               sizeof(attr_handle_table));
        ESP_LOGI(TAG, "Attr table created, service handle=%d",
                 attr_handle_table[IDX_SVC]);
        esp_ble_gatts_start_service(attr_handle_table[IDX_SVC]);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Service started, handle=%d", param->start.service_handle);
        break;

    /* Client connected */
    case ESP_GATTS_CONNECT_EVT:
        profile_tab[PROFILE_APP_ID].conn_id   = param->connect.conn_id;
        profile_tab[PROFILE_APP_ID].connected = true;
        profile_tab[PROFILE_APP_ID].notify_enabled = false;
        ESP_LOGI(TAG, "Client connected, conn_id=%d", param->connect.conn_id);

        /* Request a shorter connection interval for lower latency */
        esp_ble_conn_update_params_t conn_params = {
            .min_int  = 0x10,    /* 20  ms */
            .max_int  = 0x20,    /* 40  ms */
            .latency  = 0,
            .timeout  = 400,     /* 4 s  */
        };
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    /* Client disconnected — restart advertising */
    case ESP_GATTS_DISCONNECT_EVT:
        profile_tab[PROFILE_APP_ID].connected      = false;
        profile_tab[PROFILE_APP_ID].notify_enabled = false;
        ESP_LOGI(TAG, "Client disconnected, reason=0x%x",
                 param->disconnect.reason);
        esp_ble_gap_start_advertising(&adv_params);
        break;

    /* Client wrote to a characteristic (e.g. enabling/disabling CCCD) */
    case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep) {
            /* Check whether this is a CCCD write */
            if (attr_handle_table[IDX_CHAR_CFG] == param->write.handle
                && param->write.len == 2)
            {
                uint16_t cccd = (param->write.value[1] << 8)
                              |  param->write.value[0];
                if (cccd == 0x0001) {
                    profile_tab[PROFILE_APP_ID].notify_enabled = true;
                    ESP_LOGI(TAG, "Notifications ENABLED by client");
                } else {
                    profile_tab[PROFILE_APP_ID].notify_enabled = false;
                    ESP_LOGI(TAG, "Notifications DISABLED by client");
                }
            }
        }
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU set to %d", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/*  Global GATTS dispatch (routes to profile handler)  */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    /* On registration, remember which interface belongs to our profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            profile_tab[PROFILE_APP_ID].gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "Reg app failed, app_id=%04x, status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gatts_if == ESP_GATT_IF_NONE ||
            gatts_if == profile_tab[i].gatts_if)
        {
            if (profile_tab[i].gatts_cb) {
                profile_tab[i].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

/*  Notification task: fires every 2 s while a client is connected  */
static void notify_task(void *arg)
{
    const char *msg = "Hello from ESP-EYE!";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (profile_tab[PROFILE_APP_ID].connected &&
            profile_tab[PROFILE_APP_ID].notify_enabled)
        {
            send_notification(profile_tab[PROFILE_APP_ID].gatts_if,
                              profile_tab[PROFILE_APP_ID].conn_id,
                              msg);
        }
    }
}

/*  app_main  */
void app_main(void)
{
    esp_err_t ret;

    /* NVS is required by the BT stack */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Release Classic BT memory — we only need BLE */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    /* Init and enable BT controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    ESP_ERROR_CHECK(ret);

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    ESP_ERROR_CHECK(ret);

    /* Init and enable Bluedroid host stack */
    ret = esp_bluedroid_init();
    ESP_ERROR_CHECK(ret);

    ret = esp_bluedroid_enable();
    ESP_ERROR_CHECK(ret);

    /* Register callbacks */
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    ESP_ERROR_CHECK(ret);

    ret = esp_ble_gap_register_callback(gap_event_handler);
    ESP_ERROR_CHECK(ret);

    /* Register application profile — triggers ESP_GATTS_REG_EVT */
    ret = esp_ble_gatts_app_register(PROFILE_APP_ID);
    ESP_ERROR_CHECK(ret);

    /* Increase MTU to allow larger payloads (useful for image chunking later) */
    ret = esp_ble_gatt_set_local_mtu(500);
    ESP_ERROR_CHECK(ret);

    /* Start notification task */
    xTaskCreate(notify_task, "notify_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE Server initialised — advertising as \"ESP-EYE\"");
}