# MiniScheme Agent Development & Debugging Guide

This document is specifically crafted for Autonomous AI Agents and Software Engineers developing, debugging, extending, and maintaining the MiniScheme ecosystem.

---

## 1. System Ecosystem & Repository Topology

The MiniScheme project consists of five decoupled, collaborative repositories:

```
minischeme-gemini/repos/
├── minischeme/          # Core ISO C99 compiler, bytecode interpreter, Cheney GC VM
├── minischeme-esp32/    # ESP-IDF v5.x firmware with zero-RAM XIP & band rendering
├── minischeme-apps/     # Standard Scheme application suites (Calendar, Weather, etc.)
├── minischeme-proxy/    # AES-GCM encrypted application proxy & protocol converter
└── minischeme-webrepl/  # Web IDE, WASM compiler, Web Serial flasher & console
```

---

## 2. Core VM Architecture & Memory Model

### 2.1 ISO C99 & Cross-Platform Target
- **Strict Compliance**: ISO C99 with `-Wall -Wextra` and zero compiler warnings.
- **Portability**: Native compilation on MinGW/Linux/macOS, bare-metal microcontrollers (ESP32/ESP8266/STM32/RP2040), and WebAssembly (`wasm32-unknown-unknown`).

### 2.2 Memory Optimization & Flash ROM Execution (XIP)
- **Execute-In-Place (XIP)**: Bytecode images (`.msbc`) are 8-byte aligned and executed directly from Flash ROM memory without copying into RAM.
- **Zero-RAM Constants**: Literals, symbols, and constant vectors are referenced directly in ROM.
- **Garbage Collector**: Semispace Cheney copying collector.
  - Semi-space heap (e.g. 108 KB on ESP32, 64 KB default on WASM).
  - All temporary pointer allocations in C functions **must** be safely registered with GC roots:
    ```c
    mini_push_root(vm, &my_val);
    // ... code that might trigger allocation or GC ...
    mini_pop_root(vm);
    ```

---

## 3. MSBC Binary Format & Metadata Trailer (`MSMD`)

Each compiled application image ends with an `MSMD` binary trailer that allows the firmware, WebREPL, and file manager to identify, introspect, and configure apps without running them:

```
+-------------------------------------------------------------+
| MSBC Bytecode Payload (Compiled Scheme Image)              |
+-------------------------------------------------------------+
| (Optional Icon / Bitmap Data)                               |
+-------------------------------------------------------------+
| UTF-8 Metadata INI Text (Contains [app] and [config])       |
+-------------------------------------------------------------+
| 4-byte Little-Endian uint32: Metadata Length (Bytes)        |
+-------------------------------------------------------------+
| 4-byte Little-Endian uint32: FNV-1a Checksum of Metadata   |
+-------------------------------------------------------------+
| 4-byte Magic ASCII: "MSMD" (0x4D, 0x53, 0x4D, 0x44)         |
+-------------------------------------------------------------+
```

### 3.1 Metadata INI Structure
```ini
[app]
id = calendar
name = 万年历
description = 农历廿四节气黄历时钟
version = 1.0.0
pages = 2
api = 1.5

[config]
proxy_url = http://proxy.local:8087
converter_url = http://converter.local:8087
```

### 3.2 FNV-1a Checksum Algorithm
```python
def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h
```

---

## 4. Hardware Interfaces & Communication Protocols

### 4.1 Web Serial UART Protocol
Every line sent to the device begins with `@` and is answered by `@OK {json}` or `@ERR {json}`:

- `@INFO`: Return device info (MAC, version, free heap, display mode, compiler availability).
- `@EXEC <base64>`: Execute inline base64 MSBC bytecode.
- `@BEGIN <size> [path]`: Begin multi-chunk upload.
- `@DATA <base64>`: Chunk payload (default 768 bytes raw / 1024 base64 chars).
- `@END run|save`: Finish upload and either execute or write to SPIFFS.
- `@LS`: List files in `/spiffs`.
- `@APPS`: List all installed apps in `/spiffs/apps`.
- `@READ <offset> <path>`: Read file chunk.
- `@RM <path>`: Delete file.
- `@MV <from> <to>`: Rename file.
- `@RESET`: Hard reboot the microcontroller.

### 4.2 HTTP REST API & Authentication
When connected over WiFi HTTP:
- `GET /api/info`: Device status and capabilities.
- `POST /api/exec`: Execute MSBC bytecode sent in body (`Content-Type: text/plain`).
- `POST /api/eval`: Compile and evaluate Scheme source directly if firmware compiler is enabled.
- `POST /api/upload?path=<path>&run=<0|1>`: Upload binary file directly to SPIFFS.
- `GET /api/fs/list`: Return directory file list as JSON.
- `GET /api/fs/read?path=<path>`: Stream file bytes.
- `GET /api/screen`: Fetch current display framebuffer mirror.
- `GET /api/stats`: Fetch real-time hardware memory and telemetry figures.

#### Authentication Mechanism
To ensure zero CPU overhead on microcontrollers while preventing unauthorized HTTP operations:
- The device checks the header `X-MiniScheme-Auth: <password>` or query parameter `?auth=<password>`.
- The device performs a constant-time string match against `config.ini [device] password`.
- If authentication fails, the device responds with `HTTP 401 Unauthorized`.

---

## 5. ESP32 Flash Memory Layout & Firmware Flashing

### 5.1 Flash Address Offsets
| Partition | Offset | File Name | Description |
|-----------|--------|-----------|-------------|
| **Bootloader** | `0x1000` | `bootloader.bin` | ESP32 Second Stage Bootloader |
| **Partition Table** | `0x8000` | `partition-table.bin` | CSV Partition Table binary |
| **Main App Firmware** | `0x10000` | `minischeme_esp32.bin` | MiniScheme ESP-IDF system image |
| **SPIFFS Storage** | `0x180000` | N/A | User apps (`/spiffs/apps/<id>/app.msbc`) & config |

### 5.2 Web Serial Firmware Flasher (`esptool-js`)
- Handshake at `115200` baud with standard RTS/DTR reset sequence.
- Accelerate to `460800` or `921600` baud during block flashing.
- SLIP frame deflate compression for fast flashing.
- Hard reset after flashing via RTS pin toggle.

---

## 6. External App Proxy & Protocol Converter Specification

The external service acts as a secure, caching intermediary between MiniScheme devices and external internet APIs (Weather, Stocks, News, Feeds).

### 6.1 Cryptographic Protocol
- **Transport**: Standard HTTP with **AES-GCM-256** payload encryption.
- **Key Derivation**:
  1. Primary Master Key ($K_{master1}$) on server.
  2. Device registers with hardware password hash ($K_{hw\_hash}$).
  3. Server derives Secondary Master Key ($K_{master2} = \text{HKDF}(K_{master1}, K_{hw\_hash}, \text{MAC})$).
  4. Per-request dynamic subkey: $K_{session} = \text{HKDF}(K_{master2}, \text{timestamp} \parallel \text{nonce})$.
- **Factory Reset Re-authentication**:
  - If a device reconnects after factory reset, the server issues a challenge nonce.
  - Device verifies ownership by replying with $\text{HMAC}(K_{hw\_hash}, \text{nonce})$.
- **Replay Protection**:
  - Timestamps must fall within $\pm 300$ seconds of server time.
  - Nonces are recorded and rejected if duplicated within the validity window.

### 6.2 SQLite Persistence Schema
- `devices`: `mac_address`, `hw_hash`, `master2_key`, `created_at`, `last_login`, `total_bytes`
- `access_logs`: `id`, `mac_address`, `app_id`, `endpoint`, `client_ip`, `status_code`, `timestamp`
- `app_cache`: `cache_key`, `app_id`, `encrypted_data`, `expires_at`

---

## 7. Development & Debugging Quick Reference

### 7.1 Running WebREPL Locally
```bash
python server.py --port 8080
```
With co-located App Proxy:
```bash
python server.py --port 8080 --with-proxy
```

### 7.2 Running Proxy Service & Tests
```bash
cd repos/minischeme-proxy
python app_proxy.py --mode local --port 8087
pytest tests/ -v
```

### 7.3 Building Apps to MSBC
```bash
cd repos/minischeme-apps
python tools/build_apps.py
```

### 7.4 Coding Guidelines for Agents
1. **Language Strictness**: All code, identifiers, commit messages, and comments MUST be 100% in English.
2. **Zero Compiler Warnings**: Strict ISO C99 with `-Wall -Wextra`.
3. **No Fake Progress / Mocking**: All Web Serial flashing and compilation logic must be genuine.
4. **Security**: Never commit or hardcode actual credentials, tokens, passwords, or personal IPs.
