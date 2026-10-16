#include <stdlib.h>
#include <string.h>

#include "platform_model.h"

PlatformModel model;

static uint8_t wireStorage[MODEL_WIRE_SIZE];

// Free the heap payloads of envelopes a test left in a queue
static void queueDrop(ModelQueue& q) {
  for (int i = 0; i < q.count; i++) {
    rscEnvelope_t& e = q.items[(q.head + i) % MODEL_QUEUE_MAX];
    if ( e.rscEnvelopePayload.rscPayloadHeader.rscPayloadDataLen > RSC_EMB_DATA_LEN ) free(e.rscEnvelopePayload.rscPayloadData.ptr);
  }
}

void modelReset(void) {
  queueDrop(model.tx);
  queueDrop(model.rx);
  model.wire = wireStorage;
  model.wHead = model.wTail = 0;
  model.now = 1000;
  model.millisStep = 0;
  model.delayCalls = 0;
  model.onDelay = nullptr;
  model.allocCalls = model.allocs = model.frees = 0;
  model.failAllocAt = 0;
  model.sendCalls = 0;
  model.failSendAt = 0;
  model.shortSend = false;
  model.criticalDepth = model.criticalMax = 0;
  memset(&model.tx, 0, sizeof(model.tx));
  memset(&model.rx, 0, sizeof(model.rx));
  model.tx.capacity = RSC_MSG_Q_DEPTH;
  model.rx.capacity = RSC_MSG_Q_DEPTH;
}

void modelWirePush(const uint8_t* b, size_t n) {
  if ( model.wHead + n > MODEL_WIRE_SIZE ) abort();
  memcpy(model.wire + model.wHead, b, n);
  model.wHead += n;
}

size_t modelWirePending(void) {
  return model.wHead - model.wTail;
}

int modelQueueCount(const ModelQueue& q) {
  return q.count;
}

static rscError_t queuePut(ModelQueue& q, const rscEnvelope_t* e, rscError_t full) {
  if ( q.count >= q.capacity ) return full;
  q.items[(q.head + q.count) % MODEL_QUEUE_MAX] = *e;
  q.count++;
  return RSC_OK;
}

static rscError_t queueGet(ModelQueue& q, rscEnvelope_t* e, rscError_t empty) {
  if ( q.count == 0 ) return empty;
  *e = q.items[q.head];
  q.head = (q.head + 1) % MODEL_QUEUE_MAX;
  q.count--;
  return RSC_OK;
}


// Platform layer =============================================================
void* rscPlatformMalloc(size_t length) {
  model.allocCalls++;
  if ( model.failAllocAt == model.allocCalls ) return NULL;
  model.allocs++;
  return malloc(length);
}

void rscPlatformFree(void* ptr) {
  if ( ptr ) {
    model.frees++;
    free(ptr);
  }
}

void rscPlatformEnterCritical(void) {
  model.criticalDepth++;
  if ( model.criticalDepth > model.criticalMax ) model.criticalMax = model.criticalDepth;
}

void rscPlatformExitCritical(void) {
  model.criticalDepth--;
}

int rscPlatformSendByte(uint8_t onebyte) {
  return rscPlatformSendBytes(&onebyte, 1);
}

int rscPlatformSendBytes(uint8_t* bytes, size_t length) {
  model.sendCalls++;
  if ( model.failSendAt == model.sendCalls ) {
    if ( !model.shortSend ) return RSC_ERROR_TRX_ERROR;
    modelWirePush(bytes, length - 1);
    return (int) length - 1;
  }
  modelWirePush(bytes, length);
  return (int) length;
}

bool rscPlatformReceiveReady(void) {
  return modelWirePending() > 0;
}

int rscPlatformReceiveByte(uint32_t timeout) {
  if ( modelWirePending() > 0 ) return model.wire[model.wTail++];
  if ( timeout != RSC_BLOCKING ) model.now += timeout;
  return RSC_ERROR_REC_TIMEOUT;
}

// All expectedLength bytes, or a timeout error. Bytes are stored as they arrive
// (as the STM32 example does), so a wrong length shows up as an overflow.
int rscPlatformReceiveBytes(uint8_t* buffer, size_t expectedLength, uint32_t timeout) {
  if ( buffer == NULL || expectedLength == 0 ) return RSC_ERROR_NULL;
  for (size_t i = 0; i < expectedLength; i++) {
    if ( modelWirePending() == 0 ) {
      if ( timeout != RSC_BLOCKING ) model.now += timeout;
      return RSC_ERROR_REC_TIMEOUT;
    }
    buffer[i] = model.wire[model.wTail++];
  }
  return (int) expectedLength;
}

void rscPlatformDelayMs(uint32_t ms) {
  model.delayCalls++;
  model.now += ms;
  if ( model.onDelay ) model.onDelay();
}

uint32_t rscPlatformMillis(void) {
  uint32_t t = model.now;
  model.now += model.millisStep;
  return t;
}

rscError_t rscPlatformEnqueueTransmitData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queuePut(model.tx, qData, RSC_ERROR_TRX_Q_FULL);
}

rscError_t rscPlatformDequeueTransmitData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queueGet(model.tx, qData, RSC_ERROR_TRX_Q_EMPTY);
}

rscError_t rscPlatformEnqueueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queuePut(model.rx, qData, RSC_ERROR_REC_Q_FULL);
}

rscError_t rscPlatformDequeueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  (void) timeout;
  return queueGet(model.rx, qData, RSC_ERROR_REC_Q_EMPTY);
}
