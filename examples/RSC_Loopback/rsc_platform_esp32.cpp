// ReliableSerialComms platform layer for arduino-esp32 (2.x and 3.x) with FreeRTOS.
// The functions are declared in rsc_framework.h with C linkage.

#include <Arduino.h>
#include <rsc_framework.h>

#include "rsc_platform_esp32.h"

static HardwareSerial* rscSerial = NULL;
static QueueHandle_t   rscTxQueue = NULL;
static QueueHandle_t   rscRxQueue = NULL;
static portMUX_TYPE    rscMux = portMUX_INITIALIZER_UNLOCKED;

bool rscPlatformBegin(HardwareSerial& serial) {
  rscSerial = &serial;
  rscTxQueue = xQueueCreate(RSC_MSG_Q_DEPTH, sizeof(rscEnvelope_t));
  rscRxQueue = xQueueCreate(RSC_MSG_Q_DEPTH, sizeof(rscEnvelope_t));
  return rscTxQueue != NULL && rscRxQueue != NULL;
}

static TickType_t rscTicks(uint32_t timeout) {
  return (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);
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

// Called in a loop by rscReceiveLoop(): yield for a tick when nothing has arrived
bool rscPlatformReceiveReady(void) {
  if ( rscSerial->available() > 0 ) return true;
  vTaskDelay(1);
  return false;
}

int rscPlatformReceiveByte(uint32_t timeout) {
  uint32_t start = millis();
  while ( rscSerial->available() <= 0 ) {
    if ( timeout != RSC_BLOCKING && millis() - start >= timeout ) return RSC_ERROR_REC_TIMEOUT;
    vTaskDelay(1);
  }
  return rscSerial->read();
}

int rscPlatformReceiveBytes(uint8_t* buffer, size_t expectedLength, uint32_t timeout) {
  if ( buffer == NULL || expectedLength == 0 ) return RSC_ERROR_NULL;

  uint32_t start = millis();
  size_t got = 0;
  while ( got < expectedLength ) {
    int available = rscSerial->available();
    if ( available > 0 ) {
      size_t want = expectedLength - got;
      if ( want > (size_t) available ) want = (size_t) available;
      got += rscSerial->read(buffer + got, want);
      continue;
    }
    if ( timeout != RSC_BLOCKING && millis() - start >= timeout ) return RSC_ERROR_REC_TIMEOUT;
    vTaskDelay(1);
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
  return xQueueSend(rscTxQueue, qData, rscTicks(timeout)) == pdTRUE ? RSC_OK : RSC_ERROR_TRX_Q_FULL;
}

rscError_t rscPlatformDequeueTransmitData(rscEnvelope_t* qData, uint32_t timeout) {
  return xQueueReceive(rscTxQueue, qData, rscTicks(timeout)) == pdTRUE ? RSC_OK : RSC_ERROR_TRX_Q_EMPTY;
}

rscError_t rscPlatformEnqueueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  return xQueueSend(rscRxQueue, qData, rscTicks(timeout)) == pdTRUE ? RSC_OK : RSC_ERROR_REC_Q_FULL;
}

rscError_t rscPlatformDequeueReceiveData(rscEnvelope_t* qData, uint32_t timeout) {
  return xQueueReceive(rscRxQueue, qData, rscTicks(timeout)) == pdTRUE ? RSC_OK : RSC_ERROR_REC_Q_EMPTY;
}
