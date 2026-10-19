"""Consistent Overhead Byte Stuffing (Cheshire and Baker, 1999).

The encoded bytes contain no 0x00, so 0x00 can end a frame. The delimiter is not part of the
encoded bytes. The encoder produces the same bytes as the C library (no trailing code byte after
a final block of 254 bytes).
"""


def encode(data):
    """Encode bytes. Returns the encoded bytes without a delimiter."""
    data = bytes(data)
    n = len(data)
    out = bytearray()
    i = 0
    while True:
        j = i
        while j < n and data[j] != 0 and j - i < 254:
            j += 1
        run = j - i
        out.append(run + 1)
        out += data[i:j]
        i = j
        if i >= n:
            break
        if run < 254:
            i += 1          # the zero that ended the block is implied by its code
    return bytes(out)


def decode(data):
    """Decode encoded bytes (no delimiter). Raises ValueError when they are malformed."""
    dec = Decoder(len(data))
    for b in bytes(data):
        if b == 0:
            raise ValueError("0x00 inside COBS data")
        dec.feed(b)
    frame = dec.feed(0)
    if frame is None or frame is Decoder.ERROR:
        raise ValueError("malformed COBS data")
    return frame


class Decoder:
    """Stream decoder. feed() returns None (no frame yet), the frame bytes when a delimiter ends
    a frame, or Decoder.ERROR when a delimiter ends a malformed or oversize frame."""

    ERROR = object()

    def __init__(self, cap):
        self.cap = cap
        self._reset()

    def _reset(self):
        self.buf = bytearray()
        self.code = 0
        self.left = 0
        self.started = False
        self.overflow = False

    def _put(self, b):
        if len(self.buf) >= self.cap:
            self.overflow = True
        else:
            self.buf.append(b)

    def feed(self, b):
        if b == 0:
            result = None                       # an empty frame is ignored
            if self.overflow or self.left != 0:
                result = Decoder.ERROR          # too long, or a block cut short
            elif self.started:
                result = bytes(self.buf)
            self._reset()
            return result
        if self.overflow:
            return None
        if self.left == 0:
            # a code byte: the previous block (if any) ended with an implied zero, unless it was full
            if self.started and self.code != 0xFF:
                self._put(0)
            self.code = b
            self.left = b - 1
            self.started = True
            return None
        self._put(b)
        self.left -= 1
        return None
