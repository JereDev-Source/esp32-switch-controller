/**
 * @file input_report.h
 * @brief Construcción de input reports del Pro Controller hacia el Switch.
 *
 * Formatos implementados:
 *   0x3F — Simple HID (fase inicial antes de modo 0x30)
 *   0x21 — Subcommand reply
 *   0x30 — Full report con IMU (modo normal de streaming)
 *
 * Referencia: dekuNukem/bluetooth_hid_notes.md
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "controller_state.h"

/* Tamaño del buffer de reporte (sin el header HIDP 0xA1 que añade Bluedroid) */
#define INPUT_REPORT_SIZE      49   /* bytes del reporte en sí */
#define INPUT_REPORT_0x31_SIZE 362  /* reporte extendido NFC/IR (Fase 2) */

void input_report_set_virtual_grip(bool pressed);

/**
 * @brief Rellena el header estándar (bytes 0..12) de los reportes 0x21/0x30/0x31.
 *
 * @param buf         Buffer de salida (mínimo 13 bytes).
 * @param report_id   0x21, 0x30 ó 0x31.
 * @param timer       Byte de timer (0x00..0xFF, incrementado externamente).
 * @param state       Estado actual de botones y sticks.
 */
void input_report_fill_header(uint8_t *buf, uint8_t report_id,
                              uint8_t timer,
                              const controller_state_t *state);

/**
 * @brief Construye un Input Report 0x3F (Simple HID, 12 bytes).
 *
 * @param buf     Buffer de salida (mínimo 12 bytes).
 * @param state   Estado actual del controlador.
 */
void input_report_build_0x3F(uint8_t *buf, const controller_state_t *state);

/**
 * @brief Construye un Input Report 0x21 (Subcommand Reply, 49 bytes).
 *
 * @param buf         Buffer de salida (mínimo 49 bytes).
 * @param timer       Byte de timer.
 * @param state       Estado del controlador.
 * @param subcmd_id   ID del subcomando al que se responde.
 * @param payload     Datos de respuesta (puede ser NULL si payload_len == 0).
 * @param payload_len Número de bytes de payload (máximo 35).
 */
void input_report_build_0x21(uint8_t *buf, uint8_t timer,
                              const controller_state_t *state,
                              uint8_t subcmd_id,
                              const uint8_t *payload, uint8_t payload_len);

/**
 * @brief Construye un Input Report 0x30 (Standard Full + IMU, 49 bytes).
 *
 * @param buf     Buffer de salida (mínimo 49 bytes).
 * @param timer   Byte de timer.
 * @param state   Estado del controlador.
 */
void input_report_build_0x30(uint8_t *buf, uint8_t timer,
                              const controller_state_t *state);
