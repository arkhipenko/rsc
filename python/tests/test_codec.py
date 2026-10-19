"""CRC, COBS and golden frames. The same vectors are in the C host tests (tests/host/test_rsc.cpp)."""

import pytest

from reliableserialcomms import cobs
from reliableserialcomms.crc import crc16
from reliableserialcomms.link import Link


def hexb(s):
    return bytes.fromhex(s)


def test_crc_check_value():
    assert crc16(b"123456789") == 0x29B1
    assert crc16(b"56789", crc16(b"1234")) == 0x29B1


COBS_CASES = [
    ("00", "01 01"),
    ("00 00", "01 01 01"),
    ("00 11 00", "01 02 11 01"),
    ("11 22 00 33", "03 11 22 02 33"),
    ("11 22 33 44", "05 11 22 33 44"),
    ("11 00 00 00", "02 11 01 01 01"),
]


@pytest.mark.parametrize("raw,enc", COBS_CASES)
def test_cobs_short(raw, enc):
    assert cobs.encode(hexb(raw)) == hexb(enc)
    assert cobs.decode(hexb(enc)) == hexb(raw)


def test_cobs_long_blocks():
    cases = []
    data = bytes(range(1, 255))                                   # 01..FE
    cases.append((data, b"\xff" + data))
    data = bytes(range(0, 255))                                   # 00..FE
    cases.append((data, b"\x01\xff" + bytes(range(1, 255))))
    data = bytes(range(1, 256))                                   # 01..FF
    cases.append((data, b"\xff" + bytes(range(1, 255)) + b"\x02\xff"))
    data = bytes(range(2, 256)) + b"\x00"                         # 02..FF 00
    cases.append((data, b"\xff" + bytes(range(2, 256)) + b"\x01\x01"))
    data = bytes(range(3, 256)) + b"\x00\x01"                     # 03..FF 00 01
    cases.append((data, b"\xfe" + bytes(range(3, 256)) + b"\x02\x01"))
    for raw, enc in cases:
        assert cobs.encode(raw) == enc
        assert cobs.decode(enc) == raw


def test_cobs_malformed():
    with pytest.raises(ValueError):
        cobs.decode(hexb("03 11"))            # a block cut short
    with pytest.raises(ValueError):
        cobs.decode(hexb("02 00 11"))         # a 0x00 inside
    dec = cobs.Decoder(3)
    for b in hexb("05 11 22 33 44"):
        assert dec.feed(b) is None
    assert dec.feed(0) is cobs.Decoder.ERROR  # longer than the buffer
    assert dec.feed(0) is None                # an empty frame is ignored


def test_golden_sync():
    """The first frame of a link: a leading 0x00, then SYNC (version 2, nothing in flight, MTU 256)."""
    out = bytearray()
    link = Link(lambda d: out.extend(d), mtu=256, nonce=0x12345678, clock=lambda: 1000)
    link.service()
    assert bytes(out) == hexb("00 01 03 02 02 01 08 01 78 56 34 12 53 D2 00")


def test_golden_data_and_ack():
    """DATA SEQ 5 ACK 3 (type 0x10, request tag 1, data 01 00 02) and a pure ACK 6, as in the C tests."""
    from test_link import Sim, Peer, connect

    sim = Sim()
    a = Peer(sim, nonce=0x12345678)
    b = Peer(sim, nonce=0xCAFEBABE)
    assert connect(sim, a, b)
    for _ in range(5):
        a.link.send(0x20)
    for _ in range(3):
        b.link.send(0x20)
    sim.pump_until(a, b, 2000, lambda: a.link.queued == 0 and b.link.queued == 0)
    sim.pump(a, b, 100)

    a.out.clear()
    tag = a.link.request_async(0x10, hexb("01 00 02"), 1.0, a.rsps.append)
    assert tag == 1
    assert bytes(a.out) == hexb("00 02 5D 04 10 01 01 04 02 CF 94 00")

    b.out.clear()
    b.link.feed(bytes(a.out))
    assert bytes(b.out) == hexb("00 05 30 01 BB 08 00")
