"""The Python link against the C library: tests/interop/c_peer.c over pipes, in real time.

The C peer is built with make (gcc or cc). The tests are skipped when it cannot be built.
"""

import os
import random
import select
import shutil
import subprocess
import threading
import time

import pytest

from reliableserialcomms import (
    StreamLink, NackError, RequestTimeout, QueueFullError,
    EVENT_PEER_RESET, RESPONSE_PEER_RESET, ERROR_CMD_BAD_ARG,
)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PEER_DIR = os.path.join(ROOT, "tests", "interop")
PEER = os.path.join(PEER_DIR, "c_peer")


@pytest.fixture(scope="module")
def peer_binary():
    if shutil.which("make") is None or (shutil.which("gcc") is None and shutil.which("cc") is None):
        pytest.skip("no C compiler")
    r = subprocess.run(["make", "-C", PEER_DIR, "c_peer"], capture_output=True, text=True)
    if r.returncode != 0:
        pytest.fail("c_peer build failed:\n" + r.stdout + r.stderr)
    return PEER


class Pipe:
    """Byte transport to a c_peer process, with optional faults in both directions."""

    def __init__(self, binary, args=(), drop=0.0, flip=0.0, seed=1):
        self.binary = binary
        self.args = [str(a) for a in args]
        self.drop = drop
        self.flip = flip
        self.rng = random.Random(seed)
        self.lock = threading.Lock()
        self.proc = None
        self.start()

    def start(self):
        with self.lock:
            self.proc = subprocess.Popen([self.binary] + self.args, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, bufsize=0)

    def stop(self):
        with self.lock:
            p, self.proc = self.proc, None
        if p is not None:
            p.kill()
            p.wait()

    def _damage(self, data):
        if not (self.drop or self.flip):
            return data
        out = bytearray()
        for b in data:
            if self.rng.random() < self.drop:
                continue
            if self.rng.random() < self.flip:
                b ^= 1 << self.rng.randrange(8)
            out.append(b)
        return bytes(out)

    def read(self, timeout):
        p = self.proc
        if p is None or p.poll() is not None:
            time.sleep(timeout)
            return b""
        fd = p.stdout.fileno()
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return b""
        try:
            data = os.read(fd, 4096)
        except OSError:
            return b""
        if not data:
            time.sleep(timeout)
        return self._damage(data)

    def write(self, data):
        p = self.proc
        if p is None:
            return len(data)                # the line is dead: bytes are lost
        try:
            p.stdin.write(self._damage(data))
        except (BrokenPipeError, OSError):
            pass
        return len(data)


def open_link(pipe, **kwargs):
    kwargs.setdefault("rto_ms", 30)
    return StreamLink(pipe.read, pipe.write, **kwargs)


def ask_back_handler(link, m):
    if m.type == 0x08 and m.tag:
        link.ack(m.tag, m.data + b"!")
    return True


def test_requests(peer_binary):
    pipe = Pipe(peer_binary)
    try:
        with open_link(pipe, on_message=ask_back_handler) as link:
            assert link.wait_up()
            assert link.request(0x01, b"\x01\x02").data == b"\x01\x02"
            with pytest.raises(NackError) as e:
                link.request(0x02)
            assert e.value.code == ERROR_CMD_BAD_ARG and e.value.data == b"no"
            r = link.request(0x03, b"\x01\x02\x03")
            assert (r.type, r.data) == (0x33, b"\x03\x02\x01")

            progress = []
            r = link.request(0x04, timeout=0.5, on_progress=progress.append)
            assert r.data == b"done" and len(progress) == 1

            with pytest.raises(RequestTimeout):
                link.request(0x05, timeout=0.2)

            # the C side asks the Python side, then answers with that response
            assert link.request(0x07, b"hi", timeout=2.0).data == b"hi!"

            # refused twice by the C side (not ready), then accepted
            t0 = time.monotonic()
            link.request(0x09, timeout=3.0)
            assert time.monotonic() - t0 >= 0.1
    finally:
        pipe.stop()


def send_retry(link, type, data):
    for _ in range(10000):
        try:
            link.send(type, data)
            return
        except QueueFullError:
            time.sleep(0.002)
    raise AssertionError("queue stayed full")


def run_traffic(link, events, requests):
    pending = []
    for i in range(events):
        send_retry(link, 0x20, i.to_bytes(2, "little"))
        if i % 4 == 0 and len(pending) < requests:
            n = len(pending)
            result = {}
            done = threading.Event()

            def cb(rsp, result=result, done=done):
                if rsp.final:
                    result["r"] = rsp
                    done.set()

            for _ in range(10000):
                try:
                    link.request_async(0x01, bytes([n, n ^ 0x5A]), 30.0, cb)
                    break
                except QueueFullError:
                    time.sleep(0.002)
            pending.append((n, result, done))
    for n, result, done in pending:
        assert done.wait(60.0)
        assert result["r"].data == bytes([n, n ^ 0x5A])
    deadline = time.monotonic() + 30.0
    while link.queued and time.monotonic() < deadline:
        time.sleep(0.01)
    count = link.request(0x0A, timeout=30.0).data
    assert int.from_bytes(count[:2], "little") == events
    assert count[2] == 1, "events out of order"


def test_events_in_order(peer_binary):
    pipe = Pipe(peer_binary)
    try:
        with open_link(pipe) as link:
            assert link.wait_up()
            run_traffic(link, 300, 40)
    finally:
        pipe.stop()


@pytest.mark.parametrize("drop,flip,seed", [(0.01, 0.0, 3), (0.0, 0.01, 4), (0.01, 0.01, 5)])
def test_lossy_pipe(peer_binary, drop, flip, seed):
    pipe = Pipe(peer_binary, drop=drop, flip=flip, seed=seed)
    try:
        with open_link(pipe) as link:
            assert link.wait_up(10.0)
            run_traffic(link, 200, 30)
            assert link.stats["retransmits"] > 0 or link.stats["crc_errors"] + link.stats["framing_errors"] > 0
    finally:
        pipe.stop()


def test_peer_restart(peer_binary):
    pipe = Pipe(peer_binary)
    events = []
    try:
        with open_link(pipe, on_event=lambda l, e: events.append(e)) as link:
            assert link.wait_up()
            assert link.request(0x01, b"x").data == b"x"

            # a request the C side receives and never answers
            result = {}
            done = threading.Event()

            def cb(rsp):
                if rsp.final:
                    result["r"] = rsp
                    done.set()

            link.request_async(0x05, b"", None, cb)
            deadline = time.monotonic() + 2.0
            while link.queued and time.monotonic() < deadline:
                time.sleep(0.01)
            assert link.queued == 0

            # the C side restarts
            pipe.stop()
            pipe.start()
            assert done.wait(5.0)
            assert result["r"].event == RESPONSE_PEER_RESET
            assert EVENT_PEER_RESET in events
            assert link.wait_up(5.0)
            assert link.request(0x01, b"again", timeout=2.0).data == b"again"
    finally:
        pipe.stop()


def test_smaller_mtu_wins(peer_binary):
    pipe = Pipe(peer_binary, args=(30, 16))
    try:
        with open_link(pipe) as link:
            assert link.wait_up()
            assert link.mtu == 16
            with pytest.raises(ValueError):
                link.send(0x20, bytes(17))
            assert link.request(0x01, bytes(range(16))).data == bytes(range(16))
    finally:
        pipe.stop()
