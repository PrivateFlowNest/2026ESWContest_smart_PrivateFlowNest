#!/usr/bin/env python3
"""Host CLI for the doll axis controller (firmware/src/doll_axis).

Speaks the USB-serial line protocol of firmware/lib/hm_axis/src/hm_axis_protocol.h. Runs on
Windows, a Jetson or a Raspberry Pi (Python 3.8+, pyserial). Standalone: not part of HomeMind/MQTT.

Examples
  python doll_axis_cli.py --list-ports
  python doll_axis_cli.py --port COM7 "VERSION?" HOME "MOVE bed" "POS?"
  python doll_axis_cli.py --port /dev/ttyACM0 --interactive

Each command waits for its reply (OK/ERR). HOME and MOVE then wait for the end of the motion
(EVT HOMED / ARRIVED / STOPPED / ERROR) unless --no-wait. Ctrl+C while a motion runs sends STOP.
Exit code: 0 all commands succeeded, 1 the controller reported an error (ERR, EVT ERROR, motion
stopped or controller reset), 2 timeout / serial / usage problem.
"""

import argparse
import sys
import time

MAX_LINE = 95  # kLineCapacity - 1 in hm_axis_protocol.h
MOTION_COMMANDS = ("HOME", "MOVE")
TERMINAL_EVENTS = ("EVT HOMED", "EVT ARRIVED", "EVT STOPPED", "EVT ERROR")


class AxisTimeout(Exception):
    """No reply / event in time."""


class Result:
    def __init__(self, command):
        self.command = command
        self.reply = None   # "OK ..." / "ERR ..."
        self.event = None   # terminal EVT line of a motion, if waited for
        self.reset = False  # "EVT BOOT" during the motion: the controller restarted, motion lost

    @property
    def ok(self):
        if self.reset or self.reply is None or not self.reply.startswith("OK"):
            return False
        if self.event is None:
            return True
        return self.event.startswith("EVT HOMED") or self.event.startswith("EVT ARRIVED")


def is_motion(command):
    words = command.split()
    return bool(words) and words[0].upper() in MOTION_COMMANDS


def check_command(command):
    """Client-side check so a typo never reaches the controller as a half line."""
    if not command or not command.strip():
        raise ValueError("empty command")
    if len(command) > MAX_LINE:
        raise ValueError("command longer than %d characters" % MAX_LINE)
    if any(ord(c) < 0x20 and c != "\t" or ord(c) > 0x7E for c in command):
        raise ValueError("command must be printable ASCII on one line")


class AxisClient:
    """Line I/O over any pyserial-like object (write, read, in_waiting)."""

    def __init__(self, port, log=None, clock=time.monotonic):
        self.port = port
        self.log = log if log is not None else (lambda text: None)
        self.clock = clock
        self._buffer = bytearray()

    def send(self, command):
        check_command(command)
        self.log("> " + command)
        self.port.write((command + "\n").encode("ascii"))

    def read_line(self, timeout):
        """Next protocol line (OK/ERR/EVT). Other text (boot noise) is logged and skipped."""
        deadline = self.clock() + timeout
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                raw = bytes(self._buffer[:newline])
                del self._buffer[: newline + 1]
                line = raw.decode("ascii", errors="replace").strip("\r").strip()
                if not line:
                    continue
                if line.startswith(("OK", "ERR", "EVT")):
                    self.log("< " + line)
                    return line
                self.log("# " + line)
                continue
            if self.clock() >= deadline:
                raise AxisTimeout("no line within %.1f s" % timeout)
            chunk = self.port.read(max(1, getattr(self.port, "in_waiting", 0) or 0))
            if chunk:
                self._buffer.extend(chunk)
                if len(self._buffer) > 4096 and b"\n" not in self._buffer:
                    del self._buffer[:-256]  # endless garbage without newline

    def drain(self, seconds):
        """Reads and logs whatever arrives for a while (e.g. EVT BOOT after the port opened)."""
        end = self.clock() + seconds
        lines = []
        while True:
            remaining = end - self.clock()
            if remaining <= 0:
                return lines
            try:
                lines.append(self.read_line(remaining))
            except AxisTimeout:
                return lines

    def command(self, command, wait=True, reply_timeout=2.0, motion_timeout=60.0):
        result = Result(command)
        self.send(command)
        deadline = self.clock() + reply_timeout
        while result.reply is None:
            # An EVT BOOT before the reply only means the controller (re)started before it read
            # this command: the reply that follows is still valid.
            line = self.read_line(max(0.0, deadline - self.clock()))
            if line.startswith(("OK", "ERR")):
                result.reply = line
        if not (wait and is_motion(command) and result.reply.startswith("OK")):
            return result
        deadline = self.clock() + motion_timeout
        while result.event is None:
            line = self.read_line(max(0.0, deadline - self.clock()))
            if line.startswith("EVT BOOT"):
                result.reset = True
                return result
            if line.startswith(TERMINAL_EVENTS):
                result.event = line
        return result

    def stop(self, timeout=10.0):
        """Sends STOP and waits for the motion to end (used on Ctrl+C)."""
        self.send("STOP")
        deadline = self.clock() + timeout
        while True:
            line = self.read_line(max(0.0, deadline - self.clock()))
            if line.startswith(TERMINAL_EVENTS):
                return line


def open_serial(port, baud):
    import serial  # pyserial

    handle = serial.Serial()
    handle.port = port
    handle.baudrate = baud
    handle.timeout = 0.05
    handle.write_timeout = 2.0
    # DTR and RTS both asserted (pyserial's default, set explicitly): Teensy USB serial only sends
    # while DTR is set, and on an ESP32 DevKit auto-reset circuit "both asserted" keeps EN and IO0
    # high (the board keeps running; some OS/driver combinations may still reset it on open).
    handle.dtr = True
    handle.rts = True
    handle.open()
    return handle


def list_ports():
    from serial.tools import list_ports as tools

    for info in tools.comports():
        print("%-14s %s" % (info.device, info.description))


def run(client, commands, args, out):
    exit_code = 0
    for command in commands:
        try:
            check_command(command)
            result = client.command(command, wait=not args.no_wait,
                                    reply_timeout=args.reply_timeout,
                                    motion_timeout=args.motion_timeout)
        except ValueError as error:
            out("usage: %s: %s" % (command, error))
            exit_code = max(exit_code, 2)
            if not args.interactive:
                return exit_code
            continue
        except AxisTimeout as error:
            out("timeout: %s: %s" % (command, error))
            return 2
        except KeyboardInterrupt:
            out("interrupted: sending STOP")
            try:
                client.stop()
            except AxisTimeout:
                out("timeout: no end of motion after STOP")
            return 130
        if result.reset:
            out("controller restarted (EVT BOOT) during %s: it is no longer homed" % command)
        if not result.ok:
            exit_code = max(exit_code, 1)
            if not args.interactive:
                return exit_code
    return exit_code


def interactive_commands(prompt):
    while True:
        try:
            line = input(prompt)
        except EOFError:
            return
        line = line.strip()
        if line.lower() in ("quit", "exit"):
            return
        if line:
            yield line


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("commands", nargs="*", help='protocol commands, e.g. HOME "MOVE bed" "POS?"')
    parser.add_argument("--port", help="serial port (COM7, /dev/ttyACM0, /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    parser.add_argument("--interactive", "-i", action="store_true",
                        help="read commands from stdin (quit/exit or EOF ends)")
    parser.add_argument("--no-wait", action="store_true",
                        help="do not wait for the end of HOME / MOVE")
    parser.add_argument("--settle", type=float, default=0.5,
                        help="seconds to read boot output after opening the port")
    parser.add_argument("--reply-timeout", type=float, default=2.0)
    parser.add_argument("--motion-timeout", type=float, default=60.0)
    return parser


def main(argv=None, serial_factory=open_serial, out=print):
    args = build_parser().parse_args(argv)
    if args.list_ports:
        list_ports()
        return 0
    if not args.port:
        out("usage: --port is required (see --list-ports)")
        return 2
    if not args.commands and not args.interactive:
        out("usage: give commands or --interactive")
        return 2
    try:
        port = serial_factory(args.port, args.baud)
    except Exception as error:  # pyserial raises SerialException / OSError
        out("serial: cannot open %s: %s" % (args.port, error))
        return 2
    try:
        client = AxisClient(port, log=out)
        client.drain(args.settle)
        commands = interactive_commands("axis> ") if args.interactive else args.commands
        return run(client, commands, args, out)
    except KeyboardInterrupt:  # Ctrl+C at the interactive prompt (no motion waiting)
        return 130
    finally:
        close = getattr(port, "close", None)
        if close is not None:
            close()


if __name__ == "__main__":
    sys.exit(main())
