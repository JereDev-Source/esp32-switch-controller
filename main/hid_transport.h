#pragma once

#include <stdint.h>

void hid_transport_send_input_report(const uint8_t *data, uint16_t len);

extern uint8_t g_esp32_mac[6];
