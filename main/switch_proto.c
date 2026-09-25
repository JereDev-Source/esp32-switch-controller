/**
 * @file switch_proto.c
 * @brief Implementación del protocolo HID de Nintendo Switch 1 (legacy).
 *
 * Subcomandos implementados en Fase 1:
 *
 *  0x01 — Set pairing info       → ACK + registro de parámetros
 *  0x02 — Request Device Info    → respuesta completa
 *  0x03 — Set Input Report Mode  → ACK + cambio de modo
 *  0x04 — Trigger Button Elapsed → ACK genérico
 *  0x05 — Get Page List State    → ACK genérico
 *  0x06 — Set HCI State          → ACK genérico
 *  0x07 — Reset Pairing Info     → ACK genérico
 *  0x08 — Set Shipment State     → ACK
 *  0x10 — SPI Flash Read         → respuesta con datos de flash emulada
 *  0x11 — SPI Flash Write        → ACK genérico
 *  0x12 — SPI Flash Sector Erase → ACK genérico
 *  0x20 — Reset NFC/IR MCU       → ACK genérico
 *  0x21 — Set NFC/IR MCU Config  → ACK genérico (stub Fase 1)
 *  0x22 — Set NFC/IR MCU State   → ACK genérico (stub Fase 1)
 *  0x30 — Set Player Lights      → ACK
 *  0x31 — Get Player Lights      → ACK genérico
 *  0x38 — Set HOME Light         → ACK
 *  0x40 — Enable IMU             → ACK
 *  0x41 — Set IMU Sensitivity    → ACK genérico
 *  0x42 — Write IMU Register     → ACK genérico
 *  0x43 — Read IMU Register      → ACK genérico
 *  0x48 — Enable Vibration       → ACK
 *  Resto → ACK genérico (0x80, subcmd_id, payload=0x03)
 */
#include "switch_proto.h"
#include "hid_transport.h"
#include "spi_flash.h"
#include "input_report.h"
#include "nfc_mcu.h"
#include "controller_state.h"
#include <string.h>
#include "esp_log.h"
#include "esp_bt_defs.h"

static const char *TAG = "PROTO";

/* Estado del módulo */
static stream_mode_t s_stream_mode = STREAM_MODE_0x30;
static uint8_t       s_timer       = 0x00;
static bool          s_stream_enabled;
static uint32_t      s_last_reply_ms;

/* MAC address del ESP32 (se obtiene en bt_hid.c al conectar) */
/*
 * Respuestas de 48 bytes usadas por el firmware funcional de referencia.
 * El primer byte es el contador; el Report ID 0x21 se envía separado por
 * esp_bt_hid_device_send_report().
 */
/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Envía un Input Report 0x21 como respuesta a un subcomando.
 */
static void send_subcmd_reply(uint8_t subcmd_id,
                              const uint8_t *payload, uint8_t payload_len)
{
    uint8_t report[INPUT_REPORT_SIZE];
    controller_state_t *state = controller_state_get();

    input_report_build_0x21(report, s_timer, state,
                             subcmd_id, payload, payload_len);
    s_timer = (s_timer + 1) & 0xFF;

    ESP_LOGD(TAG, "Reply 0x21 subcmd=0x%02X timer=0x%02X ack=0x%02X len=%u",
             subcmd_id, report[1], report[13], (unsigned int)sizeof(report));
    hid_transport_send_input_report(report, INPUT_REPORT_SIZE);
}

/**
 * @brief ACK genérico para subcomandos no implementados.
 *        La respuesta usa una clase ACK fija 0x80; el subcomando va en
 *        el byte siguiente del reporte.
 */
static void send_generic_ack(uint8_t subcmd_id)
{
    uint8_t payload[1] = { 0x03 };
    ESP_LOGD(TAG, "Subcmd 0x%02X -> generic ACK", subcmd_id);
    send_subcmd_reply(subcmd_id, payload, sizeof(payload));
}

/** Subcmd 0x01: Set pairing info */
static void handle_subcmd_01_pairing(const uint8_t *args, uint16_t args_len)
{
    ESP_LOGI(TAG, "Subcmd 0x01 -> Set pairing info (len=%u)", args_len);
    if (args != NULL && args_len > 0) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, args, args_len, ESP_LOG_DEBUG);
    }

    /*
     * El firmware funcional confirma este subcomando con el ACK normal.
     * Los datos recibidos son información de pairing para el host; no deben
     * copiarse a la respuesta ni interpretarse como una dirección local.
     */
    send_generic_ack(0x01);
}

/* ------------------------------------------------------------------ */
/* Handlers de subcomandos específicos                                 */
/* ------------------------------------------------------------------ */

/** Subcmd 0x02: Request Device Info */
static void handle_subcmd_02_device_info(void)
{
    uint8_t payload[12] = {
        0x03, 0x48, 0x03, 0x02,
        g_esp32_mac[0], g_esp32_mac[1], g_esp32_mac[2],
        g_esp32_mac[3], g_esp32_mac[4], g_esp32_mac[5],
        0x01, 0x01
    };

    ESP_LOGI(TAG, "Subcmd 0x02 -> Device Info");
    send_subcmd_reply(0x02, payload, sizeof(payload));
}

/** Subcmd 0x03: Set Input Report Mode */
static void handle_subcmd_03_set_mode(uint8_t mode_arg)
{
    ESP_LOGI(TAG, "Subcmd 0x03 -> Set Mode 0x%02X", mode_arg);

    switch (mode_arg) {
    case 0x30:
        s_stream_mode = STREAM_MODE_0x30;
        s_stream_enabled = true;
        ESP_LOGI(TAG, "Modo cambiado a 0x30 (Full Report + IMU)");
        break;
    case 0x31:
        s_stream_mode = STREAM_MODE_0x31;
        s_stream_enabled = true;
        ESP_LOGI(TAG, "Modo cambiado a 0x31 (NFC read)");
        break;
    case 0x3F:
        s_stream_mode = STREAM_MODE_0x3F;
        s_stream_enabled = true;
        ESP_LOGI(TAG, "Modo cambiado a 0x3F (Simple HID)");
        break;
    default:
        ESP_LOGW(TAG, "Modo desconocido 0x%02X - ignorando", mode_arg);
        break;
    }

    send_subcmd_reply(0x03, (const uint8_t[]){0x03}, 1);
}

/** Subcmd 0x08: Set Shipment State */
static void handle_subcmd_08_shipment(uint8_t arg)
{
    ESP_LOGI(TAG, "Subcmd 0x08 -> Shipment State 0x%02X", arg);
    /* Match joycontrol _command_set_shipment_state: ACK 80 08,
     * zero-filled reply data; the generic 03 payload is not used here.
     * Reference: github.com/mart1nro/joycontrol/blob/master/joycontrol/protocol.py
     */
    send_subcmd_reply(0x08, NULL, 0);
}

/** Subcmd 0x10: SPI Flash Read */
static void handle_subcmd_10_spi_read(const uint8_t *args)
{
    /*
     * args[0..3]: dirección (Little-Endian uint32_t)
     * args[4]:    longitud en bytes
     */
    uint32_t addr = (uint32_t)args[0]
                  | ((uint32_t)args[1] << 8)
                  | ((uint32_t)args[2] << 16)
                  | ((uint32_t)args[3] << 24);
    uint8_t  len  = args[4];

    if (len > 0x1D) len = 0x1D; /* Máximo del protocolo */

    ESP_LOGI(TAG, "Subcmd 0x10 -> SPI Read addr=0x%06"PRIx32" len=%u", addr, len);

    /*
     * Payload de respuesta:
     * [0-3]  Dirección echoed (Little-Endian)
     * [4]    Longitud echoed
     * [5..]  Datos leídos
     */
    uint8_t payload[5 + 0x1D];
    memset(payload, 0xFF, sizeof(payload));

    payload[0] = args[0];
    payload[1] = args[1];
    payload[2] = args[2];
    payload[3] = args[3];
    payload[4] = len;

    spi_flash_read(addr, &payload[5], len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, &payload[5], len, ESP_LOG_DEBUG);

    send_subcmd_reply(0x10, payload, 5 + len);
}

/** Subcmd 0x30: Set Player Lights */
static void handle_subcmd_30_player_lights(uint8_t arg)
{
    ESP_LOGI(TAG, "Subcmd 0x30 -> Player Lights 0x%02X", arg);
    send_subcmd_reply(0x30, NULL, 0);
}

/** Subcmd 0x38: Set HOME Light */
static void handle_subcmd_38_home_light(void)
{
    ESP_LOGI(TAG, "Subcmd 0x38 -> HOME Light");
    send_subcmd_reply(0x38, NULL, 0);
}

/** Subcmd 0x40: Enable/Disable IMU */
static void handle_subcmd_40_imu(uint8_t arg)
{
    ESP_LOGI(TAG, "Subcmd 0x40 -> IMU %s", arg ? "ON" : "OFF");
    send_subcmd_reply(0x40, NULL, 0);
}

/** Subcmd 0x48: Enable/Disable Vibration */
static void handle_subcmd_48_vibration(uint8_t arg)
{
    ESP_LOGI(TAG, "Subcmd 0x48 -> Vibration %s", arg ? "ON" : "OFF");
    send_subcmd_reply(0x48, NULL, 0);
}

/** Subcmd 0x04: Trigger button elapsed times (7 little-endian uint16). */
static void handle_subcmd_04_trigger_elapsed(void)
{
    uint8_t payload[14] = { 0 };
    send_subcmd_reply(0x04, payload, sizeof(payload));
}

/** Subcmd 0x05: Host page-list state. A paired host is present. */
static void handle_subcmd_05_page_list(void)
{
    const uint8_t payload[] = { 0x01 };
    send_subcmd_reply(0x05, payload, sizeof(payload));
}

/** Subcmd 0x31: Current player-light state. */
static void handle_subcmd_31_player_lights(void)
{
    const uint8_t payload[] = { 0x00 };
    send_subcmd_reply(0x31, payload, sizeof(payload));
}

/**
 * Subcmd 0x21: Set NFC/IR MCU Configuration.
 * Stub para Fase 1: responde ACK indicando modo standby (MCU status = 0x01).
 * Fase 2 reemplazará este handler con la máquina de estados completa del MCU.
 */
static void handle_subcmd_21_mcu_config(const uint8_t *args, uint16_t len)
{
    uint8_t payload[34];
    if (!nfc_config(args,len,payload)) {
        ESP_LOGW(TAG, "NFC config unsupported/short");
        return;
    }
    ESP_LOGI(TAG, "NFC MCU config mode=0x%02X", args[2]);
    send_subcmd_reply(0x21,payload,sizeof(payload));
}
static void handle_subcmd_22_mcu_state(const uint8_t *args, uint16_t len)
{
    if (!len || !nfc_power(args[0])) {
        ESP_LOGW(TAG, "NFC power unsupported/short");return;
    }
    ESP_LOGI(TAG, "NFC MCU power=%u",args[0]);
    send_subcmd_reply(0x22,NULL,0);
}

/* ------------------------------------------------------------------ */
/* Dispatcher principal                                                */
/* ------------------------------------------------------------------ */

static void dispatch_subcommand(uint8_t subcmd_id, const uint8_t *args,
                                uint16_t args_len)
{
    switch (subcmd_id) {
    case 0x01: handle_subcmd_01_pairing(args, args_len); break;
    case 0x02: handle_subcmd_02_device_info();  break;
    case 0x03: handle_subcmd_03_set_mode(args_len > 0 ? args[0] : 0x3F); break;
    case 0x04: handle_subcmd_04_trigger_elapsed(); break;
    case 0x05: handle_subcmd_05_page_list(); break;
    case 0x06: send_generic_ack(0x06); break;  /* Set HCI State */
    case 0x07: send_generic_ack(0x07); break;  /* Reset Pairing Info */
    case 0x08: handle_subcmd_08_shipment(args_len > 0 ? args[0] : 0); break;
    case 0x10:
        if (args_len >= 5) {
            handle_subcmd_10_spi_read(args);
        } else {
            ESP_LOGW(TAG, "Subcmd 0x10 con args insuficientes (%u)", args_len);
            send_generic_ack(0x10);
        }
        break;
    case 0x11: send_generic_ack(0x11); break;  /* SPI Flash Write */
    case 0x12: send_generic_ack(0x12); break;  /* SPI Sector Erase */
    case 0x20: send_generic_ack(0x20); break;  /* Reset NFC/IR MCU */
    case 0x21: handle_subcmd_21_mcu_config(args,args_len); break;
    case 0x22: handle_subcmd_22_mcu_state(args,args_len);  break;
    case 0x30: handle_subcmd_30_player_lights(args_len > 0 ? args[0] : 0); break;
    case 0x31: handle_subcmd_31_player_lights(); break;
    case 0x38: handle_subcmd_38_home_light();  break;
    case 0x40: handle_subcmd_40_imu(args_len > 0 ? args[0] : 0); break;
    case 0x41: send_generic_ack(0x41); break;  /* Set IMU Sensitivity */
    case 0x42: send_generic_ack(0x42); break;  /* Write IMU Register */
    case 0x43: send_generic_ack(0x43); break;  /* Read IMU Register */
    case 0x48: handle_subcmd_48_vibration(args_len > 0 ? args[0] : 0); break;
    default:
        ESP_LOGW(TAG, "Subcmd desconocido 0x%02X", subcmd_id);
        send_generic_ack(subcmd_id);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* API pública                                                         */
/* ------------------------------------------------------------------ */

void switch_proto_init(void)
{
    nfc_reset();
    /* El Pro Controller transmite el reporte completo desde la conexión. */
    s_stream_mode = STREAM_MODE_0x30;
    s_timer       = 0x00;
    s_stream_enabled = false;
    ESP_LOGI(TAG, "Protocolo Switch inicializado");
}

void switch_proto_handle_output(const uint8_t *data, uint16_t len)
{
    if (!data || len < 1) return;

    uint8_t report_id = data[0];

    if (report_id == 0x01) {
        /*
         * Output Report 0x01: Rumble + Subcommand
         * [0]    Report ID = 0x01
         * [1]    Packet counter (0x0..0xF)
         * [2-5]  Left rumble data  (ignorar en Fase 1)
         * [6-9]  Right rumble data (ignorar en Fase 1)
         * [10]   Subcommand ID
         * [11..] Subcommand arguments
         */
        if (len < 11) {
            ESP_LOGW(TAG, "Output 0x01 demasiado corto: %u bytes", len);
            return;
        }

        uint8_t  subcmd_id  = data[10];
        const uint8_t *args = &data[11];
        uint16_t args_len   = (len > 11) ? (len - 11) : 0;

        ESP_LOGD(TAG, "Output 0x01 pkt=%02X subcmd=0x%02X args_len=%u",
                 data[1], subcmd_id, args_len);

        dispatch_subcommand(subcmd_id, args, args_len);

    } else if (report_id == 0x10) {
        /* Rumble only - ignorar silenciosamente */
        ESP_LOGV(TAG, "Output 0x10 (rumble only) - ignorado");

    } else if (report_id == 0x11) {
        bool ok = nfc_command(data,len);
        nfc_diagnostics_t d=nfc_diagnostics();
        if (d.received <= 12 || !ok || (len > 11 && data[10] == 2 && data[11] != 4)) {
            ESP_LOGI(TAG,"NFC RX cmd=%02X sub=%02X len=%u handled=%u",
                     len>10?data[10]:0,len>11?data[11]:0,len,ok);
            ESP_LOGI(TAG,"NFC state power=%u poll=%u present=%u queue=%u rx=%u rejected=%u",
                     d.power,d.poll,d.present,d.pending,d.received,d.rejected);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,data,len,ESP_LOG_INFO);
        }
    } else {
        ESP_LOGW(TAG, "Output Report ID desconocido: 0x%02X (len=%u)", report_id, len);
    }
}

stream_mode_t switch_proto_get_stream_mode(void)
{
    return s_stream_mode;
}

uint8_t switch_proto_next_timer(void)
{
    uint8_t t = s_timer;
    s_timer = (s_timer + 1) & 0xFF;
    return t;
}

bool switch_proto_stream_enabled(void)
{
    return s_stream_enabled;
}
