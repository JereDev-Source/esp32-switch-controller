/**
 * @file switch_proto.h
 * @brief Parser y dispatcher del protocolo HID de Nintendo Switch.
 *
 * Maneja los Output Reports que envía el Switch al controlador:
 *   - Output Report 0x01 (subcomando + rumble)
 *   - Output Report 0x10 (rumble only — ignorar en Fase 1)
 *   - Output Report 0x11 (MCU request — stub en Fase 1)
 *
 * Referencia principal: dekuNukem/bluetooth_hid_subcommands_notes.md
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Modos de streaming del controlador.
 */
typedef enum {
    STREAM_MODE_NONE   = 0,   /**< Solo responder subcomandos (pre-handshake) */
    STREAM_MODE_0x3F   = 1,   /**< Simple HID */
    STREAM_MODE_0x30   = 2,   /**< Full report + IMU @60Hz */
    STREAM_MODE_0x31   = 3,   /**< NFC/IR MCU mode (Fase 2) */
} stream_mode_t;

/**
 * @brief Inicializa el módulo de protocolo.
 *        Debe llamarse una sola vez después de la conexión.
 */
void switch_proto_init(void);

/**
 * @brief Procesa un Output Report recibido del Switch.
 *
 * @param data  Puntero al buffer del report (sin el header HIDP 0xA2).
 * @param len   Longitud del buffer.
 */
void switch_proto_handle_output(const uint8_t *data, uint16_t len);

/**
 * @brief Obtiene el modo de streaming actual.
 */
stream_mode_t switch_proto_get_stream_mode(void);

/**
 * @brief Obtiene y avanza el timer de input reports (0x00..0xFF wrapping).
 */
uint8_t switch_proto_next_timer(void);

/**
 * @brief Returns true after the host explicitly selects input mode 0x30.
 */
bool switch_proto_stream_enabled(void);
