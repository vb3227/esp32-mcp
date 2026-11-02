#include "native_runtime.h"
#include "device_config.h"
#ifdef MCP_PROFILE_K10
#include "voice_gate.h"
#endif
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <HardwareSerial.h>
#include <esp_wifi.h>
#include <driver/ledc.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "native_platform.h"
#include "relay_root_ca.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#ifndef MCP_ALLOWED_PINS
#define MCP_ALLOWED_PINS ""
#endif
#ifndef MCP_PROFILE
#define MCP_PROFILE "esp32-generic"
#endif

namespace {
constexpr size_t MAX_SOURCE = 4096, MAX_LOG = 1200, VM_BYTES = 65536;
constexpr uint32_t MAX_INSTRUCTIONS = 2000000;
constexpr size_t SLOTS = 4;
struct Job {
    String id, source, status, error, output, result, scratch, skillName, snapshot;
    String mailbox[4]; unsigned mailboxHead = 0, mailboxCount = 0;
    JsonDocument json;
    std::atomic<bool> cancel{false};
    bool active = false;
    bool background = false;
    bool voiceBound = false;
    uint32_t voiceEpoch = 0;
    uint32_t deadline = 0, instructions = 0, calls = 0;
    uint32_t started = 0, instructionWindow = 0, callWindow = 0, stateWindow = 0;
    std::atomic<uint32_t> heartbeat{0};
    size_t memory = 0, peak = 0;
    uint64_t pins = 0, outputs = 0;
    unsigned stateWrites = 0;
};
Job jobs[SLOTS];
SemaphoreHandle_t lock = nullptr, busLock = nullptr;
Preferences storage;
bool storageReady = false;
uint64_t allowedPins = 0;
int8_t owners[64];
uint32_t sequence = 0;
#ifdef MCP_PROFILE_K10
// Keep the built-in sensors/display controllers untouched.
TwoWire &externalWire = Wire1;
SPIClass externalSpi(HSPI);
#else
TwoWire &externalWire = Wire;
SPIClass &externalSpi = SPI;
#endif

Job *current(lua_State *L) { return *static_cast<Job **>(lua_getextraspace(L)); }
void *allocate(void *ud, void *ptr, size_t oldSize, size_t newSize) {
    Job *job = static_cast<Job *>(ud);
    const size_t previous = ptr ? oldSize : 0;
    if (!newSize) { free(ptr); job->memory -= previous; return nullptr; }
    if (newSize > VM_BYTES || job->memory - previous > VM_BYTES - newSize || (newSize > previous && ESP.getFreeHeap() < 32768 + newSize - previous)) return nullptr;
    void *next = ESP.getPsramSize() ? heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : realloc(ptr, newSize);
    if (next) { job->memory = job->memory - previous + newSize; job->peak = max(job->peak, job->memory); }
    return next;
}
void check(lua_State *L) {
    Job *job = current(L);
#ifdef MCP_PROFILE_K10
    if (job->voiceBound && !cloudVoiceEpochValid(job->voiceEpoch)) luaL_error(L, "Voice request superseded by local command");
#endif
    if (job->cancel.load()) luaL_error(L, "Script cancelled");
    job->heartbeat.store(millis());
    if (!job->background && static_cast<int32_t>(millis() - job->deadline) >= 0) luaL_error(L, "Script timed out");
}
void hook(lua_State *L, lua_Debug *) {
    Job *job = current(L); job->instructions += 1000;
    check(L);
    if (job->background) {
        if (millis() - job->instructionWindow >= 10000) { job->instructionWindow = millis(); job->instructions = 0; }
        if (job->instructions >= MAX_INSTRUCTIONS) {
            // Throttle only this task. Never delete a VM while it owns resources.
            while (millis() - job->instructionWindow < 10000) { check(L); vTaskDelay(max((TickType_t)1, pdMS_TO_TICKS(10))); }
            job->instructionWindow = millis(); job->instructions = 0;
        }
    } else if (job->instructions > MAX_INSTRUCTIONS) luaL_error(L, "Instruction budget exceeded");
    vTaskDelay(max((TickType_t)1, pdMS_TO_TICKS(job->background ? 3 : 1)));
}
void hardwareCall(lua_State *L) {
    check(L); Job *job = current(L);
    if (job->background && millis() - job->callWindow >= 10000) { job->calls = 0; job->callWindow = millis(); }
    if (job->background && job->calls >= 128) {
        while (millis() - job->callWindow < 10000) { check(L); vTaskDelay(max((TickType_t)1, pdMS_TO_TICKS(10))); }
        job->calls = 0; job->callWindow = millis();
    }
    if (++job->calls > 128) luaL_error(L, "Hardware-call budget exceeded");
}
bool pinAllowed(int pin) { return pin >= 0 && pin < 64 && GPIO_IS_VALID_GPIO(pin) && (allowedPins & (1ULL << pin)); }
void claim(lua_State *L, int pin) {
    check(L);
    if (!pinAllowed(pin)) luaL_error(L, "Pin is not in MCP_ALLOWED_PINS");
    Job *job = current(L); int index = static_cast<int>(job - jobs);
    xSemaphoreTake(lock, portMAX_DELAY);
    bool busy = owners[pin] >= 0 && owners[pin] != index;
    if (!busy) { owners[pin] = index; job->pins |= 1ULL << pin; }
    xSemaphoreGive(lock);
    if (busy) luaL_error(L, "Pin belongs to another running script");
}
int printValue(lua_State *L) {
    Job *job = current(L);
    const int count = min(lua_gettop(L), 32);
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const char *text = luaL_tolstring(L, i, &length);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (i > 1 && job->output.length() < MAX_LOG) job->output += ' ';
        for (size_t j = 0; j < length && job->output.length() < MAX_LOG; ++j) job->output += text[j];
        xSemaphoreGive(lock);
        lua_pop(L, 1);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (job->output.length() < MAX_LOG) job->output += '\n';
    xSemaphoreGive(lock);
    return 0;
}
int sleepMs(lua_State *L) {
    lua_Integer duration = luaL_checkinteger(L, 1);
    if (duration < 0 || duration > 5000) return luaL_error(L, "sleep_ms must be 0..5000");
    uint32_t until = millis() + static_cast<uint32_t>(duration);
    do { check(L); vTaskDelay(1); } while (static_cast<int32_t>(millis() - until) < 0);
    return 0;
}
int mode(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    const char *value = luaL_checkstring(L, 2);
    int setting = !strcmp(value, "output") ? OUTPUT : !strcmp(value, "input") ? INPUT : !strcmp(value, "pullup") ? INPUT_PULLUP : -1;
    if (setting < 0) return luaL_error(L, "Expected input, output or pullup");
    if (setting == OUTPUT && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return luaL_error(L, "Pin cannot be an output");
    claim(L, pin); pinMode(pin, setting);
    if (setting == OUTPUT) current(L)->outputs |= 1ULL << pin;
    return 0;
}
int writePin(lua_State *L) {
    int pin = luaL_checkinteger(L, 1), value = luaL_checkinteger(L, 2);
    if (value != 0 && value != 1) return luaL_error(L, "Digital value must be 0 or 1");
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return luaL_error(L, "Pin cannot be an output");
    claim(L, pin); pinMode(pin, OUTPUT); current(L)->outputs |= 1ULL << pin;
    digitalWrite(pin, value); return 0;
}
int readPin(lua_State *L) { int pin = luaL_checkinteger(L, 1); claim(L, pin); lua_pushinteger(L, digitalRead(pin)); return 1; }
int readAdc(lua_State *L) { int pin = luaL_checkinteger(L, 1); claim(L, pin); lua_pushinteger(L, analogRead(pin)); return 1; }
int info(lua_State *L) {
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, ESP.getFreeHeap()); lua_setfield(L, -2, "heap_free");
    lua_pushinteger(L, millis()); lua_setfield(L, -2, "uptime_ms");
    lua_pushstring(L, MCP_PROFILE); lua_setfield(L, -2, "profile");
    return 1;
}
bool fromLua(lua_State *L, int index, JsonVariant out, int depth, int &nodes) {
    if (++nodes > 128 || depth > 6) return false;
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TNIL: out.set(nullptr); return true;
        case LUA_TBOOLEAN: out.set(static_cast<bool>(lua_toboolean(L, index))); return true;
        case LUA_TNUMBER: {
            double value = lua_tonumber(L, index); if (!std::isfinite(value)) return false;
            if (lua_isinteger(L, index)) out.set(static_cast<int64_t>(lua_tointeger(L, index))); else out.set(value);
            return true;
        }
        case LUA_TSTRING: { size_t size; const char *value = lua_tolstring(L, index, &size); if (size > 1024 || memchr(value, 0, size) || (nodes += static_cast<int>(size / 16)) > 128) return false; out.set(value); return true; }
        case LUA_TTABLE: {
            size_t size = lua_rawlen(L, index);
            if (size > 128) return false;
            if (size) {
                JsonArray array = out.to<JsonArray>();
                for (size_t i = 1; i <= size; ++i) { lua_rawgeti(L, index, i); bool ok = fromLua(L, -1, array.add<JsonVariant>(), depth + 1, nodes); lua_pop(L, 1); if (!ok) return false; }
                return true;
            }
            JsonObject object = out.to<JsonObject>(); lua_pushnil(L);
            while (lua_next(L, index)) {
                if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 2); return false; }
                size_t keySize = 0;
                const char *name = lua_tolstring(L, -2, &keySize);
                if (keySize > 63 || memchr(name, 0, keySize) || (nodes += static_cast<int>(keySize / 16)) > 128) { lua_pop(L, 2); return false; }
                // Materialize the member before taking JsonVariant. Converting an
                // absent MemberProxy directly yields an unbound, unwritable variant.
                bool ok = fromLua(L, -1, object[name].to<JsonVariant>(), depth + 1, nodes); lua_pop(L, 1);
                if (!ok) { lua_pop(L, 1); return false; }
            }
            return true;
        }
        default: return false;
    }
}
void toLua(lua_State *L, JsonVariantConst value, int depth = 0) {
    if (depth > 6 || value.isNull()) lua_pushnil(L);
    else if (value.is<bool>()) lua_pushboolean(L, value.as<bool>());
    else if (value.is<int64_t>()) lua_pushinteger(L, value.as<int64_t>());
    else if (value.is<double>()) lua_pushnumber(L, value.as<double>());
    else if (value.is<const char *>()) lua_pushstring(L, value.as<const char *>());
    else if (value.is<JsonArrayConst>()) {
        lua_newtable(L); int i = 1; for (JsonVariantConst child : value.as<JsonArrayConst>()) { toLua(L, child, depth + 1); lua_rawseti(L, -2, i++); }
    } else {
        lua_newtable(L); for (JsonPairConst child : value.as<JsonObjectConst>()) { toLua(L, child.value(), depth + 1); lua_setfield(L, -2, child.key().c_str()); }
    }
}
bool localRequest(Job *job, const String &path, const String &body, bool post) {
    HTTPClient client; client.setTimeout(1500);
    if (!client.begin(String("http://127.0.0.1:") + MAIN_PORT + path)) return false;
    client.addHeader("X-Board-Token", BOARD_TOKEN);
    if (job->voiceBound) {
        client.addHeader("X-Voice-Epoch", String(job->voiceEpoch));
    }
    client.addHeader("Content-Type", "application/json");
    int code = post ? client.POST(body) : client.GET();
    job->scratch = client.getString(); client.end();
    return code >= 200 && code < 300 && job->scratch.length() <= 8192;
}
int runProgram(lua_State *L) {
    hardwareCall(L); Job *job = current(L);
    job->json.clear(); int nodes = 0;
    if (!fromLua(L, 1, job->json.to<JsonVariant>(), 0, nodes)) return luaL_error(L, "Invalid or oversized JSON program");
    job->scratch = ""; serializeJson(job->json, job->scratch);
    if (job->scratch.length() > 4096) return luaL_error(L, "Program exceeds 4096 bytes");
    bool ok = localRequest(job, "/run", job->scratch, true);
    if (!ok || deserializeJson(job->json, job->scratch)) return luaL_error(L, "Device rejected program; do not replay automatically");
    toLua(L, job->json.as<JsonVariantConst>()); return 1;
}
int statusProgram(lua_State *L) {
    size_t length; const char *id = luaL_checklstring(L, 1, &length);
    if (!length || length > 48) return luaL_error(L, "Invalid job ID");
    for (size_t i = 0; i < length; ++i) if (!isalnum(id[i]) && id[i] != '-') return luaL_error(L, "Invalid job ID");
    hardwareCall(L); Job *job = current(L);
    bool ok = localRequest(job, String("/status?job_id=") + id, "", false);
    if (!ok || deserializeJson(job->json, job->scratch)) return luaL_error(L, "Device job status unavailable");
    toLua(L, job->json.as<JsonVariantConst>()); return 1;
}
int i2cTransfer(lua_State *L) {
    int sda = luaL_checkinteger(L, 1), scl = luaL_checkinteger(L, 2), address = luaL_checkinteger(L, 3);
    size_t size; const char *data = luaL_checklstring(L, 4, &size);
    int receive = luaL_optinteger(L, 5, 0);
    if (sda == scl || !GPIO_IS_VALID_OUTPUT_GPIO(sda) || !GPIO_IS_VALID_OUTPUT_GPIO(scl) || address < 8 || address > 119 || size > 128 || receive < 0 || receive > 128) return luaL_error(L, "Invalid I2C transaction");
    claim(L, sda); claim(L, scl); check(L);
    uint8_t bytes[128]; size_t count = 0;
    xSemaphoreTake(busLock, portMAX_DELAY);
    bool opened = externalWire.begin(sda, scl); externalWire.setTimeOut(50); externalWire.beginTransmission(address);
    externalWire.write(reinterpret_cast<const uint8_t *>(data), size);
    uint8_t error = externalWire.endTransmission(receive == 0);
    if (!error && receive) { externalWire.requestFrom(address, receive); while (externalWire.available() && count < sizeof(bytes)) bytes[count++] = externalWire.read(); }
    externalWire.end();
    xSemaphoreGive(busLock);
    if (!opened || error) return luaL_error(L, "I2C transaction failed");
    lua_pushlstring(L, reinterpret_cast<const char *>(bytes), count); return 1;
}
extern std::atomic<int> sdOwner;
int spiTransfer(lua_State *L) {
    if (sdOwner >= 0) return luaL_error(L, "Close SD before raw SPI access");
    int sck = luaL_checkinteger(L, 1), miso = luaL_checkinteger(L, 2), mosi = luaL_checkinteger(L, 3), cs = luaL_checkinteger(L, 4);
    size_t size; const char *data = luaL_checklstring(L, 5, &size);
    int frequency = luaL_optinteger(L, 6, 1000000), spiMode = luaL_optinteger(L, 7, 0);
    if (frequency < 10000 || frequency > 10000000 || spiMode < 0 || spiMode > 3) return luaL_error(L, "SPI frequency 10000..10000000 Hz, mode 0..3");
    if (size > 128 || cs < -1 || sck == miso || sck == mosi || (cs >= 0 && (sck == cs || miso == cs || mosi == cs)) || miso == mosi || !GPIO_IS_VALID_OUTPUT_GPIO(sck) || !GPIO_IS_VALID_OUTPUT_GPIO(mosi) || (cs >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(cs))) return luaL_error(L, "Invalid SPI pins or payload");
    claim(L, sck); claim(L, miso); claim(L, mosi); if (cs >= 0) claim(L, cs); check(L);
    uint8_t bytes[128];
    xSemaphoreTake(busLock, portMAX_DELAY);
    externalSpi.begin(sck, miso, mosi, cs); if (cs >= 0) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
    externalSpi.beginTransaction(SPISettings(frequency, MSBFIRST, spiMode)); if (cs >= 0) digitalWrite(cs, LOW);
    for (size_t i = 0; i < size; ++i) bytes[i] = externalSpi.transfer(static_cast<uint8_t>(data[i]));
    if (cs >= 0) digitalWrite(cs, HIGH); externalSpi.endTransaction(); externalSpi.end();
    xSemaphoreGive(busLock);
    lua_pushlstring(L, reinterpret_cast<const char *>(bytes), size); return 1;
}
String keyFor(const String &name);
#include "native_api_extensions.h"
#include "native_hardware_io.h"
#include "native_network_io.h"
#include "native_interfaces.h"

void execute(void *arg) {
    Job *job = static_cast<Job *>(arg);
    lua_State *L = lua_newstate(allocate, job);
    String error, result;
    if (!L) error = "Insufficient memory for Lua state";
    else {
        *static_cast<Job **>(lua_getextraspace(L)) = job;
        // All library setup runs protected: allocator failure must not panic/reset the MCU.
        lua_CFunction initialize = [](lua_State *state) -> int {
            luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
            luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
            luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
            luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
            for (const char *name : {"load", "loadfile", "dofile", "collectgarbage", "pcall", "xpcall", "setmetatable", "getmetatable", "warn"}) { lua_pushnil(state); lua_setglobal(state, name); }
            lua_pushcfunction(state, printValue); lua_setglobal(state, "print");
            lua_newtable(state);
            luaL_setfuncs(state, deviceApi, 0);
            luaL_setfuncs(state, hardwareIoApi, 0); luaL_setfuncs(state, networkIoApi, 0);
            lua_setglobal(state, "device"); return 0;
        };
        lua_pushcfunction(L, initialize);
        int status = lua_pcall(L, 0, 0, 0);
        if (status == LUA_OK) {
            lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
            status = luaL_loadbufferx(L, job->source.c_str(), job->source.length(), "skill", "t");
            if (status == LUA_OK) status = lua_pcall(L, 0, 1, 0);
        }
        if (status != LUA_OK) error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua runtime error";
        else {
            job->json.clear(); int nodes = 0;
            if (!fromLua(L, -1, job->json.to<JsonVariant>(), 0, nodes)) error = "Result exceeds JSON limits";
            else { serializeJson(job->json, result); if (result.length() > 4096) { result = ""; error = "Result exceeds 4096 bytes"; } }
        }
        lua_close(L);
    }
    job->json.clear(); job->scratch = "";
    releaseNetworkIo(job);
    releaseHardwareIo(job);
    releaseNativeExtensions(job);
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int pin = 0; pin < 64; ++pin) if (job->pins & (1ULL << pin)) { if (job->outputs & (1ULL << pin)) digitalWrite(pin, LOW); pinMode(pin, INPUT); owners[pin] = -1; }
    job->error = error; job->result = result;
    job->status = job->cancel.load() ? "stopped" : error.length() ? "error" : "done";
    job->source = ""; job->active = false;
    xSemaphoreGive(lock);
    vTaskDelete(nullptr);
}
bool auth(WebServer &server) {
    const String given = server.header("X-Board-Token");
    size_t expected = strlen(BOARD_TOKEN); unsigned diff = given.length() ^ expected;
    for (size_t i = 0; i < expected; ++i) diff |= static_cast<unsigned char>(BOARD_TOKEN[i]) ^ (i < given.length() ? static_cast<unsigned char>(given[i]) : 0);
    if (!expected || diff) { server.send(403, "application/json", "{\"error\":\"Unauthorized\"}"); return false; }
#ifdef MCP_PROFILE_K10
    if (server.hasHeader("X-Voice-Epoch")) {
        String epoch = server.header("X-Voice-Epoch"); char *end = nullptr;
        uint32_t parsed = strtoul(epoch.c_str(), &end, 10);
        if (!epoch.length() || !end || *end || !cloudVoiceEpochValid(parsed)) {
            server.send(409, "application/json", "{\"error\":\"Stale voice request\"}"); return false;
        }
    }
#endif
    return true;
}
void send(WebServer &server, JsonDocument &reply, int code = 200) { String text; serializeJson(reply, text); server.send(code, "application/json", text); }
bool body(WebServer &server, JsonDocument &request) {
    if (!auth(server)) return false;
    if (server.arg("plain").length() > 8192 || deserializeJson(request, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"Invalid bounded JSON body\"}"); return false; }
    return true;
}
bool validName(const String &name) {
    if (!name.length() || name.length() > 31) return false;
    for (char c : name) if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    return true;
}
String keyFor(const String &name) {
    unsigned char hash[32];
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(name.c_str()), name.length(), hash, 0);
#else
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(name.c_str()), name.length(), hash, 0);
#endif
    char key[14] = {'s'}; for (int i = 0; i < 6; ++i) snprintf(key + 1 + i * 2, 3, "%02x", hash[i]);
    return String(key);
}
}

void initNativeRuntime() {
    lock = xSemaphoreCreateMutex(); busLock = xSemaphoreCreateMutex(); memset(owners, -1, sizeof(owners));
    storageReady = storage.begin("mcp_lua", false);
    initNativeExtensions();
    peripheralLock = xSemaphoreCreateMutex();
    initHardwareIo();
    espNowQueue = xQueueCreate(4, sizeof(EspNowPacket));

}
bool startNativeScript(const String &source, uint32_t timeoutMs, JsonDocument &reply, bool voiceBound, uint32_t voiceEpoch, const String &skillName, bool background) {
    if (!lock || !busLock || !scanLock || !stateLock || !peripheralLock || !source.length() || source.length() > MAX_SOURCE || timeoutMs < 10 || timeoutMs > 60000) { reply["error"] = "Invalid source, timeout or runtime allocation"; return false; }
    if (ESP.getPsramSize() ? (ESP.getFreeHeap() < (background ? 65536 : 49152) || ESP.getFreePsram() < VM_BYTES) : ESP.getFreeHeap() < VM_BYTES + (background ? 65536 : 32768)) {
        reply["error"] = "Insufficient free memory for another bounded Lua job"; return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY); Job *job = nullptr;
    unsigned backgrounds = 0; for (auto &candidate : jobs) if (candidate.active && candidate.background) ++backgrounds;
    if (background && backgrounds >= SLOTS - 1) { xSemaphoreGive(lock); reply["error"] = "At most three background jobs; one slot is reserved for foreground control"; return false; }
    for (auto &candidate : jobs) if (!candidate.active) { job = &candidate; break; }
    if (!job) { xSemaphoreGive(lock); reply["error"] = "All native script slots are busy"; return false; }
    job->id = String(esp_random(), HEX) + '-' + String(++sequence); job->source = source;
    job->status = "running"; job->error = ""; job->result = ""; job->output = "";
    job->voiceBound = voiceBound; job->voiceEpoch = voiceEpoch;
    job->skillName = skillName; job->stateWrites = 0;
    job->snapshot = ""; job->mailboxHead = job->mailboxCount = 0; for (auto &message : job->mailbox) message = "";
    job->background = background; job->started = job->instructionWindow = job->callWindow = job->stateWindow = millis();
    job->heartbeat.store(millis());
    job->cancel = false; job->active = true; job->memory = job->peak = 0;
    job->pins = job->outputs = 0; job->instructions = job->calls = 0; job->deadline = millis() + timeoutMs;
    bool started = xTaskCreate(execute, "mcp-lua", 12288, job, 1, nullptr) == pdPASS;
    if (!started) { job->active = false; job->status = "error"; job->error = "Could not allocate Lua task"; }
    reply["job_id"] = job->id; reply["status"] = job->status; reply["location"] = "device"; reply["language"] = "lua";
    reply["background"] = background;
    xSemaphoreGive(lock); return started;
}
bool readNativeJob(const String &id, JsonDocument &reply) {
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool found = false;
    for (auto &job : jobs) if (job.id == id && id.length()) {
        found = true; reply["job_id"] = id; reply["status"] = job.status;
        reply["error"] = job.error; reply["stdout"] = job.output;
        reply["background"] = job.background; reply["skill"] = job.skillName;
        reply["elapsed_ms"] = millis() - job.started; reply["heartbeat_age_ms"] = millis() - job.heartbeat.load();
        reply["memory_peak_bytes"] = job.active ? 0 : job.peak;
        if (job.snapshot.length()) { JsonDocument value; if (!deserializeJson(value, job.snapshot)) reply["snapshot"].set(value); }
        if (!job.active && job.result.length()) { JsonDocument value; if (!deserializeJson(value, job.result)) reply["result"].set(value); }
        break;
    }
    xSemaphoreGive(lock); return found;
}
bool stopNativeJob(const String &id) {
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool found = false;
    for (auto &job : jobs) if (job.id == id && id.length()) { job.cancel = true; found = true; break; }
    xSemaphoreGive(lock); return found;
}
void writeNativeCapabilities(JsonDocument &reply) {
    reply["profile"] = MCP_PROFILE; reply["language"] = "lua"; reply["lua_version"] = LUA_RELEASE;
    reply["max_source_bytes"] = MAX_SOURCE; reply["lua_heap_bytes"] = VM_BYTES;
    reply["max_runtime_ms"] = 60000; reply["max_jobs"] = SLOTS; reply["storage_ready"] = storageReady;
    reply["api_version"] = 3; reply["max_background_jobs"] = SLOTS - 1;
    reply["background_execution"] = true; reply["background_instruction_window_ms"] = 10000;
    reply["native_state_bytes"] = 8192; reply["state_writes_per_job"] = 8;
    JsonArray ceiling = reply["configurable_pins"].to<JsonArray>();
    for (int i = 0; i < 64; ++i) if (configurablePins & (1ULL << i)) ceiling.add(i);
    reply["native_execution"] = true; reply["heap_free"] = ESP.getFreeHeap();
    JsonArray pins = reply["allowed_pins"].to<JsonArray>(); for (int i = 0; i < 64; ++i) if (pinAllowed(i)) pins.add(i);
    JsonArray api = reply["api"].to<JsonArray>();
    for (const luaL_Reg *table : {deviceApi, hardwareIoApi, networkIoApi})
        for (const luaL_Reg *entry = table; entry->name; ++entry) api.add(entry->name);
    writeInterfaceCapabilities(reply);
}
void attachNativeRuntime(WebServer &server) {
    server.on("/capabilities", HTTP_GET, [&server]() {
        if (!auth(server)) return;
        JsonDocument reply;
        writeNativeCapabilities(reply);
        reply["features"]["gpio"] = true; reply["features"]["adc"] = true;
        reply["features"]["i2c"] = true; reply["features"]["spi"] = true;
        for (const char *name : {"pwm", "uart", "wifi_scan", "skill_state", "runtime_pin_policy"}) reply["features"][name] = true;
#ifdef MCP_PROFILE_K10
        for (const char *name : {"camera", "display", "voice", "rgb", "sensors", "local_asr", "media_capture", "activity_overlay"}) reply["features"][name] = true;
#else
        for (const char *name : {"camera", "display", "voice", "rgb", "local_asr", "media_capture", "activity_overlay"}) reply["features"][name] = false;
#endif
        send(server, reply);
    });
    server.on("/runtime/pins", HTTP_GET, [&server]() { if (!auth(server)) return; JsonDocument reply; writeNativeCapabilities(reply); send(server, reply); });
    server.on("/runtime/pins", HTTP_POST, [&server]() {
        JsonDocument request, reply; if (!body(server, request)) return;
        JsonArrayConst pins = request["pins"].as<JsonArrayConst>(); uint64_t mask = 0;
        if (pins.isNull() || pins.size() > 40) { reply["error"] = "Expected at most 40 explicit GPIO numbers"; send(server, reply, 400); return; }
        for (JsonVariantConst pin : pins) {
            if (!pin.is<int>() || pin.is<bool>() || pin.as<int>() < 0 || pin.as<int>() >= 64 || !(configurablePins & (1ULL << pin.as<int>()))) {
                reply["error"] = "Pin is outside this board profile's configurable ceiling"; send(server, reply, 400); return;
            }
            mask |= 1ULL << pin.as<int>();
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        bool busy = false; for (auto &job : jobs) if (job.active) busy = true;
        bool saved = !busy && pinSettings.putULong64("allowed", mask) == sizeof(uint64_t);
        if (saved) allowedPins = mask;
        xSemaphoreGive(lock);
        if (!saved) { reply["error"] = busy ? "Stop native jobs before changing pin policy" : "NVS policy write failed"; send(server, reply, busy ? 409 : 507); return; }
        writeNativeCapabilities(reply); send(server, reply);
    });
    server.on("/runtime/capabilities", HTTP_GET, [&server]() { if (!auth(server)) return; JsonDocument reply; writeNativeCapabilities(reply); send(server, reply); });
    server.on("/runtime/execute", HTTP_POST, [&server]() {
        JsonDocument request, reply; if (!body(server, request)) return;
        if (!request["background"].isNull() && !request["background"].is<bool>()) { reply["error"] = "background must be boolean"; send(server, reply, 400); return; }
        bool ok = startNativeScript(request["source"] | "", request["timeout_ms"] | 10000U, reply, server.hasHeader("X-Voice-Epoch"), strtoul(server.header("X-Voice-Epoch").c_str(), nullptr, 10), "", request["background"] | false);
        send(server, reply, ok ? 202 : 400);
    });
    server.on("/runtime/jobs", HTTP_GET, [&server]() {
        if (!auth(server)) return; JsonDocument reply; JsonArray records = reply["jobs"].to<JsonArray>();
        xSemaphoreTake(lock, portMAX_DELAY);
        for (auto &job : jobs) if (job.id.length()) {
            JsonObject item = records.add<JsonObject>(); item["job_id"] = job.id;
            item["status"] = job.status; item["background"] = job.background; item["skill"] = job.skillName;
            item["elapsed_ms"] = millis() - job.started; item["heartbeat_age_ms"] = millis() - job.heartbeat.load();
        }
        xSemaphoreGive(lock); send(server, reply);
    });
    server.on("/runtime/status", HTTP_GET, [&server]() { if (!auth(server)) return; JsonDocument reply; if (!readNativeJob(server.arg("job_id"), reply)) { reply["error"] = "Unknown or expired job ID"; send(server, reply, 404); return; } send(server, reply); });
    server.on("/runtime/message", HTTP_POST, [&server]() {
        JsonDocument request, reply; if (!body(server, request)) return;
        if (request["data"].isNull()) { reply["error"] = "Non-null data is required"; send(server, reply, 400); return; }
        String message; serializeJson(request["data"], message);
        bool ok = deliverMessage(request["job_id"] | "", message); reply["accepted"] = ok;
        if (!ok) reply["error"] = "Destination absent, mailbox full or message exceeds 512 bytes";
        send(server, reply, ok ? 200 : 409);
    });
    server.on("/runtime/stop", HTTP_POST, [&server]() { JsonDocument request, reply; if (!body(server, request)) return; bool ok = stopNativeJob(request["job_id"] | ""); reply["stop_requested"] = ok; send(server, reply, ok ? 200 : 404); });
    server.on("/runtime/skills/save", HTTP_POST, [&server]() {
        JsonDocument request, reply; if (!body(server, request)) return;
        if (!request["background"].isNull() && !request["background"].is<bool>()) { reply["error"] = "background must be boolean"; send(server, reply, 400); return; }
        String name = request["name"] | "", source = request["source"] | "";
        if (!storageReady || !validName(name) || !source.length() || source.length() > MAX_SOURCE) { reply["error"] = "Invalid skill or unavailable NVS"; send(server, reply, 400); return; }
        String key = keyFor(name), previous = storage.getString(key.c_str(), ""); JsonDocument old;
        if (previous.length() && (deserializeJson(old, previous) || old["name"].as<String>() != name)) { reply["error"] = "Storage key collision"; send(server, reply, 409); return; }
        JsonDocument index; deserializeJson(index, storage.getString("index", "[]"));
        size_t total = 0; bool listed = false;
        for (JsonVariant item : index.as<JsonArray>()) { String n = item.as<String>(); if (n == name) listed = true; else total += storage.getString(keyFor(n).c_str(), "").length(); }
        String encoded; request["language"] = "lua"; serializeJson(request, encoded);
        if ((!listed && index.size() >= 12) || total + encoded.length() > 16384) { reply["error"] = "Native skill storage quota exceeded"; send(server, reply, 413); return; }
        if (!listed) index.add(name);
        String names;
        serializeJson(index, names);
        bool ok = storage.putString(key.c_str(), encoded) == encoded.length() && storage.putString("index", names) == names.length() && storage.getString(key.c_str(), "") == encoded;
        reply["stored"] = ok; reply["name"] = name; if (!ok) reply["error"] = "NVS write failed";
        send(server, reply, ok ? 200 : 507);
    });
    server.on("/runtime/skills/list", HTTP_GET, [&server]() { if (!auth(server)) return; JsonDocument reply; JsonDocument index; deserializeJson(index, storage.getString("index", "[]")); reply["skills"].set(index); send(server, reply); });
    server.on("/runtime/skills/get", HTTP_POST, [&server]() { JsonDocument request, reply; if (!body(server, request)) return; String name = request["name"] | ""; if (!validName(name) || deserializeJson(reply, storage.getString(keyFor(name).c_str(), "")) || reply["name"].as<String>() != name) { reply.clear(); reply["error"] = "Unknown skill"; send(server, reply, 404); return; } send(server, reply); });
    server.on("/runtime/skills/run", HTTP_POST, [&server]() { JsonDocument request, saved, reply; if (!body(server, request)) return; String name = request["name"] | ""; if (!validName(name) || deserializeJson(saved, storage.getString(keyFor(name).c_str(), "")) || saved["name"].as<String>() != name) { reply["error"] = "Unknown skill"; send(server, reply, 404); return; } bool ok = startNativeScript(saved["source"] | "", request["timeout_ms"] | 10000U, reply, server.hasHeader("X-Voice-Epoch"), strtoul(server.header("X-Voice-Epoch").c_str(), nullptr, 10), name, request["background"] | (saved["background"] | false)); send(server, reply, ok ? 202 : 400); });
}
