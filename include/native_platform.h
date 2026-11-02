#pragma once
#include <soc/soc_caps.h>
#include <esp_idf_version.h>
#include <esp_arduino_version.h>
#ifndef SOC_PCNT_SUPPORTED
#define SOC_PCNT_SUPPORTED 0
#endif
#ifndef SOC_TWAI_SUPPORTED
#define SOC_TWAI_SUPPORTED 0
#endif
#ifndef SOC_BT_SUPPORTED
#define SOC_BT_SUPPORTED 0
#endif
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <esp_now.h>
#include <driver/uart.h>
#include <driver/twai.h>
#ifdef MCP_PROFILE_K10
#include "initBoard.h"
#endif

#if ESP_IDF_VERSION_MAJOR >= 5
#include <driver/i2s_std.h>
#include <driver/rmt_tx.h>
#if SOC_PCNT_SUPPORTED
#include <driver/pulse_cnt.h>
#endif
#if SOC_DAC_SUPPORTED
#include <driver/dac_oneshot.h>
#endif
#else
#include <driver/i2s.h>
#include <driver/rmt.h>
#if SOC_PCNT_SUPPORTED
#include <driver/pcnt.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
#include <driver/dac.h>
#endif
#endif

#if (defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2))
#define MCP_HAS_DAC 1
#else
#define MCP_HAS_DAC 0
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define MCP_HAS_TOUCH 1
#else
#define MCP_HAS_TOUCH 0
#endif
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && SOC_BT_SUPPORTED
#define MCP_HAS_BLE 1
#ifdef MCP_PROFILE_K10
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#else
// BLE-only host avoids linking the classic Bluetooth host on generic ESP32.
#include <NimBLEDevice.h>
#define MCP_NIMBLE 1
#endif
#else
#define MCP_HAS_BLE 0
#endif
