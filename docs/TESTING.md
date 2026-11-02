# Validation

```powershell
python -m pip install -r requirements-dev.txt
python -m unittest discover -s tests -t .
ruff check server.py mcp tests scripts
ruff format --check server.py mcp tests scripts
python scripts/check_repository.py
python scripts/package_source.py
```

Tests exercise bounds/validation, phrase matching and composition, short aliases, authenticated WebSocket exchange, duplicate metadata, disconnect/no-replay behavior, JPEG/AVI persistence, Python capability limits, process cancellation/memory caps and publication exclusions. They use fake models where appropriate; passing does not establish speech accuracy, sub-five-second latency or arbitrary hardware compatibility.

Build C++ using VS Code PlatformIO. An upload requires explicit authorization. Real acceptance should check an actual command path, saved-skill reuse without extra files, photo/video persistence and failure recovery. Device `[voice timing] stop_to_action_ms` measures from capture completion to device action/feedback delivery; host-only inference metrics exclude upload time.

Publishing source does not publish firmware, credentials or media. Review `dist/esp32-mcp-source.zip` and the Git diff. The automated publication checker is a practical preflight rather than a full secret/security audit. Measurements and limitations belong in [performance](PERFORMANCE.md).

## Current validation snapshot

- 91 Python tests passed, including real MCP SDK stdio interaction, restricted Python execution, pin-policy validation, host USB-configuration input bounds and API-v3 background/message contracts.
- All 32 generated driver programs (foreground/background variants) compile in a real Lua 5.4 test VM and fit the 4096-byte source limit. NRF register/TX/control-pin simulation, BMP280 datasheet compensation and SHT31 CRC rejection passed. These simulations do not establish electrical compatibility or physical module behavior.
- Physical K10 acceptance: 44 of 45 checks passed on the tested API-v2 firmware. Four concurrent native Lua jobs and four hardware jobs were exercised, along with JSON reuse/aliases, NVS skill state, GPIO/ADC/PWM/UART/I2C, display/RGB/sensors, microphone/ASR status, camera preview, JPEG and AVI saving, host Python, resource limits/cancellation and recovery.
- Wi-Fi connectivity/MCP relay passed; Wi-Fi scanning failed. A later driver workaround is present in source but not hardware-validated; scanning is not claimed working.
- The latest USB provisioning path compiled in all seven public profiles. Host input tests do not prove a fresh-device provisioning handshake.
- The previous API-v2 credential-free release compiled for all seven profiles. API-v3 release artifacts must pass `python scripts/verify_firmware_release.py` after rebuilding all seven profiles; do not infer completion from an older ZIP. Generic boards remain compile checks, not physical tests.
- API-v3 physical background-loop/peripheral acceptance has not been performed. The latest USB inventory had no board serial port; no new upload or attached-NRF test is claimed.
- No live spoken-phrase accuracy test was performed in this acceptance run. Tools-only gateway tests do not establish an end-to-end voice transcription/planner result.
