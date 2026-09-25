/**
 * @file spi_flash.c
 * @brief Emulación de la memoria SPI flash del Pro Controller.
 *
 * Regiones emuladas (fuente: spi_flash_notes.md + joycontrol/memory.py):
 *
 *  0x6000 — Serial Number (16 bytes ASCII)
 *  0x6020 — Factory IMU Calibration (24 bytes)
 *  0x603D — Factory Left Stick Calibration (9 bytes)
 *  0x6046 — Factory Right Stick Calibration (9 bytes)
 *  0x6050 — Controller Body/Button Colors (12 bytes)
 *  0x8010 — User Stick Calibration (22 bytes, 0xFF = sin calibrar)
 *  0x8026 — User IMU Calibration (26 bytes, 0xFF = sin calibrar)
 *
 * Cualquier otra región devuelve 0xFF (no programada).
 */
#include "spi_flash.h"
#include <string.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Datos de flash estáticos                                            */
/* ------------------------------------------------------------------ */

/**
 * Serial Number — 16 bytes ASCII, relleno con 0x00.
 * Addr: 0x6000, len hasta 16.
 */
static const uint8_t spi_serial[16] = {
    'X','A','W','0','0','0','0','0','0','0','0','0','0','0', 0x00, 0x00
};

/**
 * Factory IMU Calibration — 24 bytes.
 * Addr: 0x6020
 * Estructura (fuente dekuNukem spi_flash_notes.md):
 *   [0-5]  Accel origin XYZ (int16 LE x3, offset en unidades raw)
 *   [6-11] Accel scale XYZ  (int16 LE x3)
 *   [12-17]Gyro origin XYZ  (int16 LE x3)
 *   [18-23]Gyro scale XYZ   (int16 LE x3)
 *
 * Valores: origin = 0 (sin offset), scale = 0x4000 (escala 1.0 típica).
 * joycontrol usa 0xFFFF para "escala máxima / sin calibrar de fábrica"
 * para aceleraciones y giro. Usamos valores neutros documentados.
 *
 * NOTA (Punto Abierto #1): Si el Switch 2 reporta stick drift o recalibración
 * forzada, ajustar estos valores. El Az neutral esperado es ~4000 LSB (1G).
 */
static const uint8_t spi_imu_factory_cal[24] = {
    /* Accel origin XYZ — 0, 0, 0 */
    0x00, 0x00,  0x00, 0x00,  0x00, 0x00,
    /* Accel scale XYZ — 0x4000 = escala estándar ±8G */
    0x00, 0x40,  0x00, 0x40,  0x00, 0x40,
    /* Gyro origin XYZ — 0, 0, 0 */
    0x00, 0x00,  0x00, 0x00,  0x00, 0x00,
    /* Gyro scale XYZ — 0x3BE0 = escala estándar ±2000°/s */
    0xE0, 0x3B,  0xE0, 0x3B,  0xE0, 0x3B,
};

/**
 * Factory Stick Calibration — 9 bytes por stick.
 * Formato: Max-delta (12-bit), Center (12-bit), Min-delta (12-bit)
 * Empaquetado en nibbles:
 *   byte[0]    = MaxX[7:0]
 *   byte[1]    = MaxY[3:0] | MaxX[11:8] (nibble alto)
 *   byte[2]    = MaxY[11:4]
 *   byte[3]    = CenterX[7:0]
 *   ...etc
 *
 * MaxDelta=0x700 (1792), Center=0x800 (2048), MinDelta=0x700
 * (Rango útil: 0x100..0xF00, centro 0x800)
 *
 * Addr Left:  0x603D (9 bytes)
 * Addr Right: 0x6046 (9 bytes)
 */
static const uint8_t spi_stick_cal_left[9] = {
    /* MaxX=0x700, MaxY=0x700 */
    0x00, 0x07, 0x70,
    /* CenterX=0x800, CenterY=0x800 */
    0x00, 0x08, 0x80,
    /* MinX=0x700, MinY=0x700 */
    0x00, 0x07, 0x70,
};

static const uint8_t spi_stick_cal_right[9] = {
    0x00, 0x07, 0x70,
    0x00, 0x08, 0x80,
    0x00, 0x07, 0x70,
};

/**
 * Controller Colors — 12 bytes.
 * Addr: 0x6050
 * [0-2]  Body color RGB   → negro (0x32, 0x32, 0x32) como Pro Controller
 * [3-5]  Button color RGB → blanco (0xFF, 0xFF, 0xFF)
 * [6-8]  Left grip RGB    → gris
 * [9-11] Right grip RGB   → gris
 */
static const uint8_t spi_colors[12] = {
    0x32, 0x32, 0x32,   /* Body: gris oscuro */
    0xFF, 0xFF, 0xFF,   /* Buttons: blanco */
    0x32, 0x32, 0x32,   /* Left grip: gris */
    0x32, 0x32, 0x32,   /* Right grip: gris */
};

/**
 * User Stick/IMU Calibration — relleno de 0xFF = sin calibrar.
 * El Switch interpreta 0xFF, 0xFF en los primeros 2 bytes como
 * "usar calibración de fábrica".
 */
static const uint8_t spi_user_cal_ff[26] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

/* ------------------------------------------------------------------ */
/* Tabla de regiones mapeadas                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t       start;
    uint32_t       end;      /* inclusive */
    const uint8_t *data;
    size_t         data_len;
} spi_region_t;

static const spi_region_t spi_regions[] = {
    { 0x6000, 0x600F, spi_serial,           sizeof(spi_serial)           },
    { 0x6020, 0x6037, spi_imu_factory_cal,  sizeof(spi_imu_factory_cal)  },
    { 0x603D, 0x6045, spi_stick_cal_left,   sizeof(spi_stick_cal_left)   },
    { 0x6046, 0x604E, spi_stick_cal_right,  sizeof(spi_stick_cal_right)  },
    { 0x6050, 0x605B, spi_colors,           sizeof(spi_colors)           },
    { 0x8010, 0x8025, spi_user_cal_ff,      22                           },
    { 0x8026, 0x8041, spi_user_cal_ff,      26                           },
};

#define SPI_REGION_COUNT (sizeof(spi_regions) / sizeof(spi_regions[0]))

/* ------------------------------------------------------------------ */
/* API pública                                                         */
/* ------------------------------------------------------------------ */

void spi_flash_read(uint32_t addr, uint8_t *out, uint8_t len)
{
    /* Por defecto, todo 0xFF (memoria no programada) */
    memset(out, 0xFF, len);

    for (size_t r = 0; r < SPI_REGION_COUNT; r++) {
        const spi_region_t *reg = &spi_regions[r];

        /* ¿La solicitud (addr..addr+len-1) intersecta con esta región? */
        if (addr > reg->end || (addr + len - 1) < reg->start) {
            continue; /* Sin solapamiento */
        }

        /* Calcular el rango de intersección */
        uint32_t intersect_start = (addr > reg->start) ? addr : reg->start;
        uint32_t intersect_end   = ((addr + len - 1) < reg->end)
                                   ? (addr + len - 1) : reg->end;

        for (uint32_t byte_addr = intersect_start;
             byte_addr <= intersect_end;
             byte_addr++)
        {
            size_t out_idx  = byte_addr - addr;
            size_t data_idx = byte_addr - reg->start;

            if (data_idx < reg->data_len) {
                out[out_idx] = reg->data[data_idx];
            }
            /* Si data_idx >= data_len, deja 0xFF (ya inicializado) */
        }

        /*
         * Reads may span several adjacent regions. The working firmware
         * requests 25 bytes starting at 0x603D, crossing the left-stick
         * calibration, right-stick calibration, and color regions. Do not
         * stop after the first overlap or the remainder is returned as FF.
         */
    }
}
