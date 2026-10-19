#include <string.h>

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <RngBuffer.h>

#include "rsc_stm32.h"

extern UART_HandleTypeDef huart1;
extern RngBuffer rxRingBuffer;

rscLink_t rscStm32Link;
volatile uint32_t rscStm32RxDropped = 0;

static SemaphoreHandle_t rscMutex;
static TaskHandle_t rscTask = NULL;
static uint8_t rxByte;

// Write blocks until the bytes are in the UART: frames leave in order from the link task
static int opWrite(void* ctx, const uint8_t* data, size_t length) {
  (void) ctx;
  return HAL_UART_Transmit(&huart1, (uint8_t*) data, (uint16_t) length, HAL_MAX_DELAY) == HAL_OK ? (int) length : -1;
}

static uint32_t opMillis(void* ctx) {
  (void) ctx;
  return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Use the RNG peripheral here when the part has one. The SysTick counter at start-up differs
// little between starts; the link stays correct either way (see the README of the library).
static uint32_t opRandom(void* ctx) {
  (void) ctx;
  return SysTick->VAL ^ (xTaskGetTickCount() << 16) ^ rscStm32RxDropped;
}

static void opLock(void* ctx) {
  (void) ctx;
  xSemaphoreTake(rscMutex, portMAX_DELAY);
}

static void opUnlock(void* ctx) {
  (void) ctx;
  xSemaphoreGive(rscMutex);
}

static void opWake(void* ctx) {
  (void) ctx;
  if ( rscTask ) xTaskNotifyGive(rscTask);
}

void rscStm32Init(rscMessageCb_t onMessage, rscEventCb_t onEvent) {
  rscOps_t ops;
  rscConfig_t cfg;

  rscMutex = xSemaphoreCreateMutex();

  memset(&ops, 0, sizeof(ops));
  ops.write = opWrite;
  ops.millis = opMillis;
  ops.random = opRandom;
  ops.lock = opLock;
  ops.unlock = opUnlock;
  ops.wake = opWake;

  rscConfigDefaults(&cfg);
  cfg.baud = huart1.Init.BaudRate;
  cfg.onMessage = onMessage;
  cfg.onEvent = onEvent;
  rscInit(&rscStm32Link, &cfg, &ops);

  HAL_UART_Receive_IT(&huart1, &rxByte, 1);
}

// One byte per interrupt into the ring buffer, then wake the link task
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if ( huart->Instance == USART1 ) {
    BaseType_t woken = pdFALSE;
    if ( !rng_buffer_put(&rxRingBuffer, rxByte) ) rscStm32RxDropped++;
    HAL_UART_Receive_IT(&huart1, &rxByte, 1);
    if ( rscTask ) vTaskNotifyGiveFromISR(rscTask, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

void startRscTask(void* argument) {
  (void) argument;
  uint8_t buf[32];

  rscTask = xTaskGetCurrentTaskHandle();
  for (;;) {
    size_t n;
    do {
      n = 0;
      rng_buffer_get_elements(&rxRingBuffer, buf, sizeof(buf), &n);
      if ( n > 0 ) rscFeed(&rscStm32Link, buf, n);
    } while ( n == sizeof(buf) );

    uint32_t wait = rscService(&rscStm32Link);
    TickType_t ticks = pdMS_TO_TICKS(wait);
    ulTaskNotifyTake(pdTRUE, ticks > 0 ? ticks : 1);
  }
}
