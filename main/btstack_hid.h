#pragma once

#include "esp_err.h"
#include <stdint.h>

extern uint8_t g_esp32_mac[6];

esp_err_t btstack_hid_init(void);
void btstack_hid_send_input_report(const uint8_t *data, uint16_t len);
