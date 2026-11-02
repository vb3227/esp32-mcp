# Board profiles and feature preservation

| Capability | Generic ESP32 variants | K10 adapter |
| --- | --- | --- |
| Authenticated direct HTTP / outbound WSS | Yes | Yes |
| On-MCU Lua, NVS skills, GPIO/ADC/I2C/SPI | Yes, allowlist required | Yes, external safe pins only |
| Host restricted Python + state | Yes; operations depend on board | Yes |
| JSON skill persistence/composition/aliases | Core operations | Full K10 operation set |
| Sensor display, RGB, screen text/pixels | Adapter required | Preserved |
| Camera preview, still photo, bounded silent AVI | Adapter required | Preserved |
| Microphone, A/A capture, transcript, local ASR priority | Adapter required | Preserved |
| Optional local/Groq STT, fast router, Codex generation | With audio adapter | Preserved |
| Stage overlay and eye boot animation | No screen; Serial progress | Preserved/new shared animation |
| Four hardware jobs | Four native-backed generic jobs | Four hardware jobs plus four Lua slots |
| Local-only Python/MCP control token | Yes | Yes |

Generic build profiles require at least 4 MB flash and use a single 3 MB application partition (no OTA).

The generic JSON profile currently supports delay, gpio.write/read, adc.read and system sensor information. It explicitly rejects unsupported operations. K10 keeps its vendor drivers in `src/boards/k10`; no runtime imports or paths refer to another project. Vendor local-ASR wake constants remain for compatibility with its installed model.

No continuous analysis, person profiling or model video analysis is included. These were previously removed from the companion project. Media is saved as files.

Pin access does not imply electrical compatibility. Adding a UART/PWM/custom sensor driver requires an adapter extension if the provided bus operations are insufficient. Source compilation alone cannot establish that an arbitrary ESP32-based board is supported.

## Two firmware families

| Family | Environment | PlatformIO board |
| --- | --- | --- |
| Minimal ESP32 baseline / WROOM-32 | esp32dev | esp32dev |
| Minimal ESP32-S3 | esp32s3 | esp32-s3-devkitc-1 |
| Minimal ESP32-C3 | esp32c3 | esp32-c3-devkitm-1 |
| Minimal ESP32-C6 | esp32c6 | esp32-c6-devkitc-1 |
| Minimal ESP32-S2 | esp32s2 | esp32-s2-saola-1 |
| Minimal ESP32-WROVER | esp32wrover | esp-wrover-kit |
| Full UNIHIKER K10 | k10 | unihiker_k10 |

These five additional variants cover common development-board families, not a measured top-five sales/popularity ranking. Vendor chip families: [Espressif portfolio](https://dl.espressif.com/public/Espressif%20SoC%20Product%20Portfolio.pdf). Board definitions are taken from the pinned PlatformIO platform.

Minimal environments compile only src/core and src/boards/generic. They do not compile the K10 screen, camera, microphone, ASR, RGB/sensor adapter or its vendor framework. Their MCP client uses --profile generic (the tools-mode default), which omits K10 media/voice/UI tools. Full K10 uses --profile k10, or --voice which selects it by default.

A single source tree avoids maintaining seven drifting copies. Every environment produces its own chip-specific binary; binaries are not interchangeable. The source release does not include credential-bearing flashed images. All boards require local Wi-Fi/token configuration before use.

Profile GPIO ceilings are conservative development-board defaults, not a wiring guarantee for every clone. WROVER excludes GPIO16/17 used for PSRAM. S2/S3/C3/C6 have separate ceilings. Review hardware_capabilities.configurable_pins and the actual module schematic before granting GPIO access through MCP.
