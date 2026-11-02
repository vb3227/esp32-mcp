// Interface inventory reports hardware/profile support, not attached-device detection.
void writeInterfaceCapabilities(JsonDocument &reply) {
    struct Entry { const char *id; bool available; const char *requirements; };
    const Entry entries[] = {
        {"gpio", true, "Allowed external digital pins"},
        {"adc", true, "Allowed ADC-capable pin"},
        {"pwm", true, "Output pin; reserved LEDC channels 4/5"},
        {"uart", true, "TX/RX pins; UART1"},
        {"rs485", true, "TX/RX/DE pins and external RS485 transceiver"},
        {"i2c", true, "SDA/SCL and pull-ups"},
        {"spi", true, "SCK/MISO/MOSI/CS; exclusive with mounted SD"},
        {"onewire", true, "One digital pin and external pull-up; no parasite power"},
        {"i2s", true, "BCLK/WS/DATA; external digital mic or audio DAC"},
        {"can_twai", bool(SOC_TWAI_SUPPORTED), "TX/RX and external CAN transceiver; classic CAN 8-byte frames"},
        {"pulse_counter", bool(SOC_PCNT_SUPPORTED), "PCNT-capable chip and external pulse input"},
        {"rmt_ir", true, "Output pin; optional external IR/radio front end"},
        {"dac", bool(MCP_HAS_DAC), "Chip DAC pin (ESP32/S2); other chips use an external DAC"},
        {"capacitive_touch", bool(MCP_HAS_TOUCH), "Touch-capable chip and allowed touch pin"},
        {"sd_spi", true, "External SD card, four SPI pins, file size <=64 KiB"},
        {"tcp", true, "Connected Wi-Fi; explicit unicast IPv4 peer"},
        {"udp", true, "Connected Wi-Fi; explicit unicast IPv4 peer"},
        {"http", true, "HTTP URL; HTTPS additionally requires a trusted CA PEM"},
        {"mqtt", true, "IPv4 MQTT 3.1.1 broker; QoS0 over plain TCP"},
        {"esp_now", true, "ESP-NOW peer on the current Wi-Fi channel; <=200 bytes"},
        {"ble_gatt", bool(MCP_HAS_BLE), "BLE-capable chip/framework and enough free internal heap"},
#ifdef MCP_PROFILE_K10
        {"camera", true, "K10 built-in supported camera via adapter jobs"},
        {"display", true, "K10 built-in screen via adapter jobs"},
#else
        {"camera", false, "Requires a camera adapter; generic builds omit K10 camera drivers"},
        {"display", false, "Use an external display driver over I2C/SPI"},
#endif
        {"nvs_state", true, "Installed skill name; persistent JSON state with quota"},
    };
    JsonArray values = reply["interfaces"].to<JsonArray>(); unsigned count = 0;
    for (const auto &entry : entries) {
        JsonObject item = values.add<JsonObject>(); item["id"] = entry.id; item["available"] = entry.available; item["requires"] = entry.requirements;
        if (entry.available) ++count;
    }
    reply["interface_count"] = sizeof(entries) / sizeof(entries[0]); reply["supported_interface_count"] = count;
    reply["resource_policy"] = "One foreground slot reserved; leases exclude conflicting jobs; runtime pin grants remain within board ceiling";
}
