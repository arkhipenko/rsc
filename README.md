# Reliable Serial Communications

Version 2.0.0

Reliable Serial Communications (RSC) carries typed messages between two devices over a serial byte stream such as a UART, a USB CDC port or a TCP socket. Frames are COBS encoded and protected by a CRC-16. A sliding window of sequence numbers with retransmission delivers every message once and in order while the link holds. Requests carry a tag, and the peer answers each one with data, an ACK, a NACK, or progress messages followed by a final answer. The C library uses no heap and runs from an Arduino Uno to an ESP32 or an STM32 with an RTOS. A Python package with the same wire protocol is included.

2.0.0 is a redesign. It is not compatible with 1.x on the wire or in the API (see "Upgrading from 1.x").

## What the link guarantees

- **Once and in order.** While the link is up, each message reaches the peer application once, and in the order it was queued. A damaged or lost frame is sent again.
- **Queue kept across a broken line.** When retransmissions fail, the link reports RSC_EVENT_DOWN, keeps its queue and syncs again. The queued messages flow once the line returns.
- **Peer restart detected.** A peer that restarts is recognized (RSC_EVENT_PEER_RESET). Messages it never acknowledged go to the new peer. Requests the old peer had already received end with RSC_RESPONSE_PEER_RESET.
- **Request correlation.** Each request carries a tag (1-127). Its callback receives each PROGRESS and then exactly one final outcome: data, ACK, NACK, TIMEOUT, PEER_RESET or DROPPED.
- **Flow control.** A receiver that refuses a message pauses the sender until it resumes.

## Limits

- The queue lives in RAM. Messages queued on a device that restarts are lost.
- Across a peer restart, delivery is at least once: a message the old peer received but did not acknowledge in time reaches the new peer again.
- An idle link does not notice a cut line. The break is found when a message is sent.
- A message fits one frame: at most RSC_MTU data bytes (default 256, at most 4089). Segmentation is planned for 2.1.0.
- 2.0.0 runs over byte streams (UART, RS-232, USB CDC, TCP) between two peers. RS-485 multi-drop, SPI, I2C and CAN bindings are planned for 2.1.0.
- No encryption or authentication.

## Installation

- Arduino IDE: copy the repository into the `libraries` folder. The library is not in the Library Manager.
- PlatformIO: `lib_deps = https://github.com/arkhipenko/rsc.git` (pin a tag once 2.0.0 is released).
- Other build systems: compile `src/rsc.c`, `src/rsc_cobs.c` and `src/rsc_crc.c`, and add `src` to the include path. The sources are C99 and also compile as C++.
- Python 3.9 or later: `pip install ./python` from a clone of the repository (it requires pyserial). The package is not on PyPI.

## Quick start: Arduino

```cpp
#include <ReliableSerialComms.h>

static rscLink_t hostLink;

static bool onMessage(rscLink_t* link, const rscMessage_t* m, void* user) {
  if ( m->tag != 0 && m->type == 0x01 ) rscAck(link, m->tag, m->data, m->length);   // echo
  return true;
}

void setup() {
  Serial.begin(115200);
  rscConfig_t cfg;
  rscConfigDefaults(&cfg);
  cfg.baud = 115200;
  cfg.onMessage = onMessage;
  rscOps_t ops = rscStreamOps(Serial);
  rscInit(&hostLink, &cfg, &ops);
}

void loop() {
  rscStreamPoll(&hostLink, Serial);     // feed received bytes, run timers and transmission
}
```

`examples/RSC_Host` is a complete responder for any board (Uno included), and `python/examples/host_demo.py` talks to it from a computer. `examples/RSC_TwoLinks` runs two links on one ESP32 with FreeRTOS tasks and a blocking requester. `examples/rsc_stm32` is a fragment for STM32 HAL with FreeRTOS.

## Quick start: PlatformIO (ESP32)

`examples/RSC_Host` and `examples/RSC_TwoLinks` are also PlatformIO projects for a generic ESP32 board (`esp32dev`). Each `platformio.ini` sits next to its sketch (`src_dir = .`) and links the library from this repository (`symlink://../..`). Outside the repository, replace that line with the library's git URL.

```
pio run -d examples/RSC_Host -t upload --upload-port COM5
python python/examples/host_demo.py COM5

pio run -d examples/RSC_TwoLinks -t upload --upload-port COM5
pio device monitor --port COM5
```

`RSC_TwoLinks` needs two wires: GPIO 19 to GPIO 16, and GPIO 17 to GPIO 18.

The projects use the stock platform at a fixed version, `platformio/espressif32 @ 7.1.3` (Arduino core 2.0.17). For Arduino core 3.x, replace the platform line with the pioarduino platform:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
```

NOTE: pioarduino and the stock platform share package names. On a machine that builds both, give each pioarduino project its own `platforms_dir` and `packages_dir`, or the Arduino core is installed again at every build.

## Quick start: Python

```python
from reliableserialcomms import SerialLink, NackError

with SerialLink("/dev/ttyUSB0", 115200) as link:
    link.wait_up()
    print(link.request(0x01, b"hello", timeout=1.0).data)     # blocks until the final answer
    link.send(0x20, b"\x01\x02")                             # an event: no answer expected
```

## Execution model

The application owns one `rscLink_t` per link (a static variable) and calls two functions from one thread or loop:

1. `rscFeed(link, bytes, n)`: passes received bytes. Complete frames are handled at once, and the callbacks run inside this call.
2. `rscService(link)`: runs timers and transmission. It returns the milliseconds until it must run again (0 means at once). An RTOS task sleeps that long, or until the `wake` operation notifies it. A superloop calls it on every pass.

Other threads may queue messages (`rscSend`, `rscRequest`, `rscRespond`, `rscAck`, `rscNack`, `rscProgress`, `rscResume`) when the platform supplies `lock` and `unlock`. Callbacks never run with the lock held, so they may queue messages themselves. No function may be called from an interrupt; an interrupt stores bytes in a buffer, and the link thread passes them to `rscFeed()`.

### Platform operations (`rscOps_t`, one set per link)

| operation | contract |
| --- | --- |
| `int write(void* ctx, const uint8_t* data, size_t n)` | Required. Writes the bytes. Returns `n`, or a negative error. May block until the bytes fit. |
| `uint32_t millis(void* ctx)` | Required. Milliseconds since start. It may wrap. |
| `uint32_t random(void* ctx)` | Optional. A random value for the session nonce. A value that differs on every start lets the peer report each restart. Without it, the nonce comes from the clock and the link address. |
| `void lock(void* ctx)`, `void unlock(void* ctx)` | Optional, both or neither. Needed when other threads queue messages. Never nested. |
| `void wake(void* ctx)` | Optional. Called after a message is queued, to wake the link thread. |

`rsc_arduino.h` (included by `ReliableSerialComms.h` in Arduino builds) supplies `rscStreamOps(stream)` and `rscStreamPoll(link, stream)` for any Arduino `Stream`.

## Build options

Define them for the whole build (for example `-DRSC_MTU=128`). Every link of a firmware has the same buffer sizes. AVR gets the smaller defaults.

| option | default (AVR) | meaning |
| --- | --- | --- |
| `RSC_MTU` | 256 (64) | Largest data length of one message, 1-4089. The two peers use the smaller of their values. |
| `RSC_TX_SLOTS` | 8 (2) | Messages queued per link, including those in flight. |
| `RSC_WINDOW` | 4 (1) | Default frames in flight, 1-7. `rscConfig_t.window` overrides it per link. |
| `RSC_MAX_REQUESTS` | 8 (2) | Requests waiting for their final answer, per link, 1-127. |
| `RSC_STATS` | 1 (0) | Counters per link (`rscGetStats`). |
| `RSC_CRC_IMPL` | 2 (1) | CRC implementation: 0 bitwise, 1 nibble table (32 bytes), 2 byte table (512 bytes). |
| `RSC_RETRIES` | 6 | Retransmission timeouts without progress before the link is down. |
| `RSC_RTO_MIN_MS`, `RSC_RTO_DEFAULT_MS`, `RSC_RTO_MAX_FACTOR` | 50, 200, 8 | Retransmission timeout: `RSC_RTO_MIN_MS` + 3 x the time of a maximum frame at the configured baud, or `RSC_RTO_DEFAULT_MS` when the baud is unknown. It doubles after each expiry, up to 8 times the first value. |
| `RSC_SYNC_INTERVAL_MS` | 250 | Interval of SYNC frames while the link is not up. |
| `RSC_FLUSH_IDLE_MS` | 20 | A frame sent after this much silence starts with an extra 0x00. |

### Footprint

Measured with avr-gcc and arm-none-eabi-gcc (the PlatformIO toolchains):

- AVR defaults (Uno): one `rscLink_t` takes 337 bytes of RAM, plus 32 bytes for the CRC table. `RSC_Host` uses 593 bytes of RAM and 9.3 KB of flash in total, with the Arduino core.
- Cortex-M defaults: one `rscLink_t` takes 2728 bytes. The code takes about 5.2 KB of flash (`-Os`, Cortex-M0: 4.2 KB link, 0.45 KB COBS, 0.55 KB CRC with the byte table).

## API (C)

| function | behavior |
| --- | --- |
| `void rscConfigDefaults(rscConfig_t* cfg)` | Fills the settings with the build defaults. |
| `rscError_t rscInit(rscLink_t* link, const rscConfig_t* cfg, const rscOps_t* ops)` | Sets up a link. It starts to sync at the first `rscService()`. `RSC_ERROR_NULL` without `write` or `millis`, `RSC_ERROR_BAD_ARG` for a window above 7, an MTU above `RSC_MTU`, or `lock` without `unlock`. |
| `void rscFeed(rscLink_t* link, const uint8_t* data, size_t n)` | Passes received bytes. |
| `uint32_t rscService(rscLink_t* link)` | Runs timers and transmission. Returns the ms until the next deadline. |
| `rscError_t rscSend(link, type, data, length)` | Queues an event (no answer expected). |
| `rscError_t rscRequest(link, type, data, length, timeoutMs, callback, user, &tag)` | Queues a request. `callback` runs for each PROGRESS and once for the final outcome. The timeout counts from now and restarts at each PROGRESS; 0 means no timeout. `tag` may be NULL. |
| `rscError_t rscRespond(link, tag, type, data, length)` | Final answer with an application type and data. |
| `rscError_t rscAck(link, tag, data, length)` | Final answer: success, data optional. |
| `rscError_t rscNack(link, tag, error, data, length)` | Final answer: failure with a signed error code (-1 to -128), data optional. |
| `rscError_t rscProgress(link, tag, data, length)` | Interim answer: the request is accepted; its timeout restarts. |
| `void rscResume(rscLink_t* link)` | After the message callback refused a message: accept messages again. |
| `bool rscIsUp(link)`, `uint16_t rscMtu(link)`, `uint8_t rscQueued(link)` | Link state, the MTU in use, and the messages queued and not yet acknowledged. |
| `const rscStats_t* rscGetStats(link)` | Counters (when `RSC_STATS` is 1). |

The queueing functions return `RSC_OK`, `RSC_ERROR_NULL`, `RSC_ERROR_OVERFLOW` (data longer than the MTU), `RSC_ERROR_QUEUE_FULL`, `RSC_ERROR_NO_TAG` (requests only) or `RSC_ERROR_BAD_ARG` (a message type above 0xEF, or a tag outside 1-127).

### Callbacks

- `bool onMessage(link, const rscMessage_t* m, user)`: a request (`m->tag` 1-127) or an event (`m->tag` 0). `m->data` is valid only during the call. Return false to refuse the message: this side then reports "not ready", the peer pauses, and the message comes again after `rscResume()`.
- `void onEvent(link, uint8_t event, user)`: `RSC_EVENT_UP`, `RSC_EVENT_DOWN` or `RSC_EVENT_PEER_RESET`.
- `void callback(link, const rscResponse_t* r, user)` of a request: `r->event` is `RSC_RESPONSE_DATA`, `_ACK`, `_NACK` (`r->error` holds the code), `_PROGRESS`, `_TIMEOUT`, `_PEER_RESET` or `_DROPPED` (the request is longer than the peer's MTU and was not sent).

### Answering when the queue is full

A queueing function returns `RSC_ERROR_QUEUE_FULL` when `RSC_TX_SLOTS` messages wait. The library never drops a message silently, so the application decides:

1. A device that only answers requests may refuse the request: `onMessage` returns false when the answer does not fit, and the main loop calls `rscResume()` once `rscQueued()` is below `RSC_TX_SLOTS` (see `examples/RSC_Host`).
2. A device that also sends its own requests keeps the answer and queues it again later (see `answer()` in `tests/interop/c_peer.c`). Refusing would let two full queues wait on each other.

## API (Python)

`Link(write, mtu=256, window=4, ...)` is the transport-agnostic link. `StreamLink(read, write, ...)` adds a reader thread, and `SerialLink(port, baud, ...)` opens a serial port with pyserial. Every method may be called from any thread.

| method | behavior |
| --- | --- |
| `send(type, data=b"")` | Queues an event. |
| `request(type, data=b"", timeout=1.0, on_progress=None)` | Sends a request and blocks until its final answer. Returns a `Response` (data or ACK). Raises `NackError` (`.code`, `.data`), `RequestTimeout`, `PeerResetError` or `DroppedError`. The timeout is in seconds; `None` waits without a limit. |
| `request_async(type, data, timeout, callback)` | As above without blocking. `callback(response)` runs for each PROGRESS and for the final outcome. Returns the tag. |
| `respond(tag, type, data)`, `ack(tag, data)`, `nack(tag, error, data)`, `progress(tag, data)` | Answers a request. |
| `resume()` | Accepts messages again after `on_message` returned False. |
| `is_up`, `mtu`, `queued`, `stats` | Link state, MTU in use, messages queued, counters. |

`on_message(link, message)` and `on_event(link, event)` are passed to the constructor. They run in the reader thread. Do not call the blocking `request()` from them; use `request_async()`.

## Wire format

A frame is a link header (2 bytes), a body, and a CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF) over the header and the body, low byte first. The frame is COBS encoded and followed by a 0x00 delimiter. A sender adds a leading 0x00 when its line was silent for `RSC_FLUSH_IDLE_MS`, so that the receiver discards any partial frame left by noise.

Link byte 0:

```
bits 0-2: SEQ - sequence number of this frame (mod 8); 0 in ACK frames
bits 3-5: ACK - next sequence number expected from the peer (mod 8)
bit 6: 1 - reliable (sequenced DATA)
bit 7: 1 - receiver not ready
```

Link byte 1:

```
bits 0-3: kind: 0-DATA, 1-ACK, 2-SYNC, 3-SYNC RESPONSE, 4-15 reserved
bit 4: 1 - another segment follows (reserved for 2.1.0; a 2.0.0 receiver discards the frame)
bits 5-7: reserved, must be 0 (the receiver discards the frame)
```

Bodies:

| kind | body |
| --- | --- |
| DATA | message type (1), tag byte (1: bit 7 response, bits 0-6 tag, 0 = no answer expected), data (0 to MTU bytes) |
| ACK | empty |
| SYNC | protocol version 2 (1), frames in flight (1), MTU (2), nonce (4) |
| SYNC RESPONSE | the SYNC fields, then the nonce it answers (4) |

In SYNC frames, SEQ carries the oldest unacknowledged sequence number and ACK the next expected one. Multi-byte fields are little-endian. A receiver ignores body bytes after the fields it knows.

Message types: 0x00-0xEF belong to the application. 0xF0 (VOID) is a frame its sender withdrew; the receiver acknowledges and discards it. 0xF1-0xFC are reserved. Answers use 0xFD (PROGRESS), 0xFE (NACK, the first data byte is the signed error code) and 0xFF (ACK). An answer with an application type is a final success with data.

### Worked example: SYNC, nonce 0x12345678, MTU 256

Before COBS: `00 02 02 00 00 01 78 56 34 12 53 D2`. On the wire, as the first frame of a link (15 bytes):

```
00       - 0x00 - leading delimiter (first frame)
01       - COBS code - no bytes follow, then an implied 0x00 (link byte 0: SEQ 0, ACK 0)
03       - COBS code - 2 bytes follow, then an implied 0x00
02       - 0x02 - link byte 1: kind SYNC
02       - 0x02 - protocol version 2
           (the implied 0x00: no frames in flight)
01       - COBS code - no bytes follow, then an implied 0x00 (MTU low byte)
08       - COBS code - 7 bytes follow, end of frame
01       - 0x01 - MTU high byte: MTU 0x0100 = 256
78563412 - 0x12345678 - nonce
53D2     - 0xD253 - CRC-16
00       - 0x00 - delimiter
```

### Worked example: DATA, reliable, SEQ 5, ACK 3, type 0x10, request tag 1, data 01 00 02

Before COBS: `5D 00 10 01 01 00 02 CF 94`. On the wire after silence (12 bytes):

```
00       - 0x00 - leading delimiter (after silence)
02       - COBS code - 1 byte follows, then an implied 0x00
5D       - 0x5D - link byte 0: SEQ 5, ACK 3, reliable
           (the implied 0x00: link byte 1, kind DATA)
04       - COBS code - 3 bytes follow, then an implied 0x00
10       - 0x10 - message type
01       - 0x01 - request, tag 1
01       - 0x01 - data byte 1
           (the implied 0x00: data byte 2)
04       - COBS code - 3 bytes follow, end of frame
02       - 0x02 - data byte 3
CF94     - 0x94CF - CRC-16 over 5D 00 10 01 01 00 02
00       - 0x00 - delimiter
```

### Worked example: ACK 6

`00 05 30 01 BB 08 00`: leading delimiter, COBS code 05, link byte 0 0x30 (ACK 6), link byte 1 0x01 (kind ACK), CRC 0x08BB, delimiter. The host tests (C and Python) check these three frames byte for byte.

## Link rules

1. **Sync.** Each side draws a 32-bit nonce at start. Until the link is up it sends SYNC every `RSC_SYNC_INTERVAL_MS`, and it answers every SYNC with SYNC RESPONSE. A side is confirmed when a SYNC RESPONSE echoes its nonce. The link is up when the side is confirmed and knows the peer's nonce.
2. **Peer restart.** A new peer nonce means the peer restarted. A known nonce whose sequence state contradicts the local one is treated the same way, so the link recovers even when a restarted peer draws the same nonce. On a peer restart a side numbers its unacknowledged frames again from 0, ends the requests the old peer had received with PEER_RESET, reports RSC_EVENT_PEER_RESET and syncs again.
3. **Window.** Up to `window` DATA frames (1-7) are in flight (Go-Back-N). The receiver delivers only the next expected SEQ. It discards any other and acknowledges both. Every frame carries ACK, so traffic in one direction acknowledges the other. A pure ACK frame is sent when no DATA frame leaves.
4. **Retransmission.** When the oldest frame is not acknowledged in time, every frame in flight is sent again and the timeout doubles. After `retries` timeouts without progress the link is down: it keeps the queue and syncs again.
5. **Not ready.** A side whose application refused a message sets "receiver not ready" and discards DATA. The sender stops new frames and sends one probe per longest timeout, without counting retries, until the bit clears.
6. **MTU.** Each side announces its MTU in SYNC, and both use the smaller one. A message queued before the sync that turns out longer than the peer's MTU is sent as VOID, and its request ends with DROPPED. `RSC_MTU` stops at 4089 so that a frame before its CRC stays within 32751 bits, the longest message for which CRC-16 0x1021 detects every error of up to 3 bits (Koopman's CRC tables).

## Error codes

| code | value | meaning |
| --- | --- | --- |
| `RSC_OK` | 0 | |
| `RSC_ERROR_GENERAL` | -1 | Also the NACK error when the peer sent none. |
| `RSC_ERROR_TIMEOUT` | -2 | |
| `RSC_ERROR_OVERFLOW` | -3 | Data longer than the MTU. |
| `RSC_ERROR_NULL` | -4 | |
| `RSC_ERROR_QUEUE_FULL` | -5 | `RSC_TX_SLOTS` messages are queued. |
| `RSC_ERROR_NO_TAG` | -6 | `RSC_MAX_REQUESTS` requests are pending. |
| `RSC_ERROR_BAD_ARG` | -7 | A reserved message type, or a tag outside 1-127. |
| `RSC_ERROR_CMD_BAD_ARG`, `_NOT_IMP`, `_BUSY` | -60, -61, -62 | Suggested NACK codes. Any value from -1 to -128 may be used. |

## Tests

- `tests/host`: the C library against a simulated wire that drops, flips and inserts bytes, cuts the line and restarts peers. 24 tests in three configurations (defaults; the AVR defaults; 16 slots with window 7 and the bitwise CRC), built with AddressSanitizer and UndefinedBehaviorSanitizer, plus a C++ syntax check. Run `make -C tests/host`.
- `python/tests`: the same scenarios for the Python link, the shared golden frames, and interop tests against the C library (`tests/interop/c_peer.c`) over pipes, including a lossy pipe and a restart of the C side. Run `make -C tests/interop` and then `python -m pytest python/tests`.
- CI (`.github/workflows/ci.yml`) runs both with gcc and clang and with Python 3.9 and 3.12, compiles the examples for ESP32, ESP32-C3 (arduino-esp32 3.x), ESP32 (2.0.17) and Arduino Uno, builds the two PlatformIO projects, and runs Arduino Lint.
- Nothing has run on hardware yet.

## Upgrading from 1.x

Everything changed. In short:

- The wire format is new (COBS, sequence numbers, sync). A 1.x peer and a 2.0.0 peer do not talk.
- The global platform functions (`rscPlatform*`), the queues and the two tasks are replaced by `rscOps_t` per link and the two calls `rscFeed()` and `rscService()`.
- The application no longer acknowledges messages: the link does. `rscSendStatusAck` and `rscSendStatusNack` become answers to requests (`rscAck`, `rscNack`), and `rscSendMessageReliable` becomes `rscRequest`.
- The 32-bit CID is replaced by the sequence numbers of the link and a 7-bit request tag.
- No heap. `rsc_framework.h` is now `rsc.h` (`ReliableSerialComms.h` for Arduino), and `rsc_legacy.h` is gone.

## Version history

- **2.0.0** (not released): Redesign. COBS framing, a sliding window with retransmission, session sync with peer restart detection, request tags with PROGRESS, ACK, NACK and data answers, flow control, per-link platform operations without heap, a Python package with the same protocol, interop tests, and an AVR build in CI.
- **1.1.0** (2026-10-16): Fixes the build, the length checks (a malformed frame could overflow the receive buffer), the leak on a full transmit queue, the NACK error sign, the ACK status, queue reordering during a wait, and the payload size limits. Adds `rscWaitForReply()`, `rscSendMessageReliable()`, `rscReceivePoll()` and `rscTransmitPoll()`, a defined little-endian wire format, version macros, examples for ESP32, host tests and CI.
- **1.0.0** (source files dated 2024-11-17): First version. It did not compile.

## License

BSD 3-Clause. See `LICENSE.txt`.
