/**
 * @file spi_flash.h
 * @brief Emulación de la memoria SPI flash interna del Joy-Con / Pro Controller.
 *
 * El Switch lee varios rangos de la flash durante el handshake de conexión
 * (subcomando 0x10). Este módulo provee datos coherentes para todos los
 * rangos que el Switch consulta, y devuelve 0xFF para cualquier dirección
 * no documentada (comportamiento del hardware real cuando la región no
 * ha sido programada).
 *
 * Referencia: dekuNukem/Nintendo_Switch_Reverse_Engineering/spi_flash_notes.md
 */
#pragma once
#include <stdint.h>

/**
 * @brief Lee N bytes de la flash emulada a partir de addr.
 *
 * @param addr    Dirección de 32 bits (Little-Endian en el protocolo, pero
 *                aquí se pasa ya convertida a uint32_t).
 * @param out     Buffer de salida (al menos len bytes).
 * @param len     Número de bytes a leer (máximo 0x1D según protocolo).
 */
void spi_flash_read(uint32_t addr, uint8_t *out, uint8_t len);
