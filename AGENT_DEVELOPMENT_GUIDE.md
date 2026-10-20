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

**Password** (`config.ini [http] http_password`; when that is empty, `[server] hardware_password` written by the WebREPL setup is used):
- Sent in the `X-MiniScheme-Auth` header (the WebREPL does this), or as `?auth=<percent-encoded password>` where a header is impossible (the mirror WebSocket, a link to the device page).
- With a password set every `/api/` route needs it, except `/api/app/call` and `/api/app/file` when called by the board's own `/app` page.
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
| `factory` (app) | `0x10000` | 1536 KB | `minischeme_esp32.bin` |
| `fontrom` | `0x190000` | 768 KB | system font ROM |
| `approm` | `0x250000` | 256 KB | the running app's image (XIP) |
| `storage` | `0x290000` | 1472 KB | **LittleFS**, mounted at `/spiffs` (the name is historical; the partition subtype is still `spiffs`) |

- Firmware builds use **ESP-IDF 6.1** (locally and in CI, Docker image `espressif/idf:v6.1`).
- `firmware/fontrom.bin` (optional in the manifest, `0x190000`): the font ROM with simsun-16, all of GB2312; `[fonts] font_override` empty means `system_16:simsun-16` (built-in font when absent, `none` to keep it).
- `firmware/firmware-manifest.json` lists the three images with offset, size and SHA-256; the WebREPL flasher takes the manifest and all three images from one source and refuses files whose SHA-256 differs, and esptool-js verifies each region's MD5 after writing.
- The ILI9488 8080 bus runs on I2S DMA; the WR clock starts at `lcd_clock_hz` (20 MHz) and is verified at boot by reading pixels back, stepping down until the writes land.
- `[display] panel = ips | tft` (default ips): an IPS panel needs the driver's display inversion (INVON), a TFT one not. It is chosen on the configuration pages (the settings web page, the WebREPL setup; usually once when the firmware is flashed, not in the on-device menu), set before the panel init and changed at once from the settings page or `(config-set! "display" "panel" ...)`; `/api/hardware` reports it as `display.panel`.
- The settings web page `main/setup_page.html` is gzip-embedded at build time (`tools/embed_page.py`, run by main/CMakeLists.txt into the build folder); no header is committed, so the page in the firmware is always the one in the repository.

---

## 6. App proxy and protocol converter (minischeme-proxy)

### 6.1 Encrypted channel (MSC1, `minischeme_channel.h`)
- The firmware registers itself. Its id is the WiFi MAC (12 uppercase hex digits); its hardware hash key is `HKDF-SHA256(salt = MAC, ikm = [server] hardware_password, info = "msc1_hw_hash")`, computed on the device (the password is never sent). The first request to a server without a key posts `{"mac", "hardware_hash_key"}` to `<server>/channel/auth`: a new device gets `master_key_2 = HKDF(hardware hash key, master_key_1, MAC)`; a known one (e.g. after a factory reset) gets a challenge, answers `HMAC-SHA256(hash key, challenge)` and gets its key back.
- One key per server, stored in the value that names the server, in the file where that value lives: `[app] proxy = <key>@http://host:8087` in config.ini (also `https_proxy`, `[server] proxy_url`, `converter_url`), or an app's own `proxy_url` / `proxy` / `converter_url` in its config.ini (its https:// data goes to its `converter_url` when set). `config-get` returns the address without the key; the settings page never shows it. Each app can use its own server.
- Routing (`mini_fetch_open`): a URL goes through the channel of its own server (scheme://host:port) when the device is registered there; an `https://` URL goes to the https converter (`https_proxy`, else `proxy`), encrypted or as `<it>/fetch?url=`. No hardware password, a server without the channel (403 "disabled" / 404) or a failed registration: plain HTTP; `/api/settings` (`channel`) and the settings page show each server's state and the reason. The old `[channel]` section is ignored.
- A registered device is never re-keyed: registering again only returns its key when the password (its hash key) matches; otherwise 403. A key the server refuses is dropped and the device registers again once.
- Each request: 16 random nonce bytes; `HKDF-SHA256(salt = nonce, ikm = device key, info = "msc1 request keys")` gives the request and response keys (used once), AES-256-GCM with a zero IV.
- Request: `"MSC1" | u8 id length | id | nonce | u32 n | ciphertext | tag`; plaintext `u32 time | method | url | body`. Response: records of at most 1024 bytes.
- The converter refuses a nonce it saw in the last 10 minutes; the device's clock is not enforced (a board before NTP sends time 0).
- URLs: the proxy's own `/app/...`, `/market/...` are answered in-process; any other `http(s)` URL is fetched (GET), from internet hosts only on a public server.

### 6.2 Plain endpoints and safety rules
- `/app/<name>` (weather, news, stocks, crypto, futures, calendar, usage), `/market/<file>` (only files inside `apps/`), `/fetch?url=` (internet hosts only, every redirect re-checked; a local server also reaches its own network for clients on that network).
- Market data is real or absent: e.g. `/app/crypto` takes the first of OKX, Binance (`data-api.binance.vision`, reachable from mainland China) and Huobi that answers (a failing source is skipped for 5 minutes) and answers 502 (the device keeps its last data) when none does; it never returns invented prices.
- CORS answers go only to the WebREPL, this network's pages and the server itself.
- `X-Forwarded-For` is believed only from a reverse proxy on the same machine.
- Admin portal: password from `config.ini [security] admin_password` (or `MINISCHEME_ADMIN_PASSWORD`); a public server never runs without one; a local server without one lets only this machine's own browser in.
- Logs and the access log mask secret query values (`token`, `key`, `auth`, ...).

### 6.3 SQLite
- `devices` (`mac`, `hardware_hash_key`, `master_key_2`, `created_at`, `last_seen_at`, `last_ip`, `status`), `app_data`, `access_logs`. The database file is created with mode 0600.

---

## 7. Quick reference

```bash
# core: build, generate the MSBC test images, run the three suites
cd repos/minischeme && make test            # MinGW: mingw32-make SHELL=sh CC=gcc test

# firmware (ESP-IDF 6.1 environment)
cd repos/minischeme-esp32 && idf.py build
python tools/make_manifest.py <dir with the 3 images> <version> 6.1

# apps: compile to dist/, update index.json; --sync also updates the firmware and WebREPL copies
cd repos/minischeme-apps && python tools/build_apps.py --sync

# proxy
cd repos/minischeme-proxy && python app_proxy.py --mode local --port 8087 && python -m pytest tests

# WebREPL with the local server (proxy built in)
cd repos/minischeme-webrepl && python server.py --port 8087 && python -m pytest tests
```

### Rules for agents
1. All code, identifiers, comments and commit messages in English (conventional commits).
2. Zero compiler warnings (C99, `-Wall -Wextra`).
3. No fake data and no fake progress: flashing, compiling and market data must be real.
4. Never commit credentials, tokens, keys, passwords or personal addresses.
5. Releases: commit, push `main`, then tag `v1.0.X` and push the tag (CI publishes the Release only from a tag).
