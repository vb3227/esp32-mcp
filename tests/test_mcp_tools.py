import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from device_tools import register_tools, save_local_skill
from skill_router import SkillCatalog


class Registry:
    def __init__(self):
        self.functions = {}

    def tool(self):
        def decorate(fn):
            self.functions[fn.__name__] = fn
            return fn

        return decorate


class StandaloneToolsTests(unittest.TestCase):
    def test_offline_skill_survives_reload_and_matches_both_languages(self):
        with tempfile.TemporaryDirectory() as folder:
            item = save_local_skill(
                folder,
                "red_twice",
                "red twice",
                '{"steps":[{"op":"rgb.blink","r":40,"g":0,"b":0,"count":2}]}',
                ["красный дважды"],
            )
            self.assertEqual(item["name"], "red_twice")
            catalog = SkillCatalog(folder)
            self.assertEqual(catalog.find("red twice"), "red_twice")
            self.assertEqual(catalog.find("красный дважды"), "red_twice")

    def test_invalid_or_traversing_skill_cannot_write_files(self):
        with tempfile.TemporaryDirectory() as folder:
            for name, program, aliases in [
                ("../outside", '{"steps":[{"op":"rgb.off"}]}', []),
                ("bad", '{"steps":[{"op":"shell","command":"no"}]}', []),
                ("bad", '{"steps":[{"op":"rgb.off"}]}', [1]),
            ]:
                with self.assertRaises(ValueError):
                    save_local_skill(folder, name, "short phrase", program, aliases)
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_composition_reuses_skill_and_does_not_save_a_duplicate(self):
        with tempfile.TemporaryDirectory() as folder, patch("device_tools.ROOT", Path(folder)):
            skills = Path(folder) / "skills"
            save_local_skill(skills, "blink", "blink", '{"steps":[{"op":"rgb.blink"}]}')
            bridge = Mock()
            bridge.mcp = Registry()
            bridge.execute_board_code.return_value = '{"job_id":"one"}'
            register_tools(bridge)
            bridge.mcp.functions["run_composed_skill"](
                '[{"skill":"blink","parameters":{"r":40,"g":0,"b":0,"count":2}}]'
            )
            program = json.loads(bridge.execute_board_code.call_args.args[0])
            self.assertEqual(program["steps"][0]["count"], 2)
            self.assertEqual(len(list(skills.glob("*.json"))), 1)

    def test_capabilities_do_not_claim_native_python(self):
        bridge = Mock()
        bridge.mcp = Registry()
        bridge.request_to.return_value = '{"profile":"esp32-generic","native_execution":true}'
        register_tools(bridge)
        value = json.loads(bridge.mcp.functions["hardware_capabilities"]())
        self.assertTrue(value["native_execution"])
        self.assertEqual(value["host_python"]["location"], "laptop")
