"""Model-free transport, media and persistent Python services for MCP clients."""

import json
import os
from pathlib import Path
import queue
import secrets
import threading
from urllib.request import urlopen
import time

from gateway_control import save_gateway, stored_control_token
from media_store import MediaStore
from python_skills import PythonSkills
from realtime_gateway import start_realtime
from voice_gateway import BoardTools, ExclusiveHTTPServer, _make_handler


class DisabledSpeech:
    provider = "disabled"
    model = ""
    last = {}

    def transcribe(self, raw):
        raise RuntimeError("Start server.py --voice to handle recorded speech")


class ToolRuntime:
    def __init__(self, bridge, loop):
        self.bridge = bridge
        self.loop = loop
        self.port = int(os.environ.get("ESP_MCP_VOICE_GATEWAY_PORT", "8777"))
        self.http = None
        self.web = None
        self.thread = None
        self.python = None

    def start(self):
        agent = BoardTools(self.bridge.mcp, self.loop)
        self.python = PythonSkills(Path(__file__).resolve().parents[1], agent._mcp_json)
        token = os.environ.get("ESP_MCP_VOICE_GATEWAY_TOKEN") or secrets.token_urlsafe(32)
        control = stored_control_token(self.port) or secrets.token_urlsafe(32)
        base = _make_handler(agent, token, queue.Queue(8), control, "tools", self.python)
        direct = self.bridge.CONNECTION == "direct"

        class Handler(base):
            def do_POST(self):
                if self.path == "/voice":
                    self._deny_post("Speech is disabled; start server.py --voice", code=409)
                    return
                if self.path == "/board/request" and direct:
                    self._deny_post("Direct mode does not use the relay command proxy")
                    return
                super().do_POST()

        self.http = ExclusiveHTTPServer(("127.0.0.1", self.port + 1), Handler)
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.web = start_realtime(
            self.port, token, DisabledSpeech(), lambda *args: None, MediaStore()
        )
        deadline = time.monotonic() + 5
        ready = False
        while time.monotonic() < deadline:
            try:
                with urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2) as response:
                    health = json.load(response)
                ready = health.get("provider") == "tools"
                if ready:
                    break
            except OSError:
                time.sleep(0.02)
        if not ready:
            self.close()
            raise RuntimeError("Tools gateway did not start; check both gateway ports")
        save_gateway(self.port, control)
        return {"url": f"http://127.0.0.1:{self.port}", "token": control, "provider": "tools"}

    def close(self):
        if self.python:
            with self.python.lock:
                running = [
                    key for key, job in self.python.jobs.items() if job["status"] == "running"
                ]
            for key in running:
                self.python.stop(key)
        if self.web:
            self.web.should_exit = True
        if self.http:
            self.http.shutdown()
            self.http.server_close()
        if self.thread:
            self.thread.join(timeout=2)
