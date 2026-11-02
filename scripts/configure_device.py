"""Configure a prebuilt ESP32 MCP image over USB without rebuilding firmware."""

import argparse
import getpass
import json
from pathlib import Path
import secrets
import time

import serial


def configuration(ssid, password, board_token, gateway_token, gateway_url=""):
    if not all(
        isinstance(value, str)
        for value in (ssid, password, board_token, gateway_token, gateway_url)
    ):
        raise ValueError("Device settings must be strings")
    if any("\0" in value for value in (ssid, password, board_token, gateway_token, gateway_url)):
        raise ValueError("NUL bytes are unavailable in device settings")
    if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32:
        raise ValueError("SSID must contain 1..32 UTF-8 bytes")
    if not isinstance(password, str) or len(password.encode()) > 63:
        raise ValueError("Wi-Fi password exceeds 63 UTF-8 bytes")
    import re

    if any(
        not re.fullmatch(r"[A-Za-z0-9_-]{24,128}", token) for token in (board_token, gateway_token)
    ):
        raise ValueError("Use independent 24..128 character alphanumeric/_/- tokens")
    if len(gateway_url) > 128 or (
        gateway_url
        and (
            not gateway_url.startswith(("http://", "https://"))
            or any(ord(c) <= 32 or c in "@?#" for c in gateway_url)
        )
    ):
        raise ValueError("Invalid gateway URL")
    return dict(
        ssid=ssid,
        password=password,
        board_token=board_token,
        gateway_token=gateway_token,
        gateway_url=gateway_url,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--ssid", help="Omit to enter interactively")
    parser.add_argument("--gateway-url", default="")
    parser.add_argument("--output", type=Path, default=Path(".esp_mcp/device-settings.json"))
    args = parser.parse_args()
    settings = configuration(
        args.ssid or input("Wi-Fi SSID: ").strip(),
        getpass.getpass("Wi-Fi password: "),
        secrets.token_urlsafe(32),
        secrets.token_urlsafe(32),
        args.gateway_url,
    )
    # Store only MCP credentials, never the Wi-Fi password. Restrict local permissions
    # where supported. This output must remain outside public source/releases.
    output = args.output.resolve()
    root = Path(__file__).resolve().parents[1]
    if output.is_relative_to(root) and not output.is_relative_to(root / ".esp_mcp"):
        raise SystemExit("Private settings inside the project must stay under ignored .esp_mcp/.")
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = {
        "ESP_MCP_BOARD_TOKEN": settings["board_token"],
        "ESP_MCP_VOICE_GATEWAY_TOKEN": settings["gateway_token"],
        "ESP_MCP_VOICE_GATEWAY_PORT": "8777",
    }
    output.write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")
    try:
        output.chmod(0o600)
    except OSError:
        pass
    board = serial.Serial(port=None, baudrate=115200, timeout=0.3, write_timeout=3)
    board.dtr = board.rts = False
    board.port = args.port
    with board:
        board.write(
            (
                "ESP_MCP_CONFIG "
                + json.dumps(settings, ensure_ascii=False, separators=(",", ":"))
                + "\n"
            ).encode()
        )
        board.flush()
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            line = board.readline().decode("utf-8", errors="replace")
            # Do not echo arbitrary serial data: a device may echo credentials.
            if "[setup] configuration saved; restarting" in line:
                print("Device configuration saved; restarting.")
                print(f"Private MCP environment saved in {output}. Do not publish this file.")
                return 0
            if "[setup] configuration rejected:" in line:
                raise SystemExit(
                    "Device rejected configuration; check bounds and NVS space. Private host settings retained for retry."
                )
    raise SystemExit(
        "No acknowledgment. Check USB port/firmware and close Serial Monitor. Private host settings retained for retry."
    )


if __name__ == "__main__":
    raise SystemExit(main())
