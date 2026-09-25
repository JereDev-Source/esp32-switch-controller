#pragma once
#include <stdbool.h>
#include "esp_err.h"
void pc_control_poll(void);
esp_err_t pc_control_init(void);
void pc_control_connection(bool connected);
