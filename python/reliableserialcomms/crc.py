"""CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR.

The check value of b"123456789" is 0x29B1.
"""

CRC16_INIT = 0xFFFF


def _make_table():
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        table.append(c)
    return table


_TABLE = _make_table()


def crc16(data, crc=CRC16_INIT):
    """CRC of data, continued from crc (CRC16_INIT for a new CRC)."""
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _TABLE[(crc >> 8) ^ b]
    return crc
