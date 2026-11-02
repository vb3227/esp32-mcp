"""MCP-first capabilities and offline, reusable device skill storage."""

import json
from pathlib import Path
import re
import uuid

from program_validation import FIELDS, validate_program
from skill_commands import command_aliases
from skill_composition import expand, local_composition
from skill_router import SkillCatalog, key

ROOT = Path(__file__).resolve().parents[1]


def save_local_skill(folder, name, voice_phrase, program_json, short_commands=()):
    if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9_-]{1,31}", name):
        raise ValueError("Skill name must use 1-31 lowercase letters, digits, _ or -")
    if not isinstance(voice_phrase, str) or not 1 <= len(voice_phrase.encode()) <= 63:
        raise ValueError("Voice phrase must contain 1-63 UTF-8 bytes")
    if not isinstance(program_json, str) or len(program_json.encode()) > 3800:
        raise ValueError("Program JSON must contain at most 3800 UTF-8 bytes")
    if (
        not isinstance(short_commands, (list, tuple))
        or len(short_commands) > 8
        or any(
            not isinstance(v, str) or not 1 <= len(v.encode()) <= 63 or not 1 <= len(v.split()) <= 4
            for v in short_commands
        )
    ):
        raise ValueError("Supply up to eight short commands, 1-4 words and at most 63 bytes each")
    program = validate_program(json.loads(program_json))
    folder = Path(folder)
    if folder.is_symlink():
        raise ValueError("Symlink skill folders are unavailable")
    folder = folder.resolve()
    if (folder / "python" / (name + ".json")).exists() or (
        folder / "lua" / (name + ".json")
    ).exists():
        raise ValueError("Name belongs to another runtime")
    path = folder / (name + ".json")
    if path.is_symlink():
        raise ValueError("Symlink skills are unavailable")
    folder.mkdir(parents=True, exist_ok=True)
    item = {
        "name": name,
        "voice_phrase": voice_phrase,
        "program": program,
        "aliases": command_aliases(name, voice_phrase, program, short_commands),
    }
    temporary = folder / (uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(item, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return item


def register_tools(bridge):
    @bridge.mcp.tool()
    def hardware_capabilities() -> str:
        """Describe supported hardware, execution locations, limits and persistence."""
        board = json.loads(bridge.request_to(bridge.BOARD_URL, "/capabilities"))
        board["host_python"] = {
            "location": "laptop",
            "imports": False,
            "runtime_seconds": 60,
            "hardware_calls": 128,
            "concurrent_workers": 4,
        }
        board["host_operations"] = FIELDS
        from native_scripts import LUA_API_REFERENCE

        board["native_api_reference"] = {
            name: LUA_API_REFERENCE[name]
            for name in board.get("api", [])
            if name in LUA_API_REFERENCE
        }
        return json.dumps(board, ensure_ascii=False)

    @bridge.mcp.tool()
    def list_saved_skills() -> str:
        """List laptop JSON/Python skills and short aliases without model inference."""
        catalog = SkillCatalog(ROOT / "skills")
        items, _, _ = catalog.snapshot()
        return json.dumps({"skills": list(items.values())}, ensure_ascii=False)

    @bridge.mcp.tool()
    def save_device_skill(
        name: str,
        voice_phrase: str,
        program_json: str,
        short_commands: list[str] | None = None,
        install: bool = False,
    ) -> str:
        """Save JSON and aliases locally even offline; optionally install in board NVS."""
        item = save_local_skill(
            ROOT / "skills", name, voice_phrase, program_json, short_commands or []
        )
        result = {"saved_locally": True, "skill": item}
        if install:
            result["board_installation"] = bridge.install_device_skill(name)
        return json.dumps(result, ensure_ascii=False)

    @bridge.mcp.tool()
    def run_composed_skill(parts_json: str, feedback: str = "") -> str:
        """Execute parameterized/repeated saved JSON skills; never save a duplicate."""
        if len(parts_json.encode()) > 4096:
            raise ValueError("Composition request too large")
        items, _, _ = SkillCatalog(ROOT / "skills").snapshot()
        program = expand(json.loads(parts_json), items)
        return bridge.execute_board_code(json.dumps(program), feedback)

    @bridge.mcp.tool()
    def match_saved_command(text: str, run: bool = False) -> str:
        """Match exact aliases or deterministic compositions; no model call. Optional execution."""
        if not text.strip() or len(text.encode()) > 3000:
            raise ValueError("Command must contain 1-3000 UTF-8 bytes")
        catalog = SkillCatalog(ROOT / "skills")
        items, _, _ = catalog.snapshot()
        name = catalog.find(text)
        if name in items:
            item = items[name]
            result = {"matched": True, "name": name, "runtime": item.get("runtime", "device-json")}
            if run:
                result["execution"] = (
                    bridge.run_python_skill(name)
                    if item.get("runtime") == "python"
                    else bridge.request_to(
                        bridge.BOARD_URL, "/runtime/skills/run", "POST", {"name": name}
                    )
                    if item.get("runtime") == "lua"
                    else bridge.execute_board_code(json.dumps(item["program"]))
                )
            return json.dumps(result, ensure_ascii=False)
        composed = local_composition(text, catalog, key)
        if composed:
            result = {"matched": True, "composition": composed}
            if run:
                result["execution"] = bridge.execute_board_code(json.dumps(composed["program"]))
            return json.dumps(result, ensure_ascii=False)
        return json.dumps(
            {"matched": False, "next": "Use the connected coding agent to create a skill"}
        )
