#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controller_state.h"
#if CONFIG_SWITCH_CONTROLLER_BLE_HID
#include "ble_hid.h"
#elif CONFIG_SWITCH_CONTROLLER_BTSTACK
#include "btstack_hid.h"
#include "btstack_run_loop.h"
#else
#include "bt_hid.h"
#endif

static const char *TAG = "MAIN";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Switch controller experimental ===");

    init_nvs();
    controller_state_init(controller_state_get());

#if CONFIG_SWITCH_CONTROLLER_BLE_HID
    esp_err_t err = ble_hid_init();
#elif CONFIG_SWITCH_CONTROLLER_BTSTACK
    esp_err_t err = btstack_hid_init();
#else
    esp_err_t err = bt_hid_init();
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el transporte Bluetooth: %s",
                 esp_err_to_name(err));
        return;
    }
#if CONFIG_SWITCH_CONTROLLER_BTSTACK
    btstack_run_loop_execute();
    ESP_LOGI(TAG, "Bluetooth Classic HID iniciado");
#elif CONFIG_SWITCH_CONTROLLER_BLE_HID
    ESP_LOGI(TAG, "BLE HID experimental iniciado; esperando conexion");
#endif

#if !CONFIG_SWITCH_CONTROLLER_BTSTACK && !CONFIG_SWITCH_CONTROLLER_BLE_HID
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
