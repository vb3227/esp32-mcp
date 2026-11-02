#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "board_config.h"

namespace {
const char *const defaultSsid = WIFI_SSID;
const char *const defaultPassword = WIFI_PASSWORD;
const char *const defaultBoardToken = BOARD_TOKEN;
const char *const defaultGatewayToken = VOICE_GATEWAY_TOKEN;
const char *const defaultGatewayUrl = VOICE_GATEWAY_URL;
String ssid, password, boardToken, gatewayToken, gatewayUrl, setupLine;
Preferences configuration;
bool configured = false, storageOpen = false, droppingLine = false;

bool validToken(const String &value, bool optional = false) {
    if (optional && !value.length()) return true;
    if (value.length() < 24 || value.length() > 128) return false;
    for (char c : value) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}
bool validConfiguration(JsonDocument &value) {
    if (!value.is<JsonObject>() || value.size() != 5) return false;
    for (const char *name : {"ssid", "password", "board_token", "gateway_token", "gateway_url"}) {
        if (!value[name].is<const char *>()) return false;
        JsonString text = value[name].as<JsonString>();
        if (text.size() != strlen(text.c_str())) return false;
    }
    String network = value["ssid"].as<String>(), pass = value["password"].as<String>();
    String token = value["board_token"].as<String>(), gateway = value["gateway_token"].as<String>();
    String url = value["gateway_url"].as<String>();
    if (!network.length() || network.length() > 32 || pass.length() > 63 || !validToken(token) || !validToken(gateway, true) || url.length() > 128) return false;
    if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) return false;
    for (char c : url) if (static_cast<unsigned char>(c) <= 32 || c == '@' || c == '?' || c == '#') return false;
    return true;
}
void apply(JsonDocument &value) {
    ssid = value["ssid"].as<String>(); password = value["password"].as<String>();
    boardToken = value["board_token"].as<String>(); gatewayToken = value["gateway_token"].as<String>();
    gatewayUrl = value["gateway_url"].as<String>(); configured = true;
}
}

#include "device_config.h"

const char *deviceWifiSsid() { return ssid.c_str(); }
const char *deviceWifiPassword() { return password.c_str(); }
const char *deviceBoardToken() { return boardToken.c_str(); }
const char *deviceGatewayToken() { return gatewayToken.c_str(); }
const char *deviceGatewayUrl() { return gatewayUrl.c_str(); }
bool deviceConfigReady() { return configured; }

void initDeviceConfig() {
    ssid = defaultSsid; password = defaultPassword; boardToken = defaultBoardToken;
    gatewayToken = defaultGatewayToken; gatewayUrl = defaultGatewayUrl;
    configured = ssid.length() > 0 && ssid.length() <= 32 && validToken(boardToken);
    storageOpen = configuration.begin("mcp_config", false);
    String encoded = storageOpen ? configuration.getString("settings", "") : "";
    JsonDocument saved;
    if (encoded.length() <= 1024 && !deserializeJson(saved, encoded) && validConfiguration(saved)) apply(saved);
    if (!configured) Serial.println("[setup] USB configuration required. Run scripts/configure_device.py; MCP/network stay disabled until configured.");
}

bool handleDeviceConfigCommand(const String &line) {
    if (line == "ESP_MCP_CONFIG_STATUS") {
        Serial.printf("[setup] configured=%d storage=%d protocol=1\n", configured, storageOpen); return true;
    }
    if (!line.startsWith("ESP_MCP_CONFIG ")) return false;
    JsonDocument input;
    if (line.length() > 1100 || deserializeJson(input, line.substring(strlen("ESP_MCP_CONFIG "))) || !validConfiguration(input)) {
        Serial.println("[setup] configuration rejected: invalid fields or bounds"); return true;
    }
    String encoded; serializeJson(input, encoded);
    if (!storageOpen || configuration.putString("settings", encoded) != encoded.length() || configuration.getString("settings", "") != encoded) {
        Serial.println("[setup] configuration rejected: NVS write failed"); return true;
    }
    Serial.println("[setup] configuration saved; restarting"); Serial.flush(); delay(100); ESP.restart(); return true;
}

void serviceDeviceSetup() {
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c == '\n') {
            if (!droppingLine) { setupLine.trim(); handleDeviceConfigCommand(setupLine); }
            setupLine = ""; droppingLine = false;
        } else if (!droppingLine) {
            if (setupLine.length() >= 1100) { setupLine = ""; droppingLine = true; }
            else setupLine += c;
        }
    }
}
