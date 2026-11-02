# Setup

Use the [complete setup guide](MCP_SETUP_RU.md), [client templates](../config/mcp.example.toml) and [board feature matrix](BOARD_PROFILES.md).

The public MCP entry is root `server.py`. Default host ports are 8777/8778. Select the matching PlatformIO environment (`k10` for K10). Never publish `include/board_config.h`, runtime data or flashed binaries containing credentials.
