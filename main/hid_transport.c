#include "sdkconfig.h"
#include "hid_transport.h"

#if CONFIG_SWITCH_CONTROLLER_BLE_HID
#include "ble_hid.h"
#elif CONFIG_SWITCH_CONTROLLER_BTSTACK
#include "btstack_hid.h"
#else
#include "bt_hid.h"
#endif

void hid_transport_send_input_report(const uint8_t *data, uint16_t len)
{
#if CONFIG_SWITCH_CONTROLLER_BLE_HID
    ble_hid_send_input_report(data, len);
#elif CONFIG_SWITCH_CONTROLLER_BTSTACK
    btstack_hid_send_input_report(data, len);
#else
    bt_hid_send_input_report(data, len);
#endif
}
