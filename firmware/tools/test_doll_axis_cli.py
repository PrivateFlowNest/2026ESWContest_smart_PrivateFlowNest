"""Line-handling tests of doll_axis_cli.py with a fake serial port (no hardware, no pyserial needed).

    python -m unittest discover -s firmware/tools -p "test_doll_axis_cli.py"
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import doll_axis_cli as cli  # noqa: E402


class FakeSerial:
    """Scripted controller: responder(command) returns byte chunks delivered one read() at a time,
    so lines arrive split at arbitrary points."""

    def __init__(self, responder, boot=b""):
        self.responder = responder
        self.pending = [boot] if boot else []
        self.written = []
        self.closed = False

    @property
    def in_waiting(self):
        return len(self.pending[0]) if self.pending else 0

    def write(self, data):
        self.written.append(data)
        for line in data.decode("ascii").splitlines():
            self.pending.extend(self.responder(line))
        return len(data)

    def read(self, size=1):
        if not self.pending:
            time.sleep(0.001)  # a real port would wait for its timeout
            return b""
        chunk = self.pending.pop(0)
        if len(chunk) > size:
            self.pending.insert(0, chunk[size:])
            chunk = chunk[:size]
        return chunk

    def close(self):
        self.closed = True


def controller(command):
    replies = {
        "HOME": [b"OK HO", b"ME\r\nEVT HO", b"MED\r\n"],
        "MOVE bed": [b"OK MOVE bed 120.00\n", b"EVT ARRIVED bed 120.00\n"],
        "POS?": [b"OK POS 120.00 IDLE 1\n"],
        "VERSION?": [b"OK VERSION doll_axis hm_axis/1.0.0\n"],
    }
    return replies.get(command, [b"ERR UNKNOWN_COMMAND " + command.encode("ascii")[:16] + b"\n"])


def run_main(fake, *argv):
    output = []
    code = cli.main(["--port", "FAKE", "--settle", "0", "--reply-timeout", "0.3",
                     "--motion-timeout", "0.5"] + list(argv),
                    serial_factory=lambda port, baud: fake, out=output.append)
    return code, output


class CliTest(unittest.TestCase):
    def test_home_move_pos_wait_for_replies_and_events(self):
        fake = FakeSerial(controller)
        code, output = run_main(fake, "HOME", "MOVE bed", "POS?")
        self.assertEqual(0, code)
        self.assertEqual([b"HOME\n", b"MOVE bed\n", b"POS?\n"], fake.written)
        self.assertIn("< EVT HOMED", output)
        self.assertIn("< EVT ARRIVED bed 120.00", output)
        self.assertIn("< OK POS 120.00 IDLE 1", output)
        self.assertTrue(fake.closed)

    def test_err_reply_stops_the_batch(self):
        fake = FakeSerial(lambda command: [b"ERR NOT_HOMED send HOME first\n"])
        code, output = run_main(fake, "MOVE bed", "POS?")
        self.assertEqual(1, code)
        self.assertEqual([b"MOVE bed\n"], fake.written)
        self.assertIn("< ERR NOT_HOMED send HOME first", output)

    def test_motion_error_event_fails(self):
        fake = FakeSerial(lambda command: [b"OK HOME\n", b"EVT ERROR HOME_FAILED no_trigger\n"])
        code, _ = run_main(fake, "HOME")
        self.assertEqual(1, code)

    def test_stopped_motion_is_not_success(self):
        fake = FakeSerial(lambda command: [b"OK MOVE bed 120.00\n", b"EVT STOPPED 40.00\n"])
        code, _ = run_main(fake, "MOVE bed")
        self.assertEqual(1, code)

    def test_boot_noise_is_skipped(self):
        boot = b"\x00\xffets Jul 29 2019 12:21:46\r\nrst:0x1 (POWERON_RESET)\r\nEVT BOOT doll_axis hm_axis/1.0.0\n"
        fake = FakeSerial(controller, boot=boot)
        code, output = run_main(fake, "VERSION?")
        self.assertEqual(0, code)
        self.assertTrue(any(line.startswith("# ") and "ets Jul 29" in line for line in output))
        self.assertIn("< EVT BOOT doll_axis hm_axis/1.0.0", output)
        self.assertIn("< OK VERSION doll_axis hm_axis/1.0.0", output)

    def test_controller_reset_during_motion_fails(self):
        fake = FakeSerial(lambda command: [b"OK MOVE bed 120.00\n", b"EVT BOOT doll_axis hm_axis/1.0.0\n"])
        code, output = run_main(fake, "MOVE bed")
        self.assertEqual(1, code)
        self.assertTrue(any("restarted" in line for line in output))

    def test_events_before_the_reply_are_not_taken_as_the_reply(self):
        fake = FakeSerial(lambda command: [b"EVT ARRIVED bed 120.00\n", b"OK POS 120.00 IDLE 1\n"])
        client = cli.AxisClient(fake)
        result = client.command("POS?")
        self.assertEqual("OK POS 120.00 IDLE 1", result.reply)
        self.assertTrue(result.ok)

    def test_no_wait_returns_after_the_reply(self):
        fake = FakeSerial(lambda command: [b"OK MOVE bed 120.00\n"])
        code, _ = run_main(fake, "--no-wait", "MOVE bed")
        self.assertEqual(0, code)

    def test_reply_timeout(self):
        fake = FakeSerial(lambda command: [])
        start = time.monotonic()
        code, output = run_main(fake, "POS?")
        self.assertEqual(2, code)
        self.assertTrue(any(line.startswith("timeout") for line in output))
        self.assertLess(time.monotonic() - start, 3.0)

    def test_motion_timeout(self):
        fake = FakeSerial(lambda command: [b"OK HOME\n"])
        code, _ = run_main(fake, "HOME")
        self.assertEqual(2, code)

    def test_bad_commands_never_reach_the_port(self):
        for bad in ("MOVE\nbed", "x" * 96, "MOVE béd", "   "):
            with self.assertRaises(ValueError):
                cli.check_command(bad)
        fake = FakeSerial(controller)
        code, output = run_main(fake, "MOVE\nbed")
        self.assertEqual(2, code)
        self.assertEqual([], fake.written)

    def test_garbage_without_newline_does_not_grow_without_bound(self):
        fake = FakeSerial(lambda command: [b"\xaa" * 10000, b"\nOK POS 0.00 UNHOMED 0\n"])
        client = cli.AxisClient(fake)
        self.assertEqual("OK POS 0.00 UNHOMED 0", client.command("POS?").reply)

    def test_usage_errors(self):
        self.assertEqual(2, cli.main(["HOME"], serial_factory=None, out=lambda text: None))
        self.assertEqual(2, cli.main(["--port", "X"], serial_factory=None, out=lambda text: None))

        def broken(port, baud):
            raise OSError("no such port")

        output = []
        self.assertEqual(2, cli.main(["--port", "X", "HOME"], serial_factory=broken, out=output.append))
        self.assertTrue(output[0].startswith("serial: cannot open X"))


if __name__ == "__main__":
    unittest.main()
