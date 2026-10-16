#include "main.h"
#include "cmsis_os.h"
#include "rsc_framework.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <stdlib.h>
#include <RngBuffer.h>

#include "rsc_stm32.h"

extern UART_HandleTypeDef huart1;
extern osMessageQId qRxHandle;
extern osMessageQId qTxHandle;
extern RngBuffer rxRingBuffer;

volatile uint32_t rscStm32RxDropped = 0;

// Polling period of the receive functions: 10 ms, at least one tick
#define RSC_STM32_POLL_TICKS  ( pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1 )

/**
 * @brief Platform-specific implementation of memory allocation
 * 
 * @param length - number of bytes requested
 * @return void* - generic pointer to the allocated chunk or NULL if unavailable
 */
void* rscPlatformMalloc(size_t length) {
//  return pvPortMalloc(length);
  return malloc(length);
}

/**
 * @brief Platform-specific implementation of memory de-allocation
 * 
 * @param ptr - pointer to the memory chunk to be freed
 */
void  rscPlatformFree(void* ptr) {
  if ( ptr ) { 
//    vPortFree(ptr);
	  free(ptr);
  }
}

/**
 * @brief Platform-specific implementation of entering critical section
 * 
 */
void rscPlatformEnterCritical(void) {
	portENTER_CRITICAL();
}

/**
 * @brief Platform-specific implementation of exiting critical section
 * 
 */
void rscPlatformExitCritical(void) {
	portEXIT_CRITICAL();
}

/**
 * @brief Platform-specific implementation of sending one byte to UART
 * 
 * @param onebyte - byte to be sent
 * @return int - number of sent bytes (1) or an error code (negative)
 */
int rscPlatformSendByte(uint8_t onebyte) {
	HAL_StatusTypeDef status = HAL_UART_Transmit(&huart1, &onebyte, 1, HAL_MAX_DELAY);
  switch (status) {
  case HAL_OK:
	  return 1;
	  break;

  case HAL_ERROR:
    return RSC_ERROR_TRX_ERROR;
    break;

  case HAL_BUSY:
	  return RSC_ERROR_TRX_ERROR;
    break;

	case HAL_TIMEOUT:
    return RSC_ERROR_TRX_TIMEOUT;
    break;
  }
	return RSC_ERROR_TRX_ERROR;
}

/**
 * @brief Platform-specific implementation of sending multiple bytes to UART
 * 
 * @param bytes - bytes buffer to be sent
 * @param length - length of the buffer
 * @return int - number of bytes sent or an error code (negative)
 */
int rscPlatformSendBytes(uint8_t* bytes, size_t length) {
	HAL_StatusTypeDef status = HAL_UART_Transmit(&huart1, bytes, length, HAL_MAX_DELAY);
  switch (status) {
  case HAL_OK:
	  return (int)length;
	  break;

  case HAL_ERROR:
    return RSC_ERROR_TRX_ERROR;
    break;

  case HAL_BUSY:
	  return RSC_ERROR_TRX_ERROR;
    break;

	case HAL_TIMEOUT:
    return RSC_ERROR_TRX_TIMEOUT;
    break;
  }
	return RSC_ERROR_TRX_ERROR;
}

/**
 * @brief Platform-specific implementation of a function that should indicate
 *        that UART has received incoming bytes. Might have delay in it to support blocking
 * 
 */
bool  rscPlatformReceiveReady(void) {
  if ( rng_buffer_is_empty(&rxRingBuffer) ) {
    rscPlatformDelayMs(10);
    return false;
  }
  return true;
}

uint8_t serial_byte_received;
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {  // Ensure the callback is from the correct UART instance
      if ( !rng_buffer_put(&rxRingBuffer, serial_byte_received) ) rscStm32RxDropped++;
      HAL_UART_Receive_IT(&huart1, &serial_byte_received, 1);
    }
}

/**
 * @brief Platform-specific implementation of receiving one byte from UART with a timeout
 *  
 * @param timeout, ms - timeout to wait for the byte - if RSC_BLOCKING - wait indefinitely
 * @return int - character received (lower byte) or error code (negative)
 */
int rscPlatformReceiveByte(uint32_t timeout) {
  uint8_t c;

//  uint32_t tout = (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);
  uint32_t st = rscPlatformMillis();

  while ( rng_buffer_count(&rxRingBuffer) == 0 ) {
	  if ( timeout < RSC_BLOCKING && rscPlatformMillis() - st > timeout ) return RSC_ERROR_REC_TIMEOUT;
    vTaskDelay(RSC_STM32_POLL_TICKS);
  }

  if ( rng_buffer_get(&rxRingBuffer, &c) ) return (int) c;

  return RSC_ERROR_REC_ERROR;
}

/**
 * @brief Platform-specific implementation of receiving bytes from UART with a timeout
 * 
 * @param buffer - pointer to the buffer where to place incoming bytes
 * @param expectedLength - number of anticipated bytes (has to be received - otherwise a timeout error occurs)
 * @param timeout 
 * @param timeout, ms - timeout to wait for the bytes - if RSC_BLOCKING - wait indefinitely
 * @return int - number of characters received or error code (negative)
 */
int rscPlatformReceiveBytes(uint8_t* buffer, size_t expectedLength, uint32_t timeout) {
  if ( buffer == NULL || expectedLength == 0 ) return RSC_ERROR_NULL;

  uint32_t st = rscPlatformMillis();
  size_t got = 0;

  // take what has arrived, under one lock; got only advances by the bytes taken
  while ( got < expectedLength ) {
    size_t done = 0;
    rng_buffer_get_elements(&rxRingBuffer, &buffer[got], expectedLength - got, &done);
    got += done;
    if ( got == expectedLength ) break;
	  if ( timeout < RSC_BLOCKING && rscPlatformMillis() - st > timeout ) return RSC_ERROR_REC_TIMEOUT;
	  vTaskDelay(RSC_STM32_POLL_TICKS);
  }
  return (int) expectedLength;
}

/**
 * @brief Platform-specific implementation of delay in milliseconds
 * 
 * @param ms - delay in milliseconds
 */
void rscPlatformDelayMs(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
  // HAL_Delay(ms);
}

/**
 * @brief Platform-specific implementation of system uptime in milliseconds
 *        (configTICK_RATE_HZ must divide 1000: portTICK_PERIOD_MS is an integer)
 * 
 * @return uint32_t - system uptime in milliseconds
 */
uint32_t rscPlatformMillis(void) {
  return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief Platform-specific implementation of placing transmit envelope into the transmit queue
 * 
 * @param qData - envelope data to be placed in the queue
 * @param timeout - timeout for the operation in milliseconds
 * @return rscError_t - error code
 */
rscError_t rscPlatformEnqueueTransmitData( rscEnvelope_t* qData, uint32_t timeout) {
  uint32_t tout = (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  rscQueueEntry_t q;
  q.rscEnvelope = *qData;
  q.timestamp = rscPlatformMillis();
  if ( xQueueSend(qTxHandle, &q, tout) ) return RSC_OK;
  return RSC_ERROR_TRX_Q_ERROR;
}

/**
 * @brief Platform-specific implementation of getting transmit envelope from the transmit queue
 * 
 * @param qData - pointer to envelope data buffer
 * @param timeout - timeout for the operation in milliseconds
 * @return rscError_t - error code
 */
rscError_t rscPlatformDequeueTransmitData( rscEnvelope_t* qData, uint32_t timeout) {
  uint32_t tout = (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  rscQueueEntry_t q;
  if ( xQueueReceive(qTxHandle, &q, tout) ) {
    *qData = q.rscEnvelope;
    return RSC_OK;
  }
  if ( uxQueueMessagesWaiting( qTxHandle ) == 0 ) return RSC_ERROR_TRX_Q_EMPTY;
  return RSC_ERROR_TRX_Q_ERROR;
}

/**
 * @brief Platform-specific implementation of placing received envelope into the Rx queue
 * 
 * @param qData - envelope data to be placed in the queue
 * @param timeout - timeout for the operation in milliseconds
 * @return rscError_t - error code
 */
rscError_t rscPlatformEnqueueReceiveData( rscEnvelope_t* qData, uint32_t timeout) {
  uint32_t tout = (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  rscQueueEntry_t q;
  q.rscEnvelope = *qData;
  q.timestamp = rscPlatformMillis();
  if ( xQueueSend(qRxHandle, &q, tout) ) return RSC_OK;
  return RSC_ERROR_REC_Q_ERROR;
}

/**
 * @brief Platform-specific implementation of getting receive envelope from the Rx queue
 * 
 * @param qData - pointer to envelope data buffer
 * @param timeout - timeout for the operation in milliseconds
 * @return rscError_t - error code
 */
rscError_t rscPlatformDequeueReceiveData( rscEnvelope_t* qData, uint32_t timeout) {
  uint32_t tout = (timeout == RSC_BLOCKING) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  rscQueueEntry_t q;
  if ( xQueueReceive(qRxHandle, &q, tout)) {
    *qData = q.rscEnvelope;
    return RSC_OK;
  }
  if ( uxQueueMessagesWaiting( qRxHandle ) == 0 ) return RSC_ERROR_REC_Q_EMPTY;
  return RSC_ERROR_REC_Q_ERROR;
}

/**
 * @brief Bridge function for FreeRTOS task for Rx Data task
 * 
 * @param argument 
 */
void startDataRx(void *argument)
{
	(void) argument;
	vTaskDelay(TASK_INITIAL_DELAY);
	rscReceiveLoop();
}

/**
 * @brief Bridge function for FreeRTOS task for Tx Data task
 * 
 * @param argument 
 */
void startDataTx(void *argument)
{
	(void) argument;
	vTaskDelay(TASK_INITIAL_DELAY);
	rscTransmitLoop();
}
