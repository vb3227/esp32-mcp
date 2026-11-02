// Private runtime implementation; included inside native_runtime.cpp's namespace.
// Never raise a Lua error while holding a C++ mutex or peripheral transaction.
SemaphoreHandle_t scanLock = nullptr, stateLock = nullptr;
Preferences runtimeState, pinSettings;
HardwareSerial externalUart(1);
std::atomic<int> uartOwner{-1};
struct PwmChannel { int owner = -1, pin = -1; } pwmChannels[2];
uint64_t configurablePins = 0;

bool interrupted(Job *job) {
    if (job->cancel.load() || (!job->background && static_cast<int32_t>(millis() - job->deadline) >= 0)) return true;
#ifdef MCP_PROFILE_K10
    if (job->voiceBound && !cloudVoiceEpochValid(job->voiceEpoch)) return true;
#endif
    return false;
}
int clockMillis(lua_State *L) { lua_pushnumber(L, millis()); return 1; }
int clockMicros(lua_State *L) { lua_pushnumber(L, micros()); return 1; }
int randomValue(lua_State *L) {
    int limit = luaL_checkinteger(L, 1);
    if (limit < 1) return luaL_error(L, "random limit must be positive");
    lua_pushinteger(L, esp_random() % static_cast<uint32_t>(limit)); return 1;
}
int readMillivolts(lua_State *L) {
    int pin = luaL_checkinteger(L, 1); claim(L, pin);
    lua_pushinteger(L, analogReadMilliVolts(pin)); return 1;
}
int pulseValue(lua_State *L) {
    int pin = luaL_checkinteger(L, 1), level = luaL_checkinteger(L, 2);
    int timeout = luaL_optinteger(L, 3, 30000);
    if ((level != 0 && level != 1) || timeout < 1 || timeout > 50000) return luaL_error(L, "Invalid pulse level or timeout (1..50000 us)");
    claim(L, pin); lua_pushinteger(L, pulseIn(pin, level, timeout)); check(L); return 1;
}
int wifiInfo(lua_State *L) {
    Job *job = current(L); job->json.clear();
    job->json["connected"] = WiFi.status() == WL_CONNECTED;
    job->json["ssid"] = WiFi.SSID(); job->json["ip"] = WiFi.localIP().toString();
    job->json["rssi"] = WiFi.RSSI(); job->json["channel"] = WiFi.channel();
    toLua(L, job->json.as<JsonVariantConst>()); return 1;
}
int wifiScan(lua_State *L) {
    check(L); Job *job = current(L);
    if (!scanLock || xSemaphoreTake(scanLock, 0) != pdTRUE) return luaL_error(L, "A Wi-Fi scan is already running");
    // Use the vendor synchronous completion path and passive scan. Its async
    // scanComplete timeout can expire before a busy multi-channel scan ends.
    uint32_t started = millis();
    int count = WiFi.scanNetworks(false, false, true, 200);
    while (count == WIFI_SCAN_RUNNING && !interrupted(job) && millis() - started < 8500) {
        vTaskDelay(pdMS_TO_TICKS(20)); count = WiFi.scanComplete();
    }
    bool ok = count >= 0 && !interrupted(job);
    job->json.clear();
    if (ok) {
        job->json["count"] = count; job->json["truncated"] = count > 12;
        JsonArray networks = job->json["networks"].to<JsonArray>();
        for (int i = 0; i < count && i < 12; ++i) {
            JsonObject network = networks.add<JsonObject>();
            network["ssid"] = WiFi.SSID(i).substring(0, 32);
            network["rssi"] = WiFi.RSSI(i); network["channel"] = WiFi.channel(i);
            network["secured"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
    } else if (count == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
    WiFi.scanDelete(); xSemaphoreGive(scanLock);
    check(L);
    if (!ok) return luaL_error(L, "Wi-Fi scan failed: status=%d elapsed_ms=%d", count, static_cast<int>(millis() - started));
    toLua(L, job->json.as<JsonVariantConst>()); return 1;
}
int i2cScan(lua_State *L) {
    int sda = luaL_checkinteger(L, 1), scl = luaL_checkinteger(L, 2);
    if (sda == scl || !GPIO_IS_VALID_OUTPUT_GPIO(sda) || !GPIO_IS_VALID_OUTPUT_GPIO(scl)) return luaL_error(L, "Invalid I2C pins");
    claim(L, sda); claim(L, scl); Job *job = current(L);
    uint8_t addresses[112]; unsigned count = 0;
    xSemaphoreTake(busLock, portMAX_DELAY);
    bool opened = externalWire.begin(sda, scl); externalWire.setTimeOut(10);
    if (opened) for (int address = 8; address <= 119 && !interrupted(job); ++address) {
        externalWire.beginTransmission(address);
        if (!externalWire.endTransmission()) addresses[count++] = address;
        vTaskDelay(1);
    }
    externalWire.end(); xSemaphoreGive(busLock); check(L);
    if (!opened) return luaL_error(L, "Could not initialize external I2C bus");
    lua_createtable(L, count, 0);
    for (unsigned i = 0; i < count; ++i) { lua_pushinteger(L, addresses[i]); lua_rawseti(L, -2, i + 1); }
    return 1;
}
int pwmWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1), duty = luaL_checkinteger(L, 2), frequency = luaL_optinteger(L, 3, 1000);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || duty < 0 || duty > 1023 || frequency < 20 || frequency > 20000) return luaL_error(L, "PWM expects an output pin, duty 0..1023, frequency 20..20000 Hz");
    claim(L, pin); Job *job = current(L); int owner = job - jobs, slot = -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < 2; ++i) if (pwmChannels[i].owner == owner && pwmChannels[i].pin == pin) slot = i;
    if (slot < 0) for (int i = 0; i < 2; ++i) if (pwmChannels[i].owner < 0) { slot = i; break; }
    bool ok = false;
    if (slot >= 0) {
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE; timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = static_cast<ledc_timer_t>(2 + slot); timer.freq_hz = frequency; timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_channel_config_t channel = {};
        channel.gpio_num = pin; channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = static_cast<ledc_channel_t>(4 + slot);
        channel.intr_type = LEDC_INTR_DISABLE; channel.timer_sel = timer.timer_num; channel.duty = duty;
        ok = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK;
        if (ok) { pwmChannels[slot].owner = owner; pwmChannels[slot].pin = pin; job->outputs |= 1ULL << pin; }
    }
    xSemaphoreGive(lock);
    if (!ok) return luaL_error(L, "PWM unavailable or both reserved channels are occupied");
    return 0;
}
int pwmStop(lua_State *L) {
    int pin = luaL_checkinteger(L, 1); claim(L, pin); int owner = current(L) - jobs;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < 2; ++i) if (pwmChannels[i].owner == owner && pwmChannels[i].pin == pin) {
        ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(4 + i), 0);
        gpio_reset_pin(static_cast<gpio_num_t>(pin)); pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
        pwmChannels[i].owner = pwmChannels[i].pin = -1;
    }
    xSemaphoreGive(lock); return 0;
}
int uartOpen(lua_State *L) {
    int tx = luaL_checkinteger(L, 1), rx = luaL_checkinteger(L, 2), baud = luaL_optinteger(L, 3, 115200);
    if (tx == rx || !GPIO_IS_VALID_OUTPUT_GPIO(tx) || baud < 300 || baud > 1000000) return luaL_error(L, "Invalid UART pins or baud (300..1000000)");
    claim(L, tx); claim(L, rx); int owner = current(L) - jobs;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool available = uartOwner < 0 || uartOwner == owner;
    if (available) { externalUart.end(); externalUart.setRxBufferSize(512); externalUart.setTxBufferSize(512); externalUart.begin(baud, SERIAL_8N1, rx, tx); uartOwner = owner; current(L)->outputs |= 1ULL << tx; }
    xSemaphoreGive(lock);
    if (!available) return luaL_error(L, "External UART belongs to another job");
    return 0;
}
int uartWrite(lua_State *L) {
    check(L); size_t size; const char *data = luaL_checklstring(L, 1, &size);
    if (size > 256 || uartOwner != current(L) - jobs) return luaL_error(L, "Open UART first; payload limit is 256 bytes");
    lua_pushinteger(L, externalUart.write(reinterpret_cast<const uint8_t *>(data), size)); return 1;
}
int uartRead(lua_State *L) {
    int size = luaL_optinteger(L, 1, 128), timeout = luaL_optinteger(L, 2, 100);
    if (size < 1 || size > 256 || timeout < 0 || timeout > 1000 || uartOwner != current(L) - jobs) return luaL_error(L, "Open UART first; read 1..256 bytes, timeout 0..1000 ms");
    uint8_t bytes[256]; int count = 0; uint32_t started = millis();
    do {
        check(L);
        while (externalUart.available() && count < size) bytes[count++] = externalUart.read();
        if (count >= size || millis() - started >= static_cast<uint32_t>(timeout)) break;
        vTaskDelay(1);
    } while (true);
    lua_pushlstring(L, reinterpret_cast<const char *>(bytes), count); return 1;
}
int uartClose(lua_State *L) {
    int owner = current(L) - jobs;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (uartOwner == owner) { externalUart.end(); uartOwner = -1; }
    xSemaphoreGive(lock); return 0;
}
bool stateKey(lua_State *L, String &key) {
    Job *job = current(L); size_t size; const char *text = luaL_checklstring(L, 1, &size);
    if (!job->skillName.length() || !size || size > 31) return false;
    for (size_t i = 0; i < size; ++i) if (!isalnum(static_cast<unsigned char>(text[i])) && text[i] != '_' && text[i] != '-') return false;
    key = job->skillName + ':' + text; return true;
}
int stateGet(lua_State *L) {
    check(L); Job *job = current(L);
    if (!stateKey(L, job->scratch)) return luaL_error(L, "State requires an installed skill and a 1..31 character key");
    xSemaphoreTake(stateLock, portMAX_DELAY);
    String encoded = runtimeState.getString(keyFor(job->scratch).c_str(), "");
    job->json.clear(); bool found = encoded.length() && !deserializeJson(job->json, encoded) && job->json["key"].as<String>() == job->scratch;
    xSemaphoreGive(stateLock);
    // Destroy the temporary String before Lua can longjmp on allocation failure.
    encoded = "";
    if (!found) { lua_pushnil(L); return 1; }
    toLua(L, job->json["value"].as<JsonVariantConst>()); return 1;
}
int stateSet(lua_State *L) {
    check(L); Job *job = current(L);
    if (job->background && millis() - job->stateWindow >= 60000) { job->stateWrites = 0; job->stateWindow = millis(); }
    if (!stateKey(L, job->scratch) || job->stateWrites >= 8) return luaL_error(L, "State requires an installed skill, valid key and at most eight writes/job");
    job->json.clear(); int nodes = 0;
    if (!fromLua(L, 2, job->json["value"].to<JsonVariant>(), 0, nodes)) return luaL_error(L, "Invalid bounded state value");
    job->json["key"] = job->scratch;
    String key = keyFor(job->scratch), encoded; serializeJson(job->json, encoded);
    if (encoded.length() > 1200) { encoded = ""; key = ""; return luaL_error(L, "State record exceeds 1200 bytes"); }
    xSemaphoreTake(stateLock, portMAX_DELAY);
    JsonDocument index, previous;
    deserializeJson(index, runtimeState.getString("index", "[]"));
    bool listed = false; size_t total = encoded.length();
    for (JsonVariant entry : index.as<JsonArray>()) { String name = entry.as<String>(); if (name == key) listed = true; else total += runtimeState.getString(name.c_str(), "").length(); }
    String old = runtimeState.getString(key.c_str(), "");
    bool collision = old.length() && (deserializeJson(previous, old) || previous["key"].as<String>() != job->scratch);
    bool ok = !collision && total <= 8192 && (listed || index.size() < 24);
    if (ok) {
        if (!listed) index.add(key);
        String names; serializeJson(index, names);
        ok = runtimeState.putString(key.c_str(), encoded) == encoded.length() && runtimeState.putString("index", names) == names.length();
        if (ok) ++job->stateWrites;
    }
    xSemaphoreGive(stateLock);
    // Return an explicit success flag; callers can handle a full shared NVS partition.
    lua_pushboolean(L, ok); return 1;
}
int publishSnapshot(lua_State *L) {
    hardwareCall(L); Job *job = current(L); job->json.clear(); int nodes = 0;
    if (!fromLua(L, 1, job->json.to<JsonVariant>(), 0, nodes)) return luaL_error(L, "Snapshot exceeds JSON limits");
    job->scratch = ""; serializeJson(job->json, job->scratch);
    if (job->scratch.length() > 2048) return luaL_error(L, "Snapshot limit is 2048 bytes");
    xSemaphoreTake(lock, portMAX_DELAY); job->snapshot = job->scratch; xSemaphoreGive(lock); return 0;
}
bool deliverMessage(const String &id, const String &message) {
    if (message.length() > 512) return false;
    xSemaphoreTake(lock, portMAX_DELAY); bool accepted = false;
    for (auto &job : jobs) if (job.active && job.id == id && job.mailboxCount < 4) {
        job.mailbox[(job.mailboxHead + job.mailboxCount) % 4] = message; ++job.mailboxCount; accepted = true; break;
    }
    xSemaphoreGive(lock); return accepted;
}
int sendMessage(lua_State *L) {
    size_t size; const char *id = luaL_checklstring(L, 1, &size);
    if (!size || size > 48) return luaL_error(L, "Invalid destination job ID");
    hardwareCall(L); Job *job = current(L); job->json.clear(); int nodes = 0;
    if (!fromLua(L, 2, job->json.to<JsonVariant>(), 0, nodes)) return luaL_error(L, "Invalid message");
    job->scratch = ""; serializeJson(job->json, job->scratch);
    if (job->scratch.length() > 512) return luaL_error(L, "Message exceeds 512 bytes");
    bool accepted = deliverMessage(id, job->scratch); lua_pushboolean(L, accepted); return 1;
}
int receiveMessage(lua_State *L) {
    check(L); Job *job = current(L); xSemaphoreTake(lock, portMAX_DELAY); bool found = job->mailboxCount > 0;
    if (found) { job->scratch = job->mailbox[job->mailboxHead]; job->mailbox[job->mailboxHead] = ""; job->mailboxHead = (job->mailboxHead + 1) % 4; --job->mailboxCount; }
    xSemaphoreGive(lock);
    if (!found) { lua_pushnil(L); return 1; }
    job->json.clear(); if (deserializeJson(job->json, job->scratch)) return luaL_error(L, "Corrupt mailbox message");
    toLua(L, job->json.as<JsonVariantConst>()); return 1;
}
int hexEncode(lua_State *L) {
    size_t size; const unsigned char *data = reinterpret_cast<const unsigned char *>(luaL_checklstring(L, 1, &size));
    if (size > 512) return luaL_error(L, "Hex encoding limit is 512 bytes");
    luaL_Buffer buffer; char *out = luaL_buffinitsize(L, &buffer, size * 2);
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) { out[i*2] = digits[data[i] >> 4]; out[i*2+1] = digits[data[i] & 15]; }
    luaL_pushresultsize(&buffer, size * 2); return 1;
}
void releaseNativeExtensions(Job *job) {
    xSemaphoreTake(lock, portMAX_DELAY); int owner = job - jobs;
    if (uartOwner == owner) { externalUart.end(); uartOwner = -1; }
    for (int i = 0; i < 2; ++i) if (pwmChannels[i].owner == owner) {
        ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(4 + i), 0);
        gpio_reset_pin(static_cast<gpio_num_t>(pwmChannels[i].pin));
        pwmChannels[i].owner = pwmChannels[i].pin = -1;
    }
    xSemaphoreGive(lock);
}
uint64_t parsePinList(const char *pins) {
    uint64_t mask = 0;
    while (*pins) { char *end; long pin = strtol(pins, &end, 10); if (end == pins) break; if (pin >= 0 && pin < 64 && GPIO_IS_VALID_GPIO(pin)) mask |= 1ULL << pin; pins = *end == ',' ? end + 1 : end; }
    return mask;
}
void initNativeExtensions() {
    scanLock = xSemaphoreCreateMutex(); stateLock = xSemaphoreCreateMutex();
    runtimeState.begin("mcp_state", false); pinSettings.begin("mcp_pins", false);
#ifdef MCP_CONFIGURABLE_PINS
    configurablePins = parsePinList(MCP_CONFIGURABLE_PINS);
#elif defined(MCP_PROFILE_K10)
    configurablePins = parsePinList("1,2,3");
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    configurablePins = parsePinList("0,1,3,4,5,6,7,10");
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    configurablePins = parsePinList("0,1,2,3,6,7,10,11,14,18,19,20,21,22,23");
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    configurablePins = parsePinList("4,5,6,7,8,9,10,11,12,13,14,15,16,17,18");
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    configurablePins = parsePinList("4,5,6,7,8,9,10,11,12,13,14,15,16,17,18");
#elif defined(MCP_PROFILE_WROVER)
    configurablePins = parsePinList("4,13,14,18,19,21,22,23,25,26,27,32,33,34,35,36,39");
#else
    configurablePins = parsePinList("4,13,14,16,17,18,19,21,22,23,25,26,27,32,33,34,35,36,39");
#endif
    configurablePins |= parsePinList(MCP_ALLOWED_PINS);
    allowedPins = pinSettings.getULong64("allowed", parsePinList(MCP_ALLOWED_PINS)) & configurablePins;
}
const luaL_Reg deviceApi[] = {
    {"sleep_ms", sleepMs}, {"millis", clockMillis}, {"micros", clockMicros}, {"random", randomValue},
    {"pin_mode", mode}, {"digital_write", writePin}, {"digital_read", readPin},
    {"analog_read", readAdc}, {"analog_read_mv", readMillivolts}, {"pulse_in", pulseValue},
    {"pwm_write", pwmWrite}, {"pwm_stop", pwmStop},
    {"info", info}, {"run", runProgram}, {"status", statusProgram},
    {"wifi_info", wifiInfo}, {"wifi_scan", wifiScan},
    {"i2c_transfer", i2cTransfer}, {"i2c_scan", i2cScan}, {"spi_transfer", spiTransfer},
    {"uart_open", uartOpen}, {"uart_write", uartWrite}, {"uart_read", uartRead}, {"uart_close", uartClose},
    {"state_get", stateGet}, {"state_set", stateSet}, {"publish", publishSnapshot}, {"send_message", sendMessage}, {"receive_message", receiveMessage}, {"hex", hexEncode}, {nullptr, nullptr}
};
