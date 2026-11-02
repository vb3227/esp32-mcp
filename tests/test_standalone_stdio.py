"""Real MCP stdio + restricted host Python, with a simulated board HTTP transport."""

import asyncio
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


class StandaloneStdioTests(unittest.TestCase):
    def test_real_client_without_provider_keys_can_save_run_and_reload_skills(self):
        with tempfile.TemporaryDirectory() as folder:
            project = Path(folder).resolve()
            source = Path(__file__).resolve().parents[1]
            shutil.copytree(
                source / "mcp", project / "mcp", ignore=shutil.ignore_patterns("__pycache__")
            )
            shutil.copyfile(source / "server.py", project / "server.py")
            (project / "skills").mkdir()
            board_token = secrets.token_urlsafe(32)
            calls = []

            class Board(BaseHTTPRequestHandler):
                def log_message(self, *args):
                    pass

                def do_GET(self):
                    self.assert_auth = self.headers.get("X-Board-Token") == board_token
                    if not self.assert_auth:
                        self.send_error(403)
                        return
                    data = (
                        b"pong"
                        if self.path == "/ping"
                        else b'{"profile":"simulated","native_execution":true}'
                    )
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)

                def do_POST(self):
                    if self.headers.get("X-Board-Token") != board_token:
                        self.send_error(403)
                        return
                    body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    calls.append((self.path, body))
                    data = b'{"job_id":"simulated-1","status":"running"}'
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)

            board = ThreadingHTTPServer(("127.0.0.1", 0), Board)
            thread = threading.Thread(target=board.serve_forever, daemon=True)
            thread.start()
            with socket.socket() as candidate:
                candidate.bind(("127.0.0.1", 0))
                port = candidate.getsockname()[1]
            environment = {
                k: v
                for k, v in os.environ.items()
                if not any(
                    marker in k
                    for marker in ("API_KEY", "TOKEN", "ESP_MCP_", "TELLY_", "UNIHIKER_")
                )
            }
            environment.update(
                ESP_MCP_CONNECTION="direct",
                ESP_MCP_BOARD_URL=f"http://127.0.0.1:{board.server_port}",
                ESP_MCP_BOARD_TOKEN=board_token,
                ESP_MCP_VOICE_GATEWAY_PORT=str(port),
                PYTHONUTF8="1",
            )

            async def exercise():
                parameters = StdioServerParameters(
                    command=sys.executable, args=[str(project / "server.py")], env=environment
                )
                async with stdio_client(parameters) as (read, write):
                    async with ClientSession(read, write) as session:
                        await session.initialize()
                        names = {t.name for t in (await session.list_tools()).tools}
                        self.assertTrue(
                            {"take_photo", "record_video", "start_voice_capture"}.isdisjoint(names)
                        )
                        self.assertTrue(
                            {
                                "execute_device_code",
                                "save_device_script",
                                "save_python_skill",
                                "hardware_capabilities",
                            }
                            <= names
                        )

                        async def call(name, arguments):
                            response = await session.call_tool(name, arguments)
                            self.assertFalse(response.is_error, name)
                            return "\n".join(c.text for c in response.content if hasattr(c, "text"))

                        self.assertIn("pong", await call("ping_device", {}))
                        await call(
                            "execute_device_code", {"source": "return 42", "timeout_ms": 1000}
                        )
                        self.assertEqual(calls[-1][0], "/runtime/execute")
                        self.assertEqual(calls[-1][1]["source"], "return 42")
                        await call(
                            "save_device_script",
                            {
                                "name": "native_sample",
                                "source": "return 42",
                                "voice_phrase": "native sample",
                                "install": False,
                            },
                        )
                        self.assertIn("native_sample", await call("list_saved_skills", {}))
                        saved = json.loads(
                            await call(
                                "save_python_skill",
                                {
                                    "name": "host_sample",
                                    "source": "result = 6 * 7",
                                    "voice_phrase": "host sample",
                                    "run": True,
                                },
                            )
                        )
                        identifier = saved["execution"]["job_id"]
                        for _ in range(100):
                            status = json.loads(
                                await call("python_skill_status", {"job_id": identifier})
                            )
                            if status["status"] != "running":
                                break
                            await asyncio.sleep(0.05)
                        self.assertEqual(status["status"], "done")
                        self.assertEqual(status["result"], 42)

            try:
                asyncio.run(asyncio.wait_for(exercise(), timeout=35))
                self.assertTrue((project / "skills/lua/native_sample.lua").is_file())
                self.assertTrue((project / "skills/python/host_sample.py").is_file())
            finally:
                board.shutdown()
                board.server_close()
                thread.join(timeout=2)
