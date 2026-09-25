/**
 * @file bt_hid.h
 * @brief Capa de transporte Bluetooth Classic HID (Bluedroid ESP-IDF).
 *
 * Gestiona:
 *  - Inicialización de Bluetooth Classic
 *  - Registro del HID Device (descriptor, VID/PID, CoD)
 *  - Callbacks de conexión / desconexión / recepción de datos
 *  - Envío de Input Reports al Switch
 *  - Timer FreeRTOS para el streaming periódico
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief MAC address del ESP32 (en formato Big-Endian, tal como viene de
 *        esp_read_mac). Expuesta para que switch_proto.c la incluya en la
 *        respuesta al subcmd 0x02.
 */
extern uint8_t g_esp32_mac[6];

/**
 * @brief Inicializa el stack Bluetooth Classic y registra el HID Device.
 *
 * Debe llamarse una vez desde app_main() después de NVS init.
 * El proceso de registro es asíncrono — la conexión real ocurre más tarde
 * vía callbacks Bluedroid.
 *
 * @return ESP_OK si la inicialización comenzó correctamente.
 */
esp_err_t bt_hid_init(void);

/**
 * @brief Envía un Input Report al Switch (por el canal Interrupt, PSM 0x13).
 *
 * Thread-safe: puede llamarse desde el timer task o desde callbacks.
 * Implementa flow control básico: si Bluedroid está ocupado procesando el
 * reporte anterior, el nuevo se descarta (no se encola) para evitar
 * acumulación de lag.
 *
 * @param data  Buffer con el contenido del report (sin el header HIDP 0xA1,
 *              que Bluedroid añade automáticamente).
 * @param len   Longitud del buffer en bytes.
 */
void bt_hid_send_input_report(const uint8_t *data, uint16_t len);
