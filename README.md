# Reliable Serial Communications

Version 1.1.0 (2026-10-16)

Reliable Serial Communications (RSC) sends typed messages over a serial byte stream such as a UART. Each message carries a type, a 32-bit correlation id (CID) and up to 65528 data bytes. Every frame starts with a two-byte marker, carries its length with the bitwise inverse of that length, and ends with a CRC-16. The receiver answers with an ACK or a NACK, and the sender can wait for that reply or let the library resend the message. The library is plain C. The application supplies a small platform layer: UART access, two queues, memory, time and a critical section.

## Installation

- Arduino IDE: copy the repository into the `libraries` folder. The library is not in the Library Manager.
- PlatformIO: `lib_deps = https://github.com/arkhipenko/rsc.git#v1.1.0`. The repository is private, so the build machine needs read access to it.
- Other build systems: compile `src/rsc_framework.c` and `src/rsc_crc.c`, and add `src` to the include path.

## Supported platforms

The library is portable C99. CI compiles the examples for ESP32 and ESP32-C3 on arduino-esp32 3.x, and for ESP32 on arduino-esp32 2.0.17. Host tests run with gcc and clang. Every other target needs its own platform layer, and it is not tested. `examples/rsc_stm32` is a platform layer for STM32 HAL with FreeRTOS. It is a fragment of a CubeIDE project, and CI does not compile it.

NOTE: on a target with a 16-bit `int` (AVR), the platform send and receive functions cannot report more than 32767 bytes. Keep messages below that size there.

## Build options

Define these before the library is compiled (for example `-DRSC_REPLY_SLOTS=8`).

| option | default | meaning |
| --- | --- | --- |
| `RSC_REPLY_SLOTS` | 4 | ACK and NACK replies held until a wait takes them. When the table is full, the oldest reply is replaced. Minimum 1. |
| `RSC_WAIT_POLL_MS` | 1 | Delay between two checks of `rscWaitForReply()`, in ms. |
| `RSC_CRC16_POLYNOM` | `RSC_CRC16_CCITT` (0x1021) | CRC polynomial. Both peers must use the same one. |
| `RSC_NO_LEGACY` | not defined | Hides the 1.0.0 names (see "Upgrading to 1.1.0"). |

## Quick start (RTOS)

```c
#include <rsc_framework.h>

// 1. Implement the platform functions (see "Platform layer").
// 2. Start two tasks: one calls rscReceiveLoop(), the other rscTransmitLoop().
// 3. Send a message and wait for its reply:
const char text[] = "hello";
rscMessageHeader_t h;
h.rscMessageType = RSC_MSG_TYPE_DATA;
h.rscMessageCID = rscGetNextCID();
h.rscPayloadDataLen = sizeof(text) - 1;

rscReply_t reply;
rscError_t rc = rscSendMessageReliable(&h, (const uint8_t*) text, 3, 500, &reply);
if ( rc == RSC_OK && reply.type == RSC_MSG_TYPE_ACK ) {
  // delivered; reply.status is the status byte of the ACK
}
```

The peer takes data messages with `rscPlatformDequeueReceiveData()`, answers each with `rscSendStatusAck()` or `rscSendStatusNack()`, and frees a payload longer than 4 bytes with `rscPlatformFree()`. `examples/RSC_Loopback` shows both roles on one board. `examples/RSC_Poll` shows the same exchange in a superloop without tasks.

## Platform layer

The application implements every function below with C linkage. A C++ file includes `rsc_framework.h` before the definitions.

| function | contract |
| --- | --- |
| `void* rscPlatformMalloc(size_t length)` | Returns a buffer or NULL. |
| `void rscPlatformFree(void* ptr)` | Frees a buffer from `rscPlatformMalloc()`. NULL is allowed. |
| `void rscPlatformEnterCritical(void)`, `void rscPlatformExitCritical(void)` | Protect the CID counter and the reply table. Never nested. |
| `int rscPlatformSendByte(uint8_t b)` | Returns 1 or a negative error. The library does not call it. |
| `int rscPlatformSendBytes(uint8_t* bytes, size_t length)` | Writes all bytes. Returns `length` or a negative error. |
| `bool rscPlatformReceiveReady(void)` | True when a byte is waiting. With the loops it may delay when nothing is waiting. With `rscReceivePoll()` it must not delay. |
| `int rscPlatformReceiveByte(uint32_t timeout)` | Returns the byte (0-255), or a negative error after `timeout` ms. `RSC_BLOCKING` waits forever. 0 does not wait. |
| `int rscPlatformReceiveBytes(uint8_t* buffer, size_t n, uint32_t timeout)` | Returns `n` when all `n` bytes arrived within `timeout` ms, else a negative error. |
| `void rscPlatformDelayMs(uint32_t ms)` | Delays (and yields on an RTOS). |
| `uint32_t rscPlatformMillis(void)` | Milliseconds since start. It may wrap. |
| `rscError_t rscPlatformEnqueueTransmitData(rscEnvelope_t* e, uint32_t timeout)` | Copies the envelope into the transmit queue. `RSC_OK` or an error. |
| `rscError_t rscPlatformDequeueTransmitData(rscEnvelope_t* e, uint32_t timeout)` | Copies the oldest envelope out. `RSC_OK` or an error. |
| `rscError_t rscPlatformEnqueueReceiveData(rscEnvelope_t* e, uint32_t timeout)` | Copies a received envelope into the receive queue. |
| `rscError_t rscPlatformDequeueReceiveData(rscEnvelope_t* e, uint32_t timeout)` | Copies the oldest received envelope out. Timeout 0 does not wait. |

## API

| function | behavior |
| --- | --- |
| `rscSendMessage(const rscMessageHeader_t* h, const uint8_t* data)` | Copies the data, builds the frame and queues it (200 ms enqueue timeout). The caller sets type, CID and data length. Returns `RSC_OK`, `RSC_ERROR_NULL`, `RSC_ERROR_OVERFLOW` (data length above 65528), `RSC_ERROR_MEMORY`, or the enqueue error. |
| `rscSendMessageReliable(h, data, attempts, timeout, &reply)` | Sends, waits up to `timeout` ms for the reply, and sends again with the same CID after a timeout or a NACK that asks for a resend. 0 attempts count as 1. Returns `RSC_OK` when a reply arrived (`reply.type` tells ACK from NACK), `RSC_ERROR_TIMEOUT` after the last attempt, or the send error. |
| `rscSendStatusAck(uint32_t CID, int8_t status)` | Sends an ACK. A nonzero status travels as one data byte. |
| `rscSendStatusNack(uint32_t CID, int8_t error, bool resend)` | Sends a NACK with the error byte and the resend flag. |
| `rscWaitForReply(uint32_t CID, rscReply_t* reply, uint32_t timeout)` | Takes the ACK or NACK of the CID from the reply table, waiting up to `timeout` ms. Timeout 0 checks once without a delay. Returns `RSC_OK`, `RSC_ERROR_TIMEOUT` or `RSC_ERROR_NULL`. |
| `rscWaitForAckNack(uint32_t CID, rscError_t* rc, uint32_t timeout)` | As `rscWaitForReply()`. `*rc` is the ACK status (`RSC_OK` when the ACK carries none), or the NACK error (`RSC_ERROR_GENERAL` when the NACK carries 0). |
| `rscGetNextCID(void)` | Returns 0, 1, 2, ... under the critical section. |
| `rscReceiveLoop(void)`, `rscTransmitLoop(void)` | Task bodies. They never return. `rscReceiveError` holds the result of the last frame. |
| `rscReceivePoll(void)` | Parses the bytes that have arrived and returns at once. Returns `RSC_OK`, or the error of the last frame it rejected. A frame that stalls for more than 200 ms is dropped (`RSC_ERROR_REC_TIMEOUT`). |
| `rscTransmitPoll(void)` | Sends one queued frame. Returns `RSC_OK`, the dequeue error (`RSC_ERROR_TRX_Q_EMPTY` when nothing is queued), or the send error. |
| `rscReceiveLoopEnvelopeStart(timeout)`, `rscReceiveLoopEnvelopeRead()` | One step of the receive loop: find the start marker, then read and deliver one frame. |
| `rscTransmitLoopReady(&e, timeout)`, `rscTransmitLoopSend(&e)` | One step of the transmit loop. `rscTransmitLoopSend()` does not free the payload. |
| `rscCrc16(const uint8_t* bytes, size_t length, uint16_t init)` | CRC-16/CCITT-FALSE with the default polynomial. The check value of "123456789" with init 0xFFFF is 0x29B1. |

## Semantics

1. Data messages go to the receive queue in the order they arrive. ACK and NACK frames go to the reply table, never to the receive queue.
2. A reply for a CID already in the table replaces the earlier one.
3. A data length of 4 or less travels as 4 bytes and is held inside the envelope. A longer payload is held in a buffer from `rscPlatformMalloc()`. Whoever takes the envelope from a queue owns that buffer and frees it with `rscPlatformFree()` when the data length is above 4.
4. A resend uses the same CID. The receiver sees the message again when only the reply was lost, so it should treat a repeated CID as a duplicate.
5. A frame is rejected when the inverted length does not match, when the length does not equal 7 + max(data length, 4), or when the CRC does not match. The receiver sends nothing back. The sender learns of the loss through the missing reply.
6. Do not mix `rscReceiveLoop()` with `rscReceivePoll()`, or `rscTransmitLoop()` with `rscTransmitPoll()`.

## Limits and costs

- Data length: 0 to 65528 bytes (`RSC_MAX_DATA_LEN`).
- Overhead per frame: 15 bytes, plus padding to 4 data bytes.
- `rscEnvelope_t`: 19 bytes on a 32-bit target (23 on a 64-bit host). Each queue holds envelopes by value.
- Reply table: `RSC_REPLY_SLOTS` x 12 bytes on ARM Cortex-M and ESP32 (measured with their gcc).
- Heap: one allocation per message with more than 4 data bytes, on each side.

## Wire format

Every multi-byte field is little-endian.

| offset | size | field |
| --- | --- | --- |
| 0 | 1 | 0xAA |
| 1 | 1 | 0x55 |
| 2 | 2 | payload length L = 7 + max(data length, 4) |
| 4 | 2 | bitwise NOT of L |
| 6 | 1 | message type: 0xDA data, 0xA0 ACK, 0xAE NACK |
| 7 | 4 | CID |
| 11 | 2 | data length N |
| 13 | max(N, 4) | data, zero padded to 4 bytes |
| 13 + max(N, 4) | 2 | CRC-16 over offsets 6 to 12 + max(N, 4) |

Example: NACK for CID 42, error -33 (`RSC_ERROR_REC_CRC`), resend requested. 19 bytes.

```
AA 55        start marker
0B 00        L = 11
F4 FF        NOT L = 0xFFF4
AE           NACK
2A 00 00 00  CID 42
02 00        N = 2
DF 01 00 00  error -33, resend 1, two padding bytes
F4 C9        CRC-16 = 0xC9F4
```

## Error codes

| range | codes |
| --- | --- |
| general | `RSC_OK` 0, `RSC_ERROR_GENERAL` -1, `RSC_ERROR_TIMEOUT` -2, `RSC_ERROR_OVERFLOW` -3, `RSC_ERROR_NULL` -4, `RSC_ERROR_MEMORY` -5 |
| transmit | `RSC_ERROR_TRX_Q_FULL` -20, `RSC_ERROR_TRX_Q_EMPTY` -21, `RSC_ERROR_TRX_Q_ERROR` -22, `RSC_ERROR_TRX_ERROR` -23, `RSC_ERROR_TRX_TIMEOUT` -24 |
| receive | `RSC_ERROR_REC_BAD_LEN` -30, `RSC_ERROR_REC_OOM` -31, `RSC_ERROR_REC_TIMEOUT` -32, `RSC_ERROR_REC_CRC` -33, `RSC_ERROR_REC_Q_FULL` -34, `RSC_ERROR_REC_Q_EMPTY` -35, `RSC_ERROR_REC_Q_ERROR` -36, `RSC_ERROR_REC_ERROR` -37 |
| command (for NACK replies) | `RSC_ERROR_CMD_BAD_ARG` -60, `RSC_ERROR_CMD_NOT_IMP` -61 |

## Upgrading to 1.1.0

1.0.0 did not compile (an extra parenthesis), so no working 1.0.0 build exists. Code written against its header still compiles with 1.1.0, with these changes in behavior:

- ACK and NACK frames no longer enter the receive queue. Code that took them from the queue must use `rscWaitForReply()` or `rscWaitForAckNack()`.
- `rscWaitForAckNack()` now returns the ACK status in `*rc`, and the NACK error with its sign (-33, not 223). A NACK with error 0 reads as `RSC_ERROR_GENERAL`.
- `rscWaitForAckNack()` no longer reorders the receive queue, and it no longer spins without a delay.
- Frames with a wrong inverted length, or with a length that does not match the data length, are rejected with `RSC_ERROR_REC_BAD_LEN`. 1.0.0 accepted some of them, and a few of those overflowed the receive buffer.
- `rscSendMessage()` returns `RSC_ERROR_OVERFLOW` for a data length above 65528.
- The macro `POLYNOM` is gone. Use `RSC_CRC16_POLYNOM`.

These names moved to `src/rsc_legacy.h`, which `rsc_framework.h` includes unless `RSC_NO_LEGACY` is defined:

- `rscPlatfromSendBytes` and `rscTrasmitLoopSend` (now `rscPlatformSendBytes` and `rscTransmitLoopSend`). An application that defines `rscPlatfromSendBytes()` keeps working when that source file includes `rsc_framework.h`.
- `RSC_MSG_TYPE_RES` (the same value as `RSC_MSG_TYPE_ACK`), `RSC_ERROR_REC_BADLEN` (never returned), `RSC_ERROR_CMD_FLASH`, `RSC_MSG_RESET_SOFT`, `RSC_MSG_RESET_FACT`, `rscVersion_t`, `rscVersionComponents_t` and `rscRelayStatus_t`.

## Version history

- **1.1.0** (2026-10-16): Fixes the build, the length checks (a malformed frame could overflow the receive buffer), the leak on a full transmit queue, the NACK error sign, the ACK status, queue reordering during a wait, and the payload size limits. Adds `rscWaitForReply()`, `rscSendMessageReliable()`, `rscReceivePoll()` and `rscTransmitPoll()`, a defined little-endian wire format, version macros, examples for ESP32, host tests and CI. The STM32 fragment moves to RngBuffer 2.0.1.
- **1.0.0** (source files dated 2024-11-17): First version. It did not compile.

## License

BSD 3-Clause. See `LICENSE.txt`.
