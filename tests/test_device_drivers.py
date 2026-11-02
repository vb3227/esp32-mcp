"""Execute generated drivers in real Lua with simulated peripheral replies."""

import json
from pathlib import Path
import tempfile
import unittest
import struct
from unittest.mock import Mock

from lupa.lua54 import LuaRuntime, LuaError

from device_drivers import DRIVERS, render_driver, register_driver_tools
from native_scripts import read_script
from tests.test_mcp_tools import Registry


class DeviceDriverTests(unittest.TestCase):
    def test_bmp280_datasheet_compensation_vector(self):
        calibration = struct.pack(
            "<HhhHhhhhhhhh",
            27504,
            26435,
            -1000,
            36477,
            -10685,
            3024,
            2855,
            140,
            -7,
            15500,
            -14600,
            6000,
        )

        def sample(raw):
            return bytes((raw >> 12, (raw >> 4) & 255, (raw & 15) << 4))

        registers = {
            0xD0: b"\x58",
            0x88: calibration,
            0xF7: sample(415148) + sample(519888) + b"\x00\x00",
        }
        lua = LuaRuntime(encoding=None)
        lua.execute(b"device={sleep_ms=function(ms) end}")

        def transfer(sda, scl, address, tx, count):
            return registers[tx[0]][:count] if count else b""

        lua.globals().device[b"i2c_transfer"] = transfer
        result = lua.execute(render_driver("bmp280", {"sda": 21, "scl": 22}).encode())
        self.assertAlmostEqual(result[b"temperature_c"], 25.082, places=2)
        self.assertAlmostEqual(result[b"pressure_pa"], 100653.27, delta=1)

    def test_sht31_rejects_corrupt_crc_instead_of_reporting_measurement(self):
        lua = LuaRuntime(encoding=None)
        lua.execute(b"device={sleep_ms=function(ms) end}")
        lua.globals().device[b"i2c_transfer"] = lambda a, b, c, tx, n: bytes(n)
        with self.assertRaisesRegex(LuaError, "CRC mismatch"):
            lua.execute(render_driver("sht31", {"sda": 21, "scl": 22}).encode())

    def test_every_catalog_program_compiles_in_lua54_and_fits_device(self):
        lua = LuaRuntime()
        compile_lua = lua.eval("function(s) return assert(load(s)) end")
        for name, driver in DRIVERS.items():
            config = (
                {"sda": 21, "scl": 22}
                if driver.bus == "i2c"
                else {"sck": 18, "miso": 19, "mosi": 23, "cs": 5, "ce": 4}
                if driver.bus == "spi"
                else {"pin": 4}
            )
            for background in (False, True):
                with self.subTest(driver=name, background=background):
                    source = render_driver(name, config, background)
                    self.assertLessEqual(len(source.encode()), 4096)
                    compile_lua(source)

    def test_radio_tx_registers_width_padding_and_expander_controls(self):
        lua = LuaRuntime(encoding=None)
        lua.execute(b"""
          device={};writes={};regs={};clock=0;packet=nil
          function device.k10_gpio_write(pin,v,idle) writes[pin]={v,idle} end
          function device.sleep_ms(ms) clock=clock+ms end
          function device.spi_transfer(sck,miso,mosi,cs,s,hz,mode)
            assert(sck==1 and miso==2 and mosi==3 and cs==-1 and hz==4000000 and mode==0)
            local c=s:byte();if c>=32 and c<=63 then regs[c&31]=s:sub(2) end
            if c==160 then packet=s:sub(2) end
            if c==7 then return string.char(0,32) end
            if c<32 then return string.char(0)..(regs[c] or string.char(0)) end
            return string.rep(string.char(0),#s)
          end
        """)
        source = render_driver(
            "nrf24l01",
            {
                "sck": 1,
                "miso": 2,
                "mosi": 3,
                "cs": "P13",
                "ce": "P14",
                "width": 4,
                "channel": 76,
                "rate_kbps": 250,
                "power": 2,
                "payload_hex": "abcd",
            },
        )
        result = lua.execute(source.encode())
        self.assertTrue(result[b"tx_complete"])
        self.assertFalse(result[b"remote_ack"])
        self.assertEqual(lua.globals().packet, b"\xab\xcd\x00\x00")
        self.assertEqual(lua.globals().regs[5], b"L")
        self.assertEqual(lua.globals().regs[6], b"$")
        self.assertEqual(lua.globals().writes[b"P13"][1], 1)
        self.assertEqual(lua.globals().writes[b"P14"][1], 0)

    def test_radio_rejects_injection_bad_rates_duplicate_pins_and_oversize(self):
        base = {"sck": 18, "miso": 19, "mosi": 23, "cs": 5, "ce": 4}
        for change in (
            {"ce": 18},
            {"ce": "P11"},
            {"ce": 'P13");error(1)'},
            {"address_hex": "xx"},
            {"payload_hex": "a"},
            {"payload_hex": "ab" * 33},
            {"rate_kbps": 500},
            {"power": True},
            {"channel": 126},
        ):
            with self.subTest(change=change), self.assertRaises(ValueError):
                render_driver("nrf24l01", base | change)

    def test_radio_rx_packet_and_receiver_shutdown(self):
        lua = LuaRuntime(encoding=None)
        lua.execute(b"""
          device={};regs={};ce=0;cs=1
          function device.pin_mode(pin,mode) end
          function device.digital_write(pin,v) if pin==4 then ce=v else cs=v end end
          function device.sleep_ms(ms) end
          function device.hex(s) return (s:gsub('.',function(c) return string.format('%02x',c:byte()) end)) end
          function device.spi_transfer(a,b,c,manual,s,hz,mode)
            local cmd=s:byte()
            if cmd>=32 and cmd<=63 then regs[cmd&31]=s:sub(2) end
            if cmd==0x61 then return string.char(0,0xde,0xad,0xbe,0xef) end
            if cmd==0x17 then return string.char(0,0) end
            if cmd<32 then return string.char(0)..(regs[cmd] or string.char(0)) end
            return string.rep(string.char(0),#s)
          end
        """)
        source = render_driver(
            "nrf24l01", {"sck": 18, "miso": 19, "mosi": 23, "cs": 5, "ce": 4, "width": 4}
        )
        result = lua.execute(source.encode())
        self.assertTrue(result[b"received"])
        self.assertEqual(result[b"payload_hex"], b"deadbeef")
        self.assertEqual(lua.globals().ce, 0)
        self.assertEqual(lua.globals().cs, 1)

    def test_absent_radio_fails_readback(self):
        lua = LuaRuntime(encoding=None)
        lua.execute(b"""
          device={pin_mode=function() end,digital_write=function() end,sleep_ms=function() end}
          function device.spi_transfer(a,b,c,d,s,hz,mode) return string.rep(string.char(255),#s) end
        """)
        source = render_driver("nrf24l01", {"sck": 18, "miso": 19, "mosi": 23, "cs": 5, "ce": 4})
        with self.assertRaisesRegex(LuaError, "SPI readback failed"):
            lua.execute(source.encode())

    def test_generated_skill_survives_reload_with_background_and_short_alias(self):
        with tempfile.TemporaryDirectory() as folder:
            bridge = Mock()
            bridge.mcp = Registry()
            register_driver_tools(bridge, folder)
            result = json.loads(
                bridge.mcp.functions["create_device_driver"](
                    "nrf24l01",
                    json.dumps({"sck": 18, "miso": 19, "mosi": 23, "cs": 5, "ce": 4}),
                    "radio_rx",
                    install=False,
                    background=True,
                )
            )
            self.assertTrue(result["saved_locally"])
            item, source = read_script(Path(folder) / "skills/lua", "radio_rx")
            self.assertTrue(item["background"])
            self.assertIn("nrf24l01", item["aliases"])
            self.assertIn("while true", source)
            bridge.request_to.assert_not_called()


if __name__ == "__main__":
    unittest.main()
