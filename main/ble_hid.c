/*
 * BLE HID base transport.
 *
 * This uses the esp_hid device API shipped with ESP-IDF.  It deliberately
 * Experimental Switch 2 BLE transport. Public references identify the Pro
 * Controller 2 as VID 0x057E/PID 0x2069, but do not document its complete
 * association protocol. This profile therefore exposes standard BLE HID while
 * keeping the Classic HIDP implementation available as a separate build.
 */
#include "ble_hid.h"

#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "switch_proto.h"
#include "input_report.h"
#include "controller_state.h"
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "BLE_HID";
static esp_hidd_dev_t *s_dev;
static esp_timer_handle_t s_timer;
static volatile bool s_connected;
uint8_t g_esp32_mac[6] = {0};

/*
 * esp_hid_gap.c calls this hook after BLE authentication in the reference
 * example. Reports are driven by our esp_timer after ESP_HIDD_CONNECT_EVENT,
 * so no additional task startup is required here.
 */
void ble_hid_task_start_up(void)
{
}

/* Standard BLE HID gamepad map. Report payloads are the existing 49-byte
 * Switch-style reports; the report ID itself is supplied to esp_hidd. */
static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x3F, 0x09, 0x3F, 0x75, 0x08, 0x95, 0x0C, 0x81, 0x02,
    0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0xC0
};
static esp_hid_raw_report_map_t s_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) }
};
static const esp_hid_device_config_t s_config = {
    .vendor_id = 0x057E, .product_id = 0x2069, .version = 0x0100,
    .device_name = "Pro Controller 2",
    .manufacturer_name = "Nintendo",
    .serial_number = "ESP32-NS2",
    .report_maps = s_maps, .report_maps_len = 1
};

static void send_current_report(void *arg)
{
    if (!s_connected || !s_dev) return;
    uint8_t report[INPUT_REPORT_SIZE];
    input_report_build_0x30(report, switch_proto_next_timer(),
                             controller_state_get());
    /* esp_hidd expects the report payload without the Report ID. */
    esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, 0x30,
                                           &report[1], sizeof(report) - 1);
    if (err != ESP_OK) ESP_LOGD(TAG, "input report not sent: %s",
                                esp_err_to_name(err));
}

static void ble_hid_event(void *args, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    esp_hidd_event_data_t *event = (esp_hidd_event_data_t *)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        {
            esp_err_t err = esp_hid_ble_gap_adv_start();
            ESP_LOGI(TAG, "Advertising BLE iniciado: %s",
                     esp_err_to_name(err));
        }
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        switch_proto_init();
        if (!s_timer) {
            esp_timer_create_args_t timer_args = {
                .callback = send_current_report, .name = "ble_hid_input"
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));
            ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, 15000));
        }
        ESP_LOGI(TAG, "BLE HID host conectado");
        break;
    case ESP_HIDD_OUTPUT_EVENT: {
        uint8_t report[1 + 64];
        size_t len = event->output.length > 64 ? 64 : event->output.length;
        report[0] = (uint8_t)event->output.report_id;
        memcpy(&report[1], event->output.data, len);
        switch_proto_handle_output(report, (uint16_t)(len + 1));
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        if (s_timer) {
            esp_timer_stop(s_timer);
            esp_timer_delete(s_timer);
            s_timer = NULL;
        }
        esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

esp_err_t ble_hid_init(void)
{
    esp_err_t mac_err = esp_read_mac(g_esp32_mac, ESP_MAC_BT);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo leer MAC Bluetooth: %s",
                 esp_err_to_name(mac_err));
    }
    esp_err_t err = esp_hid_gap_init(HIDD_BLE_MODE);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "BLE identity: VID=0x057E PID=0x2069 name=%s",
             s_config.device_name);
    err = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GAMEPAD,
                                   s_config.device_name);
    if (err != ESP_OK) return err;
    err = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    if (err != ESP_OK) return err;
    return esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE,
                             ble_hid_event, &s_dev);
}

void ble_hid_send_input_report(const uint8_t *data, uint16_t len)
{
    if (!s_connected || !s_dev || !data || len < 2) return;
    (void)esp_hidd_dev_input_set(s_dev, 0, data[0],
                                 (uint8_t *)&data[1], len - 1);
}
