import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

from native_scripts import read_script, register_native_tools, save_script, validate_source
from tests.test_mcp_tools import Registry
from skill_router import SkillCatalog


class NativeScriptTests(unittest.TestCase):
    def test_background_execution_and_mailbox_contract(self):
        bridge = Mock()
        bridge.mcp = Registry()
        register_native_tools(bridge, Path.cwd())
        bridge.mcp.functions["execute_device_code"](
            "while true do device.sleep_ms(100) end", background=True
        )
        self.assertIs(bridge.request_to.call_args.args[-1]["background"], True)
        bridge.mcp.functions["send_device_message"]("native-42", '{"stop":true}')
        self.assertEqual(
            bridge.request_to.call_args.args[1:],
            ("/runtime/message", "POST", {"job_id": "native-42", "data": {"stop": True}}),
        )
        bridge.request_to.reset_mock()
        for data in ("null", "not-json", '"' + "x" * 512 + '"'):
            with self.assertRaises(ValueError):
                bridge.mcp.functions["send_device_message"]("native-42", data)
        bridge.request_to.assert_not_called()

    def test_source_persists_and_alias_survives_catalog_reload(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            save_script(root, "device_math", "return 6 * 7", "device math", ["расчёт на плате"])
            item, code = read_script(root / "lua", "device_math")
            self.assertEqual(code, "return 6 * 7")
            self.assertEqual(item["location"], "device")
            self.assertEqual(SkillCatalog(root).find("расчёт на плате"), "device_math")

    def test_modified_source_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            save_script(root, "sample", "return 1", "sample")
            (root / "lua/sample.lua").write_text("return 2", encoding="utf-8")
            with self.assertRaises(ValueError):
                read_script(root / "lua", "sample")

    def test_invalid_name_source_and_alias_do_not_write(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, source, aliases in [
                ("../escape", "return 1", []),
                ("x", "a" * 4097, []),
                ("x", "\0", []),
                ("x", "return 1", [False]),
            ]:
                with self.assertRaises(ValueError):
                    save_script(root, name, source, "sample", aliases)
            self.assertEqual(list(root.iterdir()), [])

    def test_native_rpc_does_not_reflash_or_replay(self):
        with tempfile.TemporaryDirectory() as folder:
            bridge = Mock()
            bridge.mcp = Registry()
            bridge.request_to.return_value = '{"job_id":"native-1","status":"running"}'
            register_native_tools(bridge, folder)
            result = bridge.mcp.functions["execute_device_code"]("return 42", 1000)
            self.assertEqual(json.loads(result)["job_id"], "native-1")
            self.assertEqual(bridge.request_to.call_args.args[1:3], ("/runtime/execute", "POST"))
            bridge.request_to.side_effect = TimeoutError("ambiguous delivery")
            with self.assertRaises(TimeoutError):
                bridge.mcp.functions["execute_device_code"]("return 42", 1000)
            self.assertEqual(bridge.request_to.call_count, 2)

    def test_boolean_timeout_and_injected_job_id_rejected_before_network(self):
        bridge = Mock()
        bridge.mcp = Registry()
        register_native_tools(bridge, Path.cwd())
        with self.assertRaises(ValueError):
            bridge.mcp.functions["execute_device_code"]("return 1", True)
        with self.assertRaises(ValueError):
            bridge.mcp.functions["device_code_status"]("../reset?x=1")
        bridge.request_to.assert_not_called()

    def test_binary_chunks_are_not_accepted_as_source(self):
        with self.assertRaises(ValueError):
            validate_source(b"\x1bLua")

    def test_pin_policy_is_explicit_bounded_and_never_flashes(self):
        bridge = Mock()
        bridge.mcp = Registry()
        register_native_tools(bridge, Path.cwd())
        configure = bridge.mcp.functions["configure_device_pins"]
        for pins in ([True], [-1], [64], ["1"], list(range(41))):
            with self.assertRaises(ValueError):
                configure(pins)
        bridge.request_to.assert_not_called()
        configure([3, 1, 3])
        self.assertEqual(
            bridge.request_to.call_args.args[1:],
            ("/runtime/pins", "POST", {"pins": [1, 3]}),
        )
        configure([])
        self.assertEqual(bridge.request_to.call_args.args[-1], {"pins": []})

    def test_wifi_scan_runs_on_board_as_native_code(self):
        bridge = Mock()
        bridge.mcp = Registry()
        register_native_tools(bridge, Path.cwd())
        bridge.mcp.functions["scan_wifi_networks"]()
        self.assertEqual(
            bridge.request_to.call_args.args[1:],
            (
                "/runtime/execute",
                "POST",
                {"source": "return device.wifi_scan()", "timeout_ms": 15000},
            ),
        )
