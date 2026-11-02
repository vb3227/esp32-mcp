# On-device Lua

Lua 5.4.9 uses 32-bit integers/floats (`LUA_32BITS=1`). Only text source is accepted. CLI entry points are excluded. [Provenance](../lib/lua/PROVENANCE.md).

API v3 adds background execution, messages, snapshots, hardware/network bindings
and reusable peripheral drivers. See [device drivers](DEVICE_DRIVERS.md) and MCP
`native_api_reference` for the complete current function signatures. Foreground
deadline/instruction limits below do not impose a lifetime deadline on background
jobs; they use throttled CPU/call windows and still obey cancellation/resource limits.

## Lifecycle

1. Inspect `hardware_capabilities`; review `MCP_ALLOWED_PINS` before flashing.
2. `execute_device_code(source, timeout_ms=10000)` returns a job ID.
3. Poll `device_code_status(job_id)` for status/result/stdout/error; cancel with `stop_device_code(job_id)`.
4. `save_device_script(name, source, voice_phrase, short_commands, install=True, run=False)` saves locally and optionally installs in NVS. Inspect installation separately from local save.
5. `install_device_script(name)` installs a checked local copy. `run_device_script(name)` runs an installed program. `list_device_scripts` and `read_device_script` retrieve names/source from the board, even without original host files.

Installed skills survive reset but do not auto-run at boot. Replacing source does not modify an active job. There is no automatic multi-board synchronization or delete synchronization. Local SHA-256 detects source/manifest mismatch, not malicious code. Lua syntax is checked when loaded on the MCU.

## API

| Function | Behavior |
| --- | --- |
| `device.info()` | Board/runtime information |
| `device.sleep_ms(ms)` | Cooperative sleep, 0â€“5000 ms per call |
| `device.pin_mode(pin, mode)` | `input`, `output`, `pullup` |
| `device.digital_write(pin, value)` | 0/1; sets output mode |
| `device.digital_read(pin)` | Digital input |
| `device.analog_read(pin)` | Arduino ADC; choose an allowed ADC-capable pin |
| `device.i2c_transfer(sda, scl, address, tx, rx_count)` | Byte-string TX, up to 128 RX bytes |
| `device.spi_transfer(sck, miso, mosi, cs, tx, frequency=1000000, mode=0)` | Up to 128 bytes, 10 kHz–10 MHz, modes 0–3 |
| `device.run(program)` | Adapter JSON hardware job via MCU loopback; returns response table |
| `device.status(job_id)` | Adapter hardware status via MCU loopback |
| `print(...)` | Bounded job stdout |

Use direct GPIO/bus functions on generic boards: nested `device.run` consumes additional Lua slots. K10 hardware actions use its separate existing scheduler. Standard base/math/string/table functionality is available with unsafe facilities removed. There is no OS/debug, module loading or dynamic source loading. API v3 provides explicit bounded TCP/UDP/HTTP/MQTT and external SD-card functions, not unrestricted OS sockets or host filesystem access.

```lua
local total = 0
for i = 1, 10 do
    total = total + i
    device.sleep_ms(10)
end
print("sum", total)
return {sum = total}
```

## Limits

- Source 1â€“4096 UTF-8 bytes. Deadline 10â€“60000 ms, default 10 s.
- Four Lua task slots; 64 KiB Lua allocator budget/job (PSRAM on K10); 2 million VM instructions; 128 hardware dispatches. Heap admission can reject jobs before all four slots are occupied.
- Output 1200 characters; JSON results have depth/node/string limits. Completed slot results are replaced when slots are reused.
- NVS: 12 native skills, 16 KiB aggregate payload. Other users share physical NVS capacity, so writes can fail earlier.
- Empty pin allowlist denies GPIO. `configure_device_pins` persists an exact allowlist within the compiled profile ceiling; no active Lua jobs may run during a change. `hardware_capabilities` exposes allowed/configurable pins. K10 defaults to external P0/P1/P2 = GPIO1/2/3 only. Generic development-board ceilings exclude common flash/USB/boot pins but must still be reviewed against the actual board and wiring. Override the ceiling with MCP_CONFIGURABLE_PINS when building a custom adapter.
- Pin ownership prevents conflicting scripts. Output pins are driven LOW on exit. I2C/SPI operations serialize through a shared mutex.
- K10 voice epochs reject stale voice-bound scripts. Hardware jobs retain peripheral arbitration.

Cancellation/deadlines are cooperative at hooks/API boundaries; driver calls can delay them. This is not an OS sandbox. Test external actuators carefully and keep USB recovery available.

## Expanded API, version 2

Use MCP `native_api_reference` for complete signatures without reading files. `hardware_capabilities.native_api_reference` includes only functions reported by the installed firmware. Older firmware must be upgraded once to gain new native bindings. Thereafter the scripts combining these bindings need no reflash.

| Function | Contract |
| --- | --- |
| `device.wifi_info()` | Current connected/SSID/IP/RSSI/channel; no passwords |
| `device.wifi_scan()` | Bounded passive driver scan, up to 12 SSID/RSSI/channel/security entries; total count/truncated flag; vendor driver wait up to 10 s |
| `device.millis()`, `device.micros()` | Boot clocks, wrap at 2^32; returned as Lua float |
| `device.random(limit)` | Integer 0..limit-1 |
| `device.analog_read_mv(pin)` | Calibrated ADC millivolts on an allowed ADC-capable pin |
| `device.pulse_in(pin, level, timeout_us=30000)` | Pulse length, timeout 1..50000 us |
| `device.i2c_scan(sda,scl)` | Responding addresses 8..119 on external bus |
| `device.pwm_write(pin,duty,frequency=1000)` | 10-bit duty 0..1023, frequency 20..20000 Hz |
| `device.pwm_stop(pin)` | Detach owned PWM before changing to ordinary digital output |
| `device.uart_open(tx,rx,baud=115200)` | Exclusive external UART1, 8N1, baud 300..1000000 |
| `device.uart_write(bytes)` | Up to 256 bytes |
| `device.uart_read(max_bytes=128,timeout_ms=100)` | 1..256 bytes; timeout 0..1000 ms |
| `device.uart_close()` | Release the owned UART |
| `device.state_get(key)` | Installed named skill's JSON state, nil when absent |
| `device.state_set(key,value)` | Save state; returns boolean, check for NVS/quota failure |

State persists across reset and source updates to the same installed name. Ad-hoc execution has no state namespace: install/run a named skill first. Keys use 1..31 alphanumeric/_/- characters. Limits: 1200 bytes per record including metadata, 24 records/8 KiB shared, eight writes/job to reduce flash wear. No automatic state eviction. Do not store secrets in returned state/logs.

PWM reserves low-speed LEDC channels 4/5, timers 2/3. Two PWM outputs may run concurrently across jobs; custom adapters must reserve these resources too. UART/PWM are released when a job completes, errors or is cancelled. I2C uses the separate controller on K10; external SPI uses HSPI. Do not reuse built-in peripheral pins/controllers in a custom profile.

For binary serial/bus responses, convert strings to arrays of `string.byte` before returning JSON. Timing from Lua/FreeRTOS is not hard real-time. Wi-Fi scanning can briefly increase relay latency; it does not change saved network credentials or connect to discovered networks.

Examples:

```lua
return device.wifi_scan()
```

```lua
local count = device.state_get("runs") or 0
assert(device.state_set("runs", count + 1), "NVS state write failed")
return {runs = count + 1}
```
