# MiniScheme ESP32 Firmware

MiniScheme ESP32 is a complete firmware environment running the MiniScheme bytecode virtual machine on ESP32-D0WDQ6 microcontrollers, driving a 3.5" ILI9488 8080-8bit parallel display (320x480 resolution) with band rendering, hardware peripherals, and remote management.

## Hardware Support
- **Microcontroller**: ESP32-D0WDQ6 (240 MHz dual-core, 320 KB SRAM, 4 MB SPI Flash)
- **Display**: ILI9488 3.5-inch 320x480 LCD (8-bit 8080 parallel bus via I2S DMA or GPIO bit-bang)
- **Peripherals**:
  - GPIO 22: Backlight brightness control (LEDC PWM)
  - GPIO 35: Ambient light sensor (ADC1 Channel 7)
  - GPIO 21: WS2812 RGB LED (single-wire bit-bang)
  - GPIO 18: User button (short press, long press, mid press, hold gestures)
- **Storage**: LittleFS partition (`/spiffs`, 1.4 MB) for apps, configs, and snapshots.

## Key Features
- **Band Rendering Pipeline**: 13 to 20 lines slice buffer, requiring only 8 KB to 25 KB RAM instead of 76 KB to 300 KB for full framebuffers.
- **Adaptive Memory Probing**: Measures available physical heap after WiFi/lwIP initialization and sizes the Scheme VM heap (typically 102 KB) safely.
- **Password-Protected HTTP API**: Optional password authentication via `X-MiniScheme-Auth` header or `?token=` query parameter using lightweight string matching.
- **Encrypted Channel Client**: Native AES-256-GCM encrypted transport to proxy/protocol converter without mbedTLS heap overhead.
- **Dynamic Application Loader**: Executes MSBC bytecode images directly from Flash ROM.

## Building & Flashing

### Prerequisites
- ESP-IDF v5.x or v6.x
- Python 3.10+

### Build Firmware
```bash
idf.py build
```

### Flash to ESP32
```bash
idf.py -p COM42 -b 460800 flash
```
Or via `esptool`:
```bash
python -m esptool --chip esp32 -p COM42 -b 460800 --before default_reset --after hard_reset write_flash \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/minischeme_esp32.bin
```

## Documentation
- [API Overview](docs/api/README.md)
- [Application Library (applib)](docs/api/applib.md)
- [API Specification v1.5.0](docs/api/v1.5.0.md)
- [Agent & Developer Guidelines](AGENTS.md)

## Configuration Template (`config.ini`)
```ini
[system]
device_name = MiniScheme-Card
boot_app = apps/calendar/app.msbc

[wifi]
wifi_ssid = your-wifi-ssid
wifi_password = your-wifi-password

[http]
password = your-secure-http-password

[app]
proxy = http://proxy.local:8087
https_proxy = http://proxy.local:8087
```

## License
MIT License
