// Bounded network transports. Every socket belongs to one VM/job slot.
struct NativeSockets {
    WiFiClient tcp, mqtt, httpPlain;
    WiFiClientSecure https;
    WiFiUDP udp;
    HTTPClient http;
    bool udpOpen = false;
    uint8_t mqttBuffer[1024] = {};
    size_t mqttBytes = 0;
    uint16_t packetId = 0;
    uint32_t mqttLast = 0;
};
NativeSockets sockets[SLOTS];
struct EspNowPacket { uint8_t mac[6]; uint16_t size; uint8_t data[200]; };
QueueHandle_t espNowQueue = nullptr;
std::atomic<int> espNowOwner{-1}, bleOwner{-1};
#if MCP_HAS_BLE
BLECharacteristic *bleCharacteristic = nullptr;
bool bleStarted = false;
#ifdef MCP_NIMBLE
constexpr bool BLE_RESET_ARGUMENT = true;  // Delete GATT objects; permits later init.
#else
constexpr bool BLE_RESET_ARGUMENT = false; // Vendor true permanently releases controller RAM.
#endif
#endif

NativeSockets &socketState(lua_State *L) { return sockets[current(L) - jobs]; }
void hostAddress(lua_State *L, int index, IPAddress &address) {
    const char *text = luaL_checkstring(L, index);
    if (strlen(text) > 15 || !address.fromString(text) || address[0] == 0 || address[0] >= 224 || address[3] == 255) luaL_error(L, "Use an explicit unicast IPv4 address");
}
int checkedPort(lua_State *L, int index) { int port = luaL_checkinteger(L, index); if (port < 1 || port > 65535) luaL_error(L, "Port must be 1..65535"); return port; }
int tcpOpen(lua_State *L) {
    IPAddress address; hostAddress(L, 1, address); int port = checkedPort(L, 2); hardwareCall(L);
    auto &state = socketState(L); state.tcp.stop(); state.tcp.setTimeout(250);
    bool ok = state.tcp.connect(address, port, 1500); check(L);
    if (!ok) return luaL_error(L, "TCP connection failed"); return 0;
}
int tcpWrite(lua_State *L) {
    size_t size; const char *data = luaL_checklstring(L, 1, &size); check(L);
    if (size > 512 || !socketState(L).tcp.connected()) return luaL_error(L, "Open TCP first; payload <=512 bytes");
    lua_pushinteger(L, socketState(L).tcp.write(reinterpret_cast<const uint8_t *>(data), size)); return 1;
}
int tcpRead(lua_State *L) {
    int size = luaL_optinteger(L, 1, 256), timeout = luaL_optinteger(L, 2, 100);
    if (size < 1 || size > 512 || timeout < 0 || timeout > 1000) return luaL_error(L, "TCP read 1..512 bytes, timeout 0..1000 ms");
    uint8_t data[512]; int count = 0; auto &client = socketState(L).tcp; uint32_t started = millis();
    do { check(L); while (client.available() && count < size) data[count++] = client.read();
        if (count || !client.connected() || millis() - started >= static_cast<uint32_t>(timeout)) break; vTaskDelay(1);
    } while (true);
    lua_pushlstring(L, reinterpret_cast<const char *>(data), count); return 1;
}
int tcpClose(lua_State *L) { socketState(L).tcp.stop(); return 0; }
int udpOpen(lua_State *L) {
    int port = luaL_optinteger(L, 1, 0); if (port < 0 || port > 65535) return luaL_error(L, "UDP port 0..65535");
    auto &state = socketState(L); state.udp.stop(); state.udpOpen = state.udp.begin(port);
    if (!state.udpOpen) return luaL_error(L, "UDP bind failed"); return 0;
}
int udpSend(lua_State *L) {
    IPAddress address; hostAddress(L, 1, address); int port = checkedPort(L, 2);
    size_t size; const char *data = luaL_checklstring(L, 3, &size); hardwareCall(L);
    auto &state = socketState(L);
    if (size > 512 || !state.udpOpen) return luaL_error(L, "Open UDP first; payload <=512 bytes");
    bool ok = state.udp.beginPacket(address, port);
    if (ok) { state.udp.write(reinterpret_cast<const uint8_t *>(data), size); ok = state.udp.endPacket(); }
    lua_pushboolean(L, ok); return 1;
}
int udpReceive(lua_State *L) {
    auto &state = socketState(L); check(L);
    if (!state.udpOpen) return luaL_error(L, "Open UDP first");
    int available = state.udp.parsePacket();
    if (!available) { lua_pushnil(L); return 1; }
    uint8_t data[512]; int count = state.udp.read(data, min(available, 512));
    IPAddress remote = state.udp.remoteIP(); uint16_t port = state.udp.remotePort();
    while (state.udp.available()) state.udp.read();
    char address[16]; snprintf(address, sizeof(address), "%u.%u.%u.%u", remote[0], remote[1], remote[2], remote[3]);
    lua_pushstring(L, address); lua_pushinteger(L, port); lua_pushlstring(L, reinterpret_cast<const char *>(data), max(count, 0)); return 3;
}
int udpClose(lua_State *L) { auto &state = socketState(L); state.udp.stop(); state.udpOpen = false; return 0; }
int httpRequest(lua_State *L) {
    size_t urlSize, bodySize = 0, caSize = 0;
    const char *url = luaL_checklstring(L, 1, &urlSize), *method = luaL_optstring(L, 2, "GET"), *body = luaL_optlstring(L, 3, "", &bodySize), *ca = luaL_optlstring(L, 4, "", &caSize);
    if (urlSize > 256 || memchr(url, 0, urlSize) || bodySize > 1024 || caSize > 4096 || (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "PUT"))) return luaL_error(L, "HTTP bounds: URL<=256, body<=1024, CA<=4096; GET/POST/PUT");
    bool tls = !strncmp(url, "https://", 8);
    if (!tls && strncmp(url, "http://", 7)) return luaL_error(L, "HTTP or HTTPS URL required");
    for (size_t i = 0; i < urlSize; ++i) if (static_cast<unsigned char>(url[i]) <= 32 || url[i] == '@' || url[i] == '#') return luaL_error(L, "Invalid URL");
    if (tls && (!caSize || strncmp(ca, "-----BEGIN CERTIFICATE-----", 27))) return luaL_error(L, "HTTPS requires an explicit trusted root CA PEM");
    hardwareCall(L); auto &state = socketState(L); state.http.end(); state.http.setConnectTimeout(1500); state.http.setTimeout(1500); state.http.setReuse(false);
    bool opened;
    if (tls) { state.https.setCACert(ca); state.https.setHandshakeTimeout(3); opened = state.http.begin(state.https, url); }
    else opened = state.http.begin(state.httpPlain, url);
    if (!opened) return luaL_error(L, "HTTP initialization failed");
    state.http.addHeader("Content-Type", "application/octet-stream");
    int code = state.http.sendRequest(method, reinterpret_cast<uint8_t *>(const_cast<char *>(body)), bodySize);
    uint8_t bytes[512]; size_t used = 0; int declared = state.http.getSize(); uint32_t started = millis();
    auto *stream = state.http.getStreamPtr();
    while (code > 0 && stream && used < sizeof(bytes) && millis() - started < 1500 && !interrupted(current(L))) {
        while (stream->available() && used < sizeof(bytes)) bytes[used++] = stream->read();
        if ((declared >= 0 && used >= static_cast<size_t>(declared)) || !state.http.connected()) break; vTaskDelay(1);
    }
    state.http.end(); state.https.stop(); state.httpPlain.stop(); check(L);
    if (code <= 0) return luaL_error(L, "HTTP transport failed; do not replay uncertain writes");
    lua_newtable(L); lua_pushinteger(L, code); lua_setfield(L, -2, "status");
    lua_pushlstring(L, reinterpret_cast<const char *>(bytes), used); lua_setfield(L, -2, "body");
    lua_pushboolean(L, declared < 0 || declared > static_cast<int>(used)); lua_setfield(L, -2, "truncated"); return 1;
}
size_t mqttLength(uint8_t *out, size_t value) { size_t count = 0; do { uint8_t digit = value % 128; value /= 128; out[count++] = digit | (value ? 128 : 0); } while (value && count < 4); return count; }
size_t mqttString(uint8_t *out, const char *value, size_t size) { out[0] = size >> 8; out[1] = size & 255; memcpy(out + 2, value, size); return size + 2; }
int mqttOpen(lua_State *L) {
    IPAddress address; hostAddress(L, 1, address); int port = checkedPort(L, 2); size_t idSize, userSize, passSize;
    const char *id = luaL_checklstring(L, 3, &idSize), *user = luaL_optlstring(L, 4, "", &userSize), *pass = luaL_optlstring(L, 5, "", &passSize);
    if (!idSize || idSize > 63 || userSize > 63 || passSize > 128 || (passSize && !userSize)) return luaL_error(L, "MQTT client id/user<=63, password<=128; user required with password");
    hardwareCall(L); auto &state = socketState(L); state.mqtt.stop(); state.mqttBytes = 0;
    if (!state.mqtt.connect(address, port, 1500)) return luaL_error(L, "MQTT TCP connection failed");
    uint8_t payload[320] = {0,4,'M','Q','T','T',4,2,0,60}; size_t size = 10;
    if (userSize) payload[7] |= 128; if (passSize) payload[7] |= 64;
    size += mqttString(payload + size, id, idSize);
    if (userSize) size += mqttString(payload + size, user, userSize); if (passSize) size += mqttString(payload + size, pass, passSize);
    uint8_t header[5] = {0x10}; size_t length = mqttLength(header + 1, size);
    state.mqtt.write(header, length + 1); state.mqtt.write(payload, size); uint32_t started = millis(); uint8_t ack[4]; size_t received = 0;
    while (received < 4 && millis() - started < 1500 && !interrupted(current(L))) { while (state.mqtt.available() && received < 4) ack[received++] = state.mqtt.read(); vTaskDelay(1); }
    if (received != 4 || ack[0] != 0x20 || ack[1] != 2 || ack[3] != 0) { state.mqtt.stop(); return luaL_error(L, "MQTT broker rejected or did not acknowledge CONNECT"); }
    state.mqttLast = millis(); check(L); return 0;
}
int mqttPublish(lua_State *L) {
    size_t topicSize, size; const char *topic = luaL_checklstring(L, 1, &topicSize), *data = luaL_checklstring(L, 2, &size); bool retain = lua_toboolean(L, 3);
    if (!topicSize || topicSize > 128 || memchr(topic, 0, topicSize) || size > 256) return luaL_error(L, "MQTT topic 1..128, payload<=256 bytes");
    hardwareCall(L); auto &state = socketState(L); if (!state.mqtt.connected()) return luaL_error(L, "Open MQTT first");
    uint8_t packet[400]; packet[0] = 0x30 | retain; size_t length = mqttLength(packet + 1, topicSize + size + 2), used = length + 1;
    used += mqttString(packet + used, topic, topicSize); memcpy(packet + used, data, size); used += size;
    lua_pushboolean(L, state.mqtt.write(packet, used) == used); state.mqttLast = millis(); return 1;
}
int mqttSubscribe(lua_State *L) {
    size_t size; const char *topic = luaL_checklstring(L, 1, &size); hardwareCall(L); auto &state = socketState(L);
    if (!size || size > 128 || !state.mqtt.connected()) return luaL_error(L, "Open MQTT; topic 1..128 bytes");
    uint8_t packet[140]; packet[0] = 0x82; size_t used = 1 + mqttLength(packet + 1, size + 5); uint16_t id = ++state.packetId;
    packet[used++] = id >> 8; packet[used++] = id & 255; used += mqttString(packet + used, topic, size); packet[used++] = 0;
    lua_pushboolean(L, state.mqtt.write(packet, used) == used); state.mqttLast = millis(); return 1;
}
int mqttPoll(lua_State *L) {
    auto &state = socketState(L); check(L);
    if (!state.mqtt.connected()) return luaL_error(L, "MQTT disconnected");
    if (millis() - state.mqttLast > 30000) { const uint8_t ping[2] = {0xc0, 0}; state.mqtt.write(ping, 2); state.mqttLast = millis(); }
    while (state.mqtt.available() && state.mqttBytes < sizeof(state.mqttBuffer)) state.mqttBuffer[state.mqttBytes++] = state.mqtt.read();
    if (state.mqttBytes < 2) { lua_pushnil(L); return 1; }
    size_t remaining = 0, multiplier = 1, header = 1; bool complete = false;
    for (; header < state.mqttBytes && header <= 4; ++header) { uint8_t digit = state.mqttBuffer[header]; remaining += (digit & 127) * multiplier; multiplier *= 128; if (!(digit & 128)) { ++header; complete = true; break; } }
    if (!complete && state.mqttBytes < 5) { lua_pushnil(L); return 1; }
    if (!complete || header > 5 || remaining + header > sizeof(state.mqttBuffer)) { state.mqtt.stop(); state.mqttBytes = 0; return luaL_error(L, "Oversized/malformed MQTT packet"); }
    if (remaining + header > state.mqttBytes) { lua_pushnil(L); return 1; }
    uint8_t kind = state.mqttBuffer[0]; bool publish = (kind >> 4) == 3 && remaining >= 2; size_t topicSize = publish ? (state.mqttBuffer[header] << 8 | state.mqttBuffer[header + 1]) : 0;
    if (publish && ((kind & 6) || topicSize > 128 || topicSize + 2 > remaining)) { state.mqtt.stop(); return luaL_error(L, "Only bounded MQTT QoS0 messages supported"); }
    if (publish) { lua_pushlstring(L, reinterpret_cast<char *>(state.mqttBuffer + header + 2), topicSize); lua_pushlstring(L, reinterpret_cast<char *>(state.mqttBuffer + header + 2 + topicSize), remaining - 2 - topicSize); }
    size_t consumed = header + remaining; memmove(state.mqttBuffer, state.mqttBuffer + consumed, state.mqttBytes - consumed); state.mqttBytes -= consumed;
    if (!publish) { lua_pushnil(L); return 1; } return 2;
}
int mqttClose(lua_State *L) { auto &state = socketState(L); if (state.mqtt.connected()) { const uint8_t packet[2] = {0xe0, 0}; state.mqtt.write(packet, 2); } state.mqtt.stop(); state.mqttBytes = 0; return 0; }

bool parseMac(const char *text, uint8_t *mac) {
    if (!text || strlen(text) != 17) return false;
    for (int i = 0; i < 6; ++i) {
        char value[3] = {text[3*i], text[3*i+1], 0};
        if (!isxdigit(value[0]) || !isxdigit(value[1]) || (i < 5 && text[3*i+2] != ':')) return false;
        mac[i] = strtoul(value, nullptr, 16);
    }
    return true;
}
#if ESP_IDF_VERSION_MAJOR >= 5
void receiveEspNow(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
    const uint8_t *mac = info->src_addr;
#else
void receiveEspNow(const uint8_t *mac, const uint8_t *data, int length) {
#endif
    if (!espNowQueue || length < 0 || length > 200) return;
    EspNowPacket packet = {}; memcpy(packet.mac, mac, 6); packet.size = length; memcpy(packet.data, data, length); xQueueSend(espNowQueue, &packet, 0);
}
int espNowOpen(lua_State *L) {
    hardwareCall(L); xSemaphoreTake(peripheralLock, portMAX_DELAY); bool ok = false;
    if (espNowOwner < 0 && espNowQueue) { ok = esp_now_init() == ESP_OK; if (ok) { xQueueReset(espNowQueue); ok = esp_now_register_recv_cb(receiveEspNow) == ESP_OK; if (ok) espNowOwner = current(L) - jobs; else esp_now_deinit(); } }
    xSemaphoreGive(peripheralLock); if (!ok) return luaL_error(L, "ESP-NOW unavailable or leased"); return 0;
}
int espNowSend(lua_State *L) {
    uint8_t mac[6]; const char *address = luaL_checkstring(L, 1); size_t size; const char *data = luaL_checklstring(L, 2, &size);
    if (!parseMac(address, mac) || size > 200 || espNowOwner != current(L) - jobs) return luaL_error(L, "Open ESP-NOW; MAC and <=200 bytes required");
    hardwareCall(L); esp_now_peer_info_t peer = {}; memcpy(peer.peer_addr, mac, 6); peer.ifidx = WIFI_IF_STA; peer.channel = 0;
    if (!esp_now_is_peer_exist(mac) && esp_now_add_peer(&peer) != ESP_OK) return luaL_error(L, "Could not add ESP-NOW peer");
    lua_pushboolean(L, esp_now_send(mac, reinterpret_cast<const uint8_t *>(data), size) == ESP_OK); return 1;
}
int espNowReceive(lua_State *L) {
    if (espNowOwner != current(L) - jobs) return luaL_error(L, "Open ESP-NOW first");
    EspNowPacket packet; if (xQueueReceive(espNowQueue, &packet, 0) != pdTRUE) { lua_pushnil(L); return 1; }
    char mac[18]; snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", packet.mac[0], packet.mac[1], packet.mac[2], packet.mac[3], packet.mac[4], packet.mac[5]);
    lua_pushstring(L, mac); lua_pushlstring(L, reinterpret_cast<char *>(packet.data), packet.size); return 2;
}
int espNowClose(lua_State *L) { xSemaphoreTake(peripheralLock, portMAX_DELAY); if (espNowOwner == current(L) - jobs) { esp_now_unregister_recv_cb(); esp_now_deinit(); espNowOwner = -1; } xSemaphoreGive(peripheralLock); return 0; }

int bleStart(lua_State *L) {
#if MCP_HAS_BLE
    const char *serviceId = luaL_checkstring(L, 1), *characteristicId = luaL_checkstring(L, 2);
    if ((strlen(serviceId) != 4 && strlen(serviceId) != 36) || (strlen(characteristicId) != 4 && strlen(characteristicId) != 36)) return luaL_error(L, "BLE UUID length must be 4 or 36");
    for (const char *uuid : {serviceId, characteristicId}) {
        size_t size = strlen(uuid);
        for (size_t i = 0; i < size; ++i) {
            bool dash = size == 36 && (i == 8 || i == 13 || i == 18 || i == 23);
            char c = uuid[i];
            if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return luaL_error(L, "BLE UUID must be four hex digits or canonical 128-bit UUID");
        }
    }
    hardwareCall(L); xSemaphoreTake(peripheralLock, portMAX_DELAY); bool ok = false;
    if (bleOwner < 0 && (bleStarted || ESP.getFreeHeap() > 120000)) {
        if (!bleStarted) {
#ifdef MCP_NIMBLE
            bleStarted = BLEDevice::init("ESP32 MCP");
#else
            BLEDevice::init("ESP32 MCP"); bleStarted = true;
#endif
        }
        BLEServer *server = bleStarted ? BLEDevice::createServer() : nullptr; BLEService *service = server ? server->createService(serviceId) : nullptr;
#ifdef MCP_NIMBLE
        bleCharacteristic = service ? service->createCharacteristic(characteristicId, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY, 128) : nullptr;
        if (bleCharacteristic) { server->start(); BLEDevice::getAdvertising()->addServiceUUID(serviceId); ok = BLEDevice::getAdvertising()->start(); }
#else
        bleCharacteristic = service ? service->createCharacteristic(characteristicId, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY) : nullptr;
        if (bleCharacteristic) { service->start(); BLEDevice::getAdvertising()->addServiceUUID(serviceId); BLEDevice::getAdvertising()->start(); ok = true; }
#endif
        if (ok) bleOwner = current(L) - jobs;
        else { BLEDevice::deinit(BLE_RESET_ARGUMENT); bleStarted = false; bleCharacteristic = nullptr; }
    }
    xSemaphoreGive(peripheralLock); if (!ok) return luaL_error(L, "BLE leased, unsupported configuration or insufficient heap"); return 0;
#else
    return luaL_error(L, "BLE is unsupported by this chip/framework profile");
#endif
}
int bleValue(lua_State *L) {
#if MCP_HAS_BLE
    if (bleOwner != current(L) - jobs || !bleCharacteristic) return luaL_error(L, "Start BLE GATT first");
    if (!lua_isnoneornil(L, 1)) { size_t size; const char *data = luaL_checklstring(L, 1, &size); if (size > 128) return luaL_error(L, "BLE value<=128 bytes"); bleCharacteristic->setValue(reinterpret_cast<uint8_t *>(const_cast<char *>(data)), size); if (lua_toboolean(L, 2)) bleCharacteristic->notify(); }
    char bytes[128]; size_t size;
    { auto value = bleCharacteristic->getValue(); size = min(static_cast<size_t>(value.length()), sizeof(bytes)); memcpy(bytes, value.c_str(), size); }
    lua_pushlstring(L, bytes, size); return 1;
#else
    return luaL_error(L, "BLE unsupported");
#endif
}
int bleStop(lua_State *L) {
#if MCP_HAS_BLE
    xSemaphoreTake(peripheralLock, portMAX_DELAY); if (bleOwner == current(L) - jobs) { BLEDevice::getAdvertising()->stop(); BLEDevice::deinit(BLE_RESET_ARGUMENT); bleStarted = false; bleCharacteristic = nullptr; bleOwner = -1; } xSemaphoreGive(peripheralLock);
#endif
    return 0;
}
void releaseNetworkIo(Job *job) {
    int owner = job - jobs; auto &state = sockets[owner]; state.http.end(); state.https.stop(); state.httpPlain.stop(); state.tcp.stop(); state.mqtt.stop(); state.udp.stop(); state.udpOpen = false; state.mqttBytes = 0;
    xSemaphoreTake(peripheralLock, portMAX_DELAY);
    if (espNowOwner == owner) { esp_now_unregister_recv_cb(); esp_now_deinit(); espNowOwner = -1; }
#if MCP_HAS_BLE
    if (bleOwner == owner) { BLEDevice::getAdvertising()->stop(); BLEDevice::deinit(BLE_RESET_ARGUMENT); bleStarted = false; bleCharacteristic = nullptr; bleOwner = -1; }
#endif
    xSemaphoreGive(peripheralLock);
}
const luaL_Reg networkIoApi[] = {
    {"tcp_open", tcpOpen}, {"tcp_write", tcpWrite}, {"tcp_read", tcpRead}, {"tcp_close", tcpClose},
    {"udp_open", udpOpen}, {"udp_send", udpSend}, {"udp_receive", udpReceive}, {"udp_close", udpClose},
    {"http_request", httpRequest}, {"mqtt_open", mqttOpen}, {"mqtt_publish", mqttPublish}, {"mqtt_subscribe", mqttSubscribe}, {"mqtt_poll", mqttPoll}, {"mqtt_close", mqttClose},
    {"espnow_open", espNowOpen}, {"espnow_send", espNowSend}, {"espnow_receive", espNowReceive}, {"espnow_close", espNowClose},
    {"ble_start", bleStart}, {"ble_value", bleValue}, {"ble_stop", bleStop}, {nullptr, nullptr}
};
