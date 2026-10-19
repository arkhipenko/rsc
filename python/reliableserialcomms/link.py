"""The ReliableSerialComms link: the same wire protocol and rules as the C library (src/rsc.c).

A Link is transport-agnostic. It writes through a callable, and the application passes received
bytes to feed() and calls service() regularly. StreamLink and SerialLink (stream.py) run a reader
thread that does both.

Thread safety: every public method may be called from any thread. Callbacks run in the thread
that calls feed() or service(), never with the link lock held. Do not call the blocking request()
from a callback of the same link: the callback runs in the thread that would deliver its response.
"""

import secrets
import threading
import time

from . import cobs
from .crc import crc16, CRC16_INIT

# Wire constants (see the C header src/rsc.h)
PROTOCOL_VERSION = 2
MTU_LIMIT = 4089
MAX_WINDOW = 7

L0_SEQ_MASK = 0x07
L0_ACK_SHIFT = 3
L0_RELIABLE = 0x40
L0_RNR = 0x80

KIND_DATA = 0
KIND_ACK = 1
KIND_SYNC = 2
KIND_SYNC_RSP = 3
L1_KIND_MASK = 0x0F
L1_MORE = 0x10
L1_RESERVED = 0xE0

SYNC_LEN = 8
SYNC_RSP_LEN = 12

TAG_RESPONSE = 0x80
TAG_MASK = 0x7F

TYPE_APP_MAX = 0xEF
TYPE_VOID = 0xF0
TYPE_PROGRESS = 0xFD
TYPE_NACK = 0xFE
TYPE_ACK = 0xFF

# Error codes, as in C
OK = 0
ERROR_GENERAL = -1
ERROR_CMD_BAD_ARG = -60
ERROR_CMD_NOT_IMP = -61
ERROR_CMD_BUSY = -62

# What happened to a request
RESPONSE_DATA = 0
RESPONSE_ACK = 1
RESPONSE_NACK = 2
RESPONSE_PROGRESS = 3
RESPONSE_TIMEOUT = 4
RESPONSE_PEER_RESET = 5
RESPONSE_DROPPED = 6

# Link events
EVENT_UP = 1
EVENT_DOWN = 2
EVENT_PEER_RESET = 3

# Timing defaults, in ms
RTO_MIN_MS = 50
RTO_DEFAULT_MS = 200
RTO_MAX_FACTOR = 8
SYNC_INTERVAL_MS = 250
FLUSH_IDLE_MS = 20
IDLE_MS = 1000
RETRIES = 6


class RscError(Exception):
    """Base class of the errors raised by request() and the queueing methods."""


class QueueFullError(RscError):
    """tx_slots messages are already queued."""


class NoTagError(RscError):
    """max_requests requests are already pending."""


class RequestTimeout(RscError, TimeoutError):
    """No final response arrived in time."""


class NackError(RscError):
    """The peer answered with NACK. code is its signed error code, data the bytes after it."""

    def __init__(self, code, data=b""):
        super().__init__("NACK %d" % code)
        self.code = code
        self.data = data


class PeerResetError(RscError):
    """The peer restarted after it had received the request."""


class DroppedError(RscError):
    """The request is longer than the peer's MTU; it was not sent."""


class Message:
    """A request (tag 1-127) or an event (tag 0) from the peer."""

    __slots__ = ("type", "tag", "data")

    def __init__(self, type, tag, data):
        self.type = type
        self.tag = tag
        self.data = data

    def __repr__(self):
        return "Message(type=0x%02X, tag=%d, data=%r)" % (self.type, self.tag, self.data)


class Response:
    """What happened to a request: event is one of the RESPONSE_* values."""

    __slots__ = ("tag", "event", "type", "error", "data")

    def __init__(self, tag, event, type=0, error=0, data=b""):
        self.tag = tag
        self.event = event
        self.type = type
        self.error = error
        self.data = data

    @property
    def final(self):
        return self.event != RESPONSE_PROGRESS

    def __repr__(self):
        return "Response(tag=%d, event=%d, type=0x%02X, error=%d, data=%r)" % (
            self.tag, self.event, self.type, self.error, self.data)


class _Slot:
    __slots__ = ("type", "tagb", "data")

    def __init__(self, type, tagb, data):
        self.type = type
        self.tagb = tagb
        self.data = data

    def is_request(self):
        return self.type != TYPE_VOID and self.tagb != 0 and not (self.tagb & TAG_RESPONSE)

    def void(self):
        self.type = TYPE_VOID
        self.tagb = 0
        self.data = b""


class _Pending:
    __slots__ = ("callback", "start", "timeout", "delivered")

    def __init__(self, callback, start, timeout):
        self.callback = callback
        self.start = start
        self.timeout = timeout          # ms; 0 means no timeout
        self.delivered = False


def _default_clock():
    return int(time.monotonic() * 1000)


class Link:
    """One end of a link.

    write(data) writes bytes to the bus. on_message(link, message) receives requests and events
    and may return False to refuse a message (the peer then pauses until resume()). on_event(link,
    event) receives EVENT_UP, EVENT_DOWN and EVENT_PEER_RESET. clock() returns milliseconds; nonce
    is drawn at random unless given (tests only).
    """

    def __init__(self, write, mtu=256, window=4, retries=RETRIES, baud=0, rto_ms=0, tx_slots=32,
                 max_requests=127, on_message=None, on_event=None, clock=None, nonce=None):
        if not 1 <= mtu <= MTU_LIMIT:
            raise ValueError("mtu must be 1 to %d" % MTU_LIMIT)
        if not 1 <= window <= MAX_WINDOW:
            raise ValueError("window must be 1 to %d" % MAX_WINDOW)
        if tx_slots < 1 or not 1 <= max_requests <= 127 or retries < 1:
            raise ValueError("tx_slots, max_requests (1-127) and retries must be positive")

        self._write = write
        self._clock = clock or _default_clock
        self.on_message = on_message
        self.on_event = on_event
        self._lock = threading.RLock()
        self._calls = []                  # callbacks to run once the lock is released

        self.local_mtu = mtu
        self.window = window
        self.retries = retries
        self.tx_slots = tx_slots
        self.max_requests = max_requests

        # first retransmission timeout: 3 maximum frames on the line plus a margin
        if rto_ms:
            self.rto0 = rto_ms
        elif baud:
            frame_bytes = 2 + 2 + mtu + 2
            wire_bits = (frame_bytes + frame_bytes // 254 + 3) * 10
            self.rto0 = RTO_MIN_MS + 3 * ((wire_bits * 1000 + baud - 1) // baud)
        else:
            self.rto0 = RTO_DEFAULT_MS
        self._rto = self.rto0

        self.nonce = nonce if nonce is not None else secrets.randbits(32)
        if self.nonce == 0:
            self.nonce = 1

        # session
        self._peer_nonce = 0
        self._peer_known = False
        self._peer_mtu = mtu
        self._confirmed = False
        self._up = False
        self._sync_due = True
        self._sync_rsp_due = False
        self._sync_echo = 0
        self._last_sync = 0

        # transmit: _queue[0] holds sequence number _base; _in_flight messages have been sent
        self._queue = []
        self._in_flight = 0
        self._base = 0
        self._timer_on = False
        self._rto_start = 0
        self._rto_count = 0
        self._resend = False
        self._peer_rnr = False
        self._last_tx = 0
        self._tx_ever = False

        # receive
        self._rx_next = 0
        self._ack_due = False
        self._rnr = False
        self._dec = cobs.Decoder(2 + max(2 + MTU_LIMIT, SYNC_RSP_LEN) + 2)

        # requests
        self._pending = {}
        self._next_tag = 1

        self.stats = dict.fromkeys((
            "tx_frames", "rx_frames", "tx_errors", "crc_errors", "framing_errors", "rejected",
            "retransmits", "duplicates", "timeouts", "link_downs", "peer_resets",
            "late_responses", "dropped", "busy"), 0)

    # Public ------------------------------------------------------------------

    @property
    def is_up(self):
        return self._up

    @property
    def mtu(self):
        """Data length the link carries now: the smaller MTU of the two sides."""
        with self._lock:
            return self._mtu()

    @property
    def queued(self):
        with self._lock:
            return len(self._queue)

    def feed(self, data):
        """Pass received bytes. Callbacks of complete frames run in this call."""
        self._lock.acquire()
        try:
            for b in bytes(data):
                frame = self._dec.feed(b)
                if frame is cobs.Decoder.ERROR:
                    self.stats["framing_errors"] += 1
                elif frame is not None:
                    self._handle_frame(frame)
            self._transmit()
        finally:
            self._lock.release()
        self._run_calls()

    def service(self):
        """Run timers and transmission. Returns the seconds until it needs to run again."""
        with self._lock:
            now = self._clock()
            self._check_requests(now)

            if not self._confirmed and (self._sync_due or now - self._last_sync >= SYNC_INTERVAL_MS):
                self._send_sync(False)
                self._last_sync = now
                self._sync_due = False

            if self._up and self._timer_on and now - self._rto_start >= self._rto:
                longest = self.rto0 * RTO_MAX_FACTOR
                self.stats["timeouts"] += 1
                if self._peer_rnr:
                    # the peer is not ready: probe at the longest timeout, without counting a retry
                    self._rto = longest
                    self._resend = True
                else:
                    self._rto_count += 1
                    if self._rto_count > self.retries:
                        self._link_down()
                    else:
                        self._rto = min(self._rto * 2, longest)
                        self._resend = True

            self._transmit()
            wait = self._next_wait()
        self._run_calls()
        return wait / 1000.0

    def send(self, type, data=b""):
        """Queue an event (no response expected)."""
        if not 0 <= type <= TYPE_APP_MAX:
            raise ValueError("message type must be 0x00-0xEF")
        self._queue_message(type, 0, bytes(data))

    def request_async(self, type, data=b"", timeout=1.0, callback=None):
        """Queue a request. callback(response) runs for each PROGRESS and once for the final
        outcome. timeout is in seconds (None: no timeout) and restarts at each PROGRESS.
        Returns the tag."""
        data = bytes(data)
        if not 0 <= type <= TYPE_APP_MAX:
            raise ValueError("message type must be 0x00-0xEF")
        timeout_ms = 0 if timeout is None else max(1, int(round(timeout * 1000)))
        with self._lock:
            if len(data) > self._mtu():
                raise ValueError("data longer than the MTU (%d)" % self._mtu())
            if len(self._pending) >= self.max_requests:
                raise NoTagError("%d requests pending" % self.max_requests)
            if len(self._queue) >= self.tx_slots:
                raise QueueFullError("%d messages queued" % self.tx_slots)
            tag = self._next_tag
            while tag in self._pending:
                tag = tag % TAG_MASK + 1
            self._next_tag = tag % TAG_MASK + 1
            self._pending[tag] = _Pending(callback, self._clock(), timeout_ms)
            self._queue.append(_Slot(type, tag, data))
            self._transmit()
        self._run_calls()
        return tag

    def request(self, type, data=b"", timeout=1.0, on_progress=None):
        """Send a request and wait for its final response. Returns the Response of an ACK or a
        data response. Raises NackError, RequestTimeout, PeerResetError or DroppedError."""
        done = threading.Event()
        box = []

        def callback(rsp):
            if rsp.event == RESPONSE_PROGRESS:
                if on_progress:
                    on_progress(rsp)
                return
            box.append(rsp)
            done.set()

        self.request_async(type, data, timeout, callback)
        done.wait()
        rsp = box[0]
        if rsp.event == RESPONSE_NACK:
            raise NackError(rsp.error, rsp.data)
        if rsp.event == RESPONSE_TIMEOUT:
            raise RequestTimeout("no response to request %d" % rsp.tag)
        if rsp.event == RESPONSE_PEER_RESET:
            raise PeerResetError("the peer restarted after it received request %d" % rsp.tag)
        if rsp.event == RESPONSE_DROPPED:
            raise DroppedError("request %d is longer than the peer's MTU" % rsp.tag)
        return rsp

    def respond(self, tag, type, data=b""):
        """Answer the request tag with data and an application type (a final success)."""
        if not 0 <= type <= TYPE_APP_MAX:
            raise ValueError("message type must be 0x00-0xEF")
        self._answer(tag, type, bytes(data))

    def ack(self, tag, data=b""):
        self._answer(tag, TYPE_ACK, bytes(data))

    def nack(self, tag, error, data=b""):
        if not -128 <= error <= 127:
            raise ValueError("error must fit a signed byte")
        self._answer(tag, TYPE_NACK, bytes([error & 0xFF]) + bytes(data))

    def progress(self, tag, data=b""):
        self._answer(tag, TYPE_PROGRESS, bytes(data))

    def resume(self):
        """After on_message refused a message: accept messages again."""
        with self._lock:
            if self._rnr:
                self._rnr = False
                self._ack_due = True
            self._transmit()
        self._run_calls()

    # Queueing ----------------------------------------------------------------

    def _mtu(self):
        if self._peer_known and self._peer_mtu < self.local_mtu:
            return self._peer_mtu
        return self.local_mtu

    def _answer(self, tag, type, data):
        if not 1 <= tag <= TAG_MASK:
            raise ValueError("tag must be 1-127")
        self._queue_message(type, tag | TAG_RESPONSE, data)

    def _queue_message(self, type, tagb, data):
        with self._lock:
            if len(data) > self._mtu():
                raise ValueError("data longer than the MTU (%d)" % self._mtu())
            if len(self._queue) >= self.tx_slots:
                raise QueueFullError("%d messages queued" % self.tx_slots)
            self._queue.append(_Slot(type, tagb, data))
            self._transmit()
        self._run_calls()

    def _run_calls(self):
        while True:
            with self._lock:
                if not self._calls:
                    return
                calls, self._calls = self._calls, []
            for call in calls:
                call()

    def _notify(self, callback, rsp):
        if callback is not None:
            self._calls.append(lambda: callback(rsp))

    def _event(self, event):
        cb = self.on_event
        if cb is not None:
            self._calls.append(lambda: cb(self, event))

    # Transmit ----------------------------------------------------------------

    def _link0(self, seq, reliable):
        b = (seq & L0_SEQ_MASK) | ((self._rx_next & L0_SEQ_MASK) << L0_ACK_SHIFT)
        if reliable:
            b |= L0_RELIABLE
        if self._rnr:
            b |= L0_RNR
        return b

    def _send_frame(self, frame):
        c = crc16(frame)
        frame = bytes(frame) + bytes((c & 0xFF, c >> 8))
        now = self._clock()
        out = bytearray()
        if not self._tx_ever or now - self._last_tx >= FLUSH_IDLE_MS:
            out.append(0)
        out += cobs.encode(frame)
        out.append(0)
        try:
            n = self._write(bytes(out))
            ok = n is None or n == len(out)
        except Exception:
            ok = False
        self.stats["tx_frames" if ok else "tx_errors"] += 1
        self._last_tx = self._clock()
        self._tx_ever = True

    def _send_slot(self, i):
        s = self._queue[i]
        self._send_frame(bytes((self._link0(self._base + i, True), KIND_DATA, s.type, s.tagb)) + s.data)
        self._ack_due = False

    def _send_ack(self):
        self._send_frame(bytes((self._link0(0, False), KIND_ACK)))
        self._ack_due = False

    def _send_sync(self, response):
        body = bytes((PROTOCOL_VERSION, self._in_flight)) + self.local_mtu.to_bytes(2, "little") + \
            self.nonce.to_bytes(4, "little")
        if response:
            body += self._sync_echo.to_bytes(4, "little")
        self._send_frame(bytes((self._link0(self._base, False), KIND_SYNC_RSP if response else KIND_SYNC)) + body)

    def _can_send_new(self):
        return (self._up and self._in_flight < len(self._queue) and self._in_flight < self.window and
                (not self._peer_rnr or self._in_flight == 0))

    def _transmit(self):
        if self._sync_rsp_due:
            self._send_sync(True)
            self._sync_rsp_due = False

        if self._up and self._resend:
            self._resend = False
            for i in range(self._in_flight):
                self._send_slot(i)
                self.stats["retransmits"] += 1
            if self._in_flight:
                self._timer_on = True
                self._rto_start = self._clock()

        while self._can_send_new():
            s = self._queue[self._in_flight]
            if len(s.data) > self._mtu():
                # longer than the peer accepts: sent as VOID, and its request fails
                request, tag = s.is_request(), s.tagb
                s.void()
                self.stats["dropped"] += 1
                if request:
                    self._fail(tag, RESPONSE_DROPPED)
            self._send_slot(self._in_flight)
            self._in_flight += 1
            if not self._timer_on:
                self._timer_on = True
                self._rto_start = self._clock()

        if self._up and self._ack_due:
            self._send_ack()

    def _next_wait(self):
        now = self._clock()
        wait = IDLE_MS
        if not self._confirmed:
            wait = min(wait, max(0, SYNC_INTERVAL_MS - (now - self._last_sync)))
        if self._up and self._timer_on:
            wait = min(wait, max(0, self._rto - (now - self._rto_start)))
        for p in self._pending.values():
            if p.timeout:
                wait = min(wait, max(0, p.timeout - (now - p.start)))
        if self._ack_due or self._sync_rsp_due or self._resend or self._can_send_new():
            wait = 0
        return wait

    # Link state --------------------------------------------------------------

    def _fail(self, tag, event):
        p = self._pending.pop(tag, None)
        if p is not None:
            self._notify(p.callback, Response(tag, event))

    def _check_requests(self, now):
        for tag in [t for t, p in self._pending.items() if p.timeout and now - p.start >= p.timeout]:
            p = self._pending.pop(tag)
            for s in self._queue[self._in_flight:]:
                if s.is_request() and s.tagb == tag:
                    s.void()        # not sent yet: withdrawn
                    break
            self._notify(p.callback, Response(tag, RESPONSE_TIMEOUT))

    def _check_up(self):
        if self._up or not self._peer_known or not self._confirmed:
            return
        self._up = True
        self._rto = self.rto0
        self._rto_count = 0
        if self._in_flight:
            self._resend = True
        self._event(EVENT_UP)

    def _link_down(self):
        self._up = False
        self._confirmed = False
        self._sync_due = True
        self._timer_on = False
        self._resend = False
        self._peer_rnr = False
        self._rto = self.rto0
        self._rto_count = 0
        self.stats["link_downs"] += 1
        self._event(EVENT_DOWN)

    def _peer_reset(self, nonce):
        self._peer_nonce = nonce
        self._peer_known = True
        self._confirmed = False
        self._up = False
        self._sync_due = True
        self._in_flight = 0
        self._base = 0
        self._rx_next = 0
        self._ack_due = False
        self._peer_rnr = False
        self._timer_on = False
        self._resend = False
        self._rto = self.rto0
        self._rto_count = 0
        self.stats["peer_resets"] += 1
        for tag in [t for t, p in self._pending.items() if p.delivered]:
            self._fail(tag, RESPONSE_PEER_RESET)
        self._event(EVENT_PEER_RESET)

    def _consistent(self, peer_l0, peer_in_flight):
        peer_seq = peer_l0 & L0_SEQ_MASK
        peer_ack = (peer_l0 >> L0_ACK_SHIFT) & L0_SEQ_MASK
        return ((peer_ack - self._base) & L0_SEQ_MASK) <= self._in_flight and \
            ((self._rx_next - peer_seq) & L0_SEQ_MASK) <= peer_in_flight

    def _handle_sync(self, l0, kind, body):
        response = kind == KIND_SYNC_RSP
        if len(body) < (SYNC_RSP_LEN if response else SYNC_LEN) or body[0] != PROTOCOL_VERSION:
            self.stats["rejected"] += 1
            return
        peer_in_flight = body[1]
        mtu = int.from_bytes(body[2:4], "little")
        nonce = int.from_bytes(body[4:8], "little")
        if peer_in_flight > MAX_WINDOW or mtu == 0:
            self.stats["rejected"] += 1
            return
        if response and int.from_bytes(body[8:12], "little") != self.nonce:
            self.stats["rejected"] += 1
            return

        if self._peer_known and (nonce != self._peer_nonce or not self._consistent(l0, peer_in_flight)):
            self._peer_reset(nonce)
        elif not self._peer_known:
            self._peer_nonce = nonce
            self._peer_known = True
        self._peer_mtu = mtu

        if response:
            self._confirmed = True
        else:
            self._sync_echo = nonce
            self._sync_rsp_due = True
        self._check_up()

    def _process_ack(self, l0):
        ack = (l0 >> L0_ACK_SHIFT) & L0_SEQ_MASK
        n = (ack - self._base) & L0_SEQ_MASK
        if 0 < n <= self._in_flight:
            for s in self._queue[:n]:
                if s.is_request() and s.tagb in self._pending:
                    self._pending[s.tagb].delivered = True
            del self._queue[:n]
            self._in_flight -= n
            self._base = (self._base + n) & L0_SEQ_MASK
            self._rto_count = 0
            self._rto = self.rto0
            self._timer_on = self._in_flight > 0
            if self._timer_on:
                self._rto_start = self._clock()
        rnr = bool(l0 & L0_RNR)
        if self._peer_rnr and not rnr and self._in_flight:
            self._resend = True
        self._peer_rnr = rnr

    # Receive -----------------------------------------------------------------

    def _handle_response(self, type, tag, data):
        if type > TYPE_APP_MAX and type not in (TYPE_PROGRESS, TYPE_NACK, TYPE_ACK):
            self.stats["rejected"] += 1
            return
        p = self._pending.get(tag) if tag else None
        if p is None:
            self.stats["late_responses"] += 1
            return
        error = 0
        if type == TYPE_PROGRESS:
            p.start = self._clock()
            event = RESPONSE_PROGRESS
        else:
            del self._pending[tag]
            if type == TYPE_NACK:
                event = RESPONSE_NACK
                error = ERROR_GENERAL
                if data:
                    error = data[0] - 256 if data[0] >= 128 else data[0]
                    data = data[1:]
            elif type == TYPE_ACK:
                event = RESPONSE_ACK
            else:
                event = RESPONSE_DATA
        self._notify(p.callback, Response(tag, event, type, error, data))

    def _dispatch(self, type, tagb, data):
        """Returns False when the application refused the message."""
        if type == TYPE_VOID:
            return True
        if tagb & TAG_RESPONSE:
            self._handle_response(type, tagb & TAG_MASK, data)
            return True
        if type > TYPE_APP_MAX:
            self.stats["rejected"] += 1
            return True
        cb = self.on_message
        if cb is None:
            return True
        msg = Message(type, tagb, data)
        # run the callback without the lock, after the callbacks queued before it
        self._lock.release()
        try:
            self._run_calls()
            ok = cb(self, msg)
        finally:
            self._lock.acquire()
        return ok is not False

    def _handle_data(self, l0, body):
        reliable = bool(l0 & L0_RELIABLE)
        if len(body) < 2:
            self.stats["rejected"] += 1
            return
        if reliable:
            self._ack_due = True
            if (l0 & L0_SEQ_MASK) != self._rx_next:
                self.stats["duplicates"] += 1
                return
            if self._rnr:
                self.stats["busy"] += 1
                return
        if not self._dispatch(body[0], body[1], bytes(body[2:])):
            # refused: not acknowledged; the ACK now carries "not ready"
            self._rnr = True
            self.stats["busy"] += 1
            return
        if reliable:
            self._rx_next = (self._rx_next + 1) & L0_SEQ_MASK

    def _handle_frame(self, f):
        if len(f) < 4:
            self.stats["framing_errors"] += 1
            return
        if crc16(f[:-2]) != f[-2] | (f[-1] << 8):
            self.stats["crc_errors"] += 1
            return
        self.stats["rx_frames"] += 1
        l0, l1, body = f[0], f[1], f[2:-2]
        if l1 & (L1_RESERVED | L1_MORE):
            self.stats["rejected"] += 1
            return
        kind = l1 & L1_KIND_MASK
        if kind in (KIND_SYNC, KIND_SYNC_RSP):
            self._handle_sync(l0, kind, body)
        elif kind == KIND_ACK:
            if self._up:
                self._process_ack(l0)
        elif kind == KIND_DATA:
            if self._up:
                self._process_ack(l0)
                self._handle_data(l0, body)
        else:
            self.stats["rejected"] += 1
