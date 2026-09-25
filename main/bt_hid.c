/**
 * @file bt_hid.c
 * @brief Capa de transporte Bluetooth Classic HID para Nintendo Switch.
 *
 * Configuración BT para emular un Pro Controller de Nintendo Switch 1:
 * (objetivo: que la Switch 2 lo acepte como mando de consola previa)
 *
 *  Nombre:  "Pro Controller"
 *  VID:     0x057E (Nintendo)
 *  PID:     0x2009 (Pro Controller)
 *  CoD:     0x004508 (Rendering + Peripheral + Gamepad)
 *  SSP:     Just Works (ESP_BT_IO_CAP_NONE)
 *
 * Flujo de inicialización:
 *  1. esp_bt_controller_init → esp_bt_controller_enable(CLASSIC_BT)
 *  2. esp_bluedroid_init → esp_bluedroid_enable
 *  3. GAP callback, SSP Just Works, CoD, device name
 *  4. HIDD callback → esp_bt_hid_device_init()
 *  5. [ESP_HIDD_INIT_EVT] → esp_bt_hid_device_register_app(descriptor)
 *  6. [ESP_HIDD_REGISTER_APP_EVT] → esp_bt_gap_set_scan_mode(discoverable)
 *  7. [ESP_HIDD_OPEN_EVT] → iniciar timer de streaming
 *  8. [ESP_HIDD_INTR_DATA_EVT / ESP_HIDD_SET_REPORT_EVT]
 *       → switch_proto_handle_output()
 */
#if !CONFIG_SWITCH_CONTROLLER_BLE_HID
#include "bt_hid.h"
#include "switch_proto.h"
#include "input_report.h"
#include "controller_state.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BT_HID";

/* ------------------------------------------------------------------ */
/* Constantes de identidad del Joy-Con (L) para Switch 2            */
/* ------------------------------------------------------------------ */

#define DEVICE_NAME             "Pro Controller"
#define NINTENDO_VID            0x057E
#define PRO_CONTROLLER_PID           0x2009
#define DEVICE_COD              0x002508U

/*
 * HID Report Descriptor del auténtico dekuNukem Joy-Con (fuente: battle-tested con
 * el Switch 2 via protocolo custom).
 *
 * Publica los Report IDs que el firmware ya usa:
 *   - Report 0x30    : input 49 bytes — full button/status + IMU @60Hz
 *   - Report 0x21    : input 49 bytes — respuesta a subcomandos
 *   - Report 0x80/81/82: inputs/outputs para comandos SPI/Flash/NFC
 */
static const uint8_t s_hid_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x06, 0x01, 0xFF,
    0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x3F, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01,
    0x81, 0x42, 0x05, 0x09, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x16, 0x00, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10,
    0x95, 0x04, 0x81, 0x02, 0x06, 0x01, 0xFF,
    0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0xC0
};
_Static_assert(sizeof(s_hid_descriptor) == 170,
               "Unexpected Joy-Con HID descriptor size");

/* ------------------------------------------------------------------ */
/* Estado global                                                       */
/* ------------------------------------------------------------------ */

uint8_t g_esp32_mac[6] = {0};

static bool              s_connected         = false;
static volatile bool     s_send_in_progress  = false;
static SemaphoreHandle_t s_send_mutex        = NULL;
static esp_timer_handle_t s_input_timer      = NULL;
static esp_timer_handle_t s_pairing_timer    = NULL;
/*
 * esp_bt_hid_device_register_app() solo encola punteros en BTC; no copia
 * esp_hidd_app_param_t ni los QoS antes de retornar. Deben vivir hasta que
 * btc_hd_register_app() los consuma en la tarea Bluetooth.
 */
static esp_hidd_app_param_t s_hid_app_param;
static esp_hidd_qos_param_t s_hid_qos;

/*
 * Estado de diagnóstico de descubrimiento remoto. No se inicia inquiry:
 * durante "Cambiar orden/grip", la Switch actúa como host HID y conecta
 * hacia este dispositivo discoverable.
 */
static bool       s_disc_active          = false; /* inquiry en curso */
static bool       s_connect_in_progress  = false; /* connect() lanzado */
static bool       s_name_candidate_pending = false;
static esp_bd_addr_t s_switch_candidate  = {0};   /* última dispositivo console-like visto */
static esp_bd_addr_t s_switch_bd_addr    = {0};

/* ------------------------------------------------------------------ */
/* Diagnóstico de heap                                                 */
/* ------------------------------------------------------------------ */

/*
 * Bluedroid clásico consume mucho heap en init. Si register_app devuelve
 * ESP_HIDD_NO_RES (status=2) es porque osi_malloc() (= malloc() del sistema)
 * no encontró hueco para el descriptor (~100 bytes). Estos logs del heap
 * permiten ver cuánta RAM queda en cada fase y dónde recortar si falta.
 */
static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "HEAP [%s]: free=%d largest=%d internal_largest=%d",
             stage,
             (int)esp_get_free_heap_size(),
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT));
}

/* ------------------------------------------------------------------ */
/* Timer de streaming de input reports                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Callback del timer FreeRTOS: envía un input report según el modo actual.
 * Se llama cada 15ms (~66Hz) cuando el controlador está conectado.
 */
static void input_report_timer_cb(void *arg)
{
    if (!s_connected) return;

    stream_mode_t mode = switch_proto_get_stream_mode();
    uint8_t       buf[INPUT_REPORT_SIZE];
    uint8_t       timer = switch_proto_next_timer();
    controller_state_t *state = controller_state_get();

    switch (mode) {
    case STREAM_MODE_0x3F:
        input_report_build_0x3F(buf, state);
        bt_hid_send_input_report(buf, 12); /* 0x3F es de 12 bytes */
        break;

    case STREAM_MODE_0x30:
        input_report_build_0x30(buf, timer, state);
        bt_hid_send_input_report(buf, INPUT_REPORT_SIZE);
        break;

    case STREAM_MODE_0x31:
        /*
         * Modo NFC — implementado en Fase 2.
         * Por ahora seguir enviando 0x30 para no quedar en silencio
         * (el Switch desconecta si no recibe reportes).
         */
        input_report_build_0x30(buf, timer, state);
        bt_hid_send_input_report(buf, INPUT_REPORT_SIZE);
        break;

    default:
        /* Sin modo definido: no enviar */
        break;
    }
}

/** Inicia el timer de streaming */
static void start_input_timer(void)
{
    if (s_input_timer) return; /* Ya iniciado */

    esp_timer_create_args_t timer_args = {
        .callback        = input_report_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "hid_input",
        .skip_unhandled_events = true,
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_input_timer));
    /* Periodo de 15ms = 15000 µs → ~66Hz (ligeramente más que los 60Hz nominales) */
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_input_timer, 15000));

    ESP_LOGI(TAG, "Timer de input reports iniciado (15ms / ~66Hz)");
}

/** Detiene el timer de streaming */
static void stop_input_timer(void)
{
    if (s_input_timer) {
        esp_timer_stop(s_input_timer);
        esp_timer_delete(s_input_timer);
        s_input_timer = NULL;
        ESP_LOGI(TAG, "Timer de input reports detenido");
    }

}

static void pairing_window_cb(void *arg)
{
    (void)arg;
    if (!s_connected) {
        esp_err_t err = esp_bt_gap_set_scan_mode(
            ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo renovar discoverability: %s",
                     esp_err_to_name(err));
        }
    }
}

static void start_pairing_window(void)
{
    if (s_pairing_timer != NULL) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = pairing_window_cb,
        .name = "hid_pairing",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_pairing_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_pairing_timer, 2000000));
    pairing_window_cb(NULL);
}

static void stop_pairing_window(void)
{
    if (s_pairing_timer != NULL) {
        esp_timer_stop(s_pairing_timer);
        esp_timer_delete(s_pairing_timer);
        s_pairing_timer = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Callbacks GAP                                                       */
/* ------------------------------------------------------------------ */

/*
 * La Switch normalmente inicia la conexión HID, pero algunos firmwares de
 * Switch 2 solo anuncian la consola durante el escaneo de "Cambiar orden".
 * Un inquiry único de respaldo permite comprobar ese camino sin mantener el
 * controlador ocupado permanentemente.
 */
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "GAP Auth OK — dispositivo: %s",
                     param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "GAP Auth FAILED (status=%d)", param->auth_cmpl.stat);
        }

        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
        /* No debería ocurrir con Just Works, pero por seguridad: */
        ESP_LOGW(TAG, "GAP PIN Request — respondiendo con PIN vacío");
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        /* SSP Just Works: confirmar automáticamente */
        ESP_LOGI(TAG, "GAP SSP Confirm (num_val=%"PRIu32") — aceptando",
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "GAP SSP Passkey: %"PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGD(TAG, "GAP Mode change: mode=%d", param->mode_chg.mode);
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_disc_active = false;
            ESP_LOGD(TAG, "Inquiry terminado (STOPPED)");
        }
        break;

    case ESP_BT_GAP_DISC_RES_EVT:
        /*
         * Resultado del inquiry: en v5.4 el CoD/nombre/RSSI vienen como
         * propiedades (param->disc_res.prop[]), no como campos directos.
         * Logueamos TODO lo que el aire devuelva para poder ver qué
         * CoD/nombre usa la Switch real en tu entorno.
         */
        {
            esp_bd_addr_t *bda = &param->disc_res.bda;
            char      name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
            uint32_t  cod  = 0;
            int8_t    rssi = 0;

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
                switch (p->type) {
                case ESP_BT_GAP_DEV_PROP_BDNAME:
                    strncpy(name, (const char *)p->val, sizeof(name) - 1);
                    break;
                case ESP_BT_GAP_DEV_PROP_COD:
                    cod = *(uint32_t *)p->val;
                    break;
                case ESP_BT_GAP_DEV_PROP_RSSI:
                    rssi = *(int8_t *)p->val;
                    break;
                default:
                    break;
                }
            }

            ESP_LOGD(TAG, "INQ: %02X:%02X:.. '%s' cod=0x%06X rssi=%d",
                     (*bda)[0], (*bda)[1], name, (unsigned)cod, (int)rssi);

            if (name[0] == '\0' && cod == 0) {
                break; /* sin datos útiles */
            }
            if (memcmp(*bda, g_esp32_mac, 6) == 0) {
                break; /* somos nosotros */
            }

            /* Heurística "parece una consola/gamepad" para el candidato */
            uint32_t major = (cod >> 8) & 0x1F;
            uint32_t minor = (cod >> 2) & 0x3F;
            bool looks_switch = (major == 0x05 && minor == 0x02) || /* Peripheral+Gamepad */
                                (major == 0x06);                    /* Gaming device  */

            if (looks_switch && !s_name_candidate_pending) {
                s_name_candidate_pending = true;
                memcpy(s_switch_candidate, *bda, 6);
                ESP_LOGI(TAG, "Candidato console: %02X:%02X cod=0x%06X — leyendo nombre...",
                         (*bda)[0], (*bda)[1], (unsigned)cod);
                esp_err_t err = esp_bt_gap_read_remote_name(*bda);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "read_remote_name: %s", esp_err_to_name(err));
                    s_name_candidate_pending = false;
                }
            } else if (looks_switch && s_name_candidate_pending) {
                /* Para Switch 2, conectarnos al primero que parezca una consola;
                 * si un segundo candidato también coincide (p.ej. ambos tienen CoD de gamepad),
                 * registrar una alerta pero continuar esperando el primero. */
                ESP_LOGW(TAG, "Otro candidato consola (%02X:%02X) pero esperando primero (%02X:%02X)",
                         (*bda)[0], (*bda)[1],
                         s_switch_candidate[0], s_switch_candidate[1]);
            }
        }
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        /*
         * Ya leímos el nombre de un candidato. Si contiene "Switch"/"Nintendo"
         * (o un mando Joy-Con), es la consola: cancelamos inquiry y conectamos
         * activamente. Esto sustituye al "L+R" físico del mando real.
         */
        {
            const char *name = (const char *)param->read_rmt_name.rmt_name;
            if (memcmp(param->read_rmt_name.bda, s_switch_candidate, 6) != 0) {
                s_name_candidate_pending = false;
                break;
            }
            s_name_candidate_pending = false;

            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS &&
                name != NULL && name[0] != '\0') {
                ESP_LOGI(TAG, "Nombre del candidato: '%s'", name);
                if (strstr(name, "Switch") || strstr(name, "Nintendo") ||
                    strstr(name, "Joy-Con") || strstr(name, "Pro Controller")) {
                    ESP_LOGI(TAG, "Es la Switch — conectando activamente (%02X:%02X)",
                             param->read_rmt_name.bda[0], param->read_rmt_name.bda[1]);
                    esp_bt_gap_cancel_discovery();
                    s_disc_active          = false;
                    s_connect_in_progress  = true;
                    memcpy(s_switch_bd_addr, param->read_rmt_name.bda, 6);

                    esp_err_t err = esp_bt_hid_device_connect(param->read_rmt_name.bda);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "hid connect: %s — reintentando en el próximo ciclo",
                                 esp_err_to_name(err));
                        s_connect_in_progress = false;
                    }
                }
            } else {
                ESP_LOGD(TAG, "Candidato sin nombre/descartado");
            }
        }
        break;

    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Callbacks HID Device                                                */
/* ------------------------------------------------------------------ */

static void bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {

    case ESP_HIDD_INIT_EVT:
        if (param->init.status == ESP_HIDD_SUCCESS) {
            ESP_LOGI(TAG, "HID Device inicializado — registrando app...");
            log_heap("antes de register_app");

            /*
             * Inicialización explícita: el perfil HID clásico copia esta
             * estructura a través de la cola BTC; mantener todos los campos
             * completamente definidos evita que un valor indeterminado de
             * longitud provoque malloc(0) y deje el descriptor en NULL.
             */
            memset(&s_hid_app_param, 0, sizeof(s_hid_app_param));
            s_hid_app_param.name = DEVICE_NAME;
            s_hid_app_param.description = "Gamepad";
            s_hid_app_param.provider = "Nintendo";
            s_hid_app_param.subclass = 0x08;
            s_hid_app_param.desc_list = (uint8_t *)s_hid_descriptor;
            s_hid_app_param.desc_list_len = (int)sizeof(s_hid_descriptor);
            ESP_LOGI(TAG, "HID descriptor: ptr=%p len=%d",
                     s_hid_app_param.desc_list, s_hid_app_param.desc_list_len);

            /*
             * IMPORTANTE: los QoS in/out NO pueden ser NULL.
             * btc_hd.c:333-344 los desreferencia incondicionalmente; pasar
             * NULL crashea el Core 0 en cuanto los mallocs internos tengan
             * éxito. El ejemplo oficial (bt_hid_mouse_device) usa QoS a ceros.
             */
            memset(&s_hid_qos, 0, sizeof(s_hid_qos));
            /*
             * Use the same guaranteed QoS profile as ESP-IDF's esp_hid
             * Classic HID helper.  In L2CAP, 0x01 is BEST_EFFORT and 0x02 is
             * GUARANTEED; using 0x01 here would not reproduce the reference
             * HID device.
             */
            s_hid_qos.service_type = 0x02;
            s_hid_qos.token_rate = sizeof(s_hid_descriptor) * 100;
            s_hid_qos.token_bucket_size = sizeof(s_hid_descriptor);
            s_hid_qos.peak_bandwidth = sizeof(s_hid_descriptor) * 100;
            s_hid_qos.access_latency = 10;
            s_hid_qos.delay_variation = 10;

            esp_err_t err = esp_bt_hid_device_register_app(
                &s_hid_app_param, &s_hid_qos, &s_hid_qos);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "register_app falló: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "HIDD init falló (status=%d)", param->init.status);
        }
        break;

    case ESP_HIDD_REGISTER_APP_EVT:
        if (param->register_app.status == ESP_HIDD_SUCCESS) {
            ESP_LOGI(TAG, "HID App + SDP HID Record registrados OK");

            esp_bt_cod_t cod = {
                .service = (DEVICE_COD >> 13) & 0x07FF,
                .major = (DEVICE_COD >> 8) & 0x1F,
                .minor = (DEVICE_COD >> 2) & 0x3F,
                .reserved_2 = 0,
                .reserved_8 = 0,
            };
            /*
             * SET_COD_MAJOR_MINOR deja intact la clase de servicio. Aquí
             * necesitamos escribir también el bit de servicio contenido en
             * DEVICE_COD; de lo contrario el valor impreso sería 0x002508,
             * pero el controlador anunciaría solamente 0x000508.
             */
            esp_err_t cod_err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
            if (cod_err != ESP_OK) {
                ESP_LOGW(TAG, "set_cod: %s", esp_err_to_name(cod_err));
            }

            ESP_LOGI(TAG, "Esperando conexión del Switch...");
            ESP_LOGI(TAG, "  Nombre BT : " DEVICE_NAME);
            ESP_LOGI(TAG, "  VID/PID   : 0x%04X / 0x%04X",
                     NINTENDO_VID, PRO_CONTROLLER_PID);
            ESP_LOGI(TAG, "  CoD       : 0x%06X", DEVICE_COD);

            /* INICIAR INQUIRY COMO FALLBACK PARA SWITCH 2 */
            ESP_LOGI(TAG, "Iniciando inquiry como respaldo para Switch 2...");
            esp_err_t inq_err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 24, 0);
            if (inq_err != ESP_OK) {
                ESP_LOGW(TAG, "inquiry falló: %s", esp_err_to_name(inq_err));
            } else {
                ESP_LOGI(TAG, "Inquiry iniciado - esperando dispositivos...");
            }

            esp_err_t name_err = esp_bt_gap_set_device_name(DEVICE_NAME);
            if (name_err != ESP_OK) {
                ESP_LOGW(TAG, "set_device_name: %s",
                         esp_err_to_name(name_err));
            }
            esp_err_t scan_err = esp_bt_gap_set_scan_mode(
                ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            if (scan_err != ESP_OK) {
                ESP_LOGE(TAG, "No se pudo publicar discoverable: %s",
                         esp_err_to_name(scan_err));
            }

            /* INICIAR INQUIRY COMO FALLBACK PARA SWITCH 2 */
            ESP_LOGI(TAG, "Iniciando inquiry como respaldo para Switch 2...");
            esp_err_t inq_err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 24, 0);
            if (inq_err != ESP_OK) {
                ESP_LOGW(TAG, "inquiry falló: %s", esp_err_to_name(inq_err));
            } else {
                ESP_LOGI(TAG, "Inquiry iniciado - esperando dispositivos...");
            }

            start_pairing_window();

            int bond_count = esp_bt_gap_get_bond_device_num();
            if (bond_count > 0) {
                esp_bd_addr_t *bonded =
                    malloc(sizeof(esp_bd_addr_t) * (size_t)bond_count);
                if (bonded != NULL) {
                    int list_count = bond_count;
                    esp_err_t list_err =
                        esp_bt_gap_get_bond_device_list(&list_count, bonded);
                    if (list_err == ESP_OK && list_count > 0) {
                        ESP_LOGI(TAG, "Reconectando dispositivo enlazado "
                                 "%02X:%02X:%02X:%02X:%02X:%02X",
                                 bonded[0][0], bonded[0][1], bonded[0][2],
                                 bonded[0][3], bonded[0][4], bonded[0][5]);
                        esp_err_t connect_err =
                            esp_bt_hid_device_connect(bonded[0]);
                        if (connect_err != ESP_OK) {
                            ESP_LOGW(TAG, "Reconexión HID falló: %s",
                                     esp_err_to_name(connect_err));
                        }
                    }
                    free(bonded);
                } else {
                    ESP_LOGW(TAG, "No hay memoria para leer dispositivos enlazados");
                }
            } else {
                ESP_LOGI(TAG, "Sin dispositivos enlazados; esperando pairing");
            }

        } else {
            ESP_LOGE(TAG, "register_app falló (status=%d)",
                     param->register_app.status);
            ESP_LOGE(TAG, "HID no está publicado; no se intentará aparentar modo discoverable");
        }
        break;

    case ESP_HIDD_OPEN_EVT:
        if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
            ESP_LOGI(TAG, "=== Switch conectado! ===");
            s_connected           = true;
            s_send_in_progress    = false;
            s_connect_in_progress = false;
            stop_pairing_window();

            /* Conectado: parar el inquiry activo si aún corría */
            esp_bt_gap_cancel_discovery();
            s_disc_active = false;

            switch_proto_init();
            controller_state_init(controller_state_get());

            /* Dejar de ser discoverable durante la sesión */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                     ESP_BT_NON_DISCOVERABLE);

            start_input_timer();
        } else if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTING) {
            ESP_LOGI(TAG, "Conectando...");
        }
        break;

    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG, "=== Switch desconectado ===");
        s_connected           = false;
        s_send_in_progress    = false;
        s_connect_in_progress = false;
        s_disc_active         = false;
        s_name_candidate_pending = false;

        stop_input_timer();
        start_pairing_window();

        /* Volver a ser discoverable para reconexión */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                 ESP_BT_GENERAL_DISCOVERABLE);
        ESP_LOGI(TAG, "Mando discoverable de nuevo para reconexión");
        break;

    case ESP_HIDD_SEND_REPORT_EVT:
        /*
         * El reporte anterior fue transmitido por L2CAP.
         * Liberamos el semáforo de flow control.
         */
        s_send_in_progress = false;
        if (param->send_report.status != ESP_HIDD_SUCCESS) {
            ESP_LOGW(TAG, "Send report falló (reason=%d)",
                     param->send_report.reason);
        }
        break;

    case ESP_HIDD_INTR_DATA_EVT:
        /*
         * Output Report recibido en el canal Interrupt (PSM 0x13).
         * El Switch envía subcomandos aquí.
         * ESP-IDF entrega el Report ID separado del payload.
         */
        {
            uint8_t report[65];
            uint16_t payload_len = param->intr_data.len;
            if (payload_len > sizeof(report) - 1) {
                payload_len = sizeof(report) - 1;
            }
            report[0] = param->intr_data.report_id;
            if (payload_len > 0 && param->intr_data.data != NULL) {
                memcpy(&report[1], param->intr_data.data, payload_len);
            }

            ESP_LOGD(TAG, "INTR Data recibido (len=%u): [%02X %02X %02X ...]",
                     (unsigned)(payload_len + 1),
                     report[0],
                     payload_len > 0 ? report[1] : 0xFF,
                     payload_len > 1 ? report[2] : 0xFF);

            switch_proto_handle_output(report, payload_len + 1);
        }
        break;

    case ESP_HIDD_SET_REPORT_EVT:
        /*
         * Output Report recibido en el canal Control (PSM 0x11).
         * Menos común con el Switch, pero hay que manejarlo.
         */
        {
            uint8_t report[64];
            uint16_t len = param->set_report.len;

            /*
             * En SET_REPORT, ESP-IDF entrega el ID por separado y data
             * contiene solo el payload. El parser común usa el formato
             * Nintendo completo (ID en data[0]), así que lo reconstruimos.
             */
            if (len > sizeof(report) - 1) {
                len = sizeof(report) - 1;
            }
            report[0] = param->set_report.report_id;
            if (len > 0) {
                memcpy(&report[1], param->set_report.data, len);
            }

            ESP_LOGD(TAG, "SET_REPORT (type=%d id=0x%02X len=%u)",
                     param->set_report.report_type,
                     report[0], (unsigned)(len + 1));

            switch_proto_handle_output(report, len + 1);
        }
        break;

    case ESP_HIDD_GET_REPORT_EVT:
        /*
         * El Switch pide un reporte via GET_REPORT en el canal Control.
         * Responder con el estado actual en modo 0x3F.
         */
        {
            ESP_LOGD(TAG, "GET_REPORT (type=%d id=0x%02X)",
                     param->get_report.report_type,
                     param->get_report.report_id);

            uint8_t buf[12];
            input_report_build_0x3F(buf, controller_state_get());
            esp_bt_hid_device_send_report(
                (esp_hidd_report_type_t)param->get_report.report_type,
                param->get_report.report_id,
                sizeof(buf) - 1, &buf[1]);
        }
        break;

    default:
        ESP_LOGD(TAG, "HIDD event: %d", event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* API pública                                                         */
/* ------------------------------------------------------------------ */

void bt_hid_send_input_report(const uint8_t *data, uint16_t len)
{
    if (!s_connected) return;

    /*
     * Flow control simple: si el stack Bluedroid aún está procesando el
     * reporte anterior (s_send_in_progress == true), descartar este reporte.
     * Esto evita acumular backlog de lag en el buffer L2CAP.
     *
     * A 66Hz con un RTT BT de ~10ms, la tasa de descarte debería ser ~0%.
     */
    if (s_send_in_progress) {
        ESP_LOGV(TAG, "send ocupado — reporte descartado");
        return;
    }

    s_send_in_progress = true;

    /*
     * esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, id, len, data)
     *
     * - type = ESP_HIDD_REPORT_TYPE_INTRDATA → envía por canal Interrupt (PSM 0x13)
     * - id   = primer byte del reporte (el ID no forma parte del payload)
     * - Bluedroid añade el header HIDP 0xA1 y el ID automáticamente
     */
    if (len < 1) {
        return;
    }

    esp_err_t err = esp_bt_hid_device_send_report(
        ESP_HIDD_REPORT_TYPE_INTRDATA,
        data[0],
        len - 1,
        (uint8_t *)&data[1]
    );

    if (err != ESP_OK) {
        s_send_in_progress = false;
        ESP_LOGW(TAG, "send_report err: %s", esp_err_to_name(err));
    }
    /* Si OK, s_send_in_progress se libera en ESP_HIDD_SEND_REPORT_EVT */
}

esp_err_t bt_hid_init(void)
{
    esp_err_t err;

    s_send_mutex = xSemaphoreCreateMutex();
    if (!s_send_mutex) {
        ESP_LOGE(TAG, "No se pudo crear mutex de send");
        return ESP_ERR_NO_MEM;
    }
    /* --- 1. Obtener MAC del ESP32 --- */
    err = esp_read_mac(g_esp32_mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac falló: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MAC BT: %02X:%02X:%02X:%02X:%02X:%02X",
             g_esp32_mac[0], g_esp32_mac[1], g_esp32_mac[2],
             g_esp32_mac[3], g_esp32_mac[4], g_esp32_mac[5]);

    /* --- 2. Controlador BT ---
     * El modo del controlador lo fija la config (sdkconfig.defaults):
     *   CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y  → ESP_BT_MODE_CLASSIC_BT (0x02)
     *   CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y     → ESP_BT_MODE_BLE         (0x01) [C3]
     *   CONFIG_BTDM_CTRL_MODE_BTDM=y         → ESP_BT_MODE_BTDM        (0x03)
     *
     * esp_bt_controller_enable() exige pasar EXACTAMENTE el mismo modo que
     * quedó fijado en esp_bt_controller_init() (esp32/bt.c ~linea 1914 +
     * esp_bt.h BTDM_CONTROLLER_MODE_EFF). Cualquier otra combinación devuelve
     * ESP_ERR_INVALID_ARG. POR ESO debes validar sdkconfig antes de build:
     *   CONFIG_BTDM_CTRL_MODE_BLE_ONLY=n  (no debe estar 'y')
     *   CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y  ← el que necesita este firmware
     *
     * Si sdkconfig quedó con BLE_ONLY (p.ej. heredado de un set-target esp32c3),
     * incluso este código devuelve ESP_ERR_INVALID_ARG: no es un bug del código,
     * es la config del controlador.
     */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller_enable: %s — revisa que sdkconfig tenga "
                      "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y "
                      "(no BLE_ONLY y no BTDM)", esp_err_to_name(err));
        return err;
    }
    log_heap("tras controller_enable");

    /* --- 3. Bluedroid --- */
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(err));
        return err;
    }
    log_heap("tras bluedroid_enable");

    /* --- 4. GAP --- */
    err = esp_bt_gap_register_callback(bt_gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GAP register_callback: %s", esp_err_to_name(err));
        return err;
    }

    /* Match the identity publication order used by real Classic HID pads. */
    err = esp_bt_gap_set_device_name(DEVICE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_device_name: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Publish the name in the inquiry response as well as in the remote-name
     * database.  Some console host scans only the EIR payload and never sends
     * a separate remote-name request; phones usually do the latter, which can
     * hide this compatibility problem.
     */
    esp_bt_eir_data_t eir = {
        .include_name = true,
        .flag = ESP_BT_EIR_FLAG_GEN_DISC,
    };
    err = esp_bt_gap_config_eir_data(&eir);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config_eir_data: %s", esp_err_to_name(err));
    }

    esp_bt_cod_t cod = {
        .service = (DEVICE_COD >> 13) & 0x07FF,
        .major = (DEVICE_COD >> 8) & 0x1F,
        .minor = (DEVICE_COD >> 2) & 0x3F,
        .reserved_2 = 0,
        .reserved_8 = 0,
    };
    err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_cod: %s", esp_err_to_name(err));
        return err;
    }

    /* SSP Just Works: sin teclado ni pantalla (igual que los Joy-Con reales) */
    esp_bt_sp_param_t iocap = ESP_BT_IO_CAP_NONE;
    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                        &iocap, sizeof(uint8_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_security_param: %s", esp_err_to_name(err));
    }

    /* --- 5. HID Device --- */
    err = esp_bt_hid_device_register_callback(bt_hidd_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HIDD register_callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_hid_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HIDD init: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * No iniciar inquiry aquí. En el modo "Cambiar orden/grip", la Switch
     * actúa como host HID y abre la conexión hacia el mando discoverable.
     * Un inquiry desde el ESP32 compite por el controlador BR/EDR y puede
     * impedir que la Switch complete el SDP/HID handshake.
     */
    ESP_LOGI(TAG, "Mando discoverable; esperando conexión iniciada por la Switch");

    /* A partir de aquí el flujo continúa en bt_hidd_cb → ESP_HIDD_INIT_EVT */
    ESP_LOGI(TAG, "BT HID init completado — esperando ESP_HIDD_INIT_EVT");
    return ESP_OK;
}
#endif /* !CONFIG_SWITCH_CONTROLLER_BLE_HID */
