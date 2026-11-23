"""Tests for the parts of the Živý obraz BLE Gateway without Bluetooth or network.

    .venv/bin/python -m unittest discover tests
"""

import tempfile
import unittest
from pathlib import Path

from picksmart import pack_image
from picksmart.advert import Advertisement
from zivyobraz_ble_gateway import web, zivyobraz
from zivyobraz_ble_gateway.registry import Registry
from zivyobraz_ble_gateway.service import Gateway


def adv(mac="FF:FF:92:85:05:77", sdk_type=75, rssi=-60):
    return Advertisement(address=mac, name="NEMR", rssi=rssi, raw_type=0x4000 | sdk_type, sdk_type=sdk_type,
                         battery_dv=30, sw=0x85, hw=1)


def z2(pixels):
    """Encode palette indices as Z2 (test helper; the server does this for real)."""
    out, i = bytearray(b"Z2"), 0
    while i < len(pixels):
        run = 1
        while i + run < len(pixels) and run < 63 and pixels[i + run] == pixels[i]:
            run += 1
        out.append(pixels[i] << 6 | run)
        i += run
    return bytes(out)


class ZivyObraz(unittest.TestCase):
    def test_decode_formats(self):
        pixels = bytes([0] * 70 + [1] * 5 + [2] * 3 + [3] * 2)  # runs longer than 63 and across "rows"
        self.assertEqual(zivyobraz.decode_z(z2(pixels), 8, 10), pixels)
        self.assertEqual(zivyobraz.decode_z(bytes([90, 51, 0x22, 0xA1]), 3, 1), bytes([1, 1, 5]))
        # short stream: the rest stays white; too long: cut
        self.assertEqual(zivyobraz.decode_z(b"Z2\x42", 4, 1), bytes([1, 1, 0, 0]))
        self.assertEqual(zivyobraz.decode_z(b"Z2\x7f", 2, 1), bytes([1, 1]))

    def test_decode_image_colours(self):
        w, h = zivyobraz.display_size(46)  # 2.13" BWRY
        pixels = bytes([0] * (w * h // 4) + [1] * (w * h // 4) + [2] * (w * h // 4) + [3] * (w * h // 4))
        image = zivyobraz.decode_image(z2(pixels), 46)
        self.assertEqual(image.size, (w, h))
        self.assertEqual(image.getpixel((0, 0)), zivyobraz.WHITE)
        self.assertEqual(image.getpixel((0, h - 1)), zivyobraz.YELLOW)
        packed = pack_image(46, image)  # BWRY packing finds all four colours
        self.assertEqual({b for b in packed}, {0x00, 0x55, 0xAA, 0xFF})

    def test_not_z(self):
        with self.assertRaises(ValueError):  # PNG is not supported: the server sends Z2/Z3 only
            zivyobraz.decode_image(b"\x89PNG\r\n\x1a\n", 75)
        with self.assertRaises(ValueError):  # Z1 is no longer used by the server
            zivyobraz.decode_image(b"Z1\x01\x03", 75)
        with self.assertRaises(ValueError):
            zivyobraz.decode_image(b"<html>", 75)

    def test_answer(self):
        a = zivyobraz.parse_answer([("timestamp", "05242116"), ("Sleep", "2"), ("Rotate", "1")], b"Z2")
        self.assertEqual((a.timestamp, a.sleep, a.rotate), ("05242116", 120, True))
        a = zivyobraz.parse_answer([("PreciseSleep", "75"), ("Sleep", "2")], b"")
        self.assertEqual((a.timestamp, a.sleep, a.rotate), (None, 75, False))

    def test_request(self):
        r = zivyobraz.build_request("ff:ff:92:85:05:77", 46, 2.9, -70)
        self.assertEqual(r["network"]["mac"], "FF:FF:92:85:05:77")
        self.assertEqual(r["display"], {"type": "Picksmart-46", "width": 296, "height": 128, "colorType": "4C"})
        self.assertEqual(r["system"]["vccVoltage"], 2.9)
        self.assertEqual(zivyobraz.color_type(75), "3C")
        self.assertEqual(zivyobraz.color_type(160), "BW")
        key = zivyobraz.new_api_key()
        self.assertTrue(key.isdigit() and len(key) == 8 and key[0] != "0")


class Registry_(unittest.TestCase):
    def test_add_save_load(self):
        with tempfile.TemporaryDirectory() as d:
            reg = Registry(Path(d) / "tags.json")
            entry = reg.add(adv())
            self.assertIn("ff:ff:92:85:05:77", reg)
            reg.update_advert(adv(rssi=-80))
            again = Registry(Path(d) / "tags.json")
            self.assertEqual(again.get("FF:FF:92:85:05:77")["api_key"], entry["api_key"])
            self.assertEqual(again.get("FF:FF:92:85:05:77")["rssi"], -80)
            self.assertTrue(again.remove("FF:FF:92:85:05:77"))
            self.assertEqual(Registry(Path(d) / "tags.json").tags, {})


class Page(unittest.TestCase):
    def test_render(self):
        with tempfile.TemporaryDirectory() as d:
            gw = Gateway(Registry(Path(d) / "tags.json"), Path(d))
            gw.seen["FF:FF:92:85:05:77"] = adv()
            gw.seen["FF:FF:92:85:05:74"] = adv("FF:FF:92:85:05:74", 46)
            gw.add("FF:FF:92:85:05:77")
            gw.registry.get("FF:FF:92:85:05:77")["poll_result"] = "error: <b>"
            ring = web.LogRing()
            ring.lines.append("12:00:00 added <x>")
            html = web.page(gw, ring, {"style.css": "1"})
            self.assertIn(gw.registry.get("FF:FF:92:85:05:77")["api_key"], html)
            self.assertIn("action=/add", html)  # the second tag can be added
            self.assertIn("4.2<span>", html)
            self.assertNotIn("<b>\"", html)
            self.assertIn("error: &lt;b&gt;", html)
            self.assertIn("added &lt;x&gt;", html)
            with self.assertRaises(ValueError):
                gw.add("00:00:00:00:00:00")


if __name__ == "__main__":
    unittest.main()


class Guard(unittest.TestCase):
    """Same cases as esp32/test/host/test_host.c test_guard."""

    def test_host(self):
        good = ["zo-ble-gateway.local", "ZO-BLE-Gateway.local:80", "zo-ble-gateway", "zo-ble-gateway.lan",
                "zo-ble-gateway.home.arpa", "zo-ble-gateway.local.", "192.168.1.20", "192.168.1.20:8080",
                "[fe80::1]", "[fe80::1]:80", "localhost:8080"]
        bad = ["evil.example", "zo-ble-gateway.evil.example", "zo-ble-gateway.local.evil.example",
               "zo-ble-gatewayx.local", "192.168.1", "1.2.3.4.example", "1234.1.1.1", "zo-ble-gateway.local:8o",
               "", ":80", None]
        for host in good:
            self.assertTrue(web.host_ok(host, {"zo-ble-gateway"}), host)
        for host in bad:
            self.assertFalse(web.host_ok(host, {"zo-ble-gateway"}), host)

    def test_origin(self):
        h = "zo-ble-gateway.local"
        self.assertTrue(web.origin_ok(None, h) and web.origin_ok("", h))
        self.assertTrue(web.origin_ok("http://zo-ble-gateway.local", h))
        self.assertTrue(web.origin_ok("http://ZO-ble-gateway.local/settings?x", h))
        for origin in ["http://evil.example", "http://zo-ble-gateway.local.evil.example",
                       "http://zo-ble-gateway.local:8080", "null"]:
            self.assertFalse(web.origin_ok(origin, h), origin)
        self.assertTrue(web.origin_ok("http://192.168.1.20:8080", "192.168.1.20:8080"))

    def test_middleware(self):
        import asyncio

        from aiohttp.test_utils import TestClient, TestServer

        async def run():
            with tempfile.TemporaryDirectory() as d:
                gw = Gateway(Registry(Path(d) / "tags.json"), Path(d), "http://unused", 60, 86400)
                app = await web.app(gw, web.LogRing(), allow_hosts=["gw.example"])
                async with TestClient(TestServer(app)) as c:
                    ok = {"Host": "gw.example:8080"}
                    self.assertEqual((await c.get("/", headers=ok)).status, 200)
                    self.assertEqual((await c.get("/", headers={"Host": "evil.example"})).status, 403)
                    r = await c.post("/refresh", data={"mac": "x"}, allow_redirects=False,
                                     headers={**ok, "Origin": "http://gw.example:8080"})
                    self.assertEqual(r.status, 303)
                    r = await c.post("/refresh", data={"mac": "x"}, allow_redirects=False,
                                     headers={**ok, "Origin": "http://evil.example"})
                    self.assertEqual(r.status, 403)
        asyncio.run(run())
