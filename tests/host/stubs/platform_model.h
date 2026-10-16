// Host model of the ReliableSerialComms platform layer.
//
// One node talks to itself: every byte sent is appended to a loopback wire, and the
// receive functions read from that wire. The queues follow FreeRTOS-Kernel queue.c
// (xQueueGenericSend, xQueueReceive): fixed capacity, items copied in and out by value,
// timeout 0 never waits. Time is simulated: rscPlatformMillis() returns model.now, and
// rscPlatformDelayMs() and the receive timeouts advance it.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <functional>

#include "rsc_framework.h"

#define MODEL_WIRE_SIZE   (200 * 1024)
#define MODEL_QUEUE_MAX   32

struct ModelQueue {
  rscEnvelope_t items[MODEL_QUEUE_MAX];
  int           head;
  int           count;
  int           capacity;     // 0 .. MODEL_QUEUE_MAX
};

struct PlatformModel {
  // loopback wire
  uint8_t*  wire;
  size_t    wHead;            // bytes written
  size_t    wTail;            // bytes read

  // clock
  uint32_t  now;
  uint32_t  millisStep;       // added to now on every rscPlatformMillis() call
  int       delayCalls;
  std::function<void(void)> onDelay;   // runs inside rscPlatformDelayMs(): the "peer"

  // memory
  int       allocCalls;       // calls to rscPlatformMalloc()
  int       allocs;           // successful allocations
  int       frees;
  int       failAllocAt;      // fail the call with this number (1-based), 0: never

  // send faults
  int       sendCalls;
  int       failSendAt;       // fail this send call (1-based), 0: never
  bool      shortSend;        // the failing call reports one byte less instead of an error

  // critical section
  int       criticalDepth;
  int       criticalMax;

  ModelQueue tx;
  ModelQueue rx;
};

extern PlatformModel model;

void   modelReset(void);                          // empty wire and queues, counters 0, capacities 16
void   modelWirePush(const uint8_t* b, size_t n); // bytes "arriving" on the wire
size_t modelWirePending(void);
int    modelQueueCount(const ModelQueue& q);
