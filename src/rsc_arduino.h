#pragma once

// Platform operations for an Arduino Stream (Serial, Serial1, SoftwareSerial, ...).
// Included by ReliableSerialComms.h in Arduino C++ builds.

#if defined(ARDUINO) && defined(__cplusplus)

#include <Arduino.h>
#include <string.h>

#include "rsc.h"

static inline int rscStreamWrite( void* ctx, const uint8_t* data, size_t length ) {
  return (int) ((Stream*) ctx)->write(data, length);
}

static inline uint32_t rscStreamMillis( void* ctx ) {
  (void) ctx;
  return millis();
}

// Arduino random(): seed it in setup() (for example randomSeed(analogRead(A0)) on AVR), so the
// nonce differs on every start
static inline uint32_t rscStreamRandom( void* ctx ) {
  (void) ctx;
  return ((uint32_t) random(0x10000) << 16) ^ (uint32_t) random(0x10000) ^ (uint32_t) micros();
}

// Operations for one Stream. Add lock, unlock and wake when other tasks queue messages.
static inline rscOps_t rscStreamOps( Stream& stream ) {
  rscOps_t ops;
  memset(&ops, 0, sizeof(ops));
  ops.write = rscStreamWrite;
  ops.millis = rscStreamMillis;
  ops.random = rscStreamRandom;
  ops.ctx = &stream;
  return ops;
}

// Feed every byte that has arrived, then run timers and transmission. Call it from loop() or
// from the task that owns the link. Returns the ms until it needs to run again.
static inline uint32_t rscStreamPoll( rscLink_t* link, Stream& stream ) {
  uint8_t buf[16];
  int n;

  while ( (n = stream.available()) > 0 ) {
    if ( n > (int) sizeof(buf) ) n = (int) sizeof(buf);
    for (int i = 0; i < n; i++) buf[i] = (uint8_t) stream.read();
    rscFeed(link, buf, (size_t) n);
  }
  return rscService(link);
}

#endif
