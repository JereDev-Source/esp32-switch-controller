set(SWITCH_CONTROLLER_SRCS
        "main.c"
        "controller_state.c"
        "input_report.c"
        "spi_flash.c"
        "switch_proto.c"
        "nfc_mcu.c"
        "hid_transport.c"
)

if(CONFIG_SWITCH_CONTROLLER_BLE_HID)
    list(APPEND SWITCH_CONTROLLER_SRCS
        "ble_hid.c"
        "esp_hid_gap.c"
    )
elseif(CONFIG_SWITCH_CONTROLLER_BTSTACK)
    list(APPEND SWITCH_CONTROLLER_SRCS "btstack_hid.c" "pc_control.c")
else()
    list(APPEND SWITCH_CONTROLLER_SRCS "bt_hid.c")
endif()

idf_component_register(
    SRCS ${SWITCH_CONTROLLER_SRCS}
    INCLUDE_DIRS
        "."
        "include"
    PRIV_REQUIRES
        bt
        btstack
        esp_hid
        json
        esp_driver_uart
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "../components/btstack/src"
    "../components/btstack/src/classic"
    "../components/btstack/platform/freertos"
    "../components/btstack/port/include"
)
