# Third-party components

The MIT project license covers original ESP32 MCP code, not upstream components.

| Component | Source/version | License |
| --- | --- | --- |
| Embedded Lua | Lua 5.4.9, vendored in lib/lua | MIT; full notice in lib/lua/src/lua.h |
| NimBLE-Arduino | h2zero/NimBLE-Arduino 2.5.2 | Apache-2.0; upstream files retain their notices |
| Arduino-ESP32 | Espressif Arduino framework selected by the pinned platform | LGPL-2.1 and bundled component notices |
| ESP-IDF | Framework-provided libraries | Apache-2.0 and component-specific notices |
| UNIHIKER framework | DFRobot/platform-unihiker | Upstream vendor and bundled component terms |
| ArduinoJson | 7.4.2 | MIT |
| arduinoWebSockets | 2.7.3 | LGPL-2.1 |
| Python packages | requirements*.txt | Each package's upstream license |

Sources: [Lua](https://www.lua.org/license.html),
[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino),
[Arduino-ESP32](https://github.com/espressif/arduino-esp32),
[ESP-IDF](https://github.com/espressif/esp-idf),
[UNIHIKER platform](https://github.com/DFRobot/platform-unihiker),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson),
[arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets).

Public firmware excludes speech-model partition assets. Existing vendor ASR
models are preserved by app-only K10 updates. The optional vendor model-provisioning
environment obtains assets under the vendor's terms; it is not included in the release.
