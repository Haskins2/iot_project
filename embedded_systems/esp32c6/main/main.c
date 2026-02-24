/*
 * ESP32-C6  —  BLE GATT Client
 *
 * Scans for "ESP-EYE", connects, discovers service 0xABCD / char 0xFF01,
 * enables notifications, and logs received data.
 *
 * Target:   ESP32-C6
 * IDF:      v5.x  (Bluedroid BLE stack)
 *
 * Menuconfig requirements:
 *   Component config → Bluetooth → [*] Bluetooth
 *   Bluetooth Host   → Bluedroid
 *   Bluedroid options → [*] BLE, [*] GATTC
 *   [*] BLE 4.2 features   (needed for legacy scan APIs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

/* ── Tag for log messages ───────────────────────────────────────────────── */
#define TAG "BLE_CLIENT"

/* ── UUIDs (must match the server exactly) ──────────────────────────────── */
#define REMOTE_SERVICE_UUID   0xABCD
#define REMOTE_CHAR_UUID      0xFF01

/* ── Device name to look for ────────────────────────────────────────────── */
#define REMOTE_DEVICE_NAME    "ESP-EYE"

/* ── GATT profile bookkeeping ───────────────────────────────────────────── */
#define PROFILE_APP_ID        0
#define PROFILE_NUM           1

struct gattc_profile_inst {
    esp_gattc_cb_t       gattc_cb;
    uint16_t             gattc_if;
    uint16_t             conn_id;
    uint16_t             service_start_handle;
    uint16_t             service_end_handle;
    uint16_t             char_handle;
    esp_bd_addr_t        remote_bda;
    bool                 connected;
    bool                 service_found;
    bool                 char_found;
};

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param);

static struct gattc_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,
    },
};

/* ── Scan parameters ────────────────────────────────────────────────────── */
static esp_ble_scan_params_t ble_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,   /* 50 ms */
    .scan_window        = 0x30,   /* 30 ms */
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ── Filter UUID we're looking for ──────────────────────────────────────── */
static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = REMOTE_SERVICE_UUID },
};

static esp_bt_uuid_t remote_filter_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = REMOTE_CHAR_UUID },
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

/* ── GAP event handler ───────────────────────────────────────────────────── */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan param set failed, status=%x",
                     param->scan_param_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "Scan params set — starting scan (30 s)");
        esp_ble_gap_start_scanning(30);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan start failed, status=%x",
                     param->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = param;

        switch (scan_result->scan_rst.search_evt) {

        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            /* Try to find the device name in the advertisement data */
            uint8_t *adv_name = NULL;
            uint8_t  adv_name_len = 0;
            adv_name = esp_ble_resolve_adv_data(
                scan_result->scan_rst.ble_adv,
                ESP_BLE_AD_TYPE_NAME_CMPL,
                &adv_name_len);

            if (adv_name == NULL) {
                /* Also check short name */
                adv_name = esp_ble_resolve_adv_data(
                    scan_result->scan_rst.ble_adv,
                    ESP_BLE_AD_TYPE_NAME_SHORT,
                    &adv_name_len);
            }

            if (adv_name != NULL && adv_name_len > 0) {
                if (adv_name_len == strlen(REMOTE_DEVICE_NAME) &&
                    memcmp(adv_name, REMOTE_DEVICE_NAME, adv_name_len) == 0)
                {
                    ESP_LOGI(TAG, "Found \"%s\" — stopping scan and connecting",
                             REMOTE_DEVICE_NAME);
                    esp_ble_gap_stop_scanning();
                    esp_ble_gattc_open(
                        profile_tab[PROFILE_APP_ID].gattc_if,
                        scan_result->scan_rst.bda,
                        scan_result->scan_rst.ble_addr_type,
                        true);
                }
            }
            break;
        }

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            ESP_LOGI(TAG, "Scan complete — restarting scan");
            esp_ble_gap_start_scanning(30);
            break;

        default:
            break;
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan stop failed, status=%x",
                     param->scan_stop_cmpl.status);
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

/* ── GATTC profile event handler ─────────────────────────────────────────── */
static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTC reg failed, status=%d", param->reg.status);
            break;
        }
        ESP_LOGI(TAG, "GATTC registered, setting scan params");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed, status=%d", param->open.status);
            break;
        }
        ESP_LOGI(TAG, "Connected, conn_id=%d", param->open.conn_id);
        profile_tab[PROFILE_APP_ID].conn_id   = param->open.conn_id;
        profile_tab[PROFILE_APP_ID].connected = true;
        memcpy(profile_tab[PROFILE_APP_ID].remote_bda,
               param->open.remote_bda, sizeof(esp_bd_addr_t));

        /* Request MTU exchange */
        esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "MTU config failed, status=%d", param->cfg_mtu.status);
        }
        ESP_LOGI(TAG, "MTU set to %d — searching for service 0x%04X",
                 param->cfg_mtu.mtu, REMOTE_SERVICE_UUID);

        esp_ble_gattc_search_service(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            &remote_filter_service_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID)
        {
            ESP_LOGI(TAG, "Service 0x%04X found, start_handle=%d, end_handle=%d",
                     REMOTE_SERVICE_UUID,
                     param->search_res.start_handle,
                     param->search_res.end_handle);
            profile_tab[PROFILE_APP_ID].service_start_handle =
                param->search_res.start_handle;
            profile_tab[PROFILE_APP_ID].service_end_handle =
                param->search_res.end_handle;
            profile_tab[PROFILE_APP_ID].service_found = true;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Service search failed, status=%d",
                     param->search_cmpl.status);
            break;
        }
        if (!profile_tab[PROFILE_APP_ID].service_found) {
            ESP_LOGE(TAG, "Service 0x%04X not found on server",
                     REMOTE_SERVICE_UUID);
            break;
        }

        /* Get characteristic by UUID */
        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            ESP_GATT_DB_CHARACTERISTIC,
            profile_tab[PROFILE_APP_ID].service_start_handle,
            profile_tab[PROFILE_APP_ID].service_end_handle,
            0,
            &count);

        if (status != ESP_GATT_OK || count == 0) {
            ESP_LOGE(TAG, "No characteristics found (status=%d, count=%d)",
                     status, count);
            break;
        }

        esp_gattc_char_elem_t *char_elem =
            (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!char_elem) {
            ESP_LOGE(TAG, "malloc failed for char_elem");
            break;
        }

        status = esp_ble_gattc_get_char_by_uuid(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            profile_tab[PROFILE_APP_ID].service_start_handle,
            profile_tab[PROFILE_APP_ID].service_end_handle,
            remote_filter_char_uuid,
            char_elem,
            &count);

        if (status != ESP_GATT_OK || count == 0) {
            ESP_LOGE(TAG, "Char 0x%04X not found", REMOTE_CHAR_UUID);
            free(char_elem);
            break;
        }

        profile_tab[PROFILE_APP_ID].char_handle = char_elem[0].char_handle;
        profile_tab[PROFILE_APP_ID].char_found = true;
        ESP_LOGI(TAG, "Char 0x%04X found, handle=%d",
                 REMOTE_CHAR_UUID, char_elem[0].char_handle);

        /* Register for notifications on this characteristic */
        esp_ble_gattc_register_for_notify(
            gattc_if,
            profile_tab[PROFILE_APP_ID].remote_bda,
            char_elem[0].char_handle);

        free(char_elem);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register for notify failed, status=%d",
                     param->reg_for_notify.status);
            break;
        }
        ESP_LOGI(TAG, "Registered for notify — writing CCCD to enable");

        /* Find the CCCD descriptor and write 0x0001 to enable notifications */
        uint16_t descr_count = 0;
        esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            ESP_GATT_DB_DESCRIPTOR,
            profile_tab[PROFILE_APP_ID].service_start_handle,
            profile_tab[PROFILE_APP_ID].service_end_handle,
            profile_tab[PROFILE_APP_ID].char_handle,
            &descr_count);

        if (ret_status != ESP_GATT_OK || descr_count == 0) {
            ESP_LOGE(TAG, "No descriptors found (status=%d, count=%d)",
                     ret_status, descr_count);
            break;
        }

        esp_gattc_descr_elem_t *descr_elem =
            (esp_gattc_descr_elem_t *)malloc(
                sizeof(esp_gattc_descr_elem_t) * descr_count);
        if (!descr_elem) {
            ESP_LOGE(TAG, "malloc failed for descr_elem");
            break;
        }

        ret_status = esp_ble_gattc_get_descr_by_char_handle(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            profile_tab[PROFILE_APP_ID].char_handle,
            notify_descr_uuid,
            descr_elem,
            &descr_count);

        if (ret_status != ESP_GATT_OK || descr_count == 0) {
            ESP_LOGE(TAG, "CCCD descriptor not found");
            free(descr_elem);
            break;
        }

        uint16_t notify_en = 0x0001;
        esp_ble_gattc_write_char_descr(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            descr_elem[0].handle,
            sizeof(notify_en),
            (uint8_t *)&notify_en,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);

        free(descr_elem);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write CCCD failed, status=%d", param->write.status);
            break;
        }
        ESP_LOGI(TAG, "Notifications enabled on server (CCCD written)");
        break;

    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(TAG, "Data (%d bytes): %.*s",
                 param->notify.value_len,
                 param->notify.value_len,
                 (char *)param->notify.value);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected, reason=0x%x — restarting scan",
                 param->disconnect.reason);
        profile_tab[PROFILE_APP_ID].connected     = false;
        profile_tab[PROFILE_APP_ID].service_found = false;
        profile_tab[PROFILE_APP_ID].char_found    = false;
        profile_tab[PROFILE_APP_ID].char_handle   = 0;
        esp_ble_gap_start_scanning(30);
        break;

    default:
        break;
    }
}

/* ── Global GATTC dispatch (routes to profile handler) ───────────────────── */
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                 esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param)
{
    /* On registration, remember which interface belongs to our profile */
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            profile_tab[PROFILE_APP_ID].gattc_if = gattc_if;
        } else {
            ESP_LOGE(TAG, "Reg app failed, app_id=%04x, status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gattc_if == ESP_GATT_IF_NONE ||
            gattc_if == profile_tab[i].gattc_if)
        {
            if (profile_tab[i].gattc_cb) {
                profile_tab[i].gattc_cb(event, gattc_if, param);
            }
        }
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */
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
    ret = esp_ble_gattc_register_callback(gattc_event_handler);
    ESP_ERROR_CHECK(ret);

    ret = esp_ble_gap_register_callback(gap_event_handler);
    ESP_ERROR_CHECK(ret);

    /* Register application profile — triggers ESP_GATTC_REG_EVT */
    ret = esp_ble_gattc_app_register(PROFILE_APP_ID);
    ESP_ERROR_CHECK(ret);

    /* Increase MTU to allow larger payloads (useful for image chunking later) */
    ret = esp_ble_gatt_set_local_mtu(500);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "BLE Client initialised — scanning for \"%s\"",
             REMOTE_DEVICE_NAME);
}
