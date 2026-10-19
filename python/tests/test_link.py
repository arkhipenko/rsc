"""Two Python links joined by a simulated wire with faults (the same scenarios as the C host tests)."""

import random

import pytest

from reliableserialcomms import (
    Link, QueueFullError, NoTagError, EVENT_UP, EVENT_DOWN, EVENT_PEER_RESET,
    RESPONSE_ACK, RESPONSE_NACK, RESPONSE_DATA, RESPONSE_PROGRESS, RESPONSE_TIMEOUT,
    RESPONSE_PEER_RESET, RESPONSE_DROPPED, ERROR_CMD_BAD_ARG,
)


class Sim:
    def __init__(self, start=1000):
        self.now = start
        self.drop = 0.0
        self.flip = 0.0
        self.insert = 0.0
        self.cut = False
        self.rng = random.Random(7)

    def clock(self):
        return self.now

    def deliver(self, src, dst):
        if not src.out:
            return
        data, src.out = bytes(src.out), bytearray()
        if self.cut:
            return
        got = bytearray()
        for b in data:
            if self.insert and self.rng.random() < self.insert:
                got.append(self.rng.randrange(256))
            if self.drop and self.rng.random() < self.drop:
                continue
            if self.flip and self.rng.random() < self.flip:
                b ^= 1 << self.rng.randrange(8)
            got.append(b)
        if got:
            dst.link.feed(bytes(got))

    def pump(self, a, b, ms):
        for _ in range(ms):
            a.run_deferred(self.now)
            b.run_deferred(self.now)
            a.link.service()
            b.link.service()
            self.deliver(a, b)
            self.deliver(b, a)
            self.now += 1

    def pump_until(self, a, b, ms, cond):
        for _ in range(ms):
            if cond():
                return True
            self.pump(a, b, 1)
        return cond()


class Peer:
    """A link with a responder that answers each request type in its own way."""

    def __init__(self, sim, handler=None, **kwargs):
        self.sim = sim
        self.out = bytearray()
        self.msgs = []
        self.rsps = []
        self.events = []
        self.deferred = []          # (time, tag, final)
        self.handler = handler
        kwargs.setdefault("rto_ms", 40)
        self.link = Link(self._write, clock=sim.clock, on_message=self._on_message,
                         on_event=lambda link, e: self.events.append(e), **kwargs)

    def _write(self, data):
        self.out += data
        return len(data)

    def _on_message(self, link, m):
        if self.handler is not None:
            if self.handler(self, m) is False:
                return False
        else:
            self.respond(m)
        self.msgs.append(m)
        return True

    def respond(self, m):
        if m.tag == 0:
            return
        if m.type == 0x01:
            self.link.ack(m.tag, m.data)
        elif m.type == 0x02:
            self.link.nack(m.tag, ERROR_CMD_BAD_ARG, b"no")
        elif m.type == 0x03:
            self.link.respond(m.tag, 0x33, m.data)
        elif m.type == 0x04:
            self.link.progress(m.tag)
            self.deferred.append((self.sim.now + 300, m.tag, True))
        elif m.type == 0x06:
            self.link.progress(m.tag)
            self.deferred.append((self.sim.now + 200, m.tag, False))
            self.deferred.append((self.sim.now + 400, m.tag, True))

    def run_deferred(self, now):
        due = [d for d in self.deferred if d[0] <= now]
        self.deferred = [d for d in self.deferred if d[0] > now]
        for _, tag, final in due:
            if final:
                self.link.ack(tag, b"done")
            else:
                self.link.progress(tag, b"\x32")

    def final(self, tag):
        for r in self.rsps:
            if r.tag == tag and r.event != RESPONSE_PROGRESS:
                return r
        return None


def connect(sim, a, b, ms=3000):
    return sim.pump_until(a, b, ms, lambda: a.link.is_up and b.link.is_up)


def send_retry(sim, a, b, type, data=b""):
    for _ in range(20000):
        try:
            a.link.send(type, data)
            return
        except QueueFullError:
            sim.pump(a, b, 1)
    raise AssertionError("queue stayed full")


def request_retry(sim, a, b, type, data=b"", timeout=1.0):
    for _ in range(20000):
        try:
            return a.link.request_async(type, data, timeout, a.rsps.append)
        except (QueueFullError, NoTagError):
            sim.pump(a, b, 1)
    raise AssertionError("queue stayed full")


@pytest.fixture
def sim():
    return Sim()


def test_connect(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    sim.pump(a, b, 500)
    assert a.events == [EVENT_UP]
    assert b.events == [EVENT_UP]


def test_requests(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    t1 = request_retry(sim, a, b, 0x01, b"\x01\x02")
    t2 = request_retry(sim, a, b, 0x02)
    t3 = request_retry(sim, a, b, 0x03, b"\x09\x08\x07")
    sim.pump_until(a, b, 1000, lambda: all(a.final(t) for t in (t1, t2, t3)))
    r = a.final(t1)
    assert (r.event, r.data) == (RESPONSE_ACK, b"\x01\x02")
    r = a.final(t2)
    assert (r.event, r.error, r.data) == (RESPONSE_NACK, ERROR_CMD_BAD_ARG, b"no")
    r = a.final(t3)
    assert (r.event, r.type, r.data) == (RESPONSE_DATA, 0x33, b"\x09\x08\x07")
    assert len(a.rsps) == 3


def test_progress_timeout(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    t = request_retry(sim, a, b, 0x04, timeout=0.2)        # done after 300 ms
    sim.pump_until(a, b, 2000, lambda: a.final(t))
    sim.pump(a, b, 300)
    assert [r.event for r in a.rsps] == [RESPONSE_PROGRESS, RESPONSE_TIMEOUT]
    assert a.link.stats["late_responses"] == 1


def test_progress_restarts_timeout(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    t = request_retry(sim, a, b, 0x06, timeout=0.25)       # done at 400 ms
    sim.pump_until(a, b, 2000, lambda: a.final(t))
    assert [r.event for r in a.rsps] == [RESPONSE_PROGRESS, RESPONSE_PROGRESS, RESPONSE_ACK]
    assert a.rsps[1].data == b"\x32"


def test_timeout_and_void(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    t = request_retry(sim, a, b, 0x05, timeout=0.1)
    sim.pump(a, b, 150)
    assert a.final(t).event == RESPONSE_TIMEOUT

    sim.cut = True
    send_retry(sim, a, b, 0x22, b"\x01")
    sim.pump_until(a, b, 10000, lambda: not a.link.is_up)
    assert a.events.count(EVENT_DOWN) == 1
    b.msgs.clear()
    t2 = a.link.request_async(0x01, b"", 0.05, a.rsps.append)
    sim.pump(a, b, 100)
    assert a.final(t2).event == RESPONSE_TIMEOUT
    sim.cut = False
    assert connect(sim, a, b)
    sim.pump(a, b, 300)
    assert [m.type for m in b.msgs] == [0x22]       # the withdrawn request never arrives
    assert a.link.queued == 0


def test_arguments(sim):
    with pytest.raises(ValueError):
        Link(lambda d: None, window=8)
    with pytest.raises(ValueError):
        Link(lambda d: None, mtu=5000)
    a = Peer(sim, tx_slots=4, max_requests=2)
    with pytest.raises(ValueError):
        a.link.send(0xF0)
    with pytest.raises(ValueError):
        a.link.send(0x01, bytes(257))
    with pytest.raises(ValueError):
        a.link.respond(0, 0x01)
    with pytest.raises(ValueError):
        a.link.respond(5, 0xFE)
    with pytest.raises(ValueError):
        a.link.nack(5, -1, bytes(256))              # error byte + data
    a.link.request_async(0x01)
    a.link.request_async(0x01)
    with pytest.raises(NoTagError):
        a.link.request_async(0x01)
    a.link.send(0x01)
    a.link.send(0x01)
    with pytest.raises(QueueFullError):
        a.link.send(0x01)


def lossy_run(sim, drop, flip, insert, seed, events, requests, window=4):
    sim.drop, sim.flip, sim.insert = drop, flip, insert
    sim.rng.seed(seed)
    a, b = Peer(sim, window=window), Peer(sim, window=window)
    assert connect(sim, a, b, 60000)       # a SYNC exchange survives this much damage about 1 time in 5
    tags = []
    for i in range(events):
        send_retry(sim, a, b, 0x22, i.to_bytes(2, "little"))
        if i % 3 == 0 and len(tags) < requests:
            tags.append(request_retry(sim, a, b, 0x01, bytes([len(tags)]), timeout=60.0))
    sim.pump_until(a, b, 600000, lambda: a.link.queued == 0 and b.link.queued == 0 and len(a.rsps) >= len(tags))
    got = [int.from_bytes(m.data, "little") for m in b.msgs if m.type == 0x22]
    assert got == list(range(events))
    assert len(a.rsps) == len(tags)
    assert all(r.event == RESPONSE_ACK for r in a.rsps)
    if drop + flip + insert:
        assert a.link.stats["retransmits"] > 0


@pytest.mark.parametrize("drop,flip,insert,seed", [
    (0.0, 0.0, 0.0, 11), (0.01, 0.0, 0.0, 12), (0.0, 0.01, 0.0, 13), (0.0, 0.0, 0.01, 14), (0.02, 0.02, 0.01, 15)])
def test_lossy_link(sim, drop, flip, insert, seed):
    lossy_run(sim, drop, flip, insert, seed, 150, 40)


def test_lossy_window_1_and_7(sim):
    lossy_run(sim, 0.02, 0.02, 0.01, 16, 100, 20, window=1)
    lossy_run(Sim(), 0.02, 0.02, 0.01, 17, 100, 20, window=7)


def test_not_ready(sim):
    state = {"refuse": True}

    def handler(peer, m):
        if state["refuse"] and m.data[:1] == b"\x03":
            return False
        peer.respond(m)

    a, b = Peer(sim), Peer(sim, handler=handler)
    assert connect(sim, a, b)
    for i in range(5):
        send_retry(sim, a, b, 0x22, bytes([i]))
    sim.pump(a, b, 3000)
    assert [m.data[0] for m in b.msgs] == [0, 1, 2]
    assert a.link.is_up
    state["refuse"] = False
    b.link.resume()
    send_retry(sim, a, b, 0x22, bytes([5]))
    sim.pump_until(a, b, 1000, lambda: len(b.msgs) == 6)
    assert [m.data[0] for m in b.msgs] == list(range(6))


def test_link_down_up(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    sim.cut = True
    for i in range(8):
        a.link.send(0x22, bytes([i]))
    sim.pump_until(a, b, 10000, lambda: not a.link.is_up)
    assert a.events.count(EVENT_DOWN) == 1
    sim.pump(a, b, 2000)
    sim.cut = False
    assert connect(sim, a, b)
    sim.pump_until(a, b, 2000, lambda: a.link.queued == 0)
    assert [m.data[0] for m in b.msgs] == list(range(8))
    assert EVENT_PEER_RESET not in a.events + b.events


@pytest.mark.parametrize("same_nonce", [False, True])
def test_peer_reset(sim, same_nonce):
    nonce = 0x55AA55AA if same_nonce else None
    a, b = Peer(sim), Peer(sim, nonce=nonce)
    assert connect(sim, a, b)
    for _ in range(3):
        send_retry(sim, a, b, 0x22, b"\x10")
    for _ in range(5):
        send_retry(sim, b, a, 0x22, b"\x10")
    sim.pump_until(a, b, 2000, lambda: a.link.queued == 0 and b.link.queued == 0)
    t = request_retry(sim, a, b, 0x05, timeout=None)
    sim.pump_until(a, b, 2000, lambda: a.link.queued == 0)
    assert a.final(t) is None

    sim.cut = True
    b2 = Peer(sim, nonce=nonce)
    a.link.send(0x22, b"\x77")
    sim.cut = False
    assert connect(sim, a, b2)
    sim.pump_until(a, b2, 3000, lambda: b2.msgs)
    assert a.events.count(EVENT_PEER_RESET) == 1
    assert a.final(t).event == RESPONSE_PEER_RESET
    assert [m.data for m in b2.msgs] == [b"\x77"]

    t2 = request_retry(sim, a, b2, 0x01, b"\x42")
    send_retry(sim, b2, a, 0x23, b"\x43")
    sim.pump_until(a, b2, 2000, lambda: a.final(t2))
    assert a.final(t2).event == RESPONSE_ACK
    sim.pump(a, b2, 200)
    assert any(m.type == 0x23 for m in a.msgs)


def test_mtu(sim):
    a, b = Peer(sim), Peer(sim, mtu=16)
    t = a.link.request_async(0x01, bytes(32), 1.0, a.rsps.append)
    a.link.send(0x22, b"\x01")
    assert connect(sim, a, b)
    assert a.link.mtu == 16 and b.link.mtu == 16
    sim.pump_until(a, b, 1000, lambda: a.link.queued == 0)
    assert a.final(t).event == RESPONSE_DROPPED
    assert len(b.msgs) == 1
    with pytest.raises(ValueError):
        a.link.send(0x22, bytes(17))
    a.link.send(0x22, bytes(16))


def test_malformed(sim):
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    rng = random.Random(99)
    for _ in range(3000):
        a.link.feed(bytes(rng.randrange(256) for _ in range(rng.randrange(64))))
    a.link.feed(b"\x00")
    assert connect(sim, a, b)
    t = request_retry(sim, b, a, 0x01, b"\xab", timeout=5.0)
    sim.pump_until(a, b, 5000, lambda: b.final(t))
    assert b.final(t).event == RESPONSE_ACK
    assert a.link.stats["framing_errors"] + a.link.stats["crc_errors"] > 100


def test_clock_wrap_is_irrelevant_but_large_clock_works():
    sim = Sim(start=0xFFFFFF00)
    a, b = Peer(sim), Peer(sim)
    assert connect(sim, a, b)
    t = request_retry(sim, a, b, 0x01, b"\x01")
    sim.pump_until(a, b, 1000, lambda: a.final(t))
    assert a.final(t).event == RESPONSE_ACK
