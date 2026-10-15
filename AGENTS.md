# Project Memory & Guidelines

## Language Standards (STRICT REQUIREMENT)
- **Code & Comments**: All code, identifiers, type names, variable names, function names, and comments across all files (*.c, *.h, *.scm, *.js, *.py, etc.) MUST strictly be written in English.
- **No Non-English Comments**: Do not write comments, docstrings, or architectural notes in Chinese or any non-English language within the codebase.
- **Commit Messages**: All git commit messages must be in English following conventional commit style (`feat:`, `fix:`, `refactor:`, `test:`, etc.).
- **Logs & Error Messages**: Runtime error messages, debug prints, and diagnostic assertions must use clear and concise English.

## Architecture & Code Quality Guidelines
- **Language Standard**: Strict ISO C99 compliance with zero compiler warnings under `-Wall -Wextra`.
- **Target Environments**: Cross-platform portability across Windows (MinGW/MSYS), Linux (GCC/Clang), macOS, WebAssembly (`wasm32-unknown-unknown`), and bare-metal/RTOS microcontrollers (ESP32, ESP8266, STM32, RP2040).
- **Memory Optimization**:
  - Maintain Execute-In-Place (XIP) compatibility with 8-byte aligned Flash ROM images.
  - Zero-RAM copying for static bytecodes, constants, and symbols from ROM.
  - Efficient Cheney copying garbage collection with precise root registration (`mini_push_root` / `mini_pop_root`).
- **Testing & Verification**:
  - Preserve 100% test pass rate across all suites (Unit/Integration, Compiler, Stress, WebREPL E2E, LCD Simulation).
  - Run regression tests after every significant architectural change.
