// A ReliableSerialComms peer for the interop tests (python/tests/test_interop.py).
// It runs the C library over stdin (bytes from the Python side) and stdout (bytes to it).
//
//   c_peer [rto_ms] [mtu] [window]
//
// Requests it answers:
//   0x01  ACK with the request data
//   0x02  NACK -60 with "no"
//   0x03  response type 0x33 with the request data reversed
//   0x04  PROGRESS now, ACK "done" 200 ms later
//   0x05  no response
//   0x07  asks back: sends request 0x08 with the same data to the Python side, then answers
//         with ACK and the data of that response
//   0x09  refused twice (receiver not ready), accepted after rscResume() 100 ms later; ACK
//   0x0A  ACK with the number of 0x20 events received (2 bytes) and 1 if they came in order
//   0x0B  ACK, then exit
// Events: 0x20 carries a 16-bit counter (little-endian), checked for order.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rsc.h"

static rscLink_t peerLink;
static uint32_t finalAt = 0;
static uint8_t finalTag = 0;
static uint32_t resumeAt = 0;
static int refusals = 0;
static uint16_t events = 0;
static int ordered = 1;
static int exitAfter = 0;

// Answers that did not fit the transmit queue (RSC_ERROR_QUEUE_FULL) wait here and are sent again
// from the main loop. An application must handle a full queue the same way.
typedef struct {
  uint8_t kind;                         // RSC_TYPE_ACK, RSC_TYPE_NACK, RSC_TYPE_PROGRESS or a data type
  uint8_t tag;
  int8_t  error;
  uint16_t length;
  uint8_t data[RSC_MTU];
} answer_t;

static answer_t outbox[64];
static int outboxCount = 0;

static rscError_t sendAnswer(rscLink_t* l, const answer_t* a) {
  switch ( a->kind ) {
    case RSC_TYPE_ACK:      return rscAck(l, a->tag, a->data, a->length);
    case RSC_TYPE_NACK:     return rscNack(l, a->tag, a->error, a->data, a->length);
    case RSC_TYPE_PROGRESS: return rscProgress(l, a->tag, a->data, a->length);
    default:                return rscRespond(l, a->tag, a->kind, a->data, a->length);
  }
}

static void answer(rscLink_t* l, uint8_t kind, uint8_t tag, int8_t error, const uint8_t* data, uint16_t length) {
  answer_t a;
  a.kind = kind;
  a.tag = tag;
  a.error = error;
  a.length = length;
  if ( length ) memcpy(a.data, data, length);
  if ( outboxCount == 0 && sendAnswer(l, &a) != RSC_ERROR_QUEUE_FULL ) return;
  if ( outboxCount < (int) (sizeof(outbox) / sizeof(outbox[0])) ) outbox[outboxCount++] = a;
}

static void flushOutbox(rscLink_t* l) {
  int i = 0;
  while ( i < outboxCount && sendAnswer(l, &outbox[i]) != RSC_ERROR_QUEUE_FULL ) i++;
  memmove(outbox, outbox + i, (size_t) (outboxCount - i) * sizeof(answer_t));
  outboxCount -= i;
}

static int opWrite(void* ctx, const uint8_t* data, size_t length) {
  (void) ctx;
  size_t done = 0;
  while ( done < length ) {
    ssize_t n = write(STDOUT_FILENO, data + done, length - done);
    if ( n < 0 ) {
      if ( errno == EINTR ) continue;
      return -1;
    }
    done += (size_t) n;
  }
  return (int) length;
}

static uint32_t opMillis(void* ctx) {
  (void) ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t) (ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32_t opRandom(void* ctx) {
  (void) ctx;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint32_t) ts.tv_nsec ^ ((uint32_t) getpid() << 16) ^ (uint32_t) ts.tv_sec;
}

static void onAskedBack(rscLink_t* l, const rscResponse_t* rsp, void* user) {
  uint8_t orig = (uint8_t) (uintptr_t) user;
  if ( rsp->event == RSC_RESPONSE_PROGRESS ) return;
  if ( rsp->event == RSC_RESPONSE_ACK || rsp->event == RSC_RESPONSE_DATA ) answer(l, RSC_TYPE_ACK, orig, 0, rsp->data, rsp->length);
  else answer(l, RSC_TYPE_NACK, orig, RSC_ERROR_CMD_BUSY, NULL, 0);
}

static bool onMessage(rscLink_t* l, const rscMessage_t* m, void* user) {
  (void) user;
  uint8_t buf[RSC_MTU];

  if ( m->tag == 0 ) {
    if ( m->type == 0x20 && m->length >= 2 ) {
      uint16_t v = (uint16_t) (m->data[0] | (m->data[1] << 8));
      if ( v != events ) ordered = 0;
      events++;
    }
    return true;
  }

  switch ( m->type ) {
    case 0x01:
      answer(l, RSC_TYPE_ACK, m->tag, 0, m->data, m->length);
      break;
    case 0x02:
      answer(l, RSC_TYPE_NACK, m->tag, RSC_ERROR_CMD_BAD_ARG, (const uint8_t*) "no", 2);
      break;
    case 0x03:
      for (uint16_t i = 0; i < m->length; i++) buf[i] = m->data[m->length - 1 - i];
      answer(l, 0x33, m->tag, 0, buf, m->length);
      break;
    case 0x04:
      answer(l, RSC_TYPE_PROGRESS, m->tag, 0, NULL, 0);
      finalTag = m->tag;
      finalAt = opMillis(NULL) + 200;
      break;
    case 0x07:
      if ( rscRequest(l, 0x08, m->data, m->length, 2000, onAskedBack, (void*) (uintptr_t) m->tag, NULL) != RSC_OK ) {
        answer(l, RSC_TYPE_NACK, m->tag, RSC_ERROR_CMD_BUSY, NULL, 0);
      }
      break;
    case 0x09:
      if ( refusals < 2 ) {
        refusals++;
        resumeAt = opMillis(NULL) + 100;
        return false;
      }
      refusals = 0;
      answer(l, RSC_TYPE_ACK, m->tag, 0, NULL, 0);
      break;
    case 0x0A:
      buf[0] = (uint8_t) events;
      buf[1] = (uint8_t) (events >> 8);
      buf[2] = (uint8_t) ordered;
      answer(l, RSC_TYPE_ACK, m->tag, 0, buf, 3);
      break;
    case 0x0B:
      answer(l, RSC_TYPE_ACK, m->tag, 0, NULL, 0);
      exitAfter = 1;
      break;
    default:
      break;
  }
  return true;
}

int main(int argc, char** argv) {
  rscConfig_t cfg;
  rscOps_t ops;

  memset(&ops, 0, sizeof(ops));
  ops.write = opWrite;
  ops.millis = opMillis;
  ops.random = opRandom;
  rscConfigDefaults(&cfg);
  cfg.rtoMs = (uint16_t) ( argc > 1 ? atoi(argv[1]) : 30 );
  if ( argc > 2 ) cfg.mtu = (uint16_t) atoi(argv[2]);
  if ( argc > 3 ) cfg.window = (uint8_t) atoi(argv[3]);
  cfg.onMessage = onMessage;
  if ( rscInit(&peerLink, &cfg, &ops) != RSC_OK ) return 2;

  uint32_t exitAt = 0;
  while (true) {
    flushOutbox(&peerLink);
    uint32_t wait = rscService(&peerLink);
    if ( outboxCount > 0 && wait > 5 ) wait = 5;
    uint32_t now = opMillis(NULL);
    if ( finalAt && (int32_t) (now - finalAt) >= 0 ) {
      answer(&peerLink, RSC_TYPE_ACK, finalTag, 0, (const uint8_t*) "done", 4);
      finalAt = 0;
      wait = 0;
    }
    if ( resumeAt && (int32_t) (now - resumeAt) >= 0 ) {
      rscResume(&peerLink);
      resumeAt = 0;
      wait = 0;
    }
    if ( exitAfter ) {
      if ( exitAt == 0 ) exitAt = now + 200;     // let the ACK leave and be acknowledged
      if ( (int32_t) (now - exitAt) >= 0 ) return 0;
    }
    if ( finalAt || resumeAt || exitAfter ) wait = wait > 5 ? 5 : wait;

    struct pollfd p;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    int r = poll(&p, 1, (int) wait);
    if ( r > 0 ) {
      uint8_t buf[512];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if ( n == 0 ) return 0;                     // the Python side closed the pipe
      if ( n > 0 ) rscFeed(&peerLink, buf, (size_t) n);
    }
  }
}
