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
        mbedtls
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
