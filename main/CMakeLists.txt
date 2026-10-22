idf_component_register(
    SRCS
        "main.c"
        "setup_portal.c"
        "market_host.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        minischeme
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_uart
        esp_adc
        driver
        esp_timer
        esp_rom
        freertos
        nvs_flash
        esp_system
        lfsvfs
        esp_wifi
        esp_http_server
        esp_http_client
        esp_lcd
        app_update
        bootloader_support
        spi_flash
        esp_partition
        esp_netif
        esp_event
        lwip
        soc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fno-tree-loop-distribute-patterns)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINISCHEME_NO_HELP)

# The settings web page, gzip-compressed into setup_page_html.h in the build
# folder whenever setup_page.html changes (never a stale committed copy)
set(SETUP_PAGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/setup_page.html)
set(SETUP_PAGE_HDR ${CMAKE_CURRENT_BINARY_DIR}/setup_page_html.h)
set(SETUP_PAGE_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_page.py)
add_custom_command(OUTPUT ${SETUP_PAGE_HDR}
    COMMAND ${PYTHON} ${SETUP_PAGE_TOOL} ${SETUP_PAGE_SRC} ${SETUP_PAGE_HDR}
    DEPENDS ${SETUP_PAGE_SRC} ${SETUP_PAGE_TOOL}
    VERBATIM)
add_custom_target(setup_page_header DEPENDS ${SETUP_PAGE_HDR})
add_dependencies(${COMPONENT_LIB} setup_page_header)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
