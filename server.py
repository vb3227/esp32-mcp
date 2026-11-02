"""Standalone ESP32 MCP entry point; tools mode needs no model credentials."""

import argparse
import asyncio
import json
import os
from pathlib import Path
import sys
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "mcp"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--voice", action="store_true", help="Enable speech/router/Codex services")
    parser.add_argument("--local-stt", action="store_true", help="Use local speech with --voice")
    parser.add_argument(
        "--profile",
        choices=("generic", "k10"),
        help="Hardware tool set (default generic, k10 with --voice)",
    )
    parser.add_argument(
        "--gateway-only", action="store_true", help="Run services without MCP stdio"
    )
    parser.add_argument(
        "--check", action="store_true", help="Check settings without contacting a board"
    )
    args = parser.parse_args()
    profile = (
        args.profile or os.environ.get("ESP_MCP_PROFILE") or ("k10" if args.voice else "generic")
    )
    if profile not in ("generic", "k10"):
        parser.error("ESP_MCP_PROFILE must be generic or k10")
    if args.voice and profile != "k10":
        parser.error("--voice requires the k10 profile")
    if args.local_stt and not args.voice:
        parser.error("--local-stt requires --voice")
    os.environ.setdefault("ESP_MCP_VOICE_GATEWAY_PORT", "8777")
    connection = os.environ.get("ESP_MCP_CONNECTION", "direct")
    if connection not in ("direct", "relay"):
        parser.error("ESP_MCP_CONNECTION must be direct or relay")
    try:
        port = int(os.environ["ESP_MCP_VOICE_GATEWAY_PORT"])
    except ValueError:
        parser.error("ESP_MCP_VOICE_GATEWAY_PORT must be an integer")
    if not 1024 <= port <= 65534:
        parser.error("Gateway port must be between 1024 and 65534")
    if not os.environ.get("ESP_MCP_BOARD_TOKEN", "").strip():
        parser.error("Set ESP_MCP_BOARD_TOKEN to match the firmware")
    if connection == "direct":
        address = urlparse(os.environ.get("ESP_MCP_BOARD_URL", ""))
        if (
            address.scheme not in ("http", "https")
            or not address.hostname
            or address.username
            or address.password
            or address.query
            or address.fragment
            or address.path not in ("", "/")
        ):
            parser.error("Direct mode requires ESP_MCP_BOARD_URL=http://BOARD-IP:8765")
    if (connection == "relay" or args.voice) and not os.environ.get(
        "ESP_MCP_VOICE_GATEWAY_TOKEN", ""
    ).strip():
        parser.error("Relay/voice requires a matching ESP_MCP_VOICE_GATEWAY_TOKEN")
    if args.local_stt:
        os.environ["ESP_MCP_LOCAL_STT"] = "1"
    provider = (
        "local"
        if os.environ.get("ESP_MCP_LOCAL_STT") == "1"
        else os.environ.get("ESP_MCP_STT_PROVIDER", "groq")
    )
    if args.voice and provider not in ("groq", "local"):
        parser.error("ESP_MCP_STT_PROVIDER must be groq or local")
    if args.voice and provider == "groq" and not os.environ.get("GROQ_API_KEY", "").strip():
        parser.error("Voice with Groq requires GROQ_API_KEY in the laptop environment")
    if args.check:
        print(
            json.dumps(
                {
                    "valid": True,
                    "mode": "voice" if args.voice else "tools",
                    "connection": connection,
                    "port": port,
                    "profile": profile,
                }
            )
        )
        return

    import server as bridge
    from device_tools import register_tools

    register_tools(bridge)
    from native_scripts import register_native_tools

    register_native_tools(bridge, ROOT)
    from device_drivers import register_driver_tools

    register_driver_tools(bridge, ROOT)
    if profile == "generic":
        for name in (
            "take_photo",
            "record_video",
            "stop_video",
            "media_capture_status",
            "start_voice_capture",
            "finish_voice_capture",
            "latest_voice_transcript",
            "display_voice_result",
            "set_voice_activity",
            "preview_loading_animation",
        ):
            bridge.mcp.remove_tool(name)

    async def run():
        from gateway_control import existing_gateway

        existing = existing_gateway(port)
        if args.voice:
            if existing and existing.get("provider") == "tools":
                raise RuntimeError("A tools-only gateway owns this port; stop it before --voice")
            from voice_gateway import start_voice_gateway

            shared = start_voice_gateway(bridge.mcp, asyncio.get_running_loop())
            if shared and connection == "relay":
                bridge.SHARED_GATEWAY = shared
            runtime = None
        else:
            from tool_runtime import ToolRuntime

            runtime = None if existing else ToolRuntime(bridge, asyncio.get_running_loop())
            shared = existing or runtime.start()
            if connection == "relay":
                bridge.SHARED_GATEWAY = shared
        try:
            if args.gateway_only:
                print(
                    "ESP32 MCP gateway ready; mode=" + ("voice" if args.voice else "tools"),
                    file=sys.stderr,
                )
                await asyncio.Event().wait()
            else:
                await bridge.mcp.run_stdio_async()
        finally:
            if runtime:
                await asyncio.to_thread(runtime.close)

    asyncio.run(run())


if __name__ == "__main__":
    main()
