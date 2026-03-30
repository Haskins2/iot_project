/**
 * @file ble_client.c
 * @brief BLE GATT Client implementation for ESP-EYE communication
 *
 * Discovers two characteristics on the ESP-EYE server (service 0xABCD):
 *   - 0xFF01: Image data characteristic (notifications — receives JPEG chunks)
 *   - 0xFF02: Trigger characteristic (write — sends capture command)
 *
 * TriggerCameraCapture() writes to 0xFF02, which the ESP-EYE server
 * handles by capturing a JPEG frame and streaming it back on 0xFF01.
 *
 * Handles disconnections gracefully — automatically restarts scanning.
 */

#include "ble_client.h"
#include "image_reassembly.h"
#include "image_encoder.h"
#include "water_sensor.h"
#include "raindrop_sensor.h"

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

/* ── Constants ──────────────────────────────────────────────────────────── */
#define TAG                   "ble_client"

#define BLE_STATIC_PASSKEY    123456
#define REMOTE_SERVICE_UUID   0xABCD
#define REMOTE_CHAR_IMG_UUID  0xFF01   /* Image data (read + notify) */
#define REMOTE_CHAR_TRIG_UUID 0xFF02   /* Trigger capture (write)    */
#define REMOTE_DEVICE_NAME    "ESP-EYE"

#define PROFILE_APP_ID        0
#define PROFILE_NUM           1

/* ── Profile bookkeeping ────────────────────────────────────────────────── */
struct gattc_profile_inst {
    esp_gattc_cb_t       gattc_cb;
    uint16_t             gattc_if;
    uint16_t             conn_id;
    uint16_t             service_start_handle;
    uint16_t             service_end_handle;
    uint16_t             img_char_handle;     /* 0xFF01 — notifications */
    uint16_t             trig_char_handle;    /* 0xFF02 — write trigger */
    esp_bd_addr_t        remote_bda;
    bool                 connected;
    bool                 service_found;
    bool                 img_char_found;
    bool                 trig_char_found;
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
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ── UUID filters ───────────────────────────────────────────────────────── */
static esp_bt_uuid_t remote_filter_service_uuid = {
    .len  = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = REMOTE_SERVICE_UUID },
};

static esp_bt_uuid_t remote_filter_img_char_uuid = {
    .len  = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = REMOTE_CHAR_IMG_UUID },
};

static esp_bt_uuid_t remote_filter_trig_char_uuid = {
    .len  = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = REMOTE_CHAR_TRIG_UUID },
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len  = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

/* ===================================================================== */
/*  Helper: discover a characteristic by UUID, return its handle         */
/* ===================================================================== */
static bool discover_char(esp_gatt_if_t gattc_if,
                          esp_bt_uuid_t *uuid,
                          uint16_t *out_handle)
{
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
        return false;
    }

    esp_gattc_char_elem_t *elems = malloc(sizeof(esp_gattc_char_elem_t) * count);
    if (!elems) {
        return false;
    }

    status = esp_ble_gattc_get_char_by_uuid(
        gattc_if,
        profile_tab[PROFILE_APP_ID].conn_id,
        profile_tab[PROFILE_APP_ID].service_start_handle,
        profile_tab[PROFILE_APP_ID].service_end_handle,
        *uuid,
        elems,
        &count);

    bool found = false;
    if (status == ESP_GATT_OK && count > 0) {
        *out_handle = elems[0].char_handle;
        found = true;
    }

    free(elems);
    return found;
}

/* ===================================================================== */
/*  GAP event handler                                                     */
/* ===================================================================== */
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
            uint8_t *adv_name     = NULL;
            uint8_t  adv_name_len = 0;

            adv_name = esp_ble_resolve_adv_data(
                scan_result->scan_rst.ble_adv,
                ESP_BLE_AD_TYPE_NAME_CMPL,
                &adv_name_len);

            if (adv_name == NULL) {
                adv_name = esp_ble_resolve_adv_data(
                    scan_result->scan_rst.ble_adv,
                    ESP_BLE_AD_TYPE_NAME_SHORT,
                    &adv_name_len);
            }

            if (adv_name != NULL && adv_name_len > 0 &&
                adv_name_len == strlen(REMOTE_DEVICE_NAME) &&
                memcmp(adv_name, REMOTE_DEVICE_NAME, adv_name_len) == 0)
            {
                ESP_LOGI(TAG, "Found \"%s\" — connecting", REMOTE_DEVICE_NAME);
                esp_ble_gap_stop_scanning();
                esp_ble_gattc_open(
                    profile_tab[PROFILE_APP_ID].gattc_if,
                    scan_result->scan_rst.bda,
                    scan_result->scan_rst.ble_addr_type,
                    true);
            }
            break;
        }

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            ESP_LOGI(TAG, "Scan complete — no ESP-EYE found, restarting scan");
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
        ESP_LOGI(TAG, "Conn params updated: status=%d, min=%d, max=%d, "
                 "latency=%d, timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "Security request from server — accepting");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "Auth complete: addr=%02x:%02x:%02x:%02x:%02x:%02x, "
                 "success=%d, auth_mode=0x%x",
                 bd_addr[0], bd_addr[1], bd_addr[2],
                 bd_addr[3], bd_addr[4], bd_addr[5],
                 param->ble_security.auth_cmpl.success,
                 param->ble_security.auth_cmpl.auth_mode);
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(TAG, "Authentication failed, reason=0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    }

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "Passkey request — replying with static passkey");
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, BLE_STATIC_PASSKEY);
        break;

    default:
        break;
    }
}

/* ===================================================================== */
/*  GATTC profile event handler                                           */
/* ===================================================================== */
static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    /* ── Registration ────────────────────────────────────────────────── */
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTC reg failed, status=%d", param->reg.status);
            break;
        }
        ESP_LOGI(TAG, "GATTC registered — setting scan params");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;

    /* ── Connection opened ───────────────────────────────────────────── */
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed, status=%d — will rescan", param->open.status);
            esp_ble_gap_start_scanning(30);
            break;
        }
        ESP_LOGI(TAG, "Connected, conn_id=%d", param->open.conn_id);
        profile_tab[PROFILE_APP_ID].conn_id   = param->open.conn_id;
        profile_tab[PROFILE_APP_ID].connected  = true;
        memcpy(profile_tab[PROFILE_APP_ID].remote_bda,
               param->open.remote_bda, sizeof(esp_bd_addr_t));

        esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
        break;

    /* ── MTU negotiated ──────────────────────────────────────────────── */
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "MTU config failed, status=%d", param->cfg_mtu.status);
        }
        ESP_LOGI(TAG, "MTU=%d — searching for service 0x%04X",
                 param->cfg_mtu.mtu, REMOTE_SERVICE_UUID);
        esp_ble_gattc_search_service(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            &remote_filter_service_uuid);
        break;

    /* ── Service discovered ──────────────────────────────────────────── */
    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID)
        {
            ESP_LOGI(TAG, "Service 0x%04X found (handles %d–%d)",
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

    /* ── Service search complete → discover BOTH characteristics ─────── */
    case ESP_GATTC_SEARCH_CMPL_EVT: {
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

        /* --- Discover 0xFF01 (image / notify) --- */
        if (discover_char(gattc_if, &remote_filter_img_char_uuid,
                          &profile_tab[PROFILE_APP_ID].img_char_handle))
        {
            profile_tab[PROFILE_APP_ID].img_char_found = true;
            ESP_LOGI(TAG, "Char 0x%04X (image) found, handle=%d",
                     REMOTE_CHAR_IMG_UUID,
                     profile_tab[PROFILE_APP_ID].img_char_handle);

            /* Register for notifications on the image characteristic */
            esp_ble_gattc_register_for_notify(
                gattc_if,
                profile_tab[PROFILE_APP_ID].remote_bda,
                profile_tab[PROFILE_APP_ID].img_char_handle);
        } else {
            ESP_LOGE(TAG, "Char 0x%04X (image) NOT found", REMOTE_CHAR_IMG_UUID);
        }

        /* --- Discover 0xFF02 (trigger / write) --- */
        if (discover_char(gattc_if, &remote_filter_trig_char_uuid,
                          &profile_tab[PROFILE_APP_ID].trig_char_handle))
        {
            profile_tab[PROFILE_APP_ID].trig_char_found = true;
            ESP_LOGI(TAG, "Char 0x%04X (trigger) found, handle=%d",
                     REMOTE_CHAR_TRIG_UUID,
                     profile_tab[PROFILE_APP_ID].trig_char_handle);
        } else {
            ESP_LOGE(TAG, "Char 0x%04X (trigger) NOT found", REMOTE_CHAR_TRIG_UUID);
        }

        break;
    }

    /* ── Notifications registered → enable CCCD ──────────────────────── */
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register for notify failed, status=%d",
                     param->reg_for_notify.status);
            break;
        }
        ESP_LOGI(TAG, "Registered for notify — writing CCCD");

        uint16_t descr_count = 0;
        esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            ESP_GATT_DB_DESCRIPTOR,
            profile_tab[PROFILE_APP_ID].service_start_handle,
            profile_tab[PROFILE_APP_ID].service_end_handle,
            profile_tab[PROFILE_APP_ID].img_char_handle,
            &descr_count);

        if (ret_status != ESP_GATT_OK || descr_count == 0) {
            ESP_LOGE(TAG, "No descriptors found");
            break;
        }

        esp_gattc_descr_elem_t *descr_elem =
            malloc(sizeof(esp_gattc_descr_elem_t) * descr_count);
        if (!descr_elem) {
            ESP_LOGE(TAG, "malloc failed");
            break;
        }

        ret_status = esp_ble_gattc_get_descr_by_char_handle(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            profile_tab[PROFILE_APP_ID].img_char_handle,
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
    }

    /* ── CCCD written ────────────────────────────────────────────────── */
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write CCCD failed, status=%d", param->write.status);
            break;
        }
        ESP_LOGI(TAG, "Notifications enabled — ESP-EYE link is fully ready");
        break;

    /* ── Characteristic write complete (trigger on 0xFF02) ────────────── */
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write to trigger char failed, status=%d",
                     param->write.status);
        } else {
            ESP_LOGI(TAG, "Camera trigger sent successfully (0xFF02)");
        }
        break;

    /* ── Incoming notification data (image chunks on 0xFF01) ─────────── */
    case ESP_GATTC_NOTIFY_EVT:
        image_reassembly_feed_chunk(param->notify.value, param->notify.value_len);

        if (image_reassembly_is_complete()) {
            const uint8_t *jpeg_data;
            size_t jpeg_len;
            if (image_reassembly_get_image(&jpeg_data, &jpeg_len) == ESP_OK) {
                ESP_LOGI(TAG, "JPEG reassembly complete: %u bytes", (unsigned)jpeg_len);

                /* Get latest sensor readings (updated by main loop) */
                extern water_data_t    g_latest_water;
                extern raindrop_data_t g_latest_rain;

                char *json_payload = NULL;
                size_t json_len = 0;
                esp_err_t err = build_image_mqtt_payload(
                    jpeg_data, jpeg_len,
                    g_latest_water.raw,
                    g_latest_rain.digital,
                    g_latest_rain.analog,
                    &json_payload, &json_len);

                if (err == ESP_OK && json_payload) {
                    ESP_LOGI(TAG, "Image MQTT payload ready: %u bytes", (unsigned)json_len);

                    /* Publish image payload over MQTT (defined in main.c) */
                    extern void mqtt_publish_image(const char *payload, int len);
                    mqtt_publish_image(json_payload, (int)json_len);

                    free(json_payload);
                }

                image_reassembly_reset();
            }
        }
        break;

    /* ── Disconnected — reset state and rescan ───────────────────────── */
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected (reason=0x%x) — will rescan for ESP-EYE",
                 param->disconnect.reason);
        profile_tab[PROFILE_APP_ID].connected       = false;
        profile_tab[PROFILE_APP_ID].service_found   = false;
        profile_tab[PROFILE_APP_ID].img_char_found  = false;
        profile_tab[PROFILE_APP_ID].trig_char_found = false;
        profile_tab[PROFILE_APP_ID].img_char_handle  = 0;
        profile_tab[PROFILE_APP_ID].trig_char_handle = 0;
        esp_ble_gap_start_scanning(30);
        break;

    default:
        break;
    }
}

/* ===================================================================== */
/*  Global GATTC dispatch                                                 */
/* ===================================================================== */
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                 esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param)
{
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

/* ===================================================================== */
/*  Public API                                                            */
/* ===================================================================== */

esp_err_t BleClientInit(void)
{
    /* NVS must be initialised by app_main() before calling BleClientInit() */

    /* Release Classic-BT memory — we only need BLE */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Configure BLE security (SMP passkey pairing) */
    uint32_t passkey = BLE_STATIC_PASSKEY;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));

    uint8_t iocap = ESP_IO_CAP_IN;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));

    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(PROFILE_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));

    ESP_LOGI(TAG, "BLE client initialised — scanning for \"%s\"",
             REMOTE_DEVICE_NAME);
    return ESP_OK;
}

bool IsBleConnected(void)
{
    return profile_tab[PROFILE_APP_ID].connected &&
           profile_tab[PROFILE_APP_ID].img_char_found &&
           profile_tab[PROFILE_APP_ID].trig_char_found;
}

esp_err_t TriggerCameraCapture(void)
{
    if (!IsBleConnected()) {
        ESP_LOGW(TAG, "ESP-EYE not connected — skipping camera trigger");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Write to the TRIGGER characteristic (0xFF02).
     * The ESP-EYE server triggers a capture on ANY write to this handle;
     * it doesn't inspect the payload, but we send a byte for clarity.
     */
    uint8_t trigger_val = 0x01;
    esp_err_t err = esp_ble_gattc_write_char(
        profile_tab[PROFILE_APP_ID].gattc_if,
        profile_tab[PROFILE_APP_ID].conn_id,
        profile_tab[PROFILE_APP_ID].trig_char_handle,
        sizeof(trigger_val),
        &trigger_val,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write trigger (0xFF02): %s", esp_err_to_name(err));
    }
    return err;
}