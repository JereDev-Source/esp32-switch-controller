#include "btstack_hid.h"

#if CONFIG_SWITCH_CONTROLLER_BTSTACK

#include "btstack_defines.h"
#include "btstack_event.h"
#include "hci.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "device_id_server.h"
#include "classic/hid_device.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_port_esp32.h"
#include "btstack_util.h"
#include "controller_state.h"
#include "hid_transport.h"
#include "input_report.h"
#include "switch_proto.h"
#include "pc_control.h"
#include "nfc_mcu.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BTSTACK_HID";
static uint8_t hid_service_buffer[512];
static uint8_t device_id_service_buffer[100];
static uint16_t hid_cid;
static bool connected;
static bool send_pending;
#define REPLY_QUEUE_SIZE 8
static struct { uint8_t data[INPUT_REPORT_SIZE]; uint16_t len; } replies[REPLY_QUEUE_SIZE];
static unsigned reply_head, reply_count;
static btstack_timer_source_t pc_timer;

static uint32_t input_report_count;
static uint32_t last_rx_ms;
static uint8_t last_subcmd;
static bool stalled_logged;

static btstack_timer_source_t input_timer;
static btstack_packet_callback_registration_t hci_callback;

#define SWITCH_INPUT_INTERVAL_MS 24

uint8_t g_esp32_mac[6];

static const uint8_t hid_descriptor[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,
    0x85,0x21,0x06,0x01,0xFF,0x09,0x21,0x15,0x00,0x25,0x00,
    0x75,0x08,0x95,0x30,0x81,0x02,
    0x85,0x30,0x09,0x30,0x15,0x00,0x25,0x00,0x75,0x08,0x95,
    0x30,0x81,0x02,
    0x85,0x31,0x09,0x31,0x75,0x08,0x96,0x69,0x01,0x81,0x02,
    0x85,0x3F,0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,
    0x01,0x75,0x01,0x95,0x10,0x81,0x02,
    0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,0x75,0x04,0x95,
    0x01,0x81,0x42,0x75,0x04,0x95,0x01,0x81,0x03,
    0x09,0x30,0x09,0x31,0x09,0x33,0x09,0x34,0x15,0x00,0x27,
    0xFF,0xFF,0x00,0x00,0x75,0x10,0x95,0x04,0x81,0x02,
    0x85,0x01,0x06,0x01,0xFF,0x09,0x01,0x15,0x00,0x27,0xFF,
    0xFF,0x00,0x00,0x75,0x08,0x95,0x30,0x91,0x02,
    0x85,0x10,0x09,0x10,0x15,0x00,0x27,0xFF,0xFF,0x00,0x00,
    0x75,0x08,0x95,0x30,0x91,0x02,
    0x85,0x11,0x09,0x11,0x75,0x08,0x95,0x30,0x91,0x02,
    0xC0
};

/* All queue operations run in the BTstack run loop. */
static void send_next_report(void)
{
    uint8_t msg[INPUT_REPORT_0x31_SIZE + 1] = { 0xA1 };
    uint16_t len;
    bool is_reply = reply_count != 0;
    bool mcu_report = false;
    bool grip = false;
    send_pending = false;
    if (is_reply) {
        len = replies[reply_head].len;
        memcpy(&msg[1], replies[reply_head].data, len);
    } else {
        /* Diagnostic L+R pulse, then release; never leave buttons held. */
        input_report_set_virtual_grip(grip);
        stream_mode_t mode = switch_proto_get_stream_mode();
        if (mode == STREAM_MODE_0x3F) {
            input_report_build_0x3F(&msg[1], controller_state_get());
            len = 12;
        } else if (mode == STREAM_MODE_0x30) {
            /* Keep initial reports flowing before the host sends 0x03. */
            input_report_build_0x30(&msg[1], switch_proto_next_timer(),
                                    controller_state_get());
            len = INPUT_REPORT_SIZE;
        } else if (mode == STREAM_MODE_0x31) {
            input_report_build_0x30(&msg[1], switch_proto_next_timer(), controller_state_get());
            msg[1] = 0x31;
            nfc_peek(&msg[1 + INPUT_REPORT_SIZE]);
            len = INPUT_REPORT_0x31_SIZE;
            mcu_report = true;
        } else { return; }
    }
    int status = hid_device_send_interrupt_message(hid_cid, msg, len + 1);
    if (status != ERROR_CODE_SUCCESS) {
        ESP_LOGW(TAG, "TX failed ID=0x%02X len=%u status=%d; retry on timer",
                 msg[1], (unsigned)len, status);
        return; /* Preserve the head reply until L2CAP accepts it. */
    }
    if (mcu_report) {
        if (msg[50]!=0xff || input_report_count % 80 == 0) {
            nfc_diagnostics_t d=nfc_diagnostics();
            ESP_LOGI(TAG,"NFC TX type=%02X len=%u power=%u poll=%u rx=%u rejected=%u",
                     msg[50],(unsigned)len,d.power,d.poll,d.received,d.rejected);
        }
        nfc_commit();
    }
    if (is_reply) {
        ESP_LOGI(TAG, "TX reply ID=0x%02X len=%u status=%d subcmd=0x%02X",
                 msg[1], (unsigned)len, status, len >= 15 ? msg[15] : 0);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, len + 1, ESP_LOG_INFO);
        reply_head = (reply_head + 1) % REPLY_QUEUE_SIZE;
        reply_count--;
    } else {
        if (input_report_count % 80 == 0) {
            ESP_LOGI(TAG, "TX input ID=0x%02X len=%u status=%d L+R=%u count=%lu",
                     msg[1], (unsigned)len, status, (unsigned)grip,
                     (unsigned long)input_report_count);
        }
        input_report_count++;

    }
}

static void input_timer_handler(btstack_timer_source_t *ts)
{
    if (connected && !switch_proto_stream_enabled() && !stalled_logged &&
        (uint32_t)(btstack_run_loop_get_time_ms() - last_rx_ms) >= 10000) {
        ESP_LOGW(TAG, "No RX for 10s: last subcmd=0x%02X queue=%u inputs=%lu",
                 last_subcmd, reply_count, (unsigned long)input_report_count);
        stalled_logged = true;
    }
    if (connected && !send_pending) {
        if (input_report_count == 0 && switch_proto_stream_enabled()) {
            ESP_LOGI(TAG, "Timer de entrada activo; solicitando CAN_SEND_NOW");
        }
        send_pending = true;
        hid_device_request_can_send_now_event(hid_cid);
    }
    /*
     * The working PABotBase2 log reports HCI mode interval 24 ms and
     * changes its cooldown to 24000 us. Match that cadence instead of
     * sending at 15 ms, which can race the early subcommand replies.
     */
    btstack_run_loop_set_timer(ts, SWITCH_INPUT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void track_output(const uint8_t *packet, uint16_t len)
{
    last_rx_ms = btstack_run_loop_get_time_ms();
    stalled_logged = false;
    if (len >= 11 && packet[0] == 0x01) last_subcmd = packet[10];
    switch_proto_handle_output(packet, len);
}

static void report_data_callback(uint16_t cid, hid_report_type_t type,
                                 uint16_t report_id, int report_size,
                                 uint8_t *report)
{
    (void)cid; (void)type; (void)report_id;
    if (report && report_size > 0) {
        uint8_t packet[65];
        if (report_size > (int)sizeof(packet) - 1) report_size = sizeof(packet) - 1;
        packet[0] = (uint8_t)report_id;
        memcpy(&packet[1], report, (size_t)report_size);
        if (packet[0] == 0x01) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, packet, (size_t)report_size + 1,
                                     ESP_LOG_DEBUG);
        }
        track_output(packet, (uint16_t)report_size + 1);
    }
}

static void set_report_callback(uint16_t cid, hid_report_type_t type,
                                int report_size, uint8_t *report)
{
    (void)cid;
    (void)type;
    if (!report || report_size < 1) return;

    /*
     * Some hosts deliver output reports through HIDP SET_REPORT instead of
     * the interrupt data callback. The report ID is the first byte here.
     */
    uint8_t packet[65];
    if (report_size > (int)sizeof(packet)) report_size = sizeof(packet);
    memcpy(packet, report, (size_t)report_size);
    if (packet[0] == 0x01) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, packet, (size_t)report_size,
                                 ESP_LOG_DEBUG);
    }
    track_output(packet, (uint16_t)report_size);
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_CONNECTION_COMPLETE:
        ESP_LOGI(TAG, "Conexion ACL recibida, estado=%u",
                 hci_event_connection_complete_get_status(packet));
        return;
    case HCI_EVENT_PIN_CODE_REQUEST:
        ESP_LOGI(TAG, "Solicitud PIN recibida");
        return;
    case HCI_EVENT_LINK_KEY_REQUEST:
        ESP_LOGI(TAG, "Solicitud de link key recibida");
        return;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        ESP_LOGI(TAG, "Autenticacion terminada, estado=%u",
                 hci_event_authentication_complete_get_status(packet));
        return;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        ESP_LOGI(TAG, "ACL desconectada, estado=%u",
                 hci_event_disconnection_complete_get_status(packet));
        return;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t address;
        hci_event_user_confirmation_request_get_bd_addr(packet, address);
        gap_ssp_confirmation_response(address);
        ESP_LOGI(TAG, "Emparejamiento SSP aceptado automaticamente");
        return;
    }
    case GAP_EVENT_PAIRING_COMPLETE:
        ESP_LOGI(TAG, "Emparejamiento completado, estado=%u",
                 gap_event_pairing_complete_get_status(packet));
        return;
    case HCI_EVENT_HID_META:
        break;
    default:
        return;
    }
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
    case HID_SUBEVENT_CONNECTION_OPENED:
        if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) return;
        hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
        connected = true; send_pending = false;
        input_report_count = 0;
        reply_head = 0; reply_count = 0;
        input_report_set_virtual_grip(false);
        last_rx_ms = btstack_run_loop_get_time_ms();
        last_subcmd = 0; stalled_logged = false;
        ESP_LOGI(TAG, "Firmware NFC read v3 short reports: Pro Controller VID=057E PID=2009 source=USB");
        pc_control_connection(true);
        switch_proto_init();
        input_timer.process = input_timer_handler;
        btstack_run_loop_set_timer(&input_timer, SWITCH_INPUT_INTERVAL_MS);
        btstack_run_loop_add_timer(&input_timer);
        ESP_LOGI(TAG, "Switch conectado (BTstack HID)");
        break;
    case HID_SUBEVENT_CONNECTION_CLOSED:
        connected = false; send_pending = false; hid_cid = 0;
        input_report_count = 0;
        reply_head = 0; reply_count = 0;
        input_report_set_virtual_grip(false);
        btstack_run_loop_remove_timer(&input_timer);
        pc_control_connection(false);
        ESP_LOGI(TAG, "Switch desconectado");
        break;
    case HID_SUBEVENT_CAN_SEND_NOW:
        if (connected) {
            ESP_LOGV(TAG, "CAN_SEND_NOW recibido");
        }
        if (connected && send_pending) {
            send_next_report();
        }
        break;
    default:
        break;
    }
}

int btstack_main(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    memcpy(g_esp32_mac, mac, sizeof(mac));
    ESP_LOGI(TAG, "MAC BT usada en Device Info: %02X:%02X:%02X:%02X:%02X:%02X",
             g_esp32_mac[0], g_esp32_mac[1], g_esp32_mac[2],
             g_esp32_mac[3], g_esp32_mac[4], g_esp32_mac[5]);

    gap_discoverable_control(1);
    gap_set_class_of_device(0x002508);
    gap_set_local_name("Pro Controller");
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    l2cap_init();
    sdp_init();

    hid_sdp_record_t params = {
        0x08, 33, 0, 1, 1, 1, 0, 0xFFFF, 0xFFFF, 3200,
        hid_descriptor, sizeof(hid_descriptor), "Pro Controller"
    };
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    hid_create_sdp_record(hid_service_buffer, sdp_create_service_record_handle(), &params);
    ESP_LOGI(TAG, "Registro SDP HID: %u bytes",
             (unsigned int)de_get_len(hid_service_buffer));
    btstack_assert(de_get_len(hid_service_buffer) <= sizeof(hid_service_buffer));
    sdp_register_service(hid_service_buffer);
    memset(device_id_service_buffer, 0, sizeof(device_id_service_buffer));
    device_id_create_sdp_record(device_id_service_buffer,
        sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_USB,
        0x057E, 0x2009, 0x0100);
    btstack_assert(de_get_len(device_id_service_buffer) <= sizeof(device_id_service_buffer));
    sdp_register_service(device_id_service_buffer);

    hid_device_init(false, sizeof(hid_descriptor), hid_descriptor);
    /* Host output reports may omit trailing descriptor padding. */
    hid_device_accept_truncated_hid_reports(true);
    hci_callback.callback = packet_handler;
    hci_add_event_handler(&hci_callback);
    hid_device_register_packet_handler(packet_handler);
    hid_device_register_report_data_callback(report_data_callback);
    hid_device_register_set_report_callback(set_report_callback);
    hci_power_control(HCI_POWER_ON);
    return 0;
}

static void pc_timer_handler(btstack_timer_source_t *ts)
{
    pc_control_poll();
    btstack_run_loop_set_timer(ts, 10);
    btstack_run_loop_add_timer(ts);
}

esp_err_t btstack_hid_init(void)
{
    if (btstack_init() != ERROR_CODE_SUCCESS) return ESP_FAIL;
    if (btstack_main(0, NULL) != 0) return ESP_FAIL;
    esp_err_t err = pc_control_init();
    if (err != ESP_OK) return err;
    pc_timer.process = pc_timer_handler;
    btstack_run_loop_set_timer(&pc_timer, 10);
    btstack_run_loop_add_timer(&pc_timer);
    return ESP_OK;
}

void btstack_hid_send_input_report(const uint8_t *data, uint16_t len)
{
    if (!connected || !data || !len) return;
    if (len > INPUT_REPORT_SIZE || reply_count == REPLY_QUEUE_SIZE) {
        ESP_LOGE(TAG, "Reply rejected: len=%u queue=%u",
                 (unsigned)len, reply_count);
        return;
    }
    unsigned tail = (reply_head + reply_count) % REPLY_QUEUE_SIZE;
    memcpy(replies[tail].data, data, len);
    replies[tail].len = len;
    reply_count++;
    if (!send_pending) {
        send_pending = true;
        hid_device_request_can_send_now_event(hid_cid);
    }
}

#endif
