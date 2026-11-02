"""Persistent Lua source/manifest pairs for execution on the microcontroller."""

import hashlib
import json
from pathlib import Path
import re
import uuid
from native_api_reference import EXTRA_API

LUA_API_REFERENCE = {
    "info": "device.info() -> profile, heap_free, uptime_ms",
    "sleep_ms": "device.sleep_ms(ms): cooperative sleep, 0..5000 ms",
    "millis": "device.millis() -> milliseconds since boot, wraps at 2^32",
    "micros": "device.micros() -> microseconds since boot, wraps at 2^32",
    "random": "device.random(limit) -> integer 0..limit-1; limit > 0",
    "pin_mode": 'device.pin_mode(pin, "input"|"output"|"pullup")',
    "digital_read": "device.digital_read(pin) -> 0 or 1",
    "digital_write": "device.digital_write(pin, 0|1)",
    "analog_read": "device.analog_read(pin) -> raw ADC; pin must support ADC",
    "analog_read_mv": "device.analog_read_mv(pin) -> calibrated millivolts",
    "pulse_in": "device.pulse_in(pin, 0|1, timeout_us=30000) -> pulse length; timeout 1..50000 us",
    "pwm_write": "device.pwm_write(pin, duty, frequency=1000): duty 0..1023, 20..20000 Hz; 2 reserved channels",
    "pwm_stop": "device.pwm_stop(pin): detach owned PWM before digital writes",
    "wifi_info": "device.wifi_info() -> connected, ssid, ip, rssi, channel; no passwords",
    "wifi_scan": "device.wifi_scan() -> count, truncated, networks[{ssid,rssi,channel,secured}]; max 12 results; vendor driver may wait up to 10 s",
    "i2c_scan": "device.i2c_scan(sda, scl) -> responding addresses 8..119",
    "i2c_transfer": "device.i2c_transfer(sda, scl, address, tx_string, rx_count=0) -> byte string; max 128 bytes each",
    "spi_transfer": "device.spi_transfer(sck,miso,mosi,cs,tx_string,frequency=1000000,mode=0) -> byte string; max 128 bytes; 10000..10000000 Hz; mode 0..3",
    "uart_open": "device.uart_open(tx, rx, baud=115200): exclusive UART1, 8N1, 300..1000000 baud",
    "uart_write": "device.uart_write(byte_string) -> bytes written; max 256",
    "uart_read": "device.uart_read(max_bytes=128, timeout_ms=100) -> byte string; 1..256 bytes, 0..1000 ms",
    "uart_close": "device.uart_close(): close this job's UART",
    "state_get": "device.state_get(key) -> JSON value or nil; installed skills only; key 1..31 alphanumeric/_/-",
    "state_set": "device.state_set(key,value) -> boolean saved; installed skills only; 1200 bytes/record, 8 writes/job, 8 KiB shared NVS quota",
    "run": 'device.run({steps={{op="sensor.read"}}}) -> adapter hardware job table; operations depend on profile',
    "status": "device.status(job_id) -> adapter hardware job status/result",
}
LUA_API_REFERENCE.update(EXTRA_API)
LUA_API_REFERENCE["spi_transfer"] += (
    "; cs=-1 for manually controlled CS, including K10 expander GPIO"
)


def valid_name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_-]{1,31}", name):
        raise ValueError("Name must use 1-31 lowercase letters, digits, _ or -")
    return name


def validate_source(source):
    if not isinstance(source, str) or not 1 <= len(source.encode("utf-8")) <= 4096:
        raise ValueError("Lua source must contain 1-4096 UTF-8 bytes")
    if "\0" in source:
        raise ValueError("NUL bytes are unavailable in Lua source")
    return source


def read_script(folder, name):
    valid_name(name)
    folder = Path(folder)
    source = folder / (name + ".lua")
    manifest = folder / (name + ".json")
    if folder.is_symlink() or source.is_symlink() or manifest.is_symlink():
        raise ValueError("Symlink skills are unavailable")
    item = json.loads(manifest.read_text(encoding="utf-8"))
    code = validate_source(source.read_text(encoding="utf-8"))
    if (
        not isinstance(item, dict)
        or item.get("name") != name
        or item.get("runtime") != "lua"
        or item.get("sha256") != hashlib.sha256(code.encode()).hexdigest()
    ):
        raise ValueError("Lua manifest/source mismatch")
    phrase = item.get("voice_phrase")
    aliases = item.get("aliases", [])
    if (
        not isinstance(phrase, str)
        or not 1 <= len(phrase.encode()) <= 63
        or not isinstance(aliases, list)
        or len(aliases) > 8
        or any(not isinstance(a, str) or not 1 <= len(a.encode()) <= 63 for a in aliases)
        or type(item.get("background", False)) is not bool
    ):
        raise ValueError("Invalid Lua skill metadata")
    return item, code


def save_script(root, name, source, voice_phrase, aliases=(), background=False):
    valid_name(name)
    validate_source(source)
    if type(background) is not bool:
        raise ValueError("background must be boolean")
    if not isinstance(voice_phrase, str) or not 1 <= len(voice_phrase.encode()) <= 63:
        raise ValueError("Voice phrase must contain 1-63 UTF-8 bytes")
    if (
        not isinstance(aliases, (list, tuple))
        or len(aliases) > 8
        or any(not isinstance(a, str) or not 1 <= len(a.encode()) <= 63 for a in aliases)
    ):
        raise ValueError("Supply up to eight short aliases")
    root = Path(root)
    if root.is_symlink() or (root / "lua").is_symlink():
        raise ValueError("Symlink skill folders are unavailable")
    if (root / (name + ".json")).exists() or (root / "python" / (name + ".json")).exists():
        raise ValueError("Name belongs to another runtime")
    folder = root / "lua"
    folder.mkdir(parents=True, exist_ok=True)
    item = {
        "name": name,
        "runtime": "lua",
        "location": "device",
        "voice_phrase": voice_phrase,
        "aliases": list(dict.fromkeys(aliases)),
        "sha256": hashlib.sha256(source.encode()).hexdigest(),
        "background": background,
    }
    for suffix, content in (
        ("lua", source),
        ("json", json.dumps(item, ensure_ascii=False, indent=2)),
    ):
        path = folder / (name + "." + suffix)
        if path.is_symlink():
            raise ValueError("Symlink skills are unavailable")
        temporary = folder / (uuid.uuid4().hex + ".tmp")
        try:
            temporary.write_text(content, encoding="utf-8")
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    return item


def register_native_tools(bridge, root):
    root = Path(root)

    def request(path, payload=None):
        return bridge.request_to(
            bridge.BOARD_URL, path, "POST" if payload is not None else "GET", payload
        )

    @bridge.mcp.tool()
    def native_api_reference() -> str:
        """Get the Lua hardware API signatures and limits without searching source files.

        Compare hardware_capabilities.api/api_version to ensure installed firmware support.
        GPIO must be explicitly allowed. UART/PWM are released at job exit. No imports,
        filesystem, shell, raw sockets or Wi-Fi injection. Binary results should be converted
        to byte arrays before returning JSON. Installed native skills can persist JSON state.
        """
        return json.dumps({"api_version": 3, "functions": LUA_API_REFERENCE})

    @bridge.mcp.tool()
    def configure_device_pins(pins: list[int]) -> str:
        """Persist the exact allowed external GPIO set within the board profile ceiling.

        Inspect hardware_capabilities and wiring first. Existing native jobs must stop.
        An empty list revokes external GPIO. Does not flash or modify built-in peripherals.
        """
        if (
            not isinstance(pins, list)
            or len(pins) > 40
            or any(type(pin) is not int or not 0 <= pin < 64 for pin in pins)
        ):
            raise ValueError("Supply at most 40 integer GPIO numbers between 0 and 63")
        return request("/runtime/pins", {"pins": sorted(set(pins))})

    @bridge.mcp.tool()
    def scan_wifi_networks() -> str:
        """Start a bounded on-device Wi-Fi scan; poll device_code_status for up to 12 networks.

        Returns SSID, RSSI, channel and security flag, never saved passwords.
        """
        return execute_device_code("return device.wifi_scan()", 15000)

    @bridge.mcp.tool()
    def execute_device_code(source: str, timeout_ms: int = 10000, background: bool = False) -> str:
        """Execute text-only Lua on the microcontroller without reflashing; returns a job ID."""
        validate_source(source)
        if type(timeout_ms) is not int or not 10 <= timeout_ms <= 60000:
            raise ValueError("Timeout must be 10-60000 milliseconds")
        if type(background) is not bool:
            raise ValueError("background must be boolean")
        payload = {"source": source, "timeout_ms": timeout_ms}
        if background:
            payload["background"] = True
        return request("/runtime/execute", payload)

    @bridge.mcp.tool()
    def list_device_jobs() -> str:
        """List foreground/background Lua jobs so any MCP client can inspect or stop them."""
        return request("/runtime/jobs")

    @bridge.mcp.tool()
    def send_device_message(job_id: str, data_json: str) -> str:
        """Deliver a JSON value to a running Lua job's four-entry mailbox (API v3)."""
        if not re.fullmatch(r"[A-Za-z0-9-]{1,48}", job_id):
            raise ValueError("Invalid native job ID")
        if not isinstance(data_json, str) or len(data_json.encode()) > 512:
            raise ValueError("Message must contain at most 512 UTF-8 bytes")
        value = json.loads(data_json)
        if value is None:
            raise ValueError("Message must not be null")
        return request("/runtime/message", {"job_id": job_id, "data": value})

    @bridge.mcp.tool()
    def device_code_status(job_id: str) -> str:
        """Read native Lua status, bounded stdout, result and runtime error."""
        if not re.fullmatch(r"[A-Za-z0-9-]{1,48}", job_id):
            raise ValueError("Invalid native job ID")
        return request("/runtime/status?job_id=" + job_id)

    @bridge.mcp.tool()
    def stop_device_code(job_id: str) -> str:
        """Request cooperative cancellation of only the selected native Lua job."""
        if not re.fullmatch(r"[A-Za-z0-9-]{1,48}", job_id):
            raise ValueError("Invalid native job ID")
        return request("/runtime/stop", {"job_id": job_id})

    @bridge.mcp.tool()
    def save_device_script(
        name: str,
        source: str,
        voice_phrase: str,
        short_commands: list[str] | None = None,
        install: bool = True,
        run: bool = False,
        background: bool = False,
    ) -> str:
        """Save native Lua locally and optionally in device NVS; never modifies firmware."""
        item = save_script(
            root / "skills", name, source, voice_phrase, short_commands or [], background
        )
        result = {"saved_locally": True, "skill": item}
        if install:
            result["installation"] = json.loads(
                request(
                    "/runtime/skills/save",
                    {
                        "name": name,
                        "source": source,
                        "voice_phrase": voice_phrase,
                        "aliases": item["aliases"],
                        "background": background,
                    },
                )
            )
        if run:
            result["execution"] = json.loads(
                request("/runtime/skills/run", {"name": name})
                if install
                else execute_device_code(source, background=background)
            )
        return json.dumps(result, ensure_ascii=False)

    @bridge.mcp.tool()
    def install_device_script(name: str) -> str:
        """Install a locally saved, checksum-verified Lua skill into device NVS."""
        item, code = read_script(root / "skills/lua", name)
        return request(
            "/runtime/skills/save",
            {
                "name": name,
                "source": code,
                "voice_phrase": item["voice_phrase"],
                "aliases": item["aliases"],
                "background": item.get("background", False),
            },
        )

    @bridge.mcp.tool()
    def list_device_scripts() -> str:
        """List Lua skill names stored in device NVS, independently of laptop files."""
        return request("/runtime/skills/list")

    @bridge.mcp.tool()
    def read_device_script(name: str) -> str:
        """Read a saved native Lua skill from the device even without its original host files."""
        valid_name(name)
        return request("/runtime/skills/get", {"name": name})

    @bridge.mcp.tool()
    def run_device_script(
        name: str, timeout_ms: int = 10000, background: bool | None = None
    ) -> str:
        """Run a Lua skill by its device NVS name; no host source file is required."""
        valid_name(name)
        if type(timeout_ms) is not int or not 10 <= timeout_ms <= 60000:
            raise ValueError("Timeout must be 10-60000 milliseconds")
        payload = {"name": name, "timeout_ms": timeout_ms}
        if background is not None:
            if type(background) is not bool:
                raise ValueError("background must be boolean")
            payload["background"] = background
        return request("/runtime/skills/run", payload)
