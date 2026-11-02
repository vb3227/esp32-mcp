#pragma once

// Copy to board_config.h and fill in private values. Do not commit that file.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define BOARD_TOKEN ""

#define MAIN_PORT 8765
#define RESCUE_PORT 8766
#define SCREEN_DIR 2

// Configure the Groq key on the laptop, never in published firmware.
#define VOICE_GATEWAY_URL ""
#define VOICE_GATEWAY_TOKEN ""

// Cross-network relay URL is configured over USB with ESP_MCP_RELAY_URL and saved in NVS.
// Keep VOICE_GATEWAY_TOKEN identical to ESP_MCP_VOICE_GATEWAY_TOKEN on the laptop.

// Initial GPIO allowlist. MCP can update it within the compiled board ceiling.
// Optional custom ceiling: #define MCP_CONFIGURABLE_PINS "..."
// Review your board pinout before permitting external pins.
#define MCP_ALLOWED_PINS ""
