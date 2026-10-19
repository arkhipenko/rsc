"""Links that run their own reader thread: StreamLink over any read and write callables, and
SerialLink over a serial port (pyserial)."""

import threading

from .link import Link


class StreamLink(Link):
    """A Link with a reader thread.

    read(timeout) returns the bytes that arrived within timeout seconds (possibly none).
    write(data) writes bytes. The thread passes received bytes to feed() and runs service().
    Use it as a context manager, or call close().
    """

    POLL = 0.05     # longest wait of one read, in seconds

    def __init__(self, read, write, **kwargs):
        super().__init__(write, **kwargs)
        self._read = read
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="rsc-link", daemon=True)
        self._thread.start()

    def _run(self):
        wait = 0.0
        while not self._stop.is_set():
            try:
                data = self._read(min(max(wait, 0.001), self.POLL))
            except Exception:
                if self._stop.is_set():
                    break
                raise
            if data:
                self.feed(data)
            wait = self.service()

    def wait_up(self, timeout=5.0):
        """Wait until the link is up. Returns True when it is."""
        step = 0.01
        waited = 0.0
        while not self.is_up and waited < timeout:
            self._stop.wait(step)
            waited += step
        return self.is_up

    def close(self):
        self._stop.set()
        if self._thread is not threading.current_thread():
            self._thread.join(timeout=2.0)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class SerialLink(StreamLink):
    """A link over a serial port. port and baud go to serial.Serial; the other keyword arguments
    go to Link (mtu, window, on_message, on_event, ...). The baud also sets the retransmission
    timeout unless rto_ms is given."""

    def __init__(self, port, baud=115200, **kwargs):
        import serial   # pyserial

        self.serial = serial.Serial(port, baudrate=baud, timeout=0)
        kwargs.setdefault("baud", baud)

        def read(timeout):
            self.serial.timeout = timeout
            return self.serial.read(max(1, self.serial.in_waiting))

        def write(data):
            return self.serial.write(data)

        super().__init__(read, write, **kwargs)

    def close(self):
        super().close()
        self.serial.close()
