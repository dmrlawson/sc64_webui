cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

# The TinyUSB shipped with pico-sdk 2.2.0 polls bulk endpoints only once per
# millisecond in host mode (~60 kB/s), so a newer one is used as a submodule.
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb)
if (NOT EXISTS ${PICO_TINYUSB_PATH}/src/tusb.h)
    message(FATAL_ERROR "lib/tinyusb is missing - run: git submodule update --init")
endif()

include(pico_sdk_import.cmake)
project(sc64_webui C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Embed the web page (gzip compressed) as a C array.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(INDEX_C ${CMAKE_CURRENT_BINARY_DIR}/index_html.c)
add_custom_command(
    OUTPUT ${INDEX_C}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/web/embed.py
            ${CMAKE_CURRENT_LIST_DIR}/web/index.html ${INDEX_C} index_html_gz
    DEPENDS web/index.html web/embed.py
)

add_executable(sc64_webui
    src/main.c
    src/config.c
    src/http.c
    src/log.c
    src/net_lwip.c
    src/sc64.c
    src/storage.c
    src/transport_tusb.c
    src/update.c
    lib/fatfs/ff.c
    lib/fatfs/ffunicode.c
    lib/dhcpserver/dhcpserver.c
    ${INDEX_C}
)

target_include_directories(sc64_webui PRIVATE src lib/fatfs lib/dhcpserver)

# Set by the release workflow; shown in the page footer and the device log.
set(WEBUI_VERSION "dev" CACHE STRING "Firmware version")
pico_set_program_version(sc64_webui "${WEBUI_VERSION}")

target_compile_definitions(sc64_webui PRIVATE CYW43_HOST_NAME="sc64" WEBUI_VERSION="${WEBUI_VERSION}")
target_compile_options(sc64_webui PRIVATE -Wall -Wextra)
set_source_files_properties(lib/fatfs/ff.c lib/dhcpserver/dhcpserver.c PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(sc64_webui
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
    tinyusb_host
    hardware_flash
    hardware_watchdog
)

# The USB port is busy being a host, so logs go to the UART (GP0/GP1) and to
# the /api/log endpoint.
pico_enable_stdio_usb(sc64_webui 0)
pico_enable_stdio_uart(sc64_webui 1)
pico_add_extra_outputs(sc64_webui)
