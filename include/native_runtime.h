#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

void initNativeRuntime();
void attachNativeRuntime(WebServer &server);
bool startNativeScript(const String &source, uint32_t timeoutMs, JsonDocument &reply, bool voiceBound = false, uint32_t voiceEpoch = 0, const String &skillName = "", bool background = false);
void writeNativeCapabilities(JsonDocument &reply);
bool readNativeJob(const String &id, JsonDocument &reply);
bool stopNativeJob(const String &id);
