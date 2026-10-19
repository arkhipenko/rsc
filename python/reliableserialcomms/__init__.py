"""ReliableSerialComms for Python: the same wire protocol as the C library.

    from reliableserialcomms import SerialLink

    with SerialLink("/dev/ttyUSB0", 115200) as link:
        link.wait_up()
        rsp = link.request(0x01, b"ping", timeout=1.0)
"""

from .link import (
    Link, Message, Response,
    RscError, QueueFullError, NoTagError, RequestTimeout, NackError, PeerResetError, DroppedError,
    EVENT_UP, EVENT_DOWN, EVENT_PEER_RESET,
    RESPONSE_DATA, RESPONSE_ACK, RESPONSE_NACK, RESPONSE_PROGRESS, RESPONSE_TIMEOUT,
    RESPONSE_PEER_RESET, RESPONSE_DROPPED,
    TYPE_APP_MAX, TYPE_PROGRESS, TYPE_NACK, TYPE_ACK,
    ERROR_GENERAL, ERROR_CMD_BAD_ARG, ERROR_CMD_NOT_IMP, ERROR_CMD_BUSY,
    PROTOCOL_VERSION,
)
from .stream import StreamLink, SerialLink

__version__ = "2.0.0"
