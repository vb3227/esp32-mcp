#pragma once
#include <Arduino.h>
#include "board_config.h"

// Runtime credentials override build defaults. This permits credential-free
// public firmware and USB provisioning without recompiling any board profile.
void initDeviceConfig();
bool deviceConfigReady();
bool handleDeviceConfigCommand(const String &line);
void serviceDeviceSetup();
const char *deviceWifiSsid();
const char *deviceWifiPassword();
const char *deviceBoardToken();
const char *deviceGatewayToken();
const char *deviceGatewayUrl();

#undef WIFI_SSID
#undef WIFI_PASSWORD
#undef BOARD_TOKEN
#undef VOICE_GATEWAY_TOKEN
#undef VOICE_GATEWAY_URL
#define WIFI_SSID deviceWifiSsid()
#define WIFI_PASSWORD deviceWifiPassword()
#define BOARD_TOKEN deviceBoardToken()
#define VOICE_GATEWAY_TOKEN deviceGatewayToken()
#define VOICE_GATEWAY_URL deviceGatewayUrl()
