# MiniScheme Agent Development & Debugging Guide

This document is for AI agents and engineers developing, debugging and maintaining the MiniScheme ecosystem. It describes what the code does today; when the two disagree, the code wins and this guide must be fixed.

---

## 1. Repositories

The project is split into five repositories (github.com/chk-jxcn/&lt;name&gt;, branch `main`):

```
repos/
├── minischeme/          # C99 core: reader, compiler, bytecode VM, generational GC, bigints
├── minischeme-esp32/    # ESP-IDF 6.1 firmware: ILI9488 display, band rendering, XIP app ROM, HTTP API
├── minischeme-apps/     # The Scheme apps (calendar, weather, ...), applib, build tool, market index
├── minischeme-proxy/    # App proxy and protocol converter (Python, SQLite), public deployment
└── minischeme-webrepl/  # Web IDE, WASM simulator, Web Serial flasher, local server (tray exe)
```

Code that exists in more than one repository (update every copy and release each repository):

| Code | Copies |
|------|--------|
| C core | `minischeme/src`, `minischeme-esp32/components/minischeme/src` (plus the device/GUI sources), compiled into `minischeme-webrepl/wasm/minischeme_vm.wasm` |
| Proxy | `minischeme-proxy/{server.py,proxy/}` and `minischeme-webrepl/{server.py,proxy/}` (identical files) |
| App images | `minischeme-apps/dist`, `minischeme-esp32/main/app_*_msbc.h`, `minischeme-webrepl/apps` (`python tools/build_apps.py --sync`) |

---

## 2. Core VM and memory

### 2.1 Language standard and targets
- ISO C99, zero warnings under `-Wall -Wextra -pedantic` (the core's CI builds with `-Werror`, gcc, clang and `-m32`).
- Targets: MinGW, Linux, macOS, ESP32 (ESP-IDF 6.1) and `wasm32` (clang, no libc: `minischeme-webrepl/wasm/wasm_libc.c`).

### 2.2 Garbage collector (`minischeme_gc.c`)
Generational, two kinds of collection:
- **Minor**: the 4 KB Eden (nursery) is copied (Cheney) into the tenured heap when it fills.
- **Full**: the tenured heap is compacted in place (sliding mark-compact). There is no second semispace, so the whole heap is usable.
- A full collection runs when tenured use + the request + Eden exceeds 80 % of the tenured capacity.
- On the ESP32 the VM heap is measured at every boot once WiFi has joined: `min(free - reserved_heap, largest block - 8 KB)`, about **102 KB** with the default 64 KB `reserved_heap` (`/api/hardware` shows the boot's probe). The WASM simulator uses the same 4 KB Eden.
- Measured pauses on the board (240 MHz, 104 KB heap): minor 0.13-0.25 ms; full about 2 ms + 0.03 ms per KB of live data. Keep live data under about 60 % of the heap.

### 2.3 GC roots in C
Any allocation may move objects. A `Value` held in a C local across an allocation must be registered, and pointers re-derived from it afterwards:
```c
mini_push_root(vm, &my_val);
/* ... code that may allocate ... */
mini_pop_root(vm);
```
Pointers into an object (e.g. a bigint's digits) never survive an allocation.

### 2.4 Flash execution (XIP)
- App images (`.msbc`) are 8-byte aligned and run in place from the `approm` flash partition; bytecode, constants and symbols are not copied to RAM.

### 2.5 Stack traces
- `MiniConfig.enable_stack_trace` / `mini_set_stack_trace_enabled`: when on, a 1 KB buffer is allocated and the call stack is recorded at each error (`mini_get_error_trace`); when off nothing is allocated.
- The ESP32 has it off (`[debug] stack_trace = 0` in config.ini); the WASM simulator has it on (`wasm_set_stack_trace` toggles it).

---

## 3. MSBC images and the metadata trailer (`MSMD`)

Each app image ends with a trailer that the firmware, the WebREPL and the market read without running the app:

```
| MSBC bytecode (the compiled app)                          |
| UTF-8 meta.ini text ([app], [fields], [config])           |
| u32 LE: length of the meta.ini text                       |
| u32 LE: FNV-1a 32 of the meta.ini text                    |
| "MSMD"                                                    |
```

```ini
[app]
id = weather
name = 天气
version = 1.2
pages = 5
api = 1.0

[config]
; empty values: the system config.ini's value is used
proxy_url =
carousel =
```

```python
def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h
```

---

## 4. Talking to the device

### 4.1 Serial protocol (115200 baud)
Each request is one line starting with `@`, answered by `@OK {json}` or `@ERR {json}`:
`@INFO`, `@EXEC <base64>`, `@BEGIN <size> [path]` / `@DATA <base64>` / `@END run|save`, `@LS`, `@APPS`, `@READ <offset> <path>`, `@RM <path>`, `@MV <from> <to>`, `@RESET`.

### 4.2 HTTP API
`GET /api/info`, `POST /api/exec` (MSBC body), `POST /api/eval` (when the firmware has a compiler), `POST /api/upload?path=&run=`, `GET /api/fs/list`, `GET /api/fs/read?path=`, `GET /api/screen` (mirror rows), `GET /api/stats`, `GET|POST /api/settings`, `GET /api/apps`, `GET /api/hardware`, ...

**Password** (`config.ini [http] http_password`; when that is empty, `[server] hardware_password` written by the WebREPL setup is used). It guards the HTTP API only; the servers use their own app service passwords (6.1):
- Sent in the `X-MiniScheme-Auth` header (the WebREPL does this), or as `?auth=<percent-encoded password>` where a header is impossible (the mirror WebSocket, a link to the device page).
- With a password set every `/api/` route needs it, except `/api/app/call` and `/api/app/file` when called by the board's own `/app` page. That page is served as `/app?t=<token>` (a 302 from `/app`, random per boot, firmware 1.0.67+) and `appweb.js` sends the token back with every call: a sandboxed page has an opaque origin and browsers send no `Referer` from one, so the token is the only proof that the call comes from the board's page.
- Compared in constant time; a wrong or missing password answers `401` with `"auth_required": true`.
- `POST /api/settings` with `http.http_password` (or `server.hardware_password`) applies the new password at once.

**Browser origins** (CORS): only pages on this network (localhost, 10.x, 172.16-31.x, 192.168.x, *.local) and the published WebREPL (`https://chk-jxcn.github.io`) get CORS answers; other origins are refused with 403.

### 4.3 config.ini
- Keys are read per section: `[wifi] ssid / password` (or `wifi_ssid / wifi_password`) are the network's, `[http] password` (or `http_password`) the API's.
- `config_version = 3`; a file with an older or missing version is replaced by the firmware's defaults at boot (so tools that write config.ini must keep `config_version`).
- A key left empty in an app's own ini takes the system config.ini's value.

### 4.4 Screen mirror stream (TCP 8088)
- Raw TCP: `AUTH <password> [since=N]\n` then frames of `u32 LE length + MSCR rows`; commands `FULL`, `PING`, `QUIT`.
- WebSocket: `GET /?auth=<password>` (RFC 6455 binary frames).
- Only changed rows are sent; nothing is sent while the screen is still.

---

## 5. ESP32 flash layout and flashing

| Region | Offset | Size | Content |
|--------|--------|------|---------|
| Bootloader | `0x1000` | | `bootloader.bin` |
| Partition table | `0x8000` | | `partition-table.bin` |
| `factory` (app) | `0x10000` | 1476 KB | `minischeme_esp32.bin` (1403 KB, about 5% free; 1536 KB before 1.0.73) |
| `fontrom` | `0x181000` | 320 KB | system font ROM (512 KB at `0x190000` in 1.0.71..1.0.72; one font, simsun-16, uses 300 KB) |
| `approm` | `0x1d1000` | 512 KB | app images (XIP) in two 256 KB slots, written in turn: an app whose image is in a slot runs from it without writing flash; another is built into the slot the last app did not run from (RTC memory; slot 0 after a power-up), so the last two apps stay built; `/spiffs/.approm` records the slots, `/api/hardware` lists them (firmware 1.0.72) |
| `storage` | `0x251000` | 1724 KB | **LittleFS** (1472 KB at `0x290000` before 1.0.73), mounted at `/spiffs` (the name is historical; the partition subtype is still `spiffs`) |

- Firmware builds use **ESP-IDF 6.1** (locally and in CI, Docker image `espressif/idf:v6.1`).
- `firmware/fontrom.bin` (optional in the manifest, `0x181000`): the font ROM with simsun-16, all of GB2312; `[fonts] font_override` empty means `system_16:simsun-16` (built-in font when absent, `none` to keep it).
- `firmware/firmware-manifest.json` lists the three images with offset, size and SHA-256; the WebREPL flasher takes the manifest and all three images from one source and refuses files whose SHA-256 differs, and esptool-js verifies each region's MD5 after writing.
- The ILI9488 8080 bus runs on I2S DMA; the WR clock starts at `lcd_clock_hz` (20 MHz) and is verified at boot by reading pixels back, stepping down until the writes land.
- `[display] panel = ips | tft` (default ips): an IPS panel needs the driver's display inversion (INVON), a TFT one not. It is chosen on the configuration pages (the settings web page, the WebREPL setup; usually once when the firmware is flashed, not in the on-device menu), set before the panel init and changed at once from the settings page or `(config-set! "display" "panel" ...)`; `/api/hardware` reports it as `display.panel`.
- `[display] lcd_gamma = driver | chip | custom` (default driver since 1.0.63; 1.0.60-1.0.62 defaulted to chip): the panel gamma (E0/E1) at boot. driver writes the driver's tables (ILI9488: ILI9488_Init.h; ST7796: TFT_eSPI's, matched to its power and VCOM settings; with chip's curve the ST7796 lost mid-tone green and looked blue), chip leaves the controller's own curve after reset, custom writes the driver's tables with `lcd_gamma_pos` / `lcd_gamma_neg` (hex bytes) over them from the first byte, so a partial list tunes the first values only. The config.ini template lists both controllers' tables; the boot log prints the tables written (`[ILI9488 DIAG] ST7796 gamma driver E0: ...`). `lcd_vcom` replaces the driver's VCOM (C5) bytes. The ST7796 has its own init sequence (`st7796_init`). Read at boot only.
- The settings web page `main/setup_page.html` is gzip-embedded at build time (`tools/embed_page.py`, run by main/CMakeLists.txt into the build folder); no header is committed, so the page in the firmware is always the one in the repository.

---

## 6. App proxy and protocol converter (minischeme-proxy)

### 6.1 Devices and their servers (`minischeme_channel.h`, firmware 1.0.69+)
- **Plain HTTP, headers**: every request to a configured server (`[app] proxy`, `https_proxy`, `[server] proxy_url`, `converter_url`, an app's own `proxy_url` / `proxy` / `converter_url`) carries `X-MiniScheme-Device` (the WiFi MAC, 12 uppercase hex digits; the simulator: its `SIM-...` id) and, when that server has an app service password (`<key>_password`), `X-MiniScheme-Password` (base64 of it, UTF-8). No TLS and no encryption: the user chose RAM over secrecy (2026-10-23); base64 is no encryption, so the password must be used nowhere else.
- **Registration (minischeme-proxy `proxy/device_auth.py`)**: the first request with a password registers the device (the server keeps `HKDF-SHA256(salt = MAC, ikm = password, info = "msc1_hw_hash")`, never the password); later requests need the same password or get 403; 20 wrong passwords from one address in 15 minutes: refused for 15 minutes. A simulator has no password. Without a password the id is only a hint: a public server never treats it as proof (the medicine records need the password there).
- **App service passwords (1.0.65)** are shown in plain text on the configuration pages and can be changed any time (the server refuses a password other than the one it registered). The hardware password (`[server] hardware_password` / `[http] http_password`) only guards the board's web page and HTTP API. Boot migration: a server value without `<key>_password` gets the hardware password written once.
- **Test**: `POST /api/channel/test` (`url=`, `password=`) asks `GET <server>/device/check` with the headers, registering nothing: `ok`, `new` (its first request registers it), `password`, `plain` (no password), `old` (a server before proxy 1.0.50), `unreachable`, `error`. `/api/settings` (`channel`) lists each server with `auth` (password, plain, relay) and the last request's `state` (unknown, ok, refused = 403, failed).
- **Routing (`mini_fetch_open`)**: an `https://` URL goes to the https converter (`https_proxy`, else `proxy`) as `<it>/fetch?url=`; an app server without a password and `<key>_relay = 1` as `<converter>/relay?url=` (the converter passes the device id on); any other URL to its own server.
- **Old keys**: firmware up to 1.0.68 kept a channel key in the value (`proxy = <key>@http://...`); 1.0.69 drops it from config.ini at boot. The server still answers the encrypted channel (`/channel`, `/channel/auth`, MSC1: AES-256-GCM, per-request HKDF keys) for those boards.

### 6.2 Plain endpoints and safety rules
- `/app/<name>` (weather, news, stocks, crypto, futures, calendar, usage), `/market/<file>` (only files inside `apps/`), `/fetch?url=` (internet hosts only, every redirect re-checked; a local server also reaches its own network for clients on that network).
- Market data is real or absent: e.g. `/app/crypto` takes the first of OKX, Binance (`data-api.binance.vision`, reachable from mainland China) and Huobi that answers (a failing source is skipped for 5 minutes) and answers 502 (the device keeps its last data) when none does; it never returns invented prices.
- CORS answers go only to the WebREPL, this network's pages and the server itself.
- `X-Forwarded-For` is believed only from a reverse proxy on the same machine.
- Admin portal: password from `config.ini [security] admin_password` (or `MINISCHEME_ADMIN_PASSWORD`); a public server never runs without one; a local server without one lets only this machine's own browser in.
- Logs and the access log mask secret query values (`token`, `key`, `auth`, ...).

### 6.3 SQLite
- `devices` (`mac`, `hardware_hash_key`, `master_key_2`, `created_at`, `last_seen_at`, `last_ip`, `status`), `app_data`, `access_logs`, `med_names` and `med_records` (the medicine app's records: `proxy/medicine.py`, `/app/medicine`). The database file is created with mode 0600.

---

## 7. Quick reference

```bash
# core: build, generate the MSBC test images, run the three suites
cd repos/minischeme && make test            # MinGW: mingw32-make SHELL=sh CC=gcc test

# firmware (ESP-IDF 6.1 environment)
cd repos/minischeme-esp32 && idf.py build
# the board's HTTP access rules against a real board (the password from its config.ini, never printed)
MINISCHEME_BOARD=192.168.2.119 MINISCHEME_PASSWORD=... python -m unittest tests/test_board_http.py -v
python tools/make_manifest.py <dir with the 3 images> <version> 6.1

# apps: compile to dist/, update index.json; --sync also updates the firmware and WebREPL copies
cd repos/minischeme-apps && python tools/build_apps.py --sync
cd repos/minischeme-apps && python -m unittest discover -s tests -v   # pages, metadata, stale images (CI too)

# proxy
cd repos/minischeme-proxy && python app_proxy.py --mode local --port 8087 && python -m pytest tests

# WebREPL with the local server (proxy built in)
cd repos/minischeme-webrepl && python server.py --port 8087 && python -m pytest tests
# (tests/test_app_medicine.py and test_sim_cli.py drive the simulator: they need Edge / Chrome)

# the simulator from the command line (headless browser): run code and apps, press KEY 18, screenshot
cd repos/minischeme-webrepl && python tools/sim.py --app calendar --press short --wait 2 --shot month.png
```

### Command-line simulator (`tools/sim.py`, minischeme-webrepl)

For agents and scripts: the real WebREPL page (same WASM VM, apps, local proxy, display
and button code as in the browser) in a headless Edge / Chrome / Chromium, driven from the
command line. It starts its own `server.py` on a free port (or `--url` a running one); the
simulator's storage (installed apps, `config.ini`) persists in `~/.minischeme-sim`
(`--profile DIR`, `--fresh` for an empty one). Commands run in the order given:

```bash
cd repos/minischeme-webrepl
python tools/sim.py --eval "(+ 1 2)" --eval "(api-version)"              # prints 3, "1.6.0"
python tools/sim.py --app weather --wait 5 --shot weather.png            # a bundled app (apps/<id>.msbc)
python tools/sim.py --app ../minischeme-apps/apps/calendar.msbc --press short --wait 2 --shot month.png
python tools/sim.py --app my_app.scm --wait 3 --shot my.png --log        # Scheme source, compiled in the page
python tools/sim.py --config app.carousel=0 --app calendar               # (config-set! "app" "carousel" "0")
python tools/sim.py --js "LOCAL_SERVER_BASE"                             # any JavaScript in the page
printf 'eval (* 6 7)\nshot a.png\nquit\n' | python tools/sim.py --repl  # one command per line
```

`--press short|long|mid|hold|<seconds>` is KEY 18 (0.15 / 1.2 / 4 / 8.5 s), `--wait S` lets
the device run (feeds, carousel, clock), `--shot` saves the 320x480 (or 480x320) LCD as PNG,
`--log` prints the WebREPL terminal lines since the last `--log`. An `--eval` error or a
missing app exits with status 1. `--show` opens a visible window. Data requests go through
the started server like the web page's (its `/api/fetch`), carrying the simulator's
`SIM-...` id in `X-MiniScheme-Device`.


### Rules for agents
1. All code, identifiers, comments and commit messages in English (conventional commits).
2. Zero compiler warnings (C99, `-Wall -Wextra`).
3. No fake data and no fake progress: flashing, compiling and market data must be real.
4. Never commit credentials, tokens, keys, passwords or personal addresses.
5. Releases: commit, push `main`, then tag `v1.0.X` and push the tag (CI publishes the Release only from a tag).
