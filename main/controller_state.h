/**
 * @file controller_state.h
 * @brief Estado de botones y sticks del controlador emulado.
 *
 * En Fase 1 el estado es siempre neutro.
 * Preparado para ser extendido con lógica de NFC en Fase 2.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Botones — bitmask de 3 bytes (ver bluetooth_hid_notes.md)          */
/* ------------------------------------------------------------------ */

/* Byte 3 del input report: botones derechos */
#define BTN_Y          (1 << 0)
#define BTN_X          (1 << 1)
#define BTN_B          (1 << 2)
#define BTN_A          (1 << 3)
#define BTN_R_SR       (1 << 4)
#define BTN_R_SL       (1 << 5)
#define BTN_R          (1 << 6)
#define BTN_ZR         (1 << 7)

/* Byte 4: botones compartidos */
#define BTN_MINUS      (1 << 0)
#define BTN_PLUS       (1 << 1)
#define BTN_STICK_R    (1 << 2)
#define BTN_STICK_L    (1 << 3)
#define BTN_HOME       (1 << 4)
#define BTN_CAPTURE    (1 << 5)

/* Byte 5: botones izquierdos */
#define BTN_DOWN       (1 << 0)
#define BTN_UP         (1 << 1)
#define BTN_RIGHT      (1 << 2)
#define BTN_LEFT       (1 << 3)
#define BTN_L_SR       (1 << 4)
#define BTN_L_SL       (1 << 5)
#define BTN_L          (1 << 6)
#define BTN_ZL         (1 << 7)

/**
 * @brief Estado completo del controlador.
 */
typedef struct {
    uint8_t right_buttons;   /**< Byte 3: Y X B A R-SR R-SL R ZR */
    uint8_t shared_buttons;  /**< Byte 4: - + LS RS Home Capture */
    uint8_t left_buttons;    /**< Byte 5: Down Up Right Left L-SR L-SL L ZL */

    /* Sticks: valores 12-bit (0x000..0xFFF), centro = 0x800 */
    uint16_t left_stick_x;
    uint16_t left_stick_y;
    uint16_t right_stick_x;
    uint16_t right_stick_y;
} controller_state_t;

/**
 * @brief Inicializa el estado a neutro (sin botones, sticks centrados).
 */
void controller_state_init(controller_state_t *state);

/**
 * @brief Obtiene puntero al estado global del controlador.
 */
controller_state_t *controller_state_get(void);
