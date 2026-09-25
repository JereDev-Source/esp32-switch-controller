# Needed to include the lwIP's HTTP app, which is not included by default.
set(IDF_PATH $ENV{IDF_PATH})
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_LIST_DIR}/cmake")
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)

if( NOT DEFINED BTSTACK_ROOT )
    if( NOT DEFINED ENV{BTSTACK_ROOT} )
        set(BTSTACK_ROOT ${CMAKE_CURRENT_LIST_DIR})
    else()
        set(BTSTACK_ROOT $ENV{BTSTACK_ROOT})
    endif()
    set(BTSTACK_ROOT "${BTSTACK_ROOT}" PARENT_SCOPE)
endif()
message( "BTstack root: \"${BTSTACK_ROOT}\"" )

if (CMAKE_HOST_WIN32)
    string(REPLACE "\\" "/" IDF_PATH $ENV{IDF_PATH})
endif()

set(include_dirs
    ${BTSTACK_ROOT}/chipset/zephyr
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/src/ble/gatt-service
    ${BTSTACK_ROOT}/src/ble
    ${BTSTACK_ROOT}/src/le-audio
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT}/platform/freertos
    ${BTSTACK_ROOT}/platform/lwip
    ${BTSTACK_ROOT}/port/include
    ${CMAKE_CURRENT_LIST_DIR}/../btstack_config/include
    ${IDF_PATH}/components/lwip/lwip/src/include)

set(src_dirs
    ${BTSTACK_ROOT}/src/classic
    ${BTSTACK_ROOT}/src/ble/gatt-service
    ${BTSTACK_ROOT}/chipset/zephyr
    ${BTSTACK_ROOT}/src/ble
    ${BTSTACK_ROOT}/src/le-audio
    ${BTSTACK_ROOT}/src/mesh
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT}/platform/freertos
    ${BTSTACK_ROOT}/platform/lwip
    )

set( exclude_srcs
    ${BTSTACK_ROOT}/platform/embedded/btstack_audio_embedded.c
    ${BTSTACK_ROOT}/platform/embedded/btstack_run_loop_embedded.c
    ${BTSTACK_ROOT}/platform/embedded/btstack_stdin_embedded.c
    ${BTSTACK_ROOT}/platform/embedded/btstack_tlv_flash_bank.c
    ${BTSTACK_ROOT}/platform/embedded/hci_dump_segger_rtt_binary.c
    ${BTSTACK_ROOT}/platform/embedded/hci_dump_segger_rtt_stdout.c
)

set( srcs port/btstack_port_esp32.c port/btstack_stdio_esp32.c port/btstack_tlv_esp32.c )

if(CONFIG_BTSTACK_AUDIO)
    if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.0")
        list(APPEND srcs "port/btstack_audio_esp32_v5.c")
    else()
        list(APPEND srcs "port/btstack_audio_esp32_v4.c")
    endif()
endif()

if(CONFIG_BTSTACK_INTERNAL_BT)
    list(APPEND srcs "port/hci_transport_esp32_vhci.c")
endif()

 

if(CONFIG_BTSTACK_HCI_DUMP_STDOUT)
    list( APPEND srcs
        ${BTSTACK_ROOT}/platform/embedded/hci_dump_embedded_stdout.c
        ${BTSTACK_ROOT}/src/btstack_audio_generator.c
        ${BTSTACK_ROOT}/src/btstack_audio_song.c
        ${BTSTACK_ROOT}/src/btstack_audio_player.c
        ${BTSTACK_ROOT}/src/btstack_lc3_google.c
        ${BTSTACK_ROOT}/src/classic/pbap_client.c)
endif()

set(priv_include_dirs)

if("${IDF_VERSION_MAJOR}" VERSION_GREATER_EQUAL "5")
    list(APPEND priv_include_dirs
        ${IDF_PATH}/components/bt/common/tinycrypt/include
    )
endif()

set(driver_requires)

if(IDF_VERSION_MAJOR GREATER 5 OR
   (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
    list(APPEND driver_requires
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2s
        esp_driver_usb_serial_jtag
    )
else()
    list(APPEND driver_requires driver)
endif()

set(priv_requires
        "nvs_flash"
        "bt"
        "lwip"
        "vfs"
        ${driver_requires}
        )

if(NOT CONFIG_SWITCH_CONTROLLER_BTSTACK)
    set(src_dirs)
    set(srcs)
endif()

idf_component_register(
    SRC_DIRS "${src_dirs}"
    EXCLUDE_SRCS "${exclude_srcs}"
    INCLUDE_DIRS "${include_dirs}"
    PRIV_INCLUDE_DIRS "${priv_include_dirs}"
    PRIV_REQUIRES "${priv_requires}"
    REQUIRES btstack_config
)

target_sources(${COMPONENT_LIB} PRIVATE ${srcs})
