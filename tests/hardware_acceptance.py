"""Opt-in physical MCP acceptance; never collected by unittest discovery.

Requires a configured, running gateway and ESP_MCP_* environment. Use --media only
with consent to capture surroundings; --pins explicitly permits testing external pins.
Results/media remain private under .esp_mcp/. No provider calls or flashing occur here.
"""

import argparse
import asyncio
import json
import os
import secrets
from pathlib import Path
import sys
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]


async def exercise(args):
    report = {"checks": [], "tools": [], "started": time.time()}
    temporary_name = "acceptance_" + secrets.token_hex(3)
    parameters = StdioServerParameters(
        command=sys.executable,
        args=[str(ROOT / "server.py"), "--profile", "k10"],
        env=dict(os.environ),
    )
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write) as client:
            await client.initialize()
            report["tools"] = [tool.name for tool in (await client.list_tools()).tools]

            async def call(tool_name, **arguments):
                result = await client.call_tool(tool_name, arguments)
                raw = "\n".join(c.text for c in result.content if c.type == "text")
                if result.is_error:
                    raise RuntimeError(raw[:600])
                try:
                    return json.loads(raw)
                except ValueError:
                    if raw.startswith(("Не удалось", "Ожидается", "Неверный")):
                        raise RuntimeError(raw[:600])
                    return raw

            async def check(label, operation):
                start = time.monotonic()
                try:
                    value = await operation()
                    report["checks"].append(
                        {
                            "check": label,
                            "ok": True,
                            "seconds": time.monotonic() - start,
                            "result": value,
                        }
                    )
                    print("PASS", label, flush=True)
                    return value
                except Exception as exc:
                    report["checks"].append({"check": label, "ok": False, "error": str(exc)[:600]})
                    print("FAIL", label, str(exc)[:160], flush=True)
                    return None

            async def wait(tool, job_id, timeout=20):
                start = time.monotonic()
                while time.monotonic() - start < timeout:
                    result = await call(tool, job_id=job_id)
                    if result.get("status") in ("done", "error", "stopped", "cancelled"):
                        return result
                    await asyncio.sleep(0.15)
                raise RuntimeError("Job did not finish within acceptance deadline")

            async def lua(source, expected=None, error=False, timeout_ms=10000):
                started = await call("execute_device_code", source=source, timeout_ms=timeout_ms)
                result = await wait("device_code_status", started["job_id"])
                if error:
                    assert result["status"] == "error", result
                else:
                    assert result["status"] == "done", result
                    if expected is not None:
                        assert result.get("result") == expected, result
                return result

            async def board(steps):
                started = await call("execute_board_code", code=json.dumps({"steps": steps}))
                result = await wait("board_job_status", started["job_id"])
                assert result["status"] == "done", result
                return result

            await check("ping", lambda: call("ping_device"))
            caps = await check("capabilities", lambda: call("hardware_capabilities"))
            if not caps or caps.get("api_version", 0) < 2:
                raise RuntimeError("Acceptance requires expanded firmware api_version>=2")
            await check("api_reference", lambda: call("native_api_reference"))
            await check("lua_math", lambda: lua("print('native Lua'); return 6*7", 42))
            await check(
                "lua_object_result",
                lambda: lua('return {one=1,label="two"}', {"one": 1, "label": "two"}),
            )
            await check("lua_array_result", lambda: lua("return {1,2,3}", [1, 2, 3]))
            await check(
                "lua_clocks_random",
                lambda: lua(
                    "return {ms=device.millis(),us=device.micros(),random=device.random(100)}"
                ),
            )
            await check("wifi_status", lambda: lua("return device.wifi_info()"))

            async def scan():
                started = await call("scan_wifi_networks")
                result = await wait("device_code_status", started["job_id"])
                assert result["status"] == "done", result
                assert isinstance(result["result"]["networks"], list), result
                return result

            await check("wifi_scan", scan)
            await check("lua_syntax_recovery", lambda: lua("this is invalid Lua", error=True))
            await check("lua_instruction_budget", lambda: lua("while true do end", error=True))
            await check(
                "lua_memory_budget", lambda: lua("return string.rep('x',1000000)", error=True)
            )
            await check("lua_after_errors", lambda: lua("return 42", 42))
            await check(
                "lua_pin_policy_denied", lambda: lua("device.digital_write(63,1)", error=True)
            )

            async def cancel_lua():
                started = await call("execute_device_code", source="device.sleep_ms(5000);return 1")
                await call("stop_device_code", job_id=started["job_id"])
                result = await wait("device_code_status", started["job_id"])
                assert result["status"] == "stopped", result
                return result

            await check("lua_cancel", cancel_lua)

            async def parallel_native():
                running = []
                try:
                    for _ in range(4):
                        job = await call(
                            "execute_device_code",
                            source="for i=1,4 do device.sleep_ms(5000) end;return true",
                            timeout_ms=30000,
                        )
                        running.append(job["job_id"])
                    for job_id in running:
                        status = await call("device_code_status", job_id=job_id)
                        assert status["status"] == "running", status
                    try:
                        await call("execute_device_code", source="return 99")
                    except RuntimeError:
                        pass
                    else:
                        raise AssertionError("A fifth native job was accepted")
                    return {"simultaneous_native_jobs": len(running)}
                finally:
                    for job_id in running:
                        await call("stop_device_code", job_id=job_id)
                        await wait("device_code_status", job_id)

            await check("four_native_jobs_and_limit", parallel_native)

            async def state_skill():
                saved = await call(
                    "save_device_script",
                    name="native_counter",
                    voice_phrase="native counter",
                    short_commands=["счетчик платы"],
                    source='local n=device.state_get("runs") or 0; assert(device.state_set("runs",n+1)); return n+1',
                )
                assert saved["installation"].get("stored"), saved
                await call("install_device_script", name="native_counter")
                values = []
                for _ in range(2):
                    started = await call("run_device_script", name="native_counter")
                    result = await wait("device_code_status", started["job_id"])
                    assert result["status"] == "done", result
                    values.append(result["result"])
                assert values[1] == values[0] + 1, values
                source = await call("read_device_script", name="native_counter")
                assert "state_get" in source["source"], source
                return {"values": values, "stored": True}

            await check("lua_persistent_skill_state", state_skill)
            await check("native_skill_list", lambda: call("list_device_scripts"))
            await check("saved_catalog", lambda: call("list_saved_skills"))

            async def json_skill():
                program = {
                    "steps": [
                        {"op": "rgb.blink", "r": 20, "g": 0, "b": 0, "count": 1, "period_ms": 100}
                    ]
                }
                await call(
                    "save_device_skill",
                    name=temporary_name,
                    voice_phrase="acceptance red",
                    short_commands=["проверка красным"],
                    program_json=json.dumps(program),
                    install=True,
                )
                await call("install_device_skill", name=temporary_name)
                await call("list_device_skills")
                started = await call("run_stored_device_skill", name=temporary_name)
                await wait("board_job_status", started["job_id"])
                combined = await call(
                    "run_composed_skill",
                    parts_json=json.dumps([{"skill": temporary_name, "parameters": {"count": 2}}]),
                )
                if "job_id" in combined:
                    await wait("board_job_status", combined["job_id"])
                matched = await call("match_saved_command", text="проверка красным", run=False)
                assert temporary_name in json.dumps(matched, ensure_ascii=False), matched
                return {"stored_and_reused": True}

            await check("json_skill_save_alias_composition", json_skill)

            if args.pins and caps["profile"] == "unihiker-k10":
                old_pins = caps["allowed_pins"]
                try:
                    await check(
                        "configure_external_pins",
                        lambda: call("configure_device_pins", pins=[1, 3]),
                    )
                    await check(
                        "gpio_adc_pulse",
                        lambda: lua(
                            'device.pin_mode(1,"input");return {digital=device.digital_read(1),adc=device.analog_read(1),mv=device.analog_read_mv(1),pulse=device.pulse_in(1,1,1000)}'
                        ),
                    )
                    await check(
                        "pwm_release",
                        lambda: lua(
                            "device.pwm_write(1,128,1000);device.sleep_ms(50);device.pwm_stop(1);return true",
                            True,
                        ),
                    )
                    await check(
                        "uart_timeout_release",
                        lambda: lua(
                            "device.uart_open(1,3,115200);local s=device.uart_read(1,20);device.uart_close();return #s",
                            0,
                        ),
                    )
                    await check("i2c_external_scan", lambda: lua("return device.i2c_scan(1,3)"))
                finally:
                    await check(
                        "restore_pin_policy", lambda: call("configure_device_pins", pins=old_pins)
                    )

            if caps["profile"] == "unihiker-k10":
                await check(
                    "display_shapes_text",
                    lambda: board(
                        [
                            {"op": "display.clear", "color": "07130D"},
                            {
                                "op": "display.text",
                                "text": "MCP acceptance / Проверка",
                                "x": 5,
                                "y": 8,
                                "color": "48E89B",
                            },
                            {
                                "op": "display.rect",
                                "x": 15,
                                "y": 60,
                                "w": 90,
                                "h": 30,
                                "color": "48E89B",
                            },
                            {
                                "op": "display.square",
                                "x": 140,
                                "y": 60,
                                "size": 30,
                                "color": "4488FF",
                            },
                            {
                                "op": "display.line",
                                "x1": 5,
                                "y1": 110,
                                "x2": 230,
                                "y2": 110,
                                "color": "FFFFFF",
                            },
                        ]
                    ),
                )
                await check(
                    "rgb_set_blink_off",
                    lambda: board(
                        [
                            {"op": "rgb.set", "r": 0, "g": 40, "b": 0, "brightness": 2},
                            {
                                "op": "rgb.blink",
                                "r": 0,
                                "g": 0,
                                "b": 40,
                                "brightness": 2,
                                "count": 2,
                                "period_ms": 100,
                            },
                            {"op": "rgb.off"},
                        ]
                    ),
                )
                await check(
                    "sensors", lambda: board([{"op": "sensor.read"}, {"op": "sensor.read.display"}])
                )
                await check(
                    "microphone_asr_status",
                    lambda: board([{"op": "mic.sample"}, {"op": "asr.last"}]),
                )
                await check("local_asr_start", lambda: board([{"op": "asr.start"}]))
                await check(
                    "camera_preview_stop",
                    lambda: board(
                        [
                            {"op": "camera.preview", "duration_ms": 500},
                            {"op": "camera.status"},
                            {"op": "camera.stop"},
                        ]
                    ),
                )
                await check("activity_preview", lambda: call("preview_loading_animation"))
                await check(
                    "voice_feedback",
                    lambda: call("display_voice_result", text="MCP: hardware checks complete"),
                )
                await check("voice_latest", lambda: call("latest_voice_transcript"))
                await check("media_status", lambda: call("media_capture_status"))

                async def parallel_hardware():
                    running = []
                    try:
                        programs = [
                            [{"op": "camera.preview", "duration_ms": 12000}],
                            [
                                {
                                    "op": "rgb.blink",
                                    "r": 0,
                                    "g": 20,
                                    "b": 0,
                                    "count": 30,
                                    "period_ms": 500,
                                }
                            ],
                            [{"op": "sensor.read"}, {"op": "delay", "duration_ms": 12000}],
                            [
                                {"op": "display.text", "text": "Four jobs", "x": 5, "y": 5},
                                {"op": "delay", "duration_ms": 12000},
                            ],
                        ]
                        for steps in programs:
                            job = await call(
                                "execute_board_code", code=json.dumps({"steps": steps})
                            )
                            running.append(job["job_id"])
                        status = await call("board_job_status", job_id=running[-1])
                        assert len(status["active_jobs"]) == 4, status
                        return {"simultaneous_hardware_jobs": 4}
                    finally:
                        for job_id in running:
                            await call("stop_board_job", job_id=job_id)
                            await wait("board_job_status", job_id)

                await check("four_hardware_jobs", parallel_hardware)

                if args.media:
                    await check("take_photo", lambda: call("take_photo"))
                    await asyncio.sleep(2)
                    await check("photo_completion", lambda: call("media_capture_status"))
                    await check("video_start", lambda: call("record_video"))
                    await asyncio.sleep(2)
                    await check("video_stop", lambda: call("stop_video"))
                    await asyncio.sleep(2)
                    await check("video_completion", lambda: call("media_capture_status"))

            async def python_skill():
                name = temporary_name + "_py"
                await call(
                    "save_python_skill",
                    name=name,
                    voice_phrase="bridge test",
                    source='device.save({"value":42}); result = device.load()',
                )
                await call("list_python_skills")
                started = await call("run_python_skill", name=name)
                result = await wait("python_skill_status", started["job_id"])
                assert result["status"] == "done", result
                return result

            await check("host_python", python_skill)
            await check(
                "cleanup_temporary_device_skill",
                lambda: call("delete_stored_device_skill", name=temporary_name),
            )
            for folder, suffixes in (
                (ROOT / "skills", (".json",)),
                (ROOT / "skills/python", (".py", ".json")),
            ):
                name = temporary_name + ("_py" if folder.name == "python" else "")
                for suffix in suffixes:
                    (folder / (name + suffix)).unlink(missing_ok=True)
            await check("final_ping", lambda: call("ping_device"))
    report["finished"] = time.time()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    failed = sum(not row["ok"] for row in report["checks"])
    print(f"Checks: {len(report['checks'])}; failed: {failed}; report: {args.report.name}")
    return bool(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media", action="store_true")
    parser.add_argument("--pins", action="store_true")
    parser.add_argument("--report", type=Path, default=ROOT / ".esp_mcp/hardware-report.json")
    return asyncio.run(exercise(parser.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
