/**
 * @file input_report.c
 * @brief Implementación de los constructores de input reports.
 *
 * Estructura de los reportes (fuente: dekuNukem/bluetooth_hid_notes.md):
 *
 * --- Header estándar (bytes 0..12) ---
 * [0]    Report ID
 * [1]    Timer (0x00..0xFF)
 * [2]    Battery (nibble alto: 0x8=full) | Connection (nibble bajo: 0x0=BT)
 *        → 0x8E = full battery, USB+BT
 *          0x8E para Joy-Con (L) en BT: 0x8 (full) << 4 | 0xE (info)
 *          joycontrol usa 0x8E
 * [3]    Right buttons (Y X B A R-SR R-SL R ZR)
 * [4]    Shared buttons (- + R-stick L-stick Home Capture)
 * [5]    Left buttons (Down Up Right Left L-SR L-SL L ZL)
 * [6-8]  Left stick (12-bit X | 12-bit Y empaquetados)
 * [9-11] Right stick (12-bit X | 12-bit Y empaquetados)
 * [12]   Vibration echo (0x00)
 *
 * --- Stick packing (12-bit X, 12-bit Y en 3 bytes) ---
 *   byte[0] = X[7:0]
 *   byte[1] = X[11:8] | (Y[3:0] << 4)
 *   byte[2] = Y[11:4]
 *
 * --- IMU neutral (por trama, 12 bytes = 6 × int16_t LE) ---
 *   Accel X = 0, Accel Y = 0, Accel Z ≈ 0x1000 (4096 ≈ 1G con rango ±8G)
 *   Gyro X = 0, Gyro Y = 0, Gyro Z = 0
 */
#include "input_report.h"
#include <string.h>

static bool s_virtual_grip;

static uint8_t get_subcmd_ack(uint8_t subcmd_id)
{
    switch (subcmd_id) {
    case 0x02:
        return 0x82; /* Device info */
    case 0x04:
        return 0x83; /* Trigger elapsed times */
    case 0x10:
        return 0x90; /* SPI flash read */
    case 0x20:
    case 0x21:
        return 0xA0; /* NFC/IR MCU */
    default:
        return 0x80; /* Generic ACK: 0x80, subcommand, status/data */
    }
}

void input_report_set_virtual_grip(bool pressed)
{
    s_virtual_grip = pressed;
}

/* ------------------------------------------------------------------ */
/* Helper: empaquetar un eje de stick 12-bit                          */
/* ------------------------------------------------------------------ */

static void pack_stick(uint8_t *dst, uint16_t x, uint16_t y)
{
    /* Clamp a 12-bit */
    x &= 0x0FFF;
    y &= 0x0FFF;

    dst[0] = (uint8_t)(x & 0xFF);
    dst[1] = (uint8_t)((x >> 8) | ((y & 0x0F) << 4));
    dst[2] = (uint8_t)(y >> 4);
}

/* ------------------------------------------------------------------ */
/* Header estándar (bytes 0..12)                                      */
/* ------------------------------------------------------------------ */

void input_report_fill_header(uint8_t *buf, uint8_t report_id,
                              uint8_t timer,
                              const controller_state_t *state)
{
    buf[0]  = report_id;
    buf[1]  = timer;
    buf[2]  = 0x8E;                    /* Batería full, conexión BT */
    buf[3]  = state->right_buttons | (s_virtual_grip ? BTN_R : 0);
    buf[4]  = state->shared_buttons;
    buf[5]  = state->left_buttons | (s_virtual_grip ? BTN_L : 0);
    pack_stick(&buf[6],  state->left_stick_x,  state->left_stick_y);
    pack_stick(&buf[9],  state->right_stick_x, state->right_stick_y);
    buf[12] = 0x00;                    /* Vibration echo */
}

/* ------------------------------------------------------------------ */
/* Trama IMU neutral (12 bytes por frame)                             */
/* ------------------------------------------------------------------ */

/**
 * Accel Z = 0x1000 (4096 LSB) ≈ 1G con calibración estándar ±8G.
 * joycontrol usa este valor. Punto Abierto #1: ajustar si el Switch
 * muestra problemas de calibración IMU.
 *
 * Formato: 6 × int16_t LE = [ax ay az gx gy gz]
 */
static void fill_imu_frame_neutral(uint8_t *frame)
{
    memset(frame, 0x00, 12);

    /* Accel Z: 0x1000 = 4096, little-endian */
    frame[4] = 0x00;
    frame[5] = 0x10;

    /* Accel X, Y, Gyro X, Y, Z = 0 (ya en cero por memset) */
}

/* ------------------------------------------------------------------ */
/* Report 0x3F — Simple HID (12 bytes)                                */
/* ------------------------------------------------------------------ */

/*
 * Layout 0x3F:
 * [0]    0x3F
 * [1-2]  Button bitmask 16-bit (diferente al estándar 0x30):
 *          bit 0=Y, 1=B, 2=A, 3=X, 4=L, 5=R, 6=ZL, 7=ZR
 *          bit 8=-, 9=+, 10=LS, 11=RS, 12=Home, 13=Capture
 * [3]    Hat switch (0=Up, 1=UR, 2=R, 3=DR, 4=D, 5=DL, 6=L, 7=UL, 8=Neutral)
 * [4-5]  Left stick X, Y (8-bit, centro=0x80)
 * [6-7]  Right stick X, Y
 * [8-11] Relleno 0x00
 */
void input_report_build_0x3F(uint8_t *buf, const controller_state_t *state)
{
    memset(buf, 0x00, 12);

    buf[0] = 0x3F;

    /* Convertir botones del formato 0x30 al formato 0x3F */
    uint16_t btns = 0;
    if (state->right_buttons & BTN_Y)       btns |= (1 << 0);
    if (state->right_buttons & BTN_B)       btns |= (1 << 1);
    if (state->right_buttons & BTN_A)       btns |= (1 << 2);
    if (state->right_buttons & BTN_X)       btns |= (1 << 3);
    if (s_virtual_grip || (state->left_buttons & BTN_L)) btns |= (1 << 4); /* L */
    if (s_virtual_grip || (state->right_buttons & BTN_R)) btns |= (1 << 5); /* R */
    if (state->left_buttons  & BTN_ZL)      btns |= (1 << 6);
    if (state->right_buttons & BTN_ZR)      btns |= (1 << 7);
    if (state->shared_buttons & BTN_MINUS)  btns |= (1 << 8);
    if (state->shared_buttons & BTN_PLUS)   btns |= (1 << 9);
    if (state->shared_buttons & BTN_STICK_L) btns |= (1 << 10);
    if (state->shared_buttons & BTN_STICK_R) btns |= (1 << 11);
    if (state->shared_buttons & BTN_HOME)   btns |= (1 << 12);
    if (state->shared_buttons & BTN_CAPTURE) btns |= (1 << 13);

    buf[1] = (uint8_t)(btns & 0xFF);
    buf[2] = (uint8_t)(btns >> 8);

    bool up = (state->left_buttons & BTN_UP) != 0;
    bool down = (state->left_buttons & BTN_DOWN) != 0;
    bool left = (state->left_buttons & BTN_LEFT) != 0;
    bool right = (state->left_buttons & BTN_RIGHT) != 0;
    int x = (int)right - (int)left;
    int y = (int)down - (int)up;
    buf[3] = y < 0 ? (x < 0 ? 7 : x > 0 ? 1 : 0) :
             y > 0 ? (x < 0 ? 5 : x > 0 ? 3 : 4) :
                     (x < 0 ? 6 : x > 0 ? 2 : 8);
    uint16_t axes[] = {state->left_stick_x, state->left_stick_y,
                       state->right_stick_x, state->right_stick_y};
    for (unsigned i = 0; i < 4; i++) {
        uint16_t value = (axes[i] & 0x0FFF) << 4;
        buf[4 + i * 2] = (uint8_t)value;
        buf[5 + i * 2] = (uint8_t)(value >> 8);
    }
}

/* ------------------------------------------------------------------ */
/* Report 0x21 — Subcommand Reply (49 bytes)                          */
/* ------------------------------------------------------------------ */

void input_report_build_0x21(uint8_t *buf, uint8_t timer,
                              const controller_state_t *state,
                              uint8_t subcmd_id,
                              const uint8_t *payload, uint8_t payload_len)
{
    memset(buf, 0x00, INPUT_REPORT_SIZE);

    input_report_fill_header(buf, 0x21, timer, state);

    /* byte[13] is the response class, not 0x80 OR subcmd_id. */
    buf[13] = get_subcmd_ack(subcmd_id);
    /* byte[14]: subcommand ID confirmado */
    buf[14] = subcmd_id;

    /* bytes[15..]: payload */
    if (payload && payload_len > 0) {
        uint8_t copy_len = payload_len;
        if (copy_len > (INPUT_REPORT_SIZE - 15)) {
            copy_len = INPUT_REPORT_SIZE - 15; /* Clamp de seguridad */
        }
        memcpy(&buf[15], payload, copy_len);
    }
}

/* ------------------------------------------------------------------ */
/* Report 0x30 — Full + IMU (49 bytes)                                */
/* ------------------------------------------------------------------ */

void input_report_build_0x30(uint8_t *buf, uint8_t timer,
                              const controller_state_t *state)
{
    memset(buf, 0x00, INPUT_REPORT_SIZE);

    input_report_fill_header(buf, 0x30, timer, state);

    /* 3 tramas IMU (bytes 13..48 = 36 bytes = 3 × 12 bytes) */
    fill_imu_frame_neutral(&buf[13]);
    fill_imu_frame_neutral(&buf[25]);
    fill_imu_frame_neutral(&buf[37]);
}
