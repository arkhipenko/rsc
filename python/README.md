# reliableserialcomms (Python)

The Python side of ReliableSerialComms: the same wire protocol as the C library in this repository (COBS framing, CRC-16, a sliding window with retransmission, session sync with peer restart detection, and request tags with PROGRESS, ACK, NACK and data answers).

```python
from reliableserialcomms import SerialLink

with SerialLink("/dev/ttyUSB0", 115200) as link:
    link.wait_up()
    print(link.request(0x01, b"hello", timeout=1.0).data)
```

Install from a clone of the repository with `pip install ./python` (Python 3.9 or later, pyserial). Run the tests from this folder with `python -m pytest tests` after `make -C ../tests/interop` has built the C peer. The full documentation is the README at the root of the repository.
