"""Host-side USB configuration bounds; no serial port is opened by these tests."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "usb_configuration", Path(__file__).resolve().parents[1] / "scripts/configure_device.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProvisioningTests(unittest.TestCase):
    def test_open_wifi_and_independent_tokens_are_representable(self):
        result = module.configuration("Example network", "", "A" * 32, "B" * 32)
        self.assertEqual(result["password"], "")
        self.assertEqual(
            set(result), {"ssid", "password", "board_token", "gateway_token", "gateway_url"}
        )

    def test_invalid_usb_settings_are_rejected_before_serial(self):
        for values in (
            ("", "password", "A" * 32, "B" * 32, ""),
            ("я" * 17, "password", "A" * 32, "B" * 32, ""),
            ("network", "p" * 64, "A" * 32, "B" * 32, ""),
            ("network", "password", "short", "B" * 32, ""),
            ("network", "password", "A" * 32, "B" * 32, "https://user:secret@example.org"),
            ("network", "password", "A" * 32, "B" * 32, "http://example.org/\nother"),
            ("network\0suffix", "password", "A" * 32, "B" * 32, ""),
        ):
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    module.configuration(*values)
