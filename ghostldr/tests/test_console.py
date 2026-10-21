"""Console discovery: PS4 and PS5 answers, and which console is picked."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import console  # noqa: E402

PS5_ON = (b"HTTP/1.1 200 Ok\nhost-id:AAAAAAAAAAAA\nhost-type:PS5\nhost-name:PS5-A\n"
          b"host-request-port:997\ndevice-discovery-protocol-version:00030010\nsystem-version:13600007\n")
PS4_SWRR = (b"HTTP/1.1 200 Ok\nhost-id:bbbbbbbbbbbb\nhost-type:PS4\nhost-name:PS4-B\n"
            b"host-request-port:997\ndevice-discovery-protocol-version:00020020\nsystem-version:09000001\n"
            b"running-app-name:STAR WARS Racer Revenge\nrunning-app-titleid:CUSA03492\n")
PS4_STANDBY = (b"HTTP/1.1 620 Server Standby\nhost-id:CCCCCCCCCCCC\nhost-type:PS4\nhost-name:PS4-C\n"
               b"device-discovery-protocol-version:00020020\nsystem-version:11000001\n")


class Parse(unittest.TestCase):
    def test_ps5(self):
        c = console.parse_reply("10.0.0.1", PS5_ON)
        self.assertEqual((c.kind, c.name, c.host_id, c.version, c.on, c.app_id), ("PS5", "PS5-A", "AAAAAAAAAAAA", "13600007", True, ""))

    def test_ps4_running_swrr(self):
        c = console.parse_reply("10.0.0.2", PS4_SWRR)
        self.assertEqual((c.kind, c.host_id, c.app_id, c.swrr), ("PS4", "BBBBBBBBBBBB", "CUSA03492", "EU"))
        self.assertIn("SWRR EU", c.describe())

    def test_standby(self):
        c = console.parse_reply("10.0.0.3", PS4_STANDBY)
        self.assertFalse(c.on)
        self.assertIn("rest mode", c.describe())

    def test_kind_from_port_when_missing(self):
        c = console.parse_reply("10.0.0.4", b"HTTP/1.1 200 Ok\nhost-name:X\n", "PS4")
        self.assertEqual(c.kind, "PS4")


class Pick(unittest.TestCase):
    def found(self, *consoles):
        return {c.ip: c for c in consoles}

    def test_only_console(self):
        hits = console.pick(self.found(console.parse_reply("10.0.0.1", PS5_ON)))
        self.assertEqual([c.ip for c in hits], ["10.0.0.1"])

    def test_prefers_swrr_and_ignores_standby(self):
        ps5 = console.parse_reply("10.0.0.1", PS5_ON)
        ps4 = console.parse_reply("10.0.0.2", PS4_SWRR)
        sleeping = console.parse_reply("10.0.0.3", PS4_STANDBY)
        self.assertEqual([c.ip for c in console.pick(self.found(ps5, ps4, sleeping))], ["10.0.0.2"])

    def test_prefers_loader(self):
        a = console.parse_reply("10.0.0.1", PS5_ON)
        b = console.parse_reply("10.0.0.5", PS5_ON.replace(b"AAAAAAAAAAAA", b"DDDDDDDDDDDD"))
        b.loader = True
        self.assertEqual([c.ip for c in console.pick(self.found(a, b))], ["10.0.0.5"])

    def test_ambiguous(self):
        a = console.parse_reply("10.0.0.1", PS5_ON)
        b = console.parse_reply("10.0.0.5", PS5_ON.replace(b"AAAAAAAAAAAA", b"DDDDDDDDDDDD"))
        self.assertEqual(len(console.pick(self.found(a, b))), 2)

    def test_by_host_id(self):
        ps5 = console.parse_reply("10.0.0.1", PS5_ON)
        ps4 = console.parse_reply("10.0.0.2", PS4_SWRR)
        self.assertEqual([c.ip for c in console.pick(self.found(ps5, ps4), "AAAAAAAAAAAA")], ["10.0.0.1"])


if __name__ == "__main__":
    unittest.main()
