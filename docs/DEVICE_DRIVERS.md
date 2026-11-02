# Device drivers without reflashing

API v3 adds a device-driver catalog and background Lua execution. The catalog is
implemented source code, **not a list of physically tested or detected modules**.
Ask `list_compatible_devices` for the current list, datasheet links and limitations.
Chip peripherals, allowed pins, wiring, voltage and module variant must agree.

## Interface families

`hardware_capabilities.interfaces` reports 24 families with per-chip availability:
GPIO, ADC, PWM, UART, RS485, I2C, SPI, 1-Wire, I2S, classic CAN/TWAI, pulse counter,
RMT/IR, internal DAC, capacitive touch, SD over SPI, TCP, UDP, HTTP/HTTPS, MQTT,
ESP-NOW, BLE GATT, camera, display and persistent NVS state. This count includes
software transports/storage, not 24 independent physical connectors. An antenna
requires a supported radio module. Camera/display bindings refer to the K10 adapter;
generic builds do not include a universal camera driver. DAC, touch, PCNT, CAN and
Bluetooth depend on the chip. K10 internal peripherals stay under its adapter.

The concrete driver catalog covers:

| Function | Drivers |
| --- | --- |
| Light/color | BH1750, VEML7700, TSL2561, APDS9960 RGBC, TCS34725 |
| Temperature | LM75, TMP102, TMP117, MCP9808, DS18B20 |
| Humidity/temperature | SHT31, SHT4x, AHT20, HDC1080, HTU21D, Si7021 |
| Pressure | BMP180, BMP280, BME280 |
| ADC | ADS1015, ADS1115 |
| Current/voltage | INA219, INA226 |
| Motion | MPU6050, ADXL345, LIS3DH, LSM6DS3TR-C |
| RTC | DS3231 |
| GPIO/PWM/DAC expansion | MCP23017, PCA9685, MCP4725 |
| Packet radio | NRF24L01+ |

Consult each catalog entry for implemented outputs. Listing APDS9960 does not claim
gesture recognition, and listing a raw color/light driver does not claim calibrated
color-temperature/lux conversion. Driver installation is not hardware auto-detection.

`create_device_driver(driver_id, config_json, name, install, run, background)`
generates a readable Lua program, saves it under `skills/lua`, and optionally installs
it in the board's NVS. Installed programs can be invoked by name through MCP;
the host catalog retains their short aliases. GPIO permissions are explicit and
are never expanded automatically by this tool. All drivers require API v3 firmware.

## NRF24L01+

The `nrf24l01` driver implements fixed-width packet transmission and reception.
It does not implement automatic acknowledgements, dynamic payloads, mesh routing
or continuous carrier modes. `tx_complete=true` means the transmitter completed
its operation; it does **not** mean a receiver received the packet.

Example configuration for an ordinary ESP32 (verify wiring for your board):

```json
{"sck":18,"miso":19,"mosi":23,"cs":5,"ce":4,"channel":40,"width":32,"rate_kbps":1000,"power":0}
```

Add `"payload_hex":"68656c6c6f"` to send a packet padded to the selected width.
Omit payload for reception; use `background=true` for a continuous receive monitor.
Both peers must use matching channel, speed, five-byte `address_hex` and width.
Power levels are 0..3, from low to high; channel is 0..125, rate is 250/1000/2000
kbps. Choose settings appropriate for local radio regulations and your hardware.
Use a suitable regulated 3.3 V supply and decoupling; an antenna alone is not a radio.

K10 has few native external GPIOs. With a compatible breakout exposing its logical
expander pins, native GPIOs 1/2/3 can carry SPI and logical pins can carry CS/CE:

```json
{"sck":1,"miso":2,"mosi":3,"cs":"P13","ce":"P14","channel":40,"width":32}
```

This is an example wiring assignment, not detection of your expansion board.
Logical `P13` differs from native ESP GPIO 13. The driver uses manual CS with
`spi_transfer(...,-1,...)`; `k10_gpio_write` restores CS high and CE low on exit.
Only external logical P3/P4/P6/P8/P9/P10/P12..P16 are exposed. Buttons and internal
display/camera/amplifier controls are excluded. A generic I2C GPIO expander is not
automatically a K10 logical-pin adapter.

## Ongoing tasks

`execute_device_code(source, background=true)` and saved background skills run until
completion, error, cancellation or reset. They do not have a lifetime deadline.
The runtime supports at most three background VMs and reserves one of four slots
for foreground work, subject to available memory. Each VM has a 64 KiB allocation
limit and a 12 KiB task stack. CPU and hardware-call budgets throttle busy background
tasks; they do not provide hard real-time execution. Driver calls may delay
cancellation until their bounded operation returns.

Use `device.sleep_ms` inside loops and `device.publish(value)` to replace a bounded
status snapshot. `list_device_jobs`, `device_code_status` and `stop_device_code`
inspect and stop individual jobs. `send_device_message` sends JSON to a four-entry
mailbox; Lua reads it with `device.receive_message()`. Mailboxes and snapshots are
volatile. Installed skill source and explicit `state_set` data persist; execution
does not automatically resume after a reboot. External actuators may retain their
own state after VM shutdown: write the intended shutdown behavior explicitly.

## Verification

Tests execute generated Lua using Lua 5.4 through Lupa. They check catalog syntax,
the board source-size limit, invalid configurations, NRF register programming,
payload padding, control pins and saved-skill reload. They simulate peripheral
responses and do not establish electrical or RF compatibility with a real module.
