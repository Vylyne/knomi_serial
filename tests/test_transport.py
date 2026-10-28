#!/usr/bin/env python3
"""The serial link never makes Klipper's reactor wait.

The module runs on the reactor thread, which also feeds the MCUs their step
queues. A write that waited on a slow display - even for the 0.2 s a write
timeout used to allow - could let a homing move's queue run dry and shut the
printer down with "Timer too close". These cases pin the non-blocking contract:
writes take what the port will accept now, frames are never cut in half,
stale state gives way to fresh state, and a display that takes nothing for long
enough is dropped rather than waited on.

    python tests/test_transport.py      # or: pytest tests/
"""

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, want {want!r}")


class Port:
    """Has `room` bytes of space left, recording what it was given."""

    def __init__(self, room):
        self.room = room
        self.data = b""

    def __call__(self, view):
        n = min(self.room, len(view))
        self.room -= n
        self.data += bytes(view[:n])
        return n


# ----------------------------------------------------------------------
# Outbox
# ----------------------------------------------------------------------


def test_a_partly_written_frame_is_finished_first():
    box = k.Outbox()
    box.put(b"AAAA")
    box.put(b"BBBB")
    port = Port(3)
    check("first call", box.flush(port), 3)
    check("left", len(box), 5)
    port.room = 100
    box.flush(port)
    check("order and integrity", port.data, b"AAAABBBB")
    check("empty", len(box), 0)


def test_flush_stops_when_the_port_is_full():
    box = k.Outbox()
    box.put(b"AAAA")
    check("nothing taken", box.flush(Port(0)), 0)
    check("still queued", len(box), 4)


def test_unsent_state_gives_way_to_newer_state():
    box = k.Outbox()
    box.put(b"old-state", replaceable=True)
    box.put(b"config")
    box.put(b"new-state", replaceable=True)
    port = Port(100)
    box.flush(port)
    check("stale state skipped, config kept", port.data, b"confignew-state")
    check("replacement is not a drop", box.dropped, 0)


def test_a_state_frame_already_on_the_wire_is_finished():
    """Replacing it half-sent would leave the device a truncated frame."""
    box = k.Outbox()
    box.put(b"STATE-1", replaceable=True)
    box.flush(Port(2))
    box.put(b"STATE-2", replaceable=True)
    port = Port(100)
    box.flush(port)
    check("tail of the first, then the second", port.data, b"ATE-1STATE-2")


def test_other_frames_are_never_replaced():
    box = k.Outbox()
    box.put(b"msg-1")
    box.put(b"msg-2")
    port = Port(100)
    box.flush(port)
    check("both kept, in order", port.data, b"msg-1msg-2")


def test_a_backlog_past_the_limit_sheds_the_oldest_unsent_frames():
    box = k.Outbox(limit=11)
    box.put(b"AAAA")
    box.flush(Port(1))  # "A" is on the wire; this frame must survive
    box.put(b"BBBB")
    box.put(b"CCCC")
    box.put(b"DDDD")
    check("within the limit", len(box) <= 11, True)
    check("counted", box.dropped, 1)
    port = Port(100)
    box.flush(port)
    check("started frame finished, oldest unsent dropped",
          port.data, b"AAACCCCDDDD")


# ----------------------------------------------------------------------
# A section on a real non-blocking descriptor
# ----------------------------------------------------------------------


class Clock:
    def __init__(self):
        self.now = 100.0

    def monotonic(self):
        return self.now


class PipeSerial:
    """The parts of pyserial the section uses, over an os.pipe() write end."""

    def __init__(self):
        self.read_fd, self.write_fd = os.pipe()
        os.set_blocking(self.write_fd, False)
        self.is_open = True

    def fileno(self):
        return self.write_fd

    def fill(self):
        while True:
            try:
                os.write(self.write_fd, b"\0" * 65536)
            except BlockingIOError:
                return

    def drain(self):
        os.set_blocking(self.read_fd, False)
        try:
            while os.read(self.read_fd, 65536):
                pass
        except BlockingIOError:
            pass

    def close(self):
        self.is_open = False
        for fd in (self.read_fd, self.write_fd):
            try:
                os.close(fd)
            except OSError:
                pass


def section(port):
    d = object.__new__(k.Knomi_Serial)
    d.name = "knomi_serial T0_knomi"
    d.reactor = Clock()
    d.serial = port
    d.pending_cmd = b""
    d.outbox = k.Outbox()
    d.tx_stalled_since = None
    return d


def test_a_full_port_queues_instead_of_waiting():
    port = PipeSerial()
    try:
        port.fill()
        d = section(port)
        d._write(b"FRAME")
        check("still connected", d.serial is port, True)
        check("queued", len(d.outbox), 5)
        port.drain()
        d._flush()
        check("sent once there was room", len(d.outbox), 0)
        check("no stall left behind", d.tx_stalled_since, None)
    finally:
        port.close()


def test_a_display_that_takes_nothing_is_dropped_not_waited_on():
    port = PipeSerial()
    try:
        port.fill()
        d = section(port)
        d._write(b"FRAME")
        d.reactor.now += k._TX_STALL_TIMEOUT - 0.1
        d._flush()
        check("not yet", d.serial is port, True)
        d.reactor.now += 0.2
        d._flush()
        check("dropped", d.serial, None)
        check("backlog cleared with it", len(d.outbox), 0)
    finally:
        port.close()


def test_a_slow_display_that_keeps_draining_is_kept():
    port = PipeSerial()
    try:
        d = section(port)
        for _ in range(50):
            port.fill()
            d._write(b"STATE", replaceable=True)
            d.reactor.now += k._TX_STALL_TIMEOUT / 4
            port.drain()
            d._flush()
        check("still connected", d.serial is port, True)
    finally:
        port.close()


def test_a_hard_write_error_drops_the_port():
    port = PipeSerial()
    d = section(port)
    os.close(port.read_fd)  # writing now raises BrokenPipeError
    try:
        d._write(b"FRAME")
        check("dropped", d.serial, None)
    finally:
        port.close()


def test_connect_puts_the_port_in_non_blocking_mode():
    port = PipeSerial()
    os.set_blocking(port.write_fd, True)
    d = section(None)
    d.name = "knomi_serial T0_knomi"
    d.config_serial = "/dev/ttyUSB0"
    real = k.serial.Serial
    k.serial.Serial = lambda *a, **kw: port
    try:
        check("connected", d._connect(), True)
        check("non-blocking", os.get_blocking(port.write_fd), False)
    finally:
        k.serial.Serial = real
        port.close()


# ----------------------------------------------------------------------
# Reading
# ----------------------------------------------------------------------


class ReadSerial:
    def __init__(self, *chunks):
        self.chunks = list(chunks)
        self.is_open = True
        self.reads = 0

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, n):
        self.reads += 1
        chunk = self.chunks.pop(0)
        assert n == len(chunk)
        return chunk

    def close(self):
        self.is_open = False


def reader(*chunks):
    d = section(ReadSerial(*chunks))
    d.seen = []
    d._process_cmd = d.seen.append
    return d


def test_everything_waiting_is_read_in_one_call():
    d = reader(b"KNOMI_CMD:STOP\nKNOMI_CMD:CFG?\n")
    d.poll(0)
    check("both commands", d.seen, [b"STOP", b"CFG?"])
    check("one read", d.serial.reads, 1)


def test_a_line_split_across_reads_is_joined():
    d = reader(b"KNOMI_CMD:RPT:id=19", b"aa44\n")
    d.poll(0)
    check("nothing yet", d.seen, [])
    d.poll(0)
    check("joined", d.seen, [b"RPT:id=19aa44"])


def test_lines_without_the_prefix_are_ignored():
    d = reader(b"boot noise\r\nKNOMI_CMD:STOP\n")
    d.poll(0)
    check("only the command", d.seen, [b"STOP"])


def test_an_overlong_line_is_discarded_and_bounded():
    noise = b"KNOMI_CMD:" + b"x" * (k._CMD_MAX_LEN + 10)
    d = reader(noise)
    d.poll(0)
    check("partial buffer bounded", len(d.pending_cmd) <= k._CMD_MAX_LEN, True)
    d = reader(noise + b"\nKNOMI_CMD:STOP\n")
    d.poll(0)
    check("long line dropped, next kept", d.seen, [b"STOP"])


def test_reading_stops_once_a_command_drops_the_port():
    d = reader(b"KNOMI_CMD:RPT:id=wrong\nKNOMI_CMD:STOP\n")

    def process(cmd):
        d.seen.append(cmd)
        d._drop("wrong display")

    d._process_cmd = process
    d.poll(0)
    check("nothing after the drop", d.seen, [b"RPT:id=wrong"])


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok    {name}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  {name}\n          {type(e).__name__}: {e}")
    print(f"\n  {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
