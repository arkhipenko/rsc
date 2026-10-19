"""Talk to the RSC_Host sketch (examples/RSC_Host) over a serial port.

    python host_demo.py COM5
    python host_demo.py /dev/ttyACM0 --baud 115200

Boards like the Uno restart when the port opens, so the first sync can take a few seconds.
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from reliableserialcomms import SerialLink, NackError, EVENT_UP, EVENT_DOWN, EVENT_PEER_RESET  # noqa: E402

EVT_UPTIME = 0x10
EVENT_NAMES = {EVENT_UP: "link up", EVENT_DOWN: "link down", EVENT_PEER_RESET: "board restarted"}


def on_message(link, m):
    if m.type == EVT_UPTIME and len(m.data) == 4:
        print("event: board uptime %.1f s" % (struct.unpack("<I", m.data)[0] / 1000.0))


def on_event(link, event):
    print("event:", EVENT_NAMES.get(event, event))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=115200)
    args = p.parse_args()

    with SerialLink(args.port, args.baud, on_message=on_message, on_event=on_event) as link:
        if not link.wait_up(10.0):
            print("no link: is RSC_Host running on the board?")
            return 1

        print("info:", link.request(0x04, timeout=2.0).data.decode())
        print("echo:", link.request(0x01, b"hello", timeout=2.0).data)

        r = link.request(0x02, struct.pack("<ii", 40, 2), timeout=2.0)
        print("40 + 2 =", struct.unpack("<i", r.data)[0])

        t0 = time.monotonic()
        link.request(0x03, timeout=2.0, on_progress=lambda r: print("slow command accepted"))
        print("slow command done after %.2f s" % (time.monotonic() - t0))

        try:
            link.request(0x7F, timeout=2.0)
        except NackError as e:
            print("unknown command: NACK %d" % e.code)

        print("waiting for uptime events (Ctrl+C to stop)")
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            pass
        print("stats:", {k: v for k, v in link.stats.items() if v})
    return 0


if __name__ == "__main__":
    sys.exit(main())
