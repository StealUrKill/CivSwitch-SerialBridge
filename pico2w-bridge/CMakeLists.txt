cmake_minimum_required(VERSION 3.13)

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Raspberry Pi Pico SDK checkout")
endif ()

set(PICO_BOARD pico2_w CACHE STRING "Target board")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(civswitch_serial_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Add receive diagnostics to a local copy of the CYW43 driver.
set(CYW43_LL_ORIGINAL "${PICO_CYW43_DRIVER_PATH}/src/cyw43_ll.c")
file(READ "${CYW43_LL_ORIGINAL}" CYW43_LL_SOURCE)
set(CYW43_RX_READ_MARKER "        int ret = cyw43_ll_sdpcm_poll_device(self, &len, &buf);\n        if (ret == -1)")
set(CYW43_RX_READ_PATCH "#if CIVSWITCH_DIAGNOSTIC_BOOT\n        static unsigned int diag_reads;\n        bool diag_print = diag_reads++ < 40;\n        if (diag_print) printf(\"[RX] Reading packet %u\\n\", diag_reads);\n#endif\n        int ret = cyw43_ll_sdpcm_poll_device(self, &len, &buf);\n#if CIVSWITCH_DIAGNOSTIC_BOOT\n        if (diag_print) printf(\"[RX] Read returned %d\\n\", ret);\n#endif\n        if (ret == -1)")
string(FIND "${CYW43_LL_SOURCE}" "${CYW43_RX_READ_MARKER}" CYW43_RX_READ_POSITION)
if (CYW43_RX_READ_POSITION EQUAL -1)
    message(FATAL_ERROR "Cannot instrument CYW43 receive read: SDK source changed")
endif()
string(REPLACE "${CYW43_RX_READ_MARKER}" "${CYW43_RX_READ_PATCH}" CYW43_LL_SOURCE "${CYW43_LL_SOURCE}")
set(CYW43_LL_LOCAL "${CMAKE_CURRENT_BINARY_DIR}/cyw43_ll_bounded.c")
file(WRITE "${CYW43_LL_LOCAL}" "${CYW43_LL_SOURCE}")
get_target_property(CYW43_DRIVER_SOURCES cyw43_driver INTERFACE_SOURCES)
list(REMOVE_ITEM CYW43_DRIVER_SOURCES "${CYW43_LL_ORIGINAL}")
list(APPEND CYW43_DRIVER_SOURCES "${CYW43_LL_LOCAL}")
set_property(TARGET cyw43_driver PROPERTY INTERFACE_SOURCES "${CYW43_DRIVER_SOURCES}")

add_executable(civswitch_serial_bridge
    main.c
    dhcpserver/dhcpserver.c
)

target_include_directories(civswitch_serial_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/dhcpserver
)

target_compile_definitions(civswitch_serial_bridge PRIVATE
    PICO_STACK_SIZE=0x1000
)

target_link_options(civswitch_serial_bridge PRIVATE
    "LINKER:--wrap=cyw43_cb_process_ethernet"
    "LINKER:--wrap=cyw43_cb_process_async_event"
    "LINKER:--wrap=cyw43_cb_tcpip_set_link_up"
)

target_link_libraries(civswitch_serial_bridge
    pico_stdlib
    pico_cyw43_arch_lwip_poll
)

pico_enable_stdio_usb(civswitch_serial_bridge 1)
pico_enable_stdio_uart(civswitch_serial_bridge 0)
pico_add_extra_outputs(civswitch_serial_bridge)

# Wait for USB serial before the diagnostic firmware starts.
add_executable(civswitch_serial_bridge_diagnostic main.c dhcpserver/dhcpserver.c)
target_include_directories(civswitch_serial_bridge_diagnostic PRIVATE
    ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/dhcpserver)
target_compile_definitions(civswitch_serial_bridge_diagnostic PRIVATE
    PICO_STACK_SIZE=0x1000 CIVSWITCH_DIAGNOSTIC_BOOT=1)
target_link_options(civswitch_serial_bridge_diagnostic PRIVATE
    "LINKER:--wrap=cyw43_cb_process_ethernet"
    "LINKER:--wrap=cyw43_cb_process_async_event"
    "LINKER:--wrap=cyw43_cb_tcpip_set_link_up"
    "LINKER:--wrap=cyw43_ll_wifi_ap_init"
    "LINKER:--wrap=cyw43_ll_wifi_ap_set_up"
    "LINKER:--wrap=cyw43_cb_tcpip_init"
    "LINKER:--wrap=cyw43_send_ethernet"
    "LINKER:--wrap=cyw43_ll_has_work"
    "LINKER:--wrap=cyw43_ll_process_packets")
target_link_libraries(civswitch_serial_bridge_diagnostic pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(civswitch_serial_bridge_diagnostic 1)
pico_enable_stdio_uart(civswitch_serial_bridge_diagnostic 0)
pico_add_extra_outputs(civswitch_serial_bridge_diagnostic)
