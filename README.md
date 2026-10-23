# MiniScheme ESP32 Firmware

MiniScheme ESP32 runs the MiniScheme bytecode VM on an ESP32-D0WDQ6 driving a 3.5" ILI9488 320x480 display (8080 8-bit parallel bus), with band rendering, hardware peripherals, a web page and an HTTP API for remote management.

## Hardware
- **Microcontroller**: ESP32-D0WDQ6 (240 MHz dual core, 320 KB SRAM, 4 MB flash).
- **Display**: ILI9488 3.5" 320x480 on the 8080 bus through I2S DMA (GPIO bit-bang as a fallback). The WR clock starts at 20 MHz (`lcd_clock_hz`) and is verified at boot by reading pixels back; it is lowered only when the writes do not land.
- **Peripherals**: GPIO 22 backlight (LEDC PWM), GPIO 35 light sensor (ADC1 channel 7), GPIO 21 WS2812 LED, GPIO 18 button (short, long, mid press and hold).
- **Storage**: a LittleFS partition of 1472 KB mounted at `/spiffs` (the mount point keeps its old name) for apps, configuration and snapshots.

## Features
- **Band rendering**: the screen is drawn 13 rows at a time (8.3 KB) instead of a 300 KB frame buffer.
- **Measured VM heap**: after WiFi joins, the firmware measures free RAM and gives the VM `min(free - reserved_heap, largest block - 4 KB)`, about 116 KB (since 1.0.70: `reserved_heap` 60 KB, half-band LCD DMA transfers, 2 static WiFi rx buffers, trimmed task stacks).
- **Apps from flash**: MSBC images run in place from the `approm` partition.
- **HTTP API with a password**: `config.ini [http] http_password` (or the WebREPL's `[server] hardware_password`), sent in the `X-MiniScheme-Auth` header or as `?auth=` (percent-encoded); every API route needs it once set, compared in constant time. Browser pages may call the API only from this network or from the published WebREPL.
- **Plain HTTP to app servers** (since 1.0.69, no TLS and no encryption: it saves RAM and about 15 KB of code): each request carries the device id (`X-MiniScheme-Device`, the WiFi MAC) and the app service password set for that server (`X-MiniScheme-Password`, base64, not encrypted) for [minischeme-proxy](https://github.com/chk-jxcn/minischeme-proxy) to tell devices apart. Keys of the old encrypted channel in config.ini values (`<key>@http://...`) are dropped at boot.
- **Screen mirror**: changed rows streamed on TCP port 8088 (raw TCP or WebSocket).
- **Stack traces**: off on the board (`[debug] stack_trace = 0`, no memory used); the WebREPL simulator has them on.

## Building and flashing

### Prerequisites
- **ESP-IDF 6.1** (the CI uses the `espressif/idf:v6.1` image too)
- Python 3.10+

```bash
idf.py build
idf.py -p COM42 -b 460800 flash
```
or with esptool:
```bash
python -m esptool --chip esp32 -p COM42 -b 460800 --before default_reset --after hard_reset write_flash \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/minischeme_esp32.bin
```

### Prebuilt images
`firmware/` holds the images of the latest release (built with IDF 6.1) and `firmware-manifest.json` with each file's offset, size and SHA-256 (`tools/make_manifest.py`). The GitHub Release of each tag carries the same files. The WebREPL flasher checks every file against the manifest before writing.

### System font (Chinese text without generating fonts)
`firmware/fontrom.bin` is a ready font ROM image with **simsun-16** (Song 16 px, all of GB2312: levels 1 and 2, 7539 glyphs, 300 KB). Write it once to the `fontrom` partition; the WebREPL flasher does it by default ("写入系统字库"):
```bash
python -m esptool --chip esp32 -p COM42 write-flash 0x181000 firmware/fontrom.bin
```
It replaces the whole font ROM (fonts installed earlier with the font manager are removed; add them again afterwards). With `[fonts] font_override` empty the firmware draws all 16 px system and app text with simsun-16 when the font ROM has it, else with the built-in font (~700 characters); `font_override = none` keeps the built-in font. The image is made with `tools/make_fontrom.py` from `minischeme-webrepl/fonts/simsun-16.mfnt`.

## Configuration (`/spiffs/config.ini`)
The firmware writes a commented default file on first boot; the WebREPL's setup dialog and the device web page edit it. Main keys:
```ini
config_version = 3          ; keep it: an older or missing version is replaced by the defaults

[system]
boot_app =                  ; empty: the built-in settings

[wifi]
wifi_ssid = your-wifi-ssid
wifi_password = your-wifi-password

[http]
http_password =             ; the web page / HTTP API password (empty: open)

[app]
proxy =                     ; the app proxy, e.g. http://<pc-ip>:8087
https_proxy =               ; protocol converter for https:// data (empty: proxy)
carousel = 1                ; apps turn their pages by themselves (an app's own ini may override)

[debug]
stack_trace = 0
```

## Documentation
- [API overview](docs/api/README.md), [applib](docs/api/applib.md), [API 1.7.0](docs/api/v1.7.0.md)
- [Development guide](AGENT_DEVELOPMENT_GUIDE.md)

## License
MIT License
