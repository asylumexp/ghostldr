"""Console discovery: PS4 and PS5 answers, and which console is picked."""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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


class ChooseAndList(unittest.TestCase):
    """console_ip(): the list on stderr, the pick, the question at a terminal."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        env = {k: v for k, v in os.environ.items() if k not in ("PS_HOST", "PS5_HOST", "PS_HOST_ID", "PS5_HOST_ID")}
        for p in (mock.patch.dict(os.environ, env, clear=True),
                  mock.patch.object(console, "CACHE", Path(tmp.name) / "console_ip")):
            p.start()
            self.addCleanup(p.stop)

    def run_ip(self, found, tty=False, answers=()):
        err = io.StringIO()
        with mock.patch.object(console, "discover", return_value=found), \
             mock.patch.object(sys.stdin, "isatty", return_value=tty), \
             mock.patch("builtins.input", side_effect=list(answers)), \
             contextlib.redirect_stderr(err):
            try:
                return console.console_ip(), err.getvalue()
            except SystemExit as e:
                return e, err.getvalue()

    def consoles(self):
        ps5 = console.parse_reply("10.0.0.1", PS5_ON)
        other = console.parse_reply("10.0.0.5", PS5_ON.replace(b"AAAAAAAAAAAA", b"DDDDDDDDDDDD").replace(b"PS5-A", b"PS5-D"))
        sleeping = console.parse_reply("10.0.0.3", PS4_STANDBY)
        return ps5, other, sleeping

    def test_one_console_is_not_listed(self):
        ip, err = self.run_ip({"10.0.0.1": console.parse_reply("10.0.0.1", PS5_ON)})
        self.assertEqual((ip, err), ("10.0.0.1", ""))

    def test_several_are_listed_with_the_pick_marked(self):
        ps5, other, sleeping = self.consoles()
        other.loader = True
        ip, err = self.run_ip({c.ip: c for c in (ps5, other, sleeping)})
        self.assertEqual(ip, "10.0.0.5")
        self.assertIn("Found 3 consoles:", err)
        self.assertIn("2) 10.0.0.5", err)
        self.assertIn("<- using this one", err.splitlines()[2])
        self.assertIn("Luac0re's loader is listening", err)
        self.assertIn("rest mode", err)
        self.assertIn("firmware 13.60", err)

    def test_undecided_without_a_terminal_stops_with_the_list(self):
        ps5, other, _ = self.consoles()
        result, err = self.run_ip({c.ip: c for c in (ps5, other)})
        self.assertIsInstance(result, SystemExit)
        self.assertIn("set PS_HOST", str(result))
        self.assertIn("Found 2 consoles:", err)

    def test_undecided_at_a_terminal_asks(self):
        ps5, other, _ = self.consoles()
        ip, err = self.run_ip({c.ip: c for c in (ps5, other)}, tty=True, answers=["9", "2"])
        self.assertEqual(ip, "10.0.0.5")
        self.assertEqual(os.environ.get("PS_HOST_ID"), "DDDDDDDDDDDD")
        self.assertIn("export PS_HOST_ID=DDDDDDDDDDDD", err)

    def test_quiet_never_prints_or_asks(self):
        ps5, other, _ = self.consoles()
        err = io.StringIO()
        with mock.patch.object(console, "discover", return_value={c.ip: c for c in (ps5, other)}), \
             mock.patch.object(sys.stdin, "isatty", return_value=True), \
             mock.patch("builtins.input", side_effect=AssertionError("asked")), \
             contextlib.redirect_stderr(err):
            with self.assertRaises(SystemExit):
                console.console_ip(quiet=True)
        self.assertEqual(err.getvalue(), "")

    def test_nothing_answers(self):
        result, _ = self.run_ip({})
        self.assertIsInstance(result, SystemExit)
        self.assertIn("no PS4 or PS5 answered", str(result))


if __name__ == "__main__":
    unittest.main()
