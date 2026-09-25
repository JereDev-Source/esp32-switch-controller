#pragma once

#include "esp_err.h"
#include <stdint.h>

/* BLE HID transport variant. The Classic transport remains in bt_hid.c. */
esp_err_t ble_hid_init(void);
void ble_hid_send_input_report(const uint8_t *data, uint16_t len);
