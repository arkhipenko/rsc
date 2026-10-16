// ReliableSerialComms platform layer for a superloop on arduino-esp32 (2.x and 3.x).
// Only loop() uses the library, so the queues need no lock.
// The functions are declared in rsc_framework.h with C linkage.

#include <Arduino.h>
#include <rsc_framework.h>

#include "rsc_platform_poll.h"

typedef struct {
  rscEnvelope_t items[RSC_MSG_Q_DEPTH];
  int           head;
  int           count;
} rscQueue_t;

static HardwareSerial* rscSerial = NULL;
static rscQueue_t      rscTxQueue;
static rscQueue_t      rscRxQueue;
static portMUX_TYPE    rscMux = portMUX_INITIALIZER_UNLOCKED;

void rscPlatformBegin(HardwareSerial& serial) {
  rscSerial = &serial;
}

static bool queuePut(rscQueue_t* q, const rscEnvelope_t* e) {
  if ( q->count >= RSC_MSG_Q_DEPTH ) return false;
  q->items[(q->head + q->count) % RSC_MSG_Q_DEPTH] = *e;
  q->count++;
  return true;
}

static bool queueGet(rscQueue_t* q, rscEnvelope_t* e) {
  if ( q->count == 0 ) return false;
  *e = q->items[q->head];
  q->head = (q->head + 1) % RSC_MSG_Q_DEPTH;
  q->count--;
  return true;
}

void* rscPlatformMalloc(size_t length) {
  return malloc(length);
}

void rscPlatformFree(void* ptr) {
  free(ptr);
}

void rscPlatformEnterCritical(void) {
  portENTER_CRITICAL(&rscMux);
}

void rscPlatformExitCritical(void) {
  portEXIT_CRITICAL(&rscMux);
}

int rscPlatformSendByte(uint8_t onebyte) {
  return rscSerial->write(onebyte) == 1 ? 1 : RSC_ERROR_TRX_ERROR;
}

int rscPlatformSendBytes(uint8_t* bytes, size_t length) {
  return rscSerial->write(bytes, length) == length ? (int) length : RSC_ERROR_TRX_ERROR;
}

bool rscPlatformReceiveReady(void) {
  return rscSerial->available() > 0;
}

int rscPlatformReceiveByte(uint32_t timeout) {
  uint32_t start = millis();
  while ( rscSerial->available() <= 0 ) {
    if ( timeout != RSC_BLOCKING && millis() - start >= timeout ) return RSC_ERROR_REC_TIMEOUT;
    delay(1);
  }
  return rscSerial->read();
}

// Not used by rscReceivePoll(); the library still links it
int rscPlatformReceiveBytes(uint8_t* buffer, size_t expectedLength, uint32_t timeout) {
  if ( buffer == NULL || expectedLength == 0 ) return RSC_ERROR_NULL;
  for (size_t i = 0; i < expectedLength; i++) {
    int c = rscPlatformReceiveByte(timeout);
    if ( c < 0 ) return c;
    buffer[i] = (uint8_t) c;
  }
  return (int) expectedLength;
}

void rscPlatformDelayMs(uint32_t ms) {
  delay(ms);
}

uint32_t rscPlatformMillis(void) {
  return millis();
}

rscError_t rscPlatformEnqueueTransmitData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queuePut(&rscTxQueue, qData) ? RSC_OK : RSC_ERROR_TRX_Q_FULL;
}

rscError_t rscPlatformDequeueTransmitData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queueGet(&rscTxQueue, qData) ? RSC_OK : RSC_ERROR_TRX_Q_EMPTY;
}

rscError_t rscPlatformEnqueueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queuePut(&rscRxQueue, qData) ? RSC_OK : RSC_ERROR_REC_Q_FULL;
}

rscError_t rscPlatformDequeueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queueGet(&rscRxQueue, qData) ? RSC_OK : RSC_ERROR_REC_Q_EMPTY;
}
